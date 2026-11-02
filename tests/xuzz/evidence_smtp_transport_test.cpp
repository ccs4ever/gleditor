#include "common/xanadu/evidence/mail_transport.hpp"

#include <gtest/gtest.h>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/xanadu/evidence/mail_transport_internal.hpp"

namespace xanadu::evidence {
namespace {

class LocalSmtpServer {
public:
  struct Behavior {
    std::vector<int> recipientCodes{250};
    int dataCommandCode{354};
    int finalCode{250};
    bool disconnectBeforeData{};
    bool disconnectAfterData{};
    bool releaseFinalResponse{};
    bool advertiseStartTls{};
    bool requireStartTls{};
    bool blockAuthAdvertisement{};
    bool multilineFinal{};
    bool truncateFinalContinuation{};
  };

  LocalSmtpServer() : LocalSmtpServer(Behavior{}) {}
  explicit LocalSmtpServer(Behavior behavior) : behavior_(std::move(behavior)) {
    if (behavior_.requireStartTls) tlsContext_ = makeTlsContext();
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("SMTP fixture socket failed");
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
      throw std::runtime_error("SMTP fixture listener setup failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("SMTP fixture port lookup failed");
    port_ = ntohs(address.sin_port);
  }
  ~LocalSmtpServer() {
    releaseFinalResponse();
    if (client_ >= 0) {
      static_cast<void>(::shutdown(client_, SHUT_RDWR));
      static_cast<void>(::close(client_));
    }
    if (listener_ >= 0) static_cast<void>(::close(listener_));
    if (thread_.joinable()) thread_.join();
    if (tls_) SSL_free(tls_);
    if (tlsContext_) SSL_CTX_free(tlsContext_);
    if (!caBundlePath_.empty()) std::filesystem::remove(caBundlePath_);
  }
  LocalSmtpServer(const LocalSmtpServer &)            = delete;
  LocalSmtpServer &operator=(const LocalSmtpServer &) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::string authority() const {
    return "smtp://127.0.0.1:" + std::to_string(port_);
  }
  [[nodiscard]] std::string caBundlePath() const { return caBundlePath_; }
  [[nodiscard]] std::string hostAuthority(std::string_view host) const {
    return "smtp://" + std::string(host) + ":" + std::to_string(port_);
  }
  bool waitForTlsEhlo() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return tlsEhloSeen_ || finished_;
    }) && tlsEhloSeen_;
  }
  void releaseAuthAdvertisement() {
    {
      const std::scoped_lock lock(mutex_);
      releaseAuthAdvertisement_ = true;
    }
    condition_.notify_all();
  }
  void start() {
    thread_ = std::thread([this] { serve(); });
  }
  bool waitForData() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return dataSeen_ || finished_;
    }) && dataSeen_;
  }
  void releaseFinalResponse() {
    {
      const std::scoped_lock lock(mutex_);
      finalReleased_ = true;
    }
    condition_.notify_all();
  }
  [[nodiscard]] std::string message() const {
    const std::scoped_lock lock(mutex_);
    return message_;
  }
  [[nodiscard]] bool sawAuth() const {
    const std::scoped_lock lock(mutex_);
    return sawAuth_;
  }
  [[nodiscard]] bool sawExpectedCredentials() const {
    const std::scoped_lock lock(mutex_);
    return expectedCredentialsReceived_;
  }

private:
  SSL_CTX *makeTlsContext() {
    EVP_PKEY_CTX *keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!keyContext || EVP_PKEY_keygen_init(keyContext) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext, 2048) <= 0) {
      if (keyContext) EVP_PKEY_CTX_free(keyContext);
      throw std::runtime_error("TLS fixture key setup failed");
    }
    EVP_PKEY *key{};
    if (EVP_PKEY_keygen(keyContext, &key) <= 0) {
      EVP_PKEY_CTX_free(keyContext);
      throw std::runtime_error("TLS fixture key generation failed");
    }
    EVP_PKEY_CTX_free(keyContext);
    X509 *certificate = X509_new();
    if (!certificate || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) ||
        X509_set_pubkey(certificate, key) != 1) {
      if (certificate) X509_free(certificate);
      EVP_PKEY_free(key);
      throw std::runtime_error("TLS fixture certificate setup failed");
    }
    auto *name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1, 0);
    X509_set_issuer_name(certificate, name);
    X509V3_CTX extensionContext;
    X509V3_set_ctx(&extensionContext, certificate, certificate, nullptr,
                   nullptr, 0);
    X509_EXTENSION *san =
        X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name,
                            const_cast<char *>("IP:127.0.0.1"));
    if (!san || X509_add_ext(certificate, san, -1) != 1 ||
        X509_sign(certificate, key, EVP_sha256()) <= 0) {
      if (san) X509_EXTENSION_free(san);
      X509_free(certificate);
      EVP_PKEY_free(key);
      throw std::runtime_error("TLS fixture certificate signing failed");
    }
    X509_EXTENSION_free(san);
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    if (!context || SSL_CTX_use_certificate(context, certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context, key) != 1) {
      if (context) SSL_CTX_free(context);
      X509_free(certificate);
      EVP_PKEY_free(key);
      throw std::runtime_error("TLS fixture server initialization failed");
    }
    char path[]  = "/tmp/xuzz-smtp-ca-XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
      SSL_CTX_free(context);
      X509_free(certificate);
      EVP_PKEY_free(key);
      throw std::runtime_error("TLS fixture CA file creation failed");
    }
    ::close(fd);
    caBundlePath_ = path;
    std::unique_ptr<FILE, decltype(&std::fclose)> caFile(
        std::fopen(caBundlePath_.c_str(), "w"), std::fclose);
    if (!caFile || PEM_write_X509(caFile.get(), certificate) != 1) {
      std::filesystem::remove(caBundlePath_);
      SSL_CTX_free(context);
      X509_free(certificate);
      EVP_PKEY_free(key);
      throw std::runtime_error("TLS fixture CA file write failed");
    }
    X509_free(certificate);
    EVP_PKEY_free(key);
    return context;
  }
  bool sendLine(std::string_view line) {
    std::size_t sent{};
    while (sent < line.size()) {
      const auto count =
          tls_ ? SSL_write(tls_, line.data() + sent,
                           static_cast<int>(line.size() - sent))
               : static_cast<int>(::send(client_, line.data() + sent,
                                         line.size() - sent, MSG_NOSIGNAL));
      if (count <= 0) return false;
      sent += static_cast<std::size_t>(count);
    }
    return true;
  }
  std::string readLine() {
    std::string line;
    while (line.size() < 65536) {
      char byte{};
      const auto read = tls_ ? SSL_read(tls_, &byte, 1)
                             : static_cast<int>(::recv(client_, &byte, 1, 0));
      if (read != 1) return {};
      line.push_back(byte);
      if (line.ends_with("\r\n")) return line;
    }
    return {};
  }
  bool sendCode(int code, std::string_view text) {
    return sendLine(std::to_string(code) + " " + std::string(text) + "\r\n");
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
    if (client_ < 0) {
      finish();
      return;
    }
    timeval timeout{5, 0};
    static_cast<void>(::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                   sizeof(timeout)));
    if (!sendCode(220, "fixture ESMTP")) {
      finish();
      return;
    }
    std::size_t recipientIndex{};
    while (true) {
      auto line = readLine();
      if (line.empty()) break;
      if (line.starts_with("EHLO ") || line.starts_with("HELO ")) {
        sendLine("250-fixture\r\n250-PIPELINING\r\n250-SIZE 40000000\r\n");
        if (behavior_.requireStartTls && !tls_) {
          sendLine("250-STARTTLS\r\n250 HELP\r\n");
          continue;
        }
        if (behavior_.advertiseStartTls && !tls_) sendLine("250-STARTTLS\r\n");
        if (behavior_.blockAuthAdvertisement) {
          {
            std::unique_lock lock(mutex_);
            tlsEhloSeen_ = true;
            condition_.notify_all();
            condition_.wait(lock, [&] { return releaseAuthAdvertisement_; });
          }
        }
        sendLine("250-AUTH PLAIN LOGIN\r\n");
        sendLine("250 HELP\r\n");
      } else if (line.starts_with("AUTH ")) {
        bool matches{};
        constexpr std::string_view expected =
            "AHN5bnRoZXRpYy11c2VyAHN5bnRoZXRpYy1wYXNzd29yZA==";
        if (line.starts_with("AUTH PLAIN ")) {
          auto value = std::string_view(line).substr(11);
          if (value.ends_with("\r\n")) value.remove_suffix(2);
          matches = value == expected;
        } else if (line == "AUTH PLAIN\r\n") {
          sendLine("334 \r\n");
          auto response = readLine();
          if (response.ends_with("\r\n")) response.resize(response.size() - 2);
          matches = response == expected;
          std::fill(response.begin(), response.end(), '\0');
        }
        {
          const std::scoped_lock lock(mutex_);
          sawAuth_                     = true;
          expectedCredentialsReceived_ = matches;
        }
        std::fill(line.begin(), line.end(), '\0');
        sendCode(matches ? 235 : 535,
                 matches ? "authenticated" : "authentication rejected");
      } else if (line == "STARTTLS\r\n") {
        if (!sendCode(220, "begin TLS")) break;
        tls_ = SSL_new(tlsContext_);
        if (!tls_ || SSL_set_fd(tls_, client_) != 1 || SSL_accept(tls_) != 1)
          break;
      } else if (line.starts_with("MAIL FROM:")) {
        sendCode(250, "sender accepted");
      } else if (line.starts_with("RCPT TO:")) {
        const auto code = recipientIndex < behavior_.recipientCodes.size()
                              ? behavior_.recipientCodes[recipientIndex]
                              : 250;
        ++recipientIndex;
        sendCode(code, code >= 200 && code < 300 ? "recipient accepted"
                                                 : "recipient rejected");
      } else if (line == "DATA\r\n") {
        if (behavior_.disconnectBeforeData) break;
        if (!sendCode(behavior_.dataCommandCode, "data status")) break;
        if (behavior_.dataCommandCode != 354) break;
        while (true) {
          auto part = readLine();
          if (part.empty()) break;
          if (part == ".\r\n") {
            {
              const std::scoped_lock lock(mutex_);
              dataSeen_ = true;
            }
            condition_.notify_all();
            break;
          }
          if (part.starts_with("..")) part.erase(part.begin());
          {
            const std::scoped_lock lock(mutex_);
            message_ += part;
          }
        }
        if (behavior_.releaseFinalResponse) {
          std::unique_lock lock(mutex_);
          condition_.wait_for(lock, std::chrono::seconds(5),
                              [&] { return finalReleased_; });
        }
        if (behavior_.disconnectAfterData) break;
        if (behavior_.multilineFinal) {
          if (!sendLine(std::to_string(behavior_.finalCode) +
                        "-message result continuation\r\n"))
            break;
          if (behavior_.truncateFinalContinuation) break;
          sendLine(std::to_string(behavior_.finalCode) +
                   " message result complete\r\n");
        } else {
          sendCode(behavior_.finalCode, "message result");
        }
      } else if (line == "QUIT\r\n") {
        sendCode(221, "closing");
        break;
      } else {
        sendCode(500, "unsupported fixture command");
      }
    }
    finish();
  }

  int listener_{-1};
  int client_{-1};
  SSL_CTX *tlsContext_{};
  SSL *tls_{};
  std::uint16_t port_{};
  std::string caBundlePath_;
  Behavior behavior_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::string message_;
  bool dataSeen_{};
  bool finalReleased_{};
  bool tlsEhloSeen_{};
  bool releaseAuthAdvertisement_{};
  bool finished_{};
  bool sawAuth_{};
  bool expectedCredentialsReceived_{};
  std::thread thread_;
};

SerializedMailDraft fixtureDraft(std::vector<std::string> recipients = {
                                     "one@example.test"}) {
  MailDraftInput input;
  input.from     = "sender@example.test";
  input.subject  = "controlled SMTP fixture";
  input.textBody = "synthetic controlled message";
  for (auto &address : recipients)
    input.recipients.push_back({"to", std::move(address)});
  return buildMimeDraft(input, "smtp-test-1");
}

MailSubmissionAuthority authorityFor(const LocalSmtpServer &server,
                                     const SerializedMailDraft &draft,
                                     std::string account = "test-account") {
  return {server.authority(),
          std::move(account),
          mailDraftFingerprint(draft),
          draft.envelopeFrom,
          draft.envelopeRecipients,
          std::chrono::steady_clock::now() + std::chrono::minutes(1),
          std::make_shared<std::atomic_bool>(false)};
}

MailSubmissionAuthority
authorityForEndpoint(std::string endpoint, const SerializedMailDraft &draft,
                     std::string account = "test-account") {
  return {endpoint,
          std::move(account),
          mailDraftFingerprint(draft),
          draft.envelopeFrom,
          draft.envelopeRecipients,
          std::chrono::steady_clock::now() + std::chrono::minutes(1),
          std::make_shared<std::atomic_bool>(false)};
}

SmtpSubmissionProfile profileFor(const LocalSmtpServer &server,
                                 std::string account = "test-account") {
  return {.serverUrl                                = server.authority(),
          .serverAuthority                          = server.authority(),
          .accountScope                             = std::move(account),
          .caBundlePath                             = {},
          .allowCleartextLoopbackWithoutCredentials = true,
          .timeoutMilliseconds                      = 5000};
}

SmtpSubmissionProfile tlsProfileFor(std::string url, std::string caBundlePath) {
  return {.serverUrl                                = url,
          .serverAuthority                          = url,
          .accountScope                             = "test-account",
          .caBundlePath                             = std::move(caBundlePath),
          .allowCleartextLoopbackWithoutCredentials = false,
          .timeoutMilliseconds                      = 5000};
}

TEST(EvidenceSmtpTransportTest, SendsExactOwnedDraftAndReportsAcceptance) {
  LocalSmtpServer::Behavior behavior;
  behavior.multilineFinal = true;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft();
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Accepted);
  EXPECT_TRUE(result.dataStarted);
  ASSERT_EQ(result.recipients.size(), 1U);
  EXPECT_EQ(result.recipients[0].state, SmtpRecipientState::Accepted);
  EXPECT_EQ(result.finalResponseCode, 250);
  EXPECT_EQ(server.message(), draft.bytes);
  EXPECT_NE(server.message().find("synthetic controlled message"),
            std::string::npos);
  EXPECT_EQ(result.safeDiagnostic.find("synthetic"), std::string::npos);
}

TEST(EvidenceSmtpTransportTest, RetainsEachEnvelopeRecipientOutcome) {
  LocalSmtpServer::Behavior behavior;
  behavior.recipientCodes = {250, 550};
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft({"one@example.test", "two@example.test"});
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Accepted);
  ASSERT_EQ(result.recipients.size(), 2U);
  EXPECT_EQ(result.recipients[0].state, SmtpRecipientState::Accepted);
  EXPECT_EQ(result.recipients[1].state, SmtpRecipientState::Rejected);
  EXPECT_EQ(result.recipients[1].responseCode, 550);
}

TEST(EvidenceSmtpTransportTest, ExplicitDataRejectionIsTerminalRejection) {
  LocalSmtpServer::Behavior behavior;
  behavior.dataCommandCode = 554;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft();
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Rejected);
  EXPECT_FALSE(result.dataStarted);
  EXPECT_EQ(result.finalResponseCode, 554);
}

TEST(EvidenceSmtpTransportTest,
     ExplicitFinalRejectionAfterDataIsNotOutcomeUnknown) {
  LocalSmtpServer::Behavior behavior;
  behavior.finalCode = 554;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft();
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Rejected);
  EXPECT_TRUE(result.dataStarted);
  EXPECT_EQ(result.finalResponseCode, 554);
}

TEST(EvidenceSmtpTransportTest, DisconnectBeforeDataIsNotRemoteAcceptance) {
  LocalSmtpServer::Behavior behavior;
  behavior.disconnectBeforeData = true;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft();
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Failed);
  EXPECT_FALSE(result.dataStarted);
}

TEST(EvidenceSmtpTransportTest, LostFinalResponseAfterDataIsUnknown) {
  LocalSmtpServer::Behavior behavior;
  behavior.disconnectAfterData = true;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft();
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::OutcomeUnknown);
  EXPECT_TRUE(result.dataStarted);
  EXPECT_EQ(result.finalResponseCode, 0);
}

TEST(EvidenceSmtpTransportTest,
     TruncatedMultilineFinalResponseRemainsOutcomeUnknown) {
  LocalSmtpServer::Behavior behavior;
  behavior.multilineFinal            = true;
  behavior.truncateFinalContinuation = true;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft = fixtureDraft();
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  const auto result = submitSmtp(profileFor(server), draft, noCredentials,
                                 authorityFor(server, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::OutcomeUnknown);
  EXPECT_TRUE(result.dataStarted);
  EXPECT_EQ(result.finalResponseCode, 0);
}

TEST(EvidenceSmtpTransportTest,
     CancellationAfterDataCannotClaimAcceptanceOrResend) {
  LocalSmtpServer::Behavior behavior;
  behavior.disconnectAfterData  = true;
  behavior.releaseFinalResponse = true;
  LocalSmtpServer server{std::move(behavior)};
  server.start();
  const auto draft     = fixtureDraft();
  const auto authority = authorityFor(server, draft);
  const MailCredentialLease noCredentials;
  std::atomic_bool cancel{};
  SmtpSubmissionResult result;
  std::thread submitter([&] {
    result =
        submitSmtp(profileFor(server), draft, noCredentials, authority, cancel);
  });
  EXPECT_TRUE(server.waitForData());
  cancel.store(true, std::memory_order_relaxed);
  server.releaseFinalResponse();
  submitter.join();
  EXPECT_EQ(result.state, SmtpTransactionState::OutcomeUnknown);
  EXPECT_TRUE(result.dataStarted);
}

TEST(EvidenceSmtpTransportTest,
     CredentialLeaseCannotAuthenticateBeforeStartTls) {
  LocalSmtpServer server;
  server.start();
  const auto draft                                 = fixtureDraft();
  auto profile                                     = profileFor(server);
  profile.allowCleartextLoopbackWithoutCredentials = false;
  MailCredentialBroker broker;
  auto lease = broker.issue({.serverAuthority = server.authority(),
                             .accountScope    = "test-account",
                             .operation = MailCredentialOperation::Submit},
                            "synthetic-user", "synthetic-password",
                            std::chrono::minutes(1));
  const std::atomic_bool cancel{};
  const auto result =
      submitSmtp(profile, draft, lease, authorityFor(server, draft), cancel);
  EXPECT_NE(result.state, SmtpTransactionState::Accepted);
  EXPECT_FALSE(server.sawAuth());
  EXPECT_EQ(result.safeDiagnostic.find("synthetic-password"),
            std::string::npos);
  broker.revoke(lease);
}

TEST(EvidenceSmtpTransportTest,
     TrustedTlsAllowsAuthenticationButRejectsUntrustedAndWrongHost) {
  LocalSmtpServer::Behavior behavior;
  behavior.requireStartTls = true;
  LocalSmtpServer trusted{behavior};
  trusted.start();
  const auto draft = fixtureDraft();
  MailCredentialBroker broker;
  auto lease = broker.issue({.serverAuthority = trusted.authority(),
                             .accountScope    = "test-account",
                             .operation = MailCredentialOperation::Submit},
                            "synthetic-user", "synthetic-password",
                            std::chrono::minutes(1));
  std::atomic_bool cancel{};
  auto profile = tlsProfileFor(trusted.authority(), trusted.caBundlePath());
  EXPECT_FALSE(smtpSubmissionProblem(profile, draft));
  auto result =
      submitSmtp(profile, draft, lease, authorityFor(trusted, draft), cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Accepted);
  EXPECT_TRUE(trusted.sawAuth());
  EXPECT_TRUE(trusted.sawExpectedCredentials());
  broker.revoke(lease);

  LocalSmtpServer untrusted{behavior};
  untrusted.start();
  auto untrustedProfile = tlsProfileFor(untrusted.authority(), "");
  auto untrustedLease   = broker.issue(
      {.serverAuthority = untrusted.authority(),
         .accountScope    = "test-account",
         .operation       = MailCredentialOperation::Submit},
      "synthetic-user", "synthetic-password", std::chrono::minutes(1));
  result = submitSmtp(untrustedProfile, draft, untrustedLease,
                      authorityFor(untrusted, draft), cancel);
  EXPECT_NE(result.state, SmtpTransactionState::Accepted);
  EXPECT_FALSE(untrusted.sawAuth());
  broker.revoke(untrustedLease);

  LocalSmtpServer wrongHost{behavior};
  wrongHost.start();
  auto wrongHostUrl     = wrongHost.hostAuthority("localhost");
  auto wrongHostProfile = tlsProfileFor(wrongHostUrl, wrongHost.caBundlePath());
  auto wrongHostLease   = broker.issue(
      {.serverAuthority = wrongHostUrl,
         .accountScope    = "test-account",
         .operation       = MailCredentialOperation::Submit},
      "synthetic-user", "synthetic-password", std::chrono::minutes(1));
  result = submitSmtp(wrongHostProfile, draft, wrongHostLease,
                      authorityForEndpoint(wrongHostUrl, draft), cancel);
  EXPECT_NE(result.state, SmtpTransactionState::Accepted);
  EXPECT_FALSE(wrongHost.sawAuth());
  broker.revoke(wrongHostLease);
}

TEST(EvidenceSmtpTransportTest,
     RevocationDuringTrustedTlsHandshakePreventsAuthentication) {
  LocalSmtpServer::Behavior behavior;
  behavior.requireStartTls        = true;
  behavior.blockAuthAdvertisement = true;
  LocalSmtpServer server{behavior};
  server.start();
  const auto draft = fixtureDraft();
  MailCredentialBroker broker;
  auto lease           = broker.issue({.serverAuthority = server.authority(),
                                       .accountScope    = "test-account",
                                       .operation = MailCredentialOperation::Submit},
                                      "synthetic-user", "synthetic-password",
                                      std::chrono::minutes(1));
  const auto authority = authorityFor(server, draft);
  auto profile = tlsProfileFor(server.authority(), server.caBundlePath());
  std::atomic_bool cancel{};
  SmtpSubmissionResult result;
  std::thread submitter(
      [&] { result = submitSmtp(profile, draft, lease, authority, cancel); });
  const bool reachedTlsEhlo = server.waitForTlsEhlo();
  broker.revoke(lease);
  server.releaseAuthAdvertisement();
  submitter.join();
  EXPECT_TRUE(reachedTlsEhlo);
  EXPECT_NE(result.state, SmtpTransactionState::Accepted);
  EXPECT_FALSE(server.sawAuth());
}

TEST(EvidenceSmtpTransportTest,
     RevokedOrMismatchedAuthorityRefusesBeforeNetworkIo) {
  LocalSmtpServer server;
  const auto draft = fixtureDraft();
  auto authority   = authorityFor(server, draft);
  authority.revoked->store(true, std::memory_order_relaxed);
  const std::atomic_bool cancel{};
  const MailCredentialLease noCredentials;
  auto result =
      submitSmtp(profileFor(server), draft, noCredentials, authority, cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Failed);
  EXPECT_FALSE(result.dataStarted);

  authority.revoked->store(false, std::memory_order_relaxed);
  const auto mismatched = fixtureDraft({"other@example.test"});
  result = submitSmtp(profileFor(server), mismatched, noCredentials, authority,
                      cancel);
  EXPECT_EQ(result.state, SmtpTransactionState::Failed);
  EXPECT_FALSE(result.dataStarted);
}

} // namespace
} // namespace xanadu::evidence
