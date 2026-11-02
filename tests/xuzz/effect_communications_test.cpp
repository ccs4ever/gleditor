#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/xanadu/evidence/effect_runtime.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {
class LoopbackPostServer {
public:
  explicit LoopbackPostServer(std::string body, bool holdResponse = false)
      : body_(std::move(body)), holdResponse_(holdResponse) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("loopback socket failed");
    int reuse = 1;
    static_cast<void>(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                                   sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0)
      throw std::runtime_error("loopback listener setup failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("loopback port lookup failed");
    port_ = ntohs(address.sin_port);
  }
  ~LoopbackPostServer() {
    release();
    if (client_ >= 0) {
      static_cast<void>(::shutdown(client_, SHUT_RDWR));
      static_cast<void>(::close(client_));
    }
    if (listener_ >= 0) static_cast<void>(::close(listener_));
    if (thread_.joinable()) thread_.join();
  }
  LoopbackPostServer(const LoopbackPostServer &)            = delete;
  LoopbackPostServer &operator=(const LoopbackPostServer &) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  void start() {
    thread_ = std::thread([this] { serve(); });
  }
  void waitForRequest() {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, std::chrono::seconds(3),
                        [&] { return requestSeen_; });
    if (!requestSeen_) throw std::runtime_error("POST was not received");
  }
  void release() {
    {
      const std::scoped_lock lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }
  [[nodiscard]] std::string request() const {
    const std::scoped_lock lock(mutex_);
    return request_;
  }

private:
  void serve() {
    client_ = ::accept(listener_, nullptr, nullptr);
    if (client_ < 0) return;
    std::string received;
    std::size_t contentLength{};
    while (received.find("\r\n\r\n") == std::string::npos &&
           received.size() < 65536) {
      char byte{};
      if (::recv(client_, &byte, 1, 0) != 1) return;
      received += byte;
    }
    const auto headerEnd = received.find("\r\n\r\n");
    const auto headers   = received.substr(0, headerEnd);
    const auto lengthAt  = headers.find("Content-Length:");
    if (lengthAt != std::string::npos)
      contentLength = static_cast<std::size_t>(std::stoul(headers.substr(
          lengthAt + std::string_view("Content-Length:").size())));
    while (received.size() - headerEnd - 4 < contentLength) {
      char byte{};
      if (::recv(client_, &byte, 1, 0) != 1) return;
      received += byte;
    }
    {
      const std::scoped_lock lock(mutex_);
      request_     = std::move(received);
      requestSeen_ = true;
    }
    condition_.notify_all();
    if (holdResponse_) {
      std::unique_lock lock(mutex_);
      condition_.wait_for(lock, std::chrono::seconds(3),
                          [&] { return released_; });
    }
    std::string response = "HTTP/1.1 201 Created\r\nContent-Type: ";
    response += "application/json\r\nConnection: close\r\nContent-Length: ";
    response += std::to_string(body_.size());
    response += "\r\n\r\n";
    response += body_;
    std::size_t sent{};
    while (sent < response.size()) {
      const auto count = ::send(client_, response.data() + sent,
                                response.size() - sent, MSG_NOSIGNAL);
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
    }
  }

  int listener_{-1};
  int client_{-1};
  std::uint16_t port_{};
  std::string body_;
  bool holdResponse_{};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool requestSeen_{};
  bool released_{};
  std::string request_;
  std::thread thread_;
};

class LoopbackIrcServer {
public:
  LoopbackIrcServer() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("IRC loopback socket failed");
    int reuse = 1;
    static_cast<void>(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                                   sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0)
      throw std::runtime_error("IRC loopback listener setup failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("IRC loopback port lookup failed");
    port_ = ntohs(address.sin_port);
  }
  ~LoopbackIrcServer() {
    if (client_ >= 0) {
      static_cast<void>(::shutdown(client_, SHUT_RDWR));
      static_cast<void>(::close(client_));
    }
    if (listener_ >= 0) static_cast<void>(::close(listener_));
    if (thread_.joinable()) thread_.join();
  }
  LoopbackIrcServer(const LoopbackIrcServer &)            = delete;
  LoopbackIrcServer &operator=(const LoopbackIrcServer &) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  void start(bool echo) {
    thread_ = std::thread([this, echo] { serve(echo); });
  }

private:
  static std::string readLine(int fd) {
    std::string result;
    while (result.size() < 8192) {
      char byte{};
      if (::recv(fd, &byte, 1, 0) != 1) return {};
      result += byte;
      if (result.ends_with("\r\n")) return result;
    }
    return {};
  }
  static bool sendText(int fd, std::string_view text) {
    std::size_t sent{};
    while (sent < text.size()) {
      const auto count =
          ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
      if (count <= 0) return false;
      sent += static_cast<std::size_t>(count);
    }
    return true;
  }
  void serve(bool echo) {
    client_ = ::accept(listener_, nullptr, nullptr);
    if (client_ < 0) return;
    timeval timeout{3, 0};
    static_cast<void>(::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                   sizeof(timeout)));
    if (!readLine(client_).starts_with("CAP LS 302")) return;
    sendText(client_, echo ? ":fixture CAP * LS :message-tags echo-message\r\n"
                           : ":fixture CAP * LS :message-tags\r\n");
    auto command = readLine(client_);
    while (command.starts_with("CAP REQ :")) {
      auto requested = command.substr(9);
      if (requested.ends_with("\r\n")) requested.resize(requested.size() - 2);
      sendText(client_, ":fixture CAP * ACK :" + requested + "\r\n");
      command = readLine(client_);
    }
    if (!command.starts_with("CAP END")) return;
    static_cast<void>(readLine(client_)); // NICK
    static_cast<void>(readLine(client_)); // USER
    sendText(client_, ":fixture 001 guest :welcome\r\n");
    if (!readLine(client_).starts_with("JOIN #room")) return;
    sendText(client_, ":guest!user@loopback JOIN #room\r\n");
    const auto sent = readLine(client_);
    if (!sent.starts_with("PRIVMSG #room :hello")) return;
    if (echo)
      sendText(client_,
               "@msgid=irc-42 :guest!user@loopback PRIVMSG #room :hello\r\n");
  }

  int listener_{-1};
  int client_{-1};
  std::uint16_t port_{};
  std::thread thread_;
};

class LoopbackSmtpServer {
public:
  explicit LoopbackSmtpServer(bool dropFinal = false, bool holdFinal = false)
      : dropFinal_(dropFinal), holdFinal_(holdFinal) {
    tlsContext_ = makeTlsContext();
    listener_   = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("SMTP socket failed");
    int reuse = 1;
    static_cast<void>(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                                   sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0)
      throw std::runtime_error("SMTP listener setup failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("SMTP port lookup failed");
    port_ = ntohs(address.sin_port);
  }
  ~LoopbackSmtpServer() {
    releaseFinal();
    if (listener_ >= 0) static_cast<void>(::shutdown(listener_, SHUT_RDWR));
    if (client_ >= 0) {
      static_cast<void>(::shutdown(client_, SHUT_RDWR));
      static_cast<void>(::close(client_));
    }
    if (listener_ >= 0) static_cast<void>(::close(listener_));
    if (thread_.joinable()) thread_.join();
    if (tls_) SSL_free(tls_);
    SSL_CTX_free(tlsContext_);
    std::filesystem::remove(caBundlePath_);
  }
  LoopbackSmtpServer(const LoopbackSmtpServer &)            = delete;
  LoopbackSmtpServer &operator=(const LoopbackSmtpServer &) = delete;
  [[nodiscard]] std::string authority() const {
    return "smtp://127.0.0.1:" + std::to_string(port_);
  }
  [[nodiscard]] std::string caBundlePath() const { return caBundlePath_; }
  void start() {
    thread_ = std::thread([this] { serve(); });
  }
  bool waitForData() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return gotData_ || finished_;
    }) && gotData_;
  }
  void releaseFinal() {
    {
      const std::scoped_lock lock(mutex_);
      finalReleased_ = true;
    }
    condition_.notify_all();
  }
  bool waitFinished() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return finished_; });
  }
  [[nodiscard]] bool gotData() const {
    const std::scoped_lock lock(mutex_);
    return gotData_;
  }

private:
  SSL_CTX *makeTlsContext() {
    EVP_PKEY_CTX *keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!keyContext || EVP_PKEY_keygen_init(keyContext) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048) <= 0)
      throw std::runtime_error("SMTP TLS fixture key setup failed");
    EVP_PKEY *key{};
    if (EVP_PKEY_keygen(keyContext, &key) <= 0) {
      EVP_PKEY_CTX_free(keyContext);
      throw std::runtime_error("SMTP TLS fixture key generation failed");
    }
    EVP_PKEY_CTX_free(keyContext);
    X509 *certificate = X509_new();
    if (!certificate || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) ||
        X509_set_pubkey(certificate, key) != 1)
      throw std::runtime_error("SMTP TLS fixture certificate setup failed");
    auto *name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1, 0);
    X509_set_issuer_name(certificate, name);
    X509V3_CTX extensionContext;
    X509V3_set_ctx(&extensionContext, certificate, certificate, nullptr,
                   nullptr, 0);
    auto *san =
        X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name,
                            const_cast<char *>("IP:127.0.0.1"));
    if (!san || X509_add_ext(certificate, san, -1) != 1 ||
        X509_sign(certificate, key, EVP_sha256()) <= 0)
      throw std::runtime_error("SMTP TLS fixture certificate signing failed");
    X509_EXTENSION_free(san);
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    if (!context || SSL_CTX_use_certificate(context, certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context, key) != 1)
      throw std::runtime_error("SMTP TLS fixture initialization failed");
    char path[]  = "/tmp/xuzz-effect-smtp-ca-XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) throw std::runtime_error("SMTP TLS CA file creation failed");
    ::close(fd);
    caBundlePath_ = path;
    std::unique_ptr<FILE, decltype(&std::fclose)> output(std::fopen(path, "w"),
                                                         std::fclose);
    if (!output || PEM_write_X509(output.get(), certificate) != 1)
      throw std::runtime_error("SMTP TLS CA file write failed");
    X509_free(certificate);
    EVP_PKEY_free(key);
    return context;
  }
  bool send(std::string_view text) {
    std::size_t offset{};
    while (offset < text.size()) {
      const auto count =
          tls_ ? SSL_write(tls_, text.data() + offset,
                           static_cast<int>(text.size() - offset))
               : static_cast<int>(::send(client_, text.data() + offset,
                                         text.size() - offset, MSG_NOSIGNAL));
      if (count <= 0) return false;
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }
  std::string readLine() {
    std::string line;
    while (line.size() < 65536) {
      char byte{};
      const auto count = tls_ ? SSL_read(tls_, &byte, 1)
                              : static_cast<int>(::recv(client_, &byte, 1, 0));
      if (count != 1) return {};
      line += byte;
      if (line.ends_with("\r\n")) return line;
    }
    return {};
  }
  void finish() {
    {
      const std::scoped_lock lock(mutex_);
      finished_ = true;
    }
    condition_.notify_all();
  }
  void serve() {
    client_ = ::accept(listener_, nullptr, nullptr);
    if (client_ < 0) return finish();
    timeval timeout{4, 0};
    static_cast<void>(::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                   sizeof(timeout)));
    if (!send("220 fixture ESMTP\r\n")) return finish();
    if (!readLine().starts_with("EHLO ")) return finish();
    if (!send("250-fixture\r\n250-STARTTLS\r\n250 HELP\r\n")) return finish();
    if (readLine() != "STARTTLS\r\n" || !send("220 begin TLS\r\n"))
      return finish();
    tls_ = SSL_new(tlsContext_);
    if (!tls_ || SSL_set_fd(tls_, client_) != 1 || SSL_accept(tls_) != 1)
      return finish();
    if (!readLine().starts_with("EHLO ") ||
        !send("250-fixture\r\n250-AUTH PLAIN\r\n250 HELP\r\n"))
      return finish();
    auto auth = readLine();
    if (!auth.starts_with("AUTH PLAIN")) return finish();
    const bool needsChallenge = auth == "AUTH PLAIN\r\n";
    std::fill(auth.begin(), auth.end(), '\0');
    if (needsChallenge) {
      if (!send("334 \r\n")) return finish();
      auto response = readLine();
      std::fill(response.begin(), response.end(), '\0');
    }
    if (!send("235 authenticated\r\n")) return finish();
    if (!readLine().starts_with("MAIL FROM:") || !send("250 sender\r\n"))
      return finish();
    while (true) {
      auto line = readLine();
      if (line.starts_with("RCPT TO:")) {
        if (!send("250 recipient\r\n")) return finish();
      } else if (line == "DATA\r\n") {
        if (!send("354 continue\r\n")) return finish();
        while (true) {
          line = readLine();
          if (line.empty()) return finish();
          if (line == ".\r\n") break;
          std::fill(line.begin(), line.end(), '\0');
        }
        {
          const std::scoped_lock lock(mutex_);
          gotData_ = true;
        }
        condition_.notify_all();
        if (holdFinal_) {
          std::unique_lock lock(mutex_);
          condition_.wait(lock, [&] { return finalReleased_; });
        }
        if (dropFinal_) {
          static_cast<void>(::shutdown(client_, SHUT_RDWR));
          return finish();
        }
        static_cast<void>(send("250 accepted locally\r\n"));
        return finish();
      } else {
        return finish();
      }
    }
  }

  int listener_{-1};
  int client_{-1};
  SSL_CTX *tlsContext_{};
  SSL *tls_{};
  std::uint16_t port_{};
  std::string caBundlePath_;
  bool dropFinal_{};
  bool holdFinal_{};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool finished_{};
  bool gotData_{};
  bool finalReleased_{};
  std::thread thread_;
};

class EffectCommunicationsTest : public ::testing::Test {
protected:
  std::filesystem::path parent =
      std::filesystem::temp_directory_path() /
      ("evidence-effect-communications-" +
       xanadu::UserPermascroll::generateUniqueDeviceId());
  std::filesystem::path path = parent / "case";
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();
  EvidenceCase evidence{scroll};

  void SetUp() override { evidence.create(path, "communication effect test"); }
  void TearDown() override {
    evidence.close();
    std::filesystem::remove_all(parent);
  }

  static EffectRequest
  fediverseRequest(std::string origin = "https://social.example") {
    EffectRequest request;
    request.kind          = EffectKind::FediversePost;
    request.fediversePost = FediversePostRequest{
        .instanceOrigin  = std::move(origin),
        .accountId       = "acct-17",
        .text            = "reviewed body",
        .contentWarning  = "warning",
        .visibility      = "unlisted",
        .replyToRestId   = "parent-2",
        .audience        = {"https://www.w3.org/ns/activitystreams#Public"},
        .idempotencyKey  = "intent-key-1",
        .accountActorUri = "https://social.example/@author"};
    return request;
  }

  static std::shared_ptr<FediverseCredentialLease>
  credential(std::string origin, std::string token = "synthetic-secret") {
    return std::make_shared<FediverseCredentialLease>(
        std::move(origin), "acct-17", std::move(token),
        std::chrono::steady_clock::now() + std::chrono::minutes(2));
  }

  static bool waitFor(EffectRuntime &runtime) {
    for (int i = 0; i < 1000; ++i) {
      if (runtime.poll()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  static IrcConnectionConfig ircConfig(std::uint16_t port) {
    const auto endpoint = "irc://127.0.0.1:" + std::to_string(port);
    IrcConnectionConfig config;
    config.host               = "127.0.0.1";
    config.port               = port;
    config.serverAuthority    = endpoint;
    config.networkIncarnation = "runtime-test-network";
    config.nick               = "guest";
    config.useTls             = false;
    config.timeout            = std::chrono::milliseconds(600);
    return config;
  }

  static std::shared_ptr<IrcSession> connectAndJoin(LoopbackIrcServer &server) {
    auto session = std::make_shared<IrcSession>(ircConfig(server.port()));
    std::atomic_bool cancel{};
    if (!session->connect(cancel).connected)
      throw std::runtime_error("controlled IRC registration failed");
    const auto identity = session->identity();
    auto revoked        = std::make_shared<std::atomic_bool>(false);
    IrcJoinAuthority authority{identity.serverAuthority,
                               identity.networkIncarnation,
                               "#room",
                               {"#room"},
                               std::chrono::steady_clock::now() +
                                   std::chrono::seconds(5),
                               revoked};
    if (!session->join(authority, cancel).joined)
      throw std::runtime_error("controlled IRC JOIN was not confirmed");
    return session;
  }

  static EffectRequest ircRequest(const IrcConnectionIdentity &identity) {
    EffectRequest request;
    request.kind = EffectKind::IrcSend;
    request.ircSend =
        IrcSendRequest{.serverAuthority       = identity.serverAuthority,
                       .networkIncarnation    = identity.networkIncarnation,
                       .connectionIncarnation = identity.connectionIncarnation,
                       .target                = "#room",
                       .payload               = "hello",
                       .audience              = {"#room"}};
    return request;
  }

  static EffectRequest mailRequest(const LoopbackSmtpServer &server) {
    MailDraftInput input;
    input.from       = "sender@example.test";
    input.recipients = {{"to", "one@example.test"},
                        {"bcc", "blind@example.test"},
                        {"to", "two@example.test"}};
    input.subject    = "runtime SMTP fixture";
    input.textBody   = "synthetic body\nBcc: body@example.test";
    auto draft       = buildMimeDraft(input, "effect-mail-runtime-1");
    auto profile =
        SmtpSubmissionProfile{.serverUrl       = server.authority(),
                              .serverAuthority = server.authority(),
                              .accountScope    = "runtime-test-account",
                              .caBundlePath    = server.caBundlePath(),
                              .allowCleartextLoopbackWithoutCredentials = false,
                              .timeoutMilliseconds                      = 5000};
    EffectRequest request;
    request.kind                 = EffectKind::MailSubmit;
    request.limits.timeoutMs     = 5000;
    request.limits.maxInputBytes = draft.bytes.size();
    request.limits.maxFields     = draft.envelopeRecipients.size();
    request.mailSubmit =
        MailSubmitRequest{std::move(profile), std::move(draft)};
    return request;
  }

  static std::shared_ptr<MailCredentialLease>
  mailCredential(const SmtpSubmissionProfile &profile, std::string host = {}) {
    MailCredentialBroker broker;
    return std::make_shared<MailCredentialLease>(broker.issue(
        {.serverAuthority = host.empty() ? profile.serverAuthority : host,
         .accountScope    = profile.accountScope,
         .operation       = MailCredentialOperation::Submit},
        "synthetic-runtime-user", "synthetic-runtime-password",
        std::chrono::minutes(2)));
  }
};

TEST_F(EffectCommunicationsTest, FediverseIntentRetainsExactTypedRequest) {
  EffectRuntime runtime(evidence, scroll);
  const auto expected = fediverseRequest();
  const auto intent   = runtime.draft(expected);
  const auto retained = runtime.request(intent);
  ASSERT_TRUE(retained.fediversePost);
  EXPECT_EQ(retained.fediversePost->instanceOrigin,
            expected.fediversePost->instanceOrigin);
  EXPECT_EQ(retained.fediversePost->accountId,
            expected.fediversePost->accountId);
  EXPECT_EQ(retained.fediversePost->text, expected.fediversePost->text);
  EXPECT_EQ(retained.fediversePost->contentWarning,
            expected.fediversePost->contentWarning);
  EXPECT_EQ(retained.fediversePost->visibility,
            expected.fediversePost->visibility);
  EXPECT_EQ(retained.fediversePost->replyToRestId,
            expected.fediversePost->replyToRestId);
  EXPECT_EQ(retained.fediversePost->audience, expected.fediversePost->audience);
  EXPECT_EQ(retained.fediversePost->idempotencyKey,
            expected.fediversePost->idempotencyKey);
  EXPECT_EQ(retained.fediversePost->accountActorUri,
            expected.fediversePost->accountActorUri);
}

TEST_F(EffectCommunicationsTest,
       CommunicationIntentRejectsUnrelatedFieldsAndUnsupportedAudience) {
  EffectRuntime runtime(evidence, scroll);
  auto withGenericField       = fediverseRequest();
  withGenericField.stdinBytes = "synthetic-secret";
  EXPECT_THROW(static_cast<void>(runtime.draft(withGenericField)),
               std::invalid_argument);
  auto withPrivateAudience                    = fediverseRequest();
  withPrivateAudience.fediversePost->audience = {"acct-17"};
  EXPECT_THROW(static_cast<void>(runtime.draft(withPrivateAudience)),
               std::invalid_argument);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-intent";
      }));
}

TEST_F(EffectCommunicationsTest,
       FediverseTargetCredentialAndActorUriRefuseBeforePersistence) {
  EffectRuntime runtime(evidence, scroll);
  auto credentialOrigin =
      fediverseRequest("https://user:secret@social.example");
  EXPECT_THROW(static_cast<void>(runtime.draft(credentialOrigin)),
               std::invalid_argument);
  auto credentialActor = fediverseRequest();
  credentialActor.fediversePost->accountActorUri =
      "https://user:secret@social.example/@author";
  EXPECT_THROW(static_cast<void>(runtime.draft(credentialActor)),
               std::invalid_argument);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-intent";
      }));
}

TEST_F(EffectCommunicationsTest, FediverseNeedsMatchingHostCredentialBinding) {
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(fediverseRequest());
  EXPECT_THROW(static_cast<void>(runtime.authorize(intent)),
               std::runtime_error);

  auto mismatched = std::make_shared<FediverseCredentialLease>(
      "https://other.example", "acct-17", "synthetic-secret",
      std::chrono::steady_clock::now() + std::chrono::minutes(2));
  EXPECT_THROW(runtime.bindFediverseCredential(intent, mismatched),
               std::invalid_argument);

  constexpr std::string_view secret = "synthetic-secret";
  auto credential                   = EffectCommunicationsTest::credential(
      "https://social.example", std::string(secret));
  runtime.bindFediverseCredential(intent, credential);
  const auto grant = runtime.authorize(intent);
  ASSERT_NE(grant, 0U);
  for (const auto &record : evidence.records()) {
    EXPECT_EQ(record.text.find(secret), std::string::npos);
    for (const auto &[name, value] : record.attributes) {
      static_cast<void>(name);
      EXPECT_EQ(value.find(secret), std::string::npos);
    }
  }
}

TEST_F(EffectCommunicationsTest, RevokedCredentialCannotBeReauthorized) {
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(fediverseRequest());
  auto credential =
      EffectCommunicationsTest::credential("https://social.example");
  runtime.bindFediverseCredential(intent, credential);
  static_cast<void>(runtime.authorize(intent));
  static_cast<void>(credential->revoke());
  EXPECT_THROW(static_cast<void>(runtime.authorize(intent)),
               std::runtime_error);
}

TEST_F(EffectCommunicationsTest, FediversePostUsesDurableAdmissionAndReceipt) {
  constexpr std::string_view body =
      R"({"id":"post-42","uri":"https://social.example/@author/42","account":{"id":"acct-17"}})";
  LoopbackPostServer server{std::string(body)};
  server.start();
  const auto origin = "http://127.0.0.1:" + std::to_string(server.port());
  EffectRuntimeOptions options;
  options.fediverse.allowLoopbackHttpForTests = true;
  EffectRuntime runtime(evidence, scroll, options);
  const auto intent = runtime.draft(fediverseRequest(origin));
  runtime.bindFediverseCredential(intent, credential(origin));
  const auto grant   = runtime.authorize(intent);
  const auto attempt = runtime.dispatch(intent, grant);
  ASSERT_TRUE(waitFor(runtime));
  const auto transcript = server.request();
  EXPECT_NE(transcript.find("POST /api/v1/statuses"), std::string::npos);
  EXPECT_NE(transcript.find("Authorization: Bearer synthetic-secret"),
            std::string::npos);
  EXPECT_NE(transcript.find("intent-key-1"), std::string::npos);
  const auto records = evidence.records();
  const auto receipt = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == attempt;
  });
  ASSERT_NE(receipt, records.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "accepted");
  EXPECT_EQ(receipt->attributes.at("post-rest-id"), "post-42");
  EXPECT_EQ(receipt->attributes.at("post-actor-identity-basis"),
            "configured-account-correspondence");
  EXPECT_EQ(receipt->attributes.at("post-actor-uri"),
            "https://social.example/@author");
  EXPECT_EQ(receipt->attributes.at("post-response-redacted"), "no");
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "fediverse-post-receipt" &&
           record.parent == receipt->id &&
           record.attributes.at("actor-identity-basis") ==
               "configured-account-correspondence" &&
           record.attributes.at("actor-uri") ==
               "https://social.example/@author";
  }));
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "fediverse-post-response" &&
           record.parent == receipt->id && record.text == body &&
           record.attributes.at("redacted") == "no";
  }));
  EXPECT_EQ(std::ranges::count_if(records,
                                  [&](const Record &record) {
                                    return record.kind == "effect-attempt" &&
                                           record.parent == intent;
                                  }),
            1);
}

TEST_F(EffectCommunicationsTest,
       LateRedactedFediverseReceiptRecoversWithoutResending) {
  constexpr std::string_view token = "synthetic-secret";
  const std::string response =
      "{\"id\":\"post-43\",\"token\":\"synthetic-secret\","
      "\"uri\":\"https://social.example/@author/43\","
      "\"account\":{\"id\":\"acct-17\"}}";
  LoopbackPostServer server{response, true};
  server.start();
  const auto origin = "http://127.0.0.1:" + std::to_string(server.port());
  EffectRuntimeOptions options;
  options.fediverse.allowLoopbackHttpForTests = true;
  EffectRuntime runtime(evidence, scroll, options);
  const auto intent = runtime.draft(fediverseRequest(origin));
  runtime.bindFediverseCredential(intent,
                                  credential(origin, std::string(token)));
  const auto grant   = runtime.authorize(intent);
  const auto attempt = runtime.dispatch(intent, grant);
  server.waitForRequest();

  const auto original = parent / "original-case";
  evidence.close();
  std::filesystem::rename(path, original);
  evidence.create(path, "replacement case");
  server.release();
  ASSERT_TRUE(waitFor(runtime));
  evidence.close();
  std::filesystem::remove_all(path);
  std::filesystem::rename(original, path);
  evidence.open(path);

  EffectRuntime recovered(evidence, scroll, options);
  const auto records = evidence.records();
  const auto receipt = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == attempt;
  });
  ASSERT_NE(receipt, records.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "accepted");
  EXPECT_EQ(receipt->attributes.at("post-response-redacted"), "yes");
  EXPECT_EQ(receipt->attributes.at("post-response-kind"),
            "withheld-derivative");
  EXPECT_EQ(receipt->attributes.at("post-actor-identity-basis"), "unresolved");
  EXPECT_TRUE(receipt->attributes.at("post-actor-uri").empty());
  EXPECT_EQ(receipt->attributes.at("post-response-body").find(token),
            std::string::npos);
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "fediverse-post-response" &&
           record.parent == receipt->id &&
           record.label == "withheld-derivative" &&
           record.attributes.at("redacted") == "yes" &&
           record.text.find(token) == std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "fediverse-post-receipt" &&
           record.parent == receipt->id &&
           record.attributes.at("actor-identity-basis") == "unresolved" &&
           record.attributes.at("actor-uri").empty();
  }));
  EXPECT_THROW(static_cast<void>(recovered.dispatch(intent, grant)),
               std::invalid_argument);
  EXPECT_EQ(std::ranges::count_if(records,
                                  [&](const Record &record) {
                                    return record.kind == "effect-attempt" &&
                                           record.parent == intent;
                                  }),
            1);
}

TEST_F(EffectCommunicationsTest, IrcIntentCannotAuthorizeWithoutBoundSession) {
  EffectRuntime runtime(evidence, scroll);
  EffectRequest request;
  request.kind        = EffectKind::IrcSend;
  request.ircSend     = IrcSendRequest{.serverAuthority       = "irc-fixture",
                                       .networkIncarnation    = "network-1",
                                       .connectionIncarnation = "connection-1",
                                       .target                = "#room",
                                       .payload               = "hello",
                                       .audience              = {"#room"}};
  const auto intent   = runtime.draft(request);
  const auto retained = runtime.request(intent);
  ASSERT_TRUE(retained.ircSend);
  EXPECT_EQ(retained.ircSend->serverAuthority, "irc-fixture");
  EXPECT_EQ(retained.ircSend->networkIncarnation, "network-1");
  EXPECT_EQ(retained.ircSend->connectionIncarnation, "connection-1");
  EXPECT_EQ(retained.ircSend->audience, (std::vector<std::string>{"#room"}));
  EXPECT_THROW(static_cast<void>(runtime.authorize(intent)),
               std::runtime_error);
}

TEST_F(EffectCommunicationsTest,
       IrcEchoIsTerminalAcceptedReceiptOnTheSharedAttempt) {
  LoopbackIrcServer server;
  server.start(true);
  auto session = connectAndJoin(server);
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(ircRequest(session->identity()));
  runtime.bindIrcSession(intent, session);
  const auto authority = runtime.authorize(intent);
  const auto attempt   = runtime.dispatch(intent, authority);
  ASSERT_TRUE(waitFor(runtime));

  const auto records = evidence.records();
  const auto receipt = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == attempt;
  });
  ASSERT_NE(receipt, records.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "accepted");
  EXPECT_EQ(receipt->attributes.at("protocol"), "irc-send");
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "irc-send-receipt" && record.parent == receipt->id &&
           record.attributes.at("echo-message-id") == "irc-42";
  }));
  EXPECT_EQ(std::ranges::count_if(records,
                                  [&](const Record &record) {
                                    return record.kind == "effect-attempt" &&
                                           record.parent == intent;
                                  }),
            1);
}

TEST_F(EffectCommunicationsTest,
       IrcWriteWithoutEchoRecoversUnknownWithoutResend) {
  LoopbackIrcServer server;
  server.start(false);
  auto session = connectAndJoin(server);
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(ircRequest(session->identity()));
  runtime.bindIrcSession(intent, session);
  const auto authority = runtime.authorize(intent);
  const auto attempt   = runtime.dispatch(intent, authority);
  ASSERT_TRUE(waitFor(runtime));
  const auto firstRecords = evidence.records();
  const auto receipt =
      std::ranges::find_if(firstRecords, [&](const Record &record) {
        return record.kind == "effect-receipt" && record.parent == attempt;
      });
  ASSERT_NE(receipt, firstRecords.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "outcome-unknown");

  evidence.close();
  evidence.open(path);
  EffectRuntime recovered(evidence, scroll);
  EXPECT_THROW(static_cast<void>(recovered.dispatch(intent, authority)),
               std::invalid_argument);
  EXPECT_EQ(std::ranges::count_if(evidence.records(),
                                  [&](const Record &record) {
                                    return record.kind == "effect-attempt" &&
                                           record.parent == intent;
                                  }),
            1);
}

TEST_F(EffectCommunicationsTest,
       IrcConnectionMismatchRefusesSessionBindingBeforeAttempt) {
  auto config   = ircConfig(6667);
  auto session  = std::make_shared<IrcSession>(config);
  auto identity = session->identity();
  identity.connectionIncarnation += "-changed";
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(ircRequest(identity));
  EXPECT_THROW(runtime.bindIrcSession(intent, session), std::invalid_argument);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-attempt";
      }));
}

TEST_F(EffectCommunicationsTest, MailIntentRoundTripsExactDraftWithoutSecrets) {
  LoopbackSmtpServer server;
  const auto request = mailRequest(server);
  EffectRuntime runtime(evidence, scroll);
  const auto intent   = runtime.draft(request);
  const auto retained = runtime.request(intent);
  ASSERT_TRUE(retained.mailSubmit);
  EXPECT_EQ(retained.mailSubmit->draft.bytes, request.mailSubmit->draft.bytes);
  EXPECT_EQ(retained.mailSubmit->draft.envelopeRecipients,
            request.mailSubmit->draft.envelopeRecipients);
  EXPECT_EQ(retained.mailSubmit->profile.serverAuthority,
            request.mailSubmit->profile.serverAuthority);
  for (const auto &record : evidence.records()) {
    EXPECT_EQ(record.text.find("synthetic-runtime-password"),
              std::string::npos);
    for (const auto &[key, value] : record.attributes) {
      static_cast<void>(key);
      EXPECT_EQ(value.find("synthetic-runtime-password"), std::string::npos);
    }
  }
}

TEST_F(EffectCommunicationsTest,
       MailSubmitRequiresExactLiveSubmitCredentialBeforeGrant) {
  LoopbackSmtpServer server;
  const auto request = mailRequest(server);
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(request);
  EXPECT_THROW(static_cast<void>(runtime.authorize(intent)),
               std::runtime_error);
  auto wrong =
      mailCredential(request.mailSubmit->profile, "smtp://other.example:587");
  EXPECT_THROW(runtime.bindMailCredential(intent, wrong),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(runtime.authorize(intent)),
               std::runtime_error);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-attempt";
      }));
}

TEST_F(EffectCommunicationsTest,
       RevokingOneMailGrantKeepsASeparatelyBoundSharedLeaseUsable) {
  LoopbackSmtpServer server;
  const auto first                       = mailRequest(server);
  auto second                            = mailRequest(server);
  second.mailSubmit->draft.correlationId = "effect-mail-runtime-2";
  EffectRuntime runtime(evidence, scroll);
  const auto firstIntent  = runtime.draft(first);
  const auto secondIntent = runtime.draft(second);
  auto sharedCredential   = mailCredential(first.mailSubmit->profile);
  runtime.bindMailCredential(firstIntent, sharedCredential);
  runtime.bindMailCredential(secondIntent, sharedCredential);
  const auto firstGrant  = runtime.authorize(firstIntent);
  const auto secondGrant = runtime.authorize(secondIntent);
  runtime.revoke(firstGrant);
  EXPECT_NO_THROW(static_cast<void>(runtime.authorize(secondIntent)));
  EXPECT_NE(secondGrant, firstGrant);
}

TEST_F(EffectCommunicationsTest,
       MailSubmitRetainsLocalAcceptanceAndOrderedRecipientOutcomes) {
  LoopbackSmtpServer server;
  server.start();
  const auto request = mailRequest(server);
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(request);
  auto credential   = mailCredential(request.mailSubmit->profile);
  runtime.bindMailCredential(intent, credential);
  const auto grant   = runtime.authorize(intent);
  const auto attempt = runtime.dispatch(intent, grant);
  ASSERT_TRUE(waitFor(runtime));
  ASSERT_TRUE(server.waitFinished());
  const auto records = evidence.records();
  const auto receipt = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == attempt;
  });
  ASSERT_NE(receipt, records.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "accepted");
  EXPECT_EQ(receipt->attributes.at("protocol"), "smtp-submission");
  EXPECT_EQ(receipt->attributes.at("local-acceptance-is-delivery"), "no");
  std::vector<std::string> recipients;
  for (const auto &record : records)
    if (record.kind == "smtp-recipient-outcome" && record.parent == receipt->id)
      recipients.push_back(record.text);
  EXPECT_EQ(recipients,
            (std::vector<std::string>{"one@example.test", "blind@example.test",
                                      "two@example.test"}));
  for (const auto &record : records) {
    EXPECT_EQ(record.text.find("synthetic-runtime-password"),
              std::string::npos);
    for (const auto &[key, value] : record.attributes) {
      static_cast<void>(key);
      EXPECT_EQ(value.find("synthetic-runtime-password"), std::string::npos);
    }
  }
}

TEST_F(EffectCommunicationsTest,
       MailLostFinalResponseRecoversUnknownWithoutResending) {
  LoopbackSmtpServer server(true);
  server.start();
  const auto request = mailRequest(server);
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(request);
  auto credential   = mailCredential(request.mailSubmit->profile);
  runtime.bindMailCredential(intent, credential);
  const auto grant   = runtime.authorize(intent);
  const auto attempt = runtime.dispatch(intent, grant);
  ASSERT_TRUE(waitFor(runtime));
  ASSERT_TRUE(server.waitFinished());
  auto records       = evidence.records();
  const auto receipt = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == attempt;
  });
  ASSERT_NE(receipt, records.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "outcome-unknown");
  EXPECT_EQ(receipt->attributes.at("may-have-executed"), "yes");
  evidence.close();
  evidence.open(path);
  EffectRuntime recovered(evidence, scroll);
  EXPECT_EQ(std::ranges::count_if(evidence.records(),
                                  [&](const Record &record) {
                                    return record.kind == "effect-attempt" &&
                                           record.parent == intent;
                                  }),
            1);
  EXPECT_THROW(static_cast<void>(recovered.dispatch(intent, grant)),
               std::invalid_argument);
}

TEST_F(EffectCommunicationsTest,
       LateMailReceiptReconcilesOriginalCaseRecipientOrderWithoutResend) {
  LoopbackSmtpServer server(false, true);
  server.start();
  const auto request = mailRequest(server);
  auto runtime       = std::make_unique<EffectRuntime>(evidence, scroll);
  const auto intent  = runtime->draft(request);
  auto credential    = mailCredential(request.mailSubmit->profile);
  runtime->bindMailCredential(intent, credential);
  const auto grant   = runtime->authorize(intent);
  const auto attempt = runtime->dispatch(intent, grant);
  ASSERT_TRUE(server.waitForData());
  const auto original = parent / "original-mail-case";
  evidence.close();
  std::filesystem::rename(path, original);
  evidence.create(path, "replacement mail case");
  server.releaseFinal();
  ASSERT_TRUE(waitFor(*runtime));
  runtime.reset();
  ASSERT_TRUE(server.waitFinished());
  evidence.close();
  std::filesystem::remove_all(path);
  std::filesystem::rename(original, path);
  evidence.open(path);
  EffectRuntime recovered(evidence, scroll);
  const auto records = evidence.records();
  const auto receipt = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == attempt;
  });
  ASSERT_NE(receipt, records.end());
  EXPECT_EQ(receipt->attributes.at("outcome"), "accepted");
  std::vector<std::string> recipients;
  for (const auto &record : records)
    if (record.kind == "smtp-recipient-outcome" && record.parent == receipt->id)
      recipients.push_back(record.text);
  EXPECT_EQ(recipients,
            (std::vector<std::string>{"one@example.test", "blind@example.test",
                                      "two@example.test"}));
  EXPECT_EQ(std::ranges::count_if(records,
                                  [&](const Record &record) {
                                    return record.kind == "effect-attempt" &&
                                           record.parent == intent;
                                  }),
            1);
  EXPECT_THROW(static_cast<void>(recovered.dispatch(intent, grant)),
               std::invalid_argument);
}
} // namespace
} // namespace xanadu::evidence
