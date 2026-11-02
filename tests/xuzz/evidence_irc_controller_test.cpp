#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/time.h>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {

std::string quoteArgument(std::string_view text) {
  std::ostringstream out;
  out << std::quoted(std::string(text));
  return out.str();
}

std::uint16_t unusedLoopbackPort() {
  const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socketFd < 0) throw std::runtime_error("loopback socket failed");
  sockaddr_in address{};
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port        = 0;
  const auto bound =
      ::bind(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  socklen_t length = sizeof(address);
  const auto named =
      ::getsockname(socketFd, reinterpret_cast<sockaddr *>(&address), &length);
  const auto port = ntohs(address.sin_port);
  ::close(socketFd);
  if (bound != 0 || named != 0 || port == 0)
    throw std::runtime_error("loopback port allocation failed");
  return port;
}

struct LocalCertificate {
  EVP_PKEY *key{};
  X509 *certificate{};

  LocalCertificate() {
    EVP_PKEY_CTX *keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!keyContext || EVP_PKEY_keygen_init(keyContext) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext,
                                               NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(keyContext, &key) <= 0) {
      if (keyContext) EVP_PKEY_CTX_free(keyContext);
      throw std::runtime_error("local TLS key generation failed");
    }
    EVP_PKEY_CTX_free(keyContext);
    certificate = X509_new();
    if (!certificate || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate), 0) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate), 60) == nullptr ||
        X509_set_pubkey(certificate, key) != 1)
      throw std::runtime_error("local TLS certificate setup failed");
    X509_NAME *name = X509_get_subject_name(certificate);
    if (X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1,
            0) != 1 ||
        X509_set_issuer_name(certificate, name) != 1)
      throw std::runtime_error("local TLS certificate subject failed");
    X509V3_CTX extensionContext{};
    X509V3_set_ctx(&extensionContext, certificate, certificate, nullptr,
                   nullptr, 0);
    X509_EXTENSION *constraints = X509V3_EXT_conf_nid(
        nullptr, &extensionContext, NID_basic_constraints, "critical,CA:TRUE");
    X509_EXTENSION *san = X509V3_EXT_conf_nid(
        nullptr, &extensionContext, NID_subject_alt_name, "IP:127.0.0.1");
    if (!constraints || !san ||
        X509_add_ext(certificate, constraints, -1) != 1 ||
        X509_add_ext(certificate, san, -1) != 1) {
      X509_EXTENSION_free(constraints);
      X509_EXTENSION_free(san);
      throw std::runtime_error("local TLS SAN setup failed");
    }
    X509_EXTENSION_free(constraints);
    X509_EXTENSION_free(san);
    if (X509_sign(certificate, key, EVP_sha256()) <= 0)
      throw std::runtime_error("local TLS certificate signing failed");
  }

  ~LocalCertificate() {
    X509_free(certificate);
    EVP_PKEY_free(key);
  }
};

class TemporaryCaFile {
public:
  explicit TemporaryCaFile(X509 *certificate) {
    std::string path = "/tmp/gleditor-irc-controller-ca-XXXXXX";
    const int fd     = ::mkstemp(path.data());
    if (fd < 0) throw std::runtime_error("temporary CA file creation failed");
    path_      = std::move(path);
    FILE *file = ::fdopen(fd, "w");
    if (!file) {
      ::close(fd);
      ::unlink(path_.c_str());
      throw std::runtime_error("temporary CA file open failed");
    }
    const bool written = PEM_write_X509(file, certificate) == 1;
    const bool closed  = std::fclose(file) == 0;
    if (!written || !closed) {
      ::unlink(path_.c_str());
      throw std::runtime_error("temporary CA certificate write failed");
    }
  }
  ~TemporaryCaFile() { ::unlink(path_.c_str()); }
  const std::string &path() const { return path_; }

private:
  std::string path_;
};

class TlsIrcFixture {
public:
  TlsIrcFixture() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("IRC fixture socket failed");
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
      throw std::runtime_error("IRC fixture listener setup failed");
    socklen_t length = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &length) != 0)
      throw std::runtime_error("IRC fixture port lookup failed");
    port_   = ntohs(address.sin_port);
    caFile_ = std::make_unique<TemporaryCaFile>(identity_.certificate);
  }

  ~TlsIrcFixture() {
    if (client_ >= 0) {
      static_cast<void>(::shutdown(client_, SHUT_RDWR));
      ::close(client_);
    }
    if (listener_ >= 0) {
      static_cast<void>(::shutdown(listener_, SHUT_RDWR));
      ::close(listener_);
    }
    if (thread_.joinable()) thread_.join();
  }

  std::uint16_t port() const { return port_; }
  const std::string &caPath() const { return caFile_->path(); }

  void start() {
    thread_ = std::thread([this] {
      client_ = ::accept(listener_, nullptr, nullptr);
      if (client_ < 0) return;
      timeval timeout{3, 0};
      static_cast<void>(::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                     sizeof(timeout)));
      std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(
          SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
      if (!context ||
          SSL_CTX_use_certificate(context.get(), identity_.certificate) != 1 ||
          SSL_CTX_use_PrivateKey(context.get(), identity_.key) != 1)
        return;
      std::unique_ptr<SSL, decltype(&SSL_free)> connection(
          SSL_new(context.get()), SSL_free);
      if (!connection || SSL_set_fd(connection.get(), client_) != 1 ||
          SSL_accept(connection.get()) != 1)
        return;
      if (line(connection.get()).empty()) return;
      send(connection.get(), ":fixture CAP * LS :batch draft/chathistory "
                             "echo-message message-tags server-time\r\n");
      auto command = line(connection.get());
      while (command.starts_with("CAP REQ :")) {
        auto capability = command.substr(9);
        if (capability.ends_with("\r\n"))
          capability.resize(capability.size() - 2);
        send(connection.get(), ":fixture CAP * ACK :" + capability + "\r\n");
        command = line(connection.get());
      }
      if (!command.starts_with("CAP END")) return;
      if (!line(connection.get()).starts_with("NICK guest")) return;
      if (!line(connection.get()).starts_with("USER guest")) return;
      send(connection.get(), ":fixture 001 guest :welcome\r\n");
      if (!line(connection.get()).starts_with("JOIN #room")) return;
      send(connection.get(), ":guest!u@host JOIN #room\r\n");
      send(connection.get(),
           "@msgid=controller-live-1 :alice!u@host PRIVMSG #room :first\r\n");
      send(connection.get(),
           "@msgid=controller-live-2 :alice!u@host PRIVMSG #room :second\r\n");
    });
  }

private:
  static std::string line(SSL *connection) {
    std::string result;
    while (result.size() < 8192) {
      char value{};
      if (SSL_read(connection, &value, 1) != 1) return {};
      result.push_back(value);
      if (result.ends_with("\r\n")) return result;
    }
    return {};
  }

  static void send(SSL *connection, std::string_view text) {
    std::size_t written{};
    while (written < text.size()) {
      const auto remaining = text.size() - written;
      const auto size =
          static_cast<int>(std::min<std::size_t>(remaining, 4096));
      const int count = SSL_write(connection, text.data() + written, size);
      if (count <= 0) return;
      written += static_cast<std::size_t>(count);
    }
  }

  LocalCertificate identity_;
  std::unique_ptr<TemporaryCaFile> caFile_;
  int listener_{-1};
  int client_{-1};
  std::uint16_t port_{};
  std::thread thread_;
};

bool waitForCompletion(EvidenceController &controller) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (controller.poll()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

TEST(EvidenceIrcControllerTest,
     FailedConnectIsRetainedWithoutGrantingSessionAuthority) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("evidence-irc-connect-" + UserPermascroll::generateUniqueDeviceId());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  std::filesystem::create_directories(directory);
  const auto casePath = directory / "case";
  auto scroll         = std::make_shared<UserPermascroll>();
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute("new " + quoteArgument(casePath.string()) +
                                 " \"IRC connection test\""));
  const auto port = unusedLoopbackPort();
  const auto profile =
      "irc fixture 127.0.0.1 " + std::to_string(port) +
      " test-account test-network guest guest \"Fixture Guest\" \"\" 500 "
      "8192 4096 16 64 32 8 4096 1048576";
  ASSERT_TRUE(controller.execute(profile));
  ASSERT_TRUE(controller.execute("irc-connect fixture"));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool completed{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (controller.poll()) {
      completed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(completed);
  EXPECT_NE(controller.snapshot().status.find("without a live session"),
            std::string::npos);

  const auto records = controller.evidenceCase().records();
  EXPECT_TRUE(std::ranges::any_of(records, [](const auto &record) {
    return record.kind == "irc-observation";
  }));
  EXPECT_TRUE(std::ranges::any_of(records, [](const auto &record) {
    return record.kind == "communication-acquisition" &&
           record.attributes.contains("protocol") &&
           record.attributes.at("protocol") == "irc";
  }));
  EXPECT_FALSE(std::ranges::any_of(records, [](const auto &record) {
    return record.kind == "effect-intent" || record.kind == "effect-attempt";
  }));
  EXPECT_FALSE(controller.execute("irc-draft fixture #room hello"));
  EXPECT_FALSE(std::ranges::any_of(controller.evidenceCase().records(),
                                   [](const auto &record) {
                                     return record.kind == "effect-intent" ||
                                            record.kind == "effect-attempt";
                                   }));

  EXPECT_TRUE(controller.execute("close"));
}

TEST(EvidenceIrcControllerTest,
     VerifiedSessionRetainsJoinAndAcknowledgesLiveSnapshotsBeforeNextRead) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("evidence-irc-live-" + UserPermascroll::generateUniqueDeviceId());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  std::filesystem::create_directories(directory);
  TlsIrcFixture server;
  server.start();

  auto scroll = std::make_shared<UserPermascroll>();
  EvidenceController controller(scroll);
  const auto casePath = directory / "case";
  ASSERT_TRUE(controller.execute("new " + quoteArgument(casePath.string()) +
                                 " \"IRC session test\""));
  const auto profile =
      "irc fixture 127.0.0.1 " + std::to_string(server.port()) +
      " test-account test-network guest guest \"Fixture Guest\" " +
      quoteArgument(server.caPath()) +
      " 2000 8192 4096 16 64 32 8 4096 1048576";
  ASSERT_TRUE(controller.execute(profile));
  ASSERT_TRUE(controller.execute("irc-connect fixture"));
  ASSERT_TRUE(waitForCompletion(controller)) << controller.snapshot().status;
  EXPECT_NE(controller.snapshot().status.find("IRC TLS session connected"),
            std::string::npos);

  ASSERT_TRUE(controller.execute("irc-join fixture #room"));
  ASSERT_TRUE(waitForCompletion(controller)) << controller.snapshot().status;
  EXPECT_NE(controller.snapshot().status.find("membership confirmed"),
            std::string::npos);
  ASSERT_TRUE(controller.execute("irc-draft fixture #room never-sent"));

  const auto readOne = [&] {
    EXPECT_TRUE(controller.execute("irc-live fixture #room 1"));
    EXPECT_TRUE(waitForCompletion(controller)) << controller.snapshot().status;
  };
  readOne();
  readOne();

  const auto records = controller.evidenceCase().records();
  std::vector<std::string> messages;
  std::size_t joinReceipts{};
  std::size_t ircAcquisitions{};
  std::size_t attempts{};
  std::size_t intents{};
  for (const auto &record : records) {
    if (record.kind == "irc-message") messages.push_back(record.text);
    if (record.kind == "irc-join-receipt") ++joinReceipts;
    if (record.kind == "communication-acquisition" &&
        record.attributes.contains("protocol") &&
        record.attributes.at("protocol") == "irc")
      ++ircAcquisitions;
    if (record.kind == "effect-attempt") ++attempts;
    if (record.kind == "effect-intent" &&
        record.attributes.contains("operation") &&
        record.attributes.at("operation") == "irc-send")
      ++intents;
  }
  std::ranges::sort(messages);
  EXPECT_EQ(messages, (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(joinReceipts, 1U);
  EXPECT_GE(ircAcquisitions, 4U); // connect, join, and two live snapshots
  EXPECT_EQ(intents, 1U);
  EXPECT_EQ(attempts, 0U); // A local draft never sends by itself.

  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute("open " + quoteArgument(casePath.string())));
  std::vector<std::string> reopenedMessages;
  for (const auto &record : controller.evidenceCase().records())
    if (record.kind == "irc-message") reopenedMessages.push_back(record.text);
  std::ranges::sort(reopenedMessages);
  EXPECT_EQ(reopenedMessages, (std::vector<std::string>{"first", "second"}));
  EXPECT_TRUE(controller.execute("close"));
}

} // namespace
} // namespace xanadu::evidence
