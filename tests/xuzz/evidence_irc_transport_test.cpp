#include "common/xanadu/evidence/irc_transport.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace xanadu::evidence {
namespace {
class LoopbackIrcServer {
public:
  LoopbackIrcServer() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("loopback socket failed");
    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
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
  ~LoopbackIrcServer() {
    if (client_ >= 0) {
      ::shutdown(client_, SHUT_RDWR);
      ::close(client_);
    }
    if (listener_ >= 0) ::close(listener_);
    if (thread_.joinable()) thread_.join();
  }
  std::uint16_t port() const { return port_; }
  void start(std::function<void(int)> handler) {
    thread_ = std::thread([this, handler = std::move(handler)] {
      client_ = ::accept(listener_, nullptr, nullptr);
      if (client_ >= 0) {
        timeval timeout{2, 0};
        (void)::setsockopt(client_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout));
        handler(client_);
      }
    });
  }
  static std::string line(int fd) {
    std::string result;
    while (result.size() < 8192) {
      char c{};
      auto n = ::recv(fd, &c, 1, 0);
      if (n != 1) return {};
      result.push_back(c);
      if (result.size() >= 2 && result.ends_with("\r\n")) return result;
    }
    return {};
  }
  static void sendText(int fd, std::string_view value) {
    std::size_t done = 0;
    while (done < value.size()) {
      auto n =
          ::send(fd, value.data() + done, value.size() - done, MSG_NOSIGNAL);
      if (n <= 0) return;
      done += static_cast<std::size_t>(n);
    }
  }
  static void handshake(int fd, std::string_view capabilities,
                        bool splitLs = false) {
    (void)line(fd); // CAP LS 302
    if (splitLs) {
      sendText(fd, ":fixture CAP * LS * :server-time\r\n");
      sendText(fd, ":fixture CAP * LS :message-tags echo-message " +
                       std::string(capabilities) + "\r\n");
    } else {
      sendText(fd, ":fixture CAP * LS :" + std::string(capabilities) + "\r\n");
    }
    auto command = line(fd);
    while (command.starts_with("CAP REQ :")) {
      auto capability = command.substr(9);
      if (capability.ends_with("\r\n"))
        capability.resize(capability.size() - 2);
      sendText(fd, ":fixture CAP * ACK :" + capability + "\r\n");
      command = line(fd);
    }
    // CAP END and registration are sent after the CAP reply.
    for (int i = 0; i < 2; ++i) (void)line(fd);
    sendText(fd, ":fixture 001 guest :welcome\r\n");
  }

private:
  int listener_{-1};
  int client_{-1};
  std::uint16_t port_{};
  std::thread thread_;
};

IrcConnectionConfig localConfig(std::uint16_t port) {
  IrcConnectionConfig config;
  config.host               = "127.0.0.1";
  config.port               = port;
  config.serverAuthority    = "irc://127.0.0.1:" + std::to_string(port);
  config.networkIncarnation = "fixture-net-1";
  config.nick               = "guest";
  config.useTls             = false;
  config.timeout            = std::chrono::milliseconds(500);
  return config;
}

IrcSendAuthority authority(const IrcConnectionConfig &config,
                           std::string connectionIncarnation,
                           std::shared_ptr<std::atomic_bool> revoked,
                           std::vector<std::string> audience = {"#room"}) {
  return {config.serverAuthority,
          config.networkIncarnation,
          std::move(connectionIncarnation),
          "#room",
          "hello",
          std::move(audience),
          std::chrono::steady_clock::now() + std::chrono::seconds(5),
          std::move(revoked)};
}

IrcJoinAuthority joinAuthority(const IrcConnectionConfig &config,
                               std::string target,
                               std::shared_ptr<std::atomic_bool> revoked) {
  return {config.serverAuthority,
          config.networkIncarnation,
          target,
          {target},
          std::chrono::steady_clock::now() + std::chrono::seconds(5),
          std::move(revoked)};
}

struct TestCertificate {
  EVP_PKEY *key{};
  X509 *certificate{};
  explicit TestCertificate(std::string_view commonName = "wrong.invalid") {
    EVP_PKEY_CTX *keyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!keyContext || EVP_PKEY_keygen_init(keyContext) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext,
                                               NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(keyContext, &key) <= 0) {
      if (keyContext) EVP_PKEY_CTX_free(keyContext);
      throw std::runtime_error("test TLS key creation failed");
    }
    EVP_PKEY_CTX_free(keyContext);
    certificate = X509_new();
    if (!certificate || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate), 0) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate), 60) == nullptr ||
        X509_set_pubkey(certificate, key) != 1)
      throw std::runtime_error("test TLS certificate creation failed");
    X509_NAME *name = X509_get_subject_name(certificate);
    const std::string commonNameText(commonName);
    if (X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>(commonNameText.c_str()), -1,
            -1, 0) != 1 ||
        X509_set_issuer_name(certificate, name) != 1 ||
        X509_sign(certificate, key, EVP_sha256()) <= 0)
      throw std::runtime_error("test TLS certificate signing failed");
    X509V3_CTX extensionContext{};
    X509V3_set_ctx(&extensionContext, certificate, certificate, nullptr,
                   nullptr, 0);
    X509_EXTENSION *constraints = X509V3_EXT_conf_nid(
        nullptr, &extensionContext, NID_basic_constraints, "critical,CA:TRUE");
    X509_EXTENSION *subjectAltName = X509V3_EXT_conf_nid(
        nullptr, &extensionContext, NID_subject_alt_name, "IP:127.0.0.1");
    if (!constraints || !subjectAltName ||
        X509_add_ext(certificate, constraints, -1) != 1 ||
        X509_add_ext(certificate, subjectAltName, -1) != 1) {
      X509_EXTENSION_free(constraints);
      X509_EXTENSION_free(subjectAltName);
      throw std::runtime_error("test TLS certificate extensions failed");
    }
    X509_EXTENSION_free(constraints);
    X509_EXTENSION_free(subjectAltName);
    if (X509_sign(certificate, key, EVP_sha256()) <= 0)
      throw std::runtime_error("test TLS certificate signing failed");
  }
  ~TestCertificate() {
    X509_free(certificate);
    EVP_PKEY_free(key);
  }
};

class TemporaryCaFile {
public:
  explicit TemporaryCaFile(X509 *certificate) {
    std::string path = "/tmp/gleditor-irc-ca-XXXXXX";
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

class TestSigpipeBlock {
public:
  TestSigpipeBlock() {
    sigemptyset(&set_);
    sigaddset(&set_, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0) {
      active_ = true;
      sigset_t pending{};
      if (sigpending(&pending) == 0)
        wasPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }
  ~TestSigpipeBlock() {
    if (!active_) return;
    if (!wasPending_) {
      timespec noWait{};
      while (sigtimedwait(&set_, nullptr, &noWait) == SIGPIPE) {
      }
    }
    static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
  }

private:
  sigset_t set_{};
  sigset_t previous_{};
  bool active_{};
  bool wasPending_{};
};

std::string tlsLine(SSL *connection) {
  std::string result;
  while (result.size() < 8192) {
    char value{};
    if (SSL_read(connection, &value, 1) != 1) return {};
    result.push_back(value);
    if (result.ends_with("\r\n")) return result;
  }
  return {};
}

void tlsSend(SSL *connection, std::string_view value) {
  std::size_t written{};
  TestSigpipeBlock noSigpipe;
  while (written < value.size()) {
    const int count = SSL_write(connection, value.data() + written,
                                static_cast<int>(value.size() - written));
    if (count <= 0) return;
    written += static_cast<std::size_t>(count);
  }
}

std::string saslPlainToken(std::string_view username,
                           std::string_view password) {
  std::string plain;
  plain.push_back('\0');
  plain += username;
  plain.push_back('\0');
  plain += password;
  std::string encoded(4 * ((plain.size() + 2) / 3), '\0');
  const int size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                      reinterpret_cast<const unsigned char *>(plain.data()),
                      static_cast<int>(plain.size()));
  encoded.resize(static_cast<std::size_t>(size));
  OPENSSL_cleanse(plain.data(), plain.size());
  return encoded;
}
} // namespace

TEST(EvidenceIrcTransportTest, ParsesOwnedTagsAndPreservesMessageIdentity) {
  LoopbackIrcServer server;
  server.start([&](int fd) {
    LoopbackIrcServer::handshake(fd, "message-tags server-time echo-message");
    LoopbackIrcServer::sendText(fd, "@msgid=local-1;time=2026-01-01T00:00:00Z "
                                    ":alice!u@host PRIVMSG guest :hello\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected) << connected.detail;
  auto result = session.readLive(1, cancel);
  ASSERT_EQ(result.observation.messages.size(), 1U);
  const auto &message = result.observation.messages.front();
  EXPECT_EQ(message.target, "guest");
  EXPECT_EQ(message.text, "hello");
  EXPECT_EQ(message.messageId, "local-1");
  EXPECT_EQ(message.serverTime, "2026-01-01T00:00:00Z");
}

TEST(EvidenceIrcTransportTest,
     MultilineCapabilityAdvertisementAndPartialNakAreRecorded) {
  LoopbackIrcServer server;
  std::atomic_bool capEndFollowedAllReplies{};
  server.start([&](int fd) {
    (void)LoopbackIrcServer::line(fd);
    LoopbackIrcServer::sendText(fd, ":fixture CAP * LS * :server-time\r\n");
    LoopbackIrcServer::sendText(
        fd, ":fixture CAP * LS :message-tags echo-message\r\n");
    auto command              = LoopbackIrcServer::line(fd);
    std::size_t responseCount = 0;
    while (command.starts_with("CAP REQ :")) {
      auto capability = command.substr(9);
      if (capability.ends_with("\r\n"))
        capability.resize(capability.size() - 2);
      const bool reject = capability == "message-tags";
      LoopbackIrcServer::sendText(fd, ":fixture CAP * " +
                                          std::string(reject ? "NAK" : "ACK") +
                                          " :" + capability + "\r\n");
      ++responseCount;
      command = LoopbackIrcServer::line(fd);
    }
    capEndFollowedAllReplies =
        command.starts_with("CAP END") && responseCount == 3;
    (void)LoopbackIrcServer::line(fd);
    (void)LoopbackIrcServer::line(fd);
    LoopbackIrcServer::sendText(fd, ":fixture 001 guest :welcome\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  auto result = session.connect(cancel);
  EXPECT_TRUE(result.connected) << result.detail;
  EXPECT_TRUE(capEndFollowedAllReplies.load());
  EXPECT_EQ(result.observation.capabilities,
            (std::vector<std::string>{"server-time", "echo-message"}));
  EXPECT_FALSE(result.observation.gaps.empty());
}

TEST(EvidenceIrcTransportTest, UnsupportedAndExpiredHistoryCursorsCreateGaps) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "server-time");
    (void)LoopbackIrcServer::line(fd);
    LoopbackIrcServer::sendText(fd, ":fixture 400 guest :cursor expired\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session.connect(cancel).connected);
  auto result = session.history({"#room", "old-id", 5}, cancel);
  EXPECT_FALSE(result.observation.gaps.empty());
  EXPECT_EQ(result.observation.gaps.back().fromCursor, "old-id");
}

TEST(EvidenceIrcTransportTest, HistoryWithoutClosedBatchIsPartial) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "draft/chathistory message-tags");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    (void)LoopbackIrcServer::line(fd);
    LoopbackIrcServer::sendText(
        fd, "@msgid=history-1 :alice!u@h PRIVMSG #room :one event\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session.connect(cancel).connected);
  ASSERT_TRUE(
      session
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  auto result = session.history({"#room", "cursor-1", 5}, cancel);
  ASSERT_EQ(result.observation.messages.size(), 1U);
  EXPECT_EQ(result.observation.messages.front().text, "one event");
  EXPECT_EQ(result.observation.state, CaptureState::Partial);
  ASSERT_FALSE(result.observation.gaps.empty());
  EXPECT_NE(result.observation.gaps.back().reason.find("BATCH"),
            std::string::npos);
}

TEST(EvidenceIrcTransportTest, ChannelEventsRequireConfirmedJoinScope) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "message-tags");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    LoopbackIrcServer::sendText(
        fd, ":alice!u@h PRIVMSG #outside :must be discarded\r\n");
    LoopbackIrcServer::sendText(fd, ":alice!u@h PRIVMSG #room :in scope\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session.connect(cancel).connected);
  ASSERT_TRUE(
      session
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  auto result = session.readLive("#room", 1, cancel);
  ASSERT_EQ(result.observation.messages.size(), 1U);
  EXPECT_EQ(result.observation.messages.front().target, "#room");
  EXPECT_EQ(result.observation.messages.front().text, "in scope");
  EXPECT_NE(result.observation.gaps.back().reason.find("bounded prefix"),
            std::string::npos);
}

TEST(EvidenceIrcTransportTest,
     LiveAndHistoryDuplicateMessageIdsRemainOccurrences) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "draft/chathistory message-tags");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd,
                                "@msgid=same :alice!u@h PRIVMSG #room :hi\r\n");
    auto request = LoopbackIrcServer::line(fd);
    EXPECT_TRUE(request.starts_with("CHATHISTORY AFTER #room same"));
    LoopbackIrcServer::sendText(fd, ":fixture BATCH +h chathistory #room\r\n");
    LoopbackIrcServer::sendText(
        fd, "@batch=h;msgid=same :alice!u@h PRIVMSG #room :hi\r\n");
    LoopbackIrcServer::sendText(fd, ":fixture BATCH -h\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session.connect(cancel).connected);
  ASSERT_TRUE(
      session
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  auto live = session.readLive("#room", 1, cancel);
  ASSERT_EQ(live.observation.messages.size(), 1U);
  ASSERT_EQ(live.observation.messages.front().messageId, "same");
  const IrcHistoryRequest request{"#room", "same", 1};
  auto result = session.pendingHistory(request, cancel);
  ASSERT_EQ(result.observation.messages.size(), 1U);
  ASSERT_TRUE(result.readTicket);
  EXPECT_EQ(live.observation.messages.front().messageId,
            result.observation.messages.front().messageId);
  EXPECT_NE(live.observation.messages.front().localSequence,
            result.observation.messages.front().localSequence);
  auto replay = session.pendingHistory(request, cancel);
  EXPECT_EQ(replay.readTicket, result.readTicket);
  ASSERT_EQ(replay.observation.messages.size(), 1U);
  EXPECT_EQ(replay.observation.messages.front().localSequence,
            result.observation.messages.front().localSequence);
  EXPECT_TRUE(session.acknowledgeRead(*result.readTicket));
}

TEST(EvidenceIrcTransportTest, RevokedAuthorityDoesNotWritePrivmsg) {
  LoopbackIrcServer server;
  std::atomic_bool sawPrivmsg{};
  server.start([&](int fd) {
    LoopbackIrcServer::handshake(fd, "echo-message");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    auto command = LoopbackIrcServer::line(fd);
    sawPrivmsg   = command.starts_with("PRIVMSG ");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  const auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected);
  ASSERT_TRUE(
      session
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  auto revoked = std::make_shared<std::atomic_bool>(true);
  auto receipt = session.send(
      authority(config, connected.observation.connectionIncarnation, revoked),
      cancel);
  EXPECT_EQ(receipt.state, IrcSendState::Rejected);
  EXPECT_FALSE(sawPrivmsg.load());
}

TEST(EvidenceIrcTransportTest,
     SendRequiresCurrentConnectionAudienceAndConfirmedMembership) {
  LoopbackIrcServer server;
  std::atomic_bool receivedLine{};
  server.start([&](int fd) {
    LoopbackIrcServer::handshake(fd, "echo-message");
    receivedLine = !LoopbackIrcServer::line(fd).empty();
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  const auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected);
  auto revoked       = std::make_shared<std::atomic_bool>(false);
  auto wrongAudience = authority(
      config, connected.observation.connectionIncarnation, revoked, {"#other"});
  EXPECT_EQ(session.send(wrongAudience, cancel).state, IrcSendState::Rejected);
  EXPECT_EQ(
      session
          .send(authority(config, connected.observation.connectionIncarnation,
                          revoked),
                cancel)
          .state,
      IrcSendState::Rejected);
  EXPECT_EQ(session.send(authority(config, "stale-connection", revoked), cancel)
                .state,
            IrcSendState::Rejected);
  EXPECT_FALSE(receivedLine.load());
}

TEST(EvidenceIrcTransportTest, EchoAcknowledgesWhileNoEchoLeavesWriteUnknown) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "echo-message");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    auto command = LoopbackIrcServer::line(fd);
    ASSERT_TRUE(command.starts_with("PRIVMSG #room :hello"));
    LoopbackIrcServer::sendText(
        fd, "@msgid=server-1 :guest!u@h PRIVMSG #room :hello\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  const auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected);
  ASSERT_TRUE(
      session
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  auto receipt = session.send(
      authority(config, connected.observation.connectionIncarnation,
                std::make_shared<std::atomic_bool>()),
      cancel);
  EXPECT_EQ(receipt.state, IrcSendState::EchoAcknowledged);
  EXPECT_EQ(receipt.echoMessageId, "server-1");

  LoopbackIrcServer noEcho;
  noEcho.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "server-time");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    (void)LoopbackIrcServer::line(fd);
  });
  config = localConfig(noEcho.port());
  IrcSession second(config);
  const auto secondConnected = second.connect(cancel);
  ASSERT_TRUE(secondConnected.connected);
  ASSERT_TRUE(
      second
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  receipt = second.send(
      authority(config, secondConnected.observation.connectionIncarnation,
                std::make_shared<std::atomic_bool>()),
      cancel);
  EXPECT_EQ(receipt.state, IrcSendState::WrittenUnknown);

  LoopbackIrcServer noAcknowledgment;
  noAcknowledgment.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "echo-message");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    (void)LoopbackIrcServer::line(fd);
  });
  config = localConfig(noAcknowledgment.port());
  IrcSession third(config);
  const auto thirdConnected = third.connect(cancel);
  ASSERT_TRUE(thirdConnected.connected);
  ASSERT_TRUE(
      third
          .join(joinAuthority(config, "#room",
                              std::make_shared<std::atomic_bool>(false)),
                cancel)
          .joined);
  receipt = third.send(
      authority(config, thirdConnected.observation.connectionIncarnation,
                std::make_shared<std::atomic_bool>()),
      cancel);
  EXPECT_EQ(receipt.state, IrcSendState::WrittenUnknown);
}

TEST(EvidenceIrcTransportTest,
     MismatchedSelfEchoesAreRetainedAndCannotAcknowledgeSend) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "echo-message");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    const auto command = LoopbackIrcServer::line(fd);
    ASSERT_TRUE(command.starts_with("PRIVMSG #room :hello"));
    LoopbackIrcServer::sendText(
        fd, ":guest!u@h PRIVMSG guest :hello\r\n"
            ":guest!u@h PRIVMSG #room :different payload\r\n"
            "@msgid=matching :guest!u@h PRIVMSG #room :hello\r\n");
  });
  auto config = localConfig(server.port());
  IrcSession session(config);
  std::atomic_bool cancel{};
  const auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected);
  ASSERT_TRUE(session
                  .join(joinAuthority(config, "#room",
                                      std::make_shared<std::atomic_bool>()),
                        cancel)
                  .joined);
  const auto receipt = session.send(
      authority(config, connected.observation.connectionIncarnation,
                std::make_shared<std::atomic_bool>()),
      cancel);
  EXPECT_EQ(receipt.state, IrcSendState::EchoAcknowledged);
  EXPECT_EQ(receipt.echoMessageId, "matching");
  EXPECT_NE(receipt.detail.find("recipient delivery is not verified"),
            std::string::npos);
  const auto retained = session.readLive(2, cancel);
  ASSERT_EQ(retained.observation.messages.size(), 2U);
  EXPECT_EQ(retained.observation.messages[0].target, "guest");
  EXPECT_EQ(retained.observation.messages[0].text, "hello");
  EXPECT_EQ(retained.observation.messages[1].text, "different payload");
}

TEST(EvidenceIrcTransportTest,
     UnmatchedEchoAfterTimeoutIsNotSwallowedByStaleExpectation) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "echo-message");
    EXPECT_EQ(LoopbackIrcServer::line(fd), "JOIN #room\r\n");
    LoopbackIrcServer::sendText(fd, ":guest!u@h JOIN #room\r\n");
    const auto command = LoopbackIrcServer::line(fd);
    ASSERT_TRUE(command.starts_with("PRIVMSG #room :hello"));
    LoopbackIrcServer::sendText(
        fd, ":guest!u@h PRIVMSG #room :different payload\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    LoopbackIrcServer::sendText(
        fd, "@msgid=late :guest!u@h PRIVMSG #room :hello\r\n");
  });
  auto config    = localConfig(server.port());
  config.timeout = std::chrono::milliseconds(400);
  IrcSession session(config);
  std::atomic_bool cancel{};
  const auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected);
  ASSERT_TRUE(session
                  .join(joinAuthority(config, "#room",
                                      std::make_shared<std::atomic_bool>()),
                        cancel)
                  .joined);
  const auto receipt = session.send(
      authority(config, connected.observation.connectionIncarnation,
                std::make_shared<std::atomic_bool>()),
      cancel);
  EXPECT_EQ(receipt.state, IrcSendState::WrittenUnknown);
  const auto later = session.readLive(1, cancel);
  ASSERT_EQ(later.observation.messages.size(), 1U);
  EXPECT_EQ(later.observation.messages.front().text, "hello");
  EXPECT_EQ(later.observation.messages.front().messageId, "late");
}

TEST(EvidenceIrcTransportTest, CancelledReadAndOversizedLineRemainBounded) {
  LoopbackIrcServer server;
  server.start([](int fd) {
    LoopbackIrcServer::handshake(fd, "message-tags");
    LoopbackIrcServer::sendText(fd, "@" + std::string(90, 'a') +
                                        " :x PRIVMSG #r :x\r\n");
  });
  auto config         = localConfig(server.port());
  config.maxLineBytes = 80;
  config.maxTagBytes  = 60;
  IrcSession session(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session.connect(cancel).connected);
  auto result = session.readLive(1, cancel);
  EXPECT_EQ(result.observation.state, CaptureState::Partial);
  EXPECT_TRUE(result.observation.gaps.back().reason.find("ended") !=
              std::string::npos);

  std::atomic_bool cancelDuringWait{};
  auto start = std::chrono::steady_clock::now();
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    cancelDuringWait = true;
  });
  auto cancelled = session.readLive(1, cancelDuringWait);
  canceller.join();
  EXPECT_TRUE(cancelled.connected);
  EXPECT_LT(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds(300));
}

TEST(EvidenceIrcTransportTest, UntrustedTlsPeerNeverReleasesSaslCredential) {
  LoopbackIrcServer server;
  TestCertificate identity;
  server.start([&](int fd) {
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(SSL_CTX_use_certificate(context, identity.certificate), 1);
    ASSERT_EQ(SSL_CTX_use_PrivateKey(context, identity.key), 1);
    SSL *connection = SSL_new(context);
    ASSERT_NE(connection, nullptr);
    ASSERT_EQ(SSL_set_fd(connection, fd), 1);
    TestSigpipeBlock noSigpipe;
    static_cast<void>(SSL_accept(connection));
    SSL_free(connection);
    SSL_CTX_free(context);
  });
  auto config            = localConfig(server.port());
  config.useTls          = true;
  config.serverAuthority = "ircs://127.0.0.1:" + std::to_string(server.port());
  config.accountScope    = "fixture-account";
  std::atomic_int callbackCount{};
  IrcCredentialBroker broker;
  auto lease =
      broker.issue(config.serverAuthority, config.accountScope, "fixture-user",
                   "do-not-leak", std::chrono::seconds(5));
  ASSERT_TRUE(lease);
  auto takeSecret   = lease->takeSecret;
  lease->takeSecret = [&callbackCount, takeSecret]() {
    ++callbackCount;
    return takeSecret();
  };
  config.sasl = std::move(*lease);
  IrcSession session(config);
  std::atomic_bool cancel{};
  auto result = session.connect(cancel);
  EXPECT_FALSE(result.connected);
  EXPECT_EQ(callbackCount.load(), 0);
  EXPECT_EQ(result.detail.find("do-not-leak"), std::string::npos);
  for (const auto &gap : result.observation.gaps)
    EXPECT_EQ(gap.reason.find("do-not-leak"), std::string::npos);
}

TEST(EvidenceIrcTransportTest,
     VerifiedSaslCredentialEchoesAreWithheldFromLiveAndHistory) {
  LoopbackIrcServer server;
  TestCertificate identity("127.0.0.1");
  TemporaryCaFile caFile(identity.certificate);
  constexpr std::string_view username = "fixture-user";
  constexpr std::string_view password = "fixture-password-echo";
  const auto encoded                  = saslPlainToken(username, password);
  std::atomic_bool authAccepted{};
  server.start([&](int fd) {
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(
        SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context ||
        SSL_CTX_use_certificate(context.get(), identity.certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context.get(), identity.key) != 1)
      return;
    std::unique_ptr<SSL, decltype(&SSL_free)> connection(SSL_new(context.get()),
                                                         SSL_free);
    if (!connection || SSL_set_fd(connection.get(), fd) != 1) return;
    TestSigpipeBlock noSigpipe;
    if (SSL_accept(connection.get()) != 1) return;
    if (tlsLine(connection.get()).empty()) return;
    tlsSend(connection.get(),
            ":fixture CAP * LS :draft/chathistory sasl=PLAIN\r\n");
    auto command = tlsLine(connection.get());
    if (!command.starts_with("CAP REQ :draft/chathistory")) return;
    tlsSend(connection.get(), ":fixture CAP * ACK :draft/chathistory\r\n");
    command = tlsLine(connection.get());
    if (!command.starts_with("CAP REQ :sasl")) return;
    tlsSend(connection.get(), ":fixture CAP * ACK :sasl\r\n");
    if (tlsLine(connection.get()) != "AUTHENTICATE PLAIN\r\n") return;
    tlsSend(connection.get(), "AUTHENTICATE +\r\n");
    authAccepted =
        tlsLine(connection.get()) == "AUTHENTICATE " + encoded + "\r\n";
    if (!authAccepted.load()) return;
    tlsSend(connection.get(),
            ":fixture 903 guest :SASL authentication successful\r\n");
    if (tlsLine(connection.get()) != "CAP END\r\n" ||
        tlsLine(connection.get()) != "NICK guest\r\n" ||
        tlsLine(connection.get()) != "USER guest 0 * :guest\r\n")
      return;
    tlsSend(connection.get(), ":fixture 001 guest :welcome\r\n");
    tlsSend(connection.get(),
            ":fixture!u@host PRIVMSG guest :" + std::string(password) + "\r\n");
    tlsSend(connection.get(),
            "@echo=" + encoded + " :fixture!u@host NOTICE guest :tag echo\r\n");
    tlsSend(connection.get(),
            ":fixture!u@host PRIVMSG guest :safe message\r\n");
    if (!tlsLine(connection.get()).starts_with("JOIN #room")) return;
    tlsSend(connection.get(), ":guest!u@host JOIN :#room\r\n");
    if (!tlsLine(connection.get()).starts_with("CHATHISTORY AFTER #room"))
      return;
    tlsSend(connection.get(), ":fixture BATCH +history chathistory #room\r\n");
    tlsSend(connection.get(), "@batch=history :fixture PRIVMSG #room :" +
                                  std::string(password) + "\r\n");
    tlsSend(connection.get(),
            "@batch=history :fixture PRIVMSG #room :" + encoded + "\r\n");
    tlsSend(connection.get(), ":fixture BATCH -history\r\n");
  });

  auto config            = localConfig(server.port());
  config.useTls          = true;
  config.serverAuthority = "ircs://127.0.0.1:" + std::to_string(server.port());
  config.accountScope    = "fixture-account";
  config.caBundlePath    = caFile.path();
  config.requestedCapabilities = {"draft/chathistory"};
  IrcCredentialBroker broker;
  config.sasl = broker.issue(config.serverAuthority, config.accountScope,
                             std::string(username), std::string(password),
                             std::chrono::seconds(5));
  ASSERT_TRUE(config.sasl);
  auto roomAuthority =
      joinAuthority(config, "#room", std::make_shared<std::atomic_bool>());
  IrcSession session(std::move(config));
  std::atomic_bool cancel{};
  auto connected = session.connect(cancel);
  ASSERT_TRUE(connected.connected) << connected.detail;
  ASSERT_TRUE(authAccepted.load());

  auto live = session.readLive(1, cancel);
  ASSERT_EQ(live.observation.messages.size(), 1U);
  EXPECT_EQ(live.observation.messages.front().text, "safe message");
  std::size_t liveCredentialEchoes{};
  for (const auto &gap : live.observation.gaps) {
    SCOPED_TRACE(gap.reason);
    EXPECT_EQ(gap.reason.find(password), std::string::npos);
    EXPECT_EQ(gap.reason.find(encoded), std::string::npos);
    if (gap.reason == "IRC message withheld because it echoed a credential")
      ++liveCredentialEchoes;
  }
  EXPECT_EQ(liveCredentialEchoes, 2U);

  auto joined = session.join(roomAuthority, cancel);
  ASSERT_TRUE(joined.joined) << joined.detail;
  auto history = session.history({"#room", "cursor-1", 2}, cancel);
  EXPECT_TRUE(history.observation.messages.empty());
  std::size_t historyCredentialEchoes{};
  for (const auto &gap : history.observation.gaps) {
    SCOPED_TRACE(gap.reason);
    EXPECT_EQ(gap.reason.find(password), std::string::npos);
    EXPECT_EQ(gap.reason.find(encoded), std::string::npos);
    if (gap.reason == "IRC message withheld because it echoed a credential")
      ++historyCredentialEchoes;
  }
  EXPECT_EQ(historyCredentialEchoes, 2U);
}

TEST(EvidenceIrcTransportTest, RefusesInvalidBoundsAndAuthority) {
  auto config         = localConfig(1);
  config.maxLineBytes = 64;
  EXPECT_THROW(IrcSession(std::move(config)), std::invalid_argument);
  IrcSendAuthority invalid{"wrong",
                           "wrong",
                           "wrong-connection",
                           "#room",
                           "hello",
                           {},
                           std::chrono::steady_clock::now() +
                               std::chrono::seconds(5),
                           std::make_shared<std::atomic_bool>()};
  EXPECT_EQ(invalid.target, "#room");
}

TEST(EvidenceIrcTransportTest, IrcCredentialBrokerScopesAndConsumesOnce) {
  IrcCredentialBroker broker;
  const std::string accountScope = "fixture account ☃";
  auto lease = broker.issue("ircs://irc.example:6697", accountScope, "user-a",
                            "secret-a", std::chrono::seconds(5));
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->serverAuthority, "ircs://irc.example:6697");
  EXPECT_EQ(lease->accountScope, accountScope);
  EXPECT_EQ(lease->saslUsername, "user-a");

  auto config            = localConfig(6697);
  config.host            = "irc.example";
  config.serverAuthority = "ircs://irc.example:6697";
  config.useTls          = true;
  config.accountScope    = accountScope;
  config.requestedCapabilities.push_back("sasl");
  config.sasl = *lease;
  EXPECT_EQ(ircServerAuthority(config), config.serverAuthority);
  EXPECT_NO_THROW(static_cast<void>(IrcSession(config)));
  config.accountScope = "fixture\naccount";
  EXPECT_FALSE(ircServerAuthority(config));
  EXPECT_THROW(IrcSession(std::move(config)), std::invalid_argument);

  ASSERT_TRUE(lease->takeSecret);
  auto secret = lease->takeSecret();
  ASSERT_TRUE(secret);
  EXPECT_EQ(*secret, "secret-a");
  OPENSSL_cleanse(secret->data(), secret->size());
  EXPECT_FALSE(lease->takeSecret());

  auto revoked = broker.issue("ircs://irc.example:6697", "account-a", "user-a",
                              "secret-b", std::chrono::seconds(5));
  ASSERT_TRUE(revoked);
  broker.revoke(*revoked);
  EXPECT_TRUE(revoked->revoked->load());
  EXPECT_FALSE(revoked->takeSecret());

  EXPECT_FALSE(broker.issue("ircs://irc.example:6697", "account\rscope",
                            "user-a", "secret-c", std::chrono::seconds(5)));
}

TEST(EvidenceIrcTransportTest,
     IrcCredentialLeaseMismatchExpiryAndCleartextNeverReleaseSecret) {
  IrcCredentialBroker broker;
  std::atomic_int callbackCount{};
  auto observeCallback = [&](IrcCredentialLease lease) {
    auto takeSecret  = lease.takeSecret;
    lease.takeSecret = [&callbackCount, takeSecret]() {
      ++callbackCount;
      return takeSecret();
    };
    return lease;
  };

  auto mismatched = broker.issue("ircs://127.0.0.1:6668", "account-a", "user-a",
                                 "secret-a", std::chrono::seconds(5));
  ASSERT_TRUE(mismatched);
  auto mismatchConfig            = localConfig(6667);
  mismatchConfig.useTls          = true;
  mismatchConfig.serverAuthority = "ircs://127.0.0.1:6667";
  mismatchConfig.accountScope    = "account-a";
  mismatchConfig.sasl            = observeCallback(std::move(*mismatched));
  EXPECT_THROW(IrcSession(std::move(mismatchConfig)), std::invalid_argument);

  auto expired = broker.issue("ircs://127.0.0.1:6667", "account-a", "user-a",
                              "secret-b", std::chrono::milliseconds(1));
  ASSERT_TRUE(expired);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto expiredConfig            = localConfig(6667);
  expiredConfig.useTls          = true;
  expiredConfig.serverAuthority = "ircs://127.0.0.1:6667";
  expiredConfig.accountScope    = "account-a";
  expiredConfig.sasl            = observeCallback(std::move(*expired));
  EXPECT_THROW(IrcSession(std::move(expiredConfig)), std::invalid_argument);

  auto cleartext = broker.issue("irc://127.0.0.1:6667", "account-a", "user-a",
                                "secret-c", std::chrono::seconds(5));
  ASSERT_TRUE(cleartext);
  auto cleartextConfig         = localConfig(6667);
  cleartextConfig.accountScope = "account-a";
  cleartextConfig.sasl         = observeCallback(std::move(*cleartext));
  EXPECT_THROW(IrcSession(std::move(cleartextConfig)), std::invalid_argument);
  EXPECT_EQ(callbackCount.load(), 0);
}
} // namespace xanadu::evidence
