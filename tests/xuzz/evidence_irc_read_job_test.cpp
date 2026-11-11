#include "common/xanadu/evidence/irc_read_job.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace xanadu::evidence {
namespace {
class IrcFixture {
public:
  IrcFixture() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("fixture socket failed");
    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0)
      throw std::runtime_error("fixture listener failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("fixture port lookup failed");
    port_ = ntohs(address.sin_port);
  }
  ~IrcFixture() {
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
    std::string value;
    while (value.size() < 8192) {
      char byte{};
      if (::recv(fd, &byte, 1, 0) != 1) return {};
      value += byte;
      if (value.ends_with("\r\n")) return value;
    }
    return {};
  }
  static void sendText(int fd, std::string_view value) {
    std::size_t sent{};
    while (sent < value.size()) {
      const auto count =
          ::send(fd, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
      if (count <= 0) return;
      sent += static_cast<std::size_t>(count);
    }
  }
  static bool
  handshake(int fd,
            std::string_view capabilities = "server-time message-tags batch") {
    if (line(fd).empty()) return false;
    sendText(fd, ":fixture CAP * LS :" + std::string(capabilities) + "\r\n");
    auto command = line(fd);
    while (command.starts_with("CAP REQ :")) {
      auto capability = command.substr(9);
      if (capability.ends_with("\r\n"))
        capability.resize(capability.size() - 2);
      sendText(fd, ":fixture CAP * ACK :" + capability + "\r\n");
      command = line(fd);
    }
    if (command.empty() || line(fd).empty() || line(fd).empty()) return false;
    sendText(fd, ":fixture 001 guest :welcome\r\n");
    return true;
  }

private:
  int listener_{-1};
  int client_{-1};
  std::uint16_t port_{};
  std::thread thread_;
};

IrcConnectionConfig configFor(std::uint16_t port) {
  IrcConnectionConfig config;
  config.host               = "127.0.0.1";
  config.port               = port;
  config.serverAuthority    = "irc://127.0.0.1:" + std::to_string(port);
  config.networkIncarnation = "job-fixture";
  config.nick               = "guest";
  config.useTls             = false;
  config.timeout            = std::chrono::milliseconds(500);
  return config;
}
} // namespace

TEST(EvidenceIrcReadJobTest, PreflightCanonicalizesOnlyTheConfiguredEndpoint) {
  IrcConnectionConfig config;
  config.host = "EXAMPLE.invalid";
  config.port = 6697;
  config.nick = "reader";
  EXPECT_EQ(ircServerAuthority(config), "ircs://example.invalid:6697");
  config.serverAuthority = "ircs://alias.invalid:6697";
  EXPECT_TRUE(ircConnectionProblem(config));
  EXPECT_FALSE(ircServerAuthority(config));
}

TEST(EvidenceIrcReadJobTest, CompletionCarriesIdentityButNotSessionCapability) {
  IrcFixture server;
  server.start([](int fd) { static_cast<void>(IrcFixture::handshake(fd)); });
  auto session        = std::make_shared<IrcSession>(configFor(server.port()));
  const auto identity = session->identity();
  IrcReadJob job;
  ASSERT_TRUE(job.start(session, IrcConnectOperation{}));
  EXPECT_FALSE(job.start(session, IrcConnectOperation{}));
  std::optional<IrcReadCompletion> completion;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!completion && std::chrono::steady_clock::now() < deadline) {
    completion = job.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(completion);
  EXPECT_EQ(completion->identity.connectionIncarnation,
            identity.connectionIncarnation);
  EXPECT_EQ(completion->observation.serverAuthority, identity.serverAuthority);
  EXPECT_FALSE(completion->cancelled);
  EXPECT_TRUE(completion->connected);
  EXPECT_FALSE(job.busy());
  EXPECT_TRUE(job.start(session, IrcConnectOperation{}));
  job.cancel();
  auto repeated = job.poll();
  if (!repeated) {
    const auto cancelDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!repeated && std::chrono::steady_clock::now() < cancelDeadline) {
      repeated = job.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->observation.state, CaptureState::Cancelled);
  EXPECT_EQ(session->identity().connectionIncarnation,
            identity.connectionIncarnation);
}

TEST(EvidenceIrcReadJobTest, TargetCursorsPreserveOtherChannelsAndDoNotRepeat) {
  std::atomic_bool releaseMessages{};
  IrcFixture server;
  server.start([&](int fd) {
    if (!IrcFixture::handshake(fd)) return;
    if (IrcFixture::line(fd).empty()) return;
    IrcFixture::sendText(fd, ":guest!u@h JOIN :#one\r\n");
    if (IrcFixture::line(fd).empty()) return;
    IrcFixture::sendText(fd, ":guest!u@h JOIN :#two\r\n");
    const auto releaseDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!releaseMessages.load() &&
           std::chrono::steady_clock::now() < releaseDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    IrcFixture::sendText(fd, ":bob!u@h PRIVMSG #two :two-first\r\n"
                             ":alice!u@h PRIVMSG #one :one-first\r\n"
                             ":bob!u@h PRIVMSG #two :two-second\r\n"
                             ":alice!u@h PRIVMSG #one :one-second\r\n");
  });
  auto config  = configFor(server.port());
  auto session = std::make_shared<IrcSession>(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session->connect(cancel).connected);
  auto revoked = std::make_shared<std::atomic_bool>(false);
  IrcJoinAuthority one{config.serverAuthority,
                       config.networkIncarnation,
                       "#one",
                       {"#one"},
                       std::chrono::steady_clock::now() +
                           std::chrono::seconds(5),
                       revoked};
  IrcJoinAuthority two{config.serverAuthority,
                       config.networkIncarnation,
                       "#two",
                       {"#two"},
                       std::chrono::steady_clock::now() +
                           std::chrono::seconds(5),
                       revoked};
  ASSERT_TRUE(session->join(one, cancel).joined);
  ASSERT_TRUE(session->join(two, cancel).joined);
  releaseMessages = true;
  IrcReadJob job;
  ASSERT_TRUE(job.start(session, IrcLiveReadRequest{"#one", 1}));
  const auto completionDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (job.busy() && std::chrono::steady_clock::now() < completionDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  job.cancel();
  std::optional<IrcReadCompletion> cancelled;
  const auto cancelDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!cancelled && std::chrono::steady_clock::now() < cancelDeadline) {
    cancelled = job.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(cancelled);
  EXPECT_TRUE(cancelled->cancelled);
  ASSERT_TRUE(cancelled->readTicket);
  ASSERT_EQ(cancelled->observation.messages.size(), 1U);
  EXPECT_EQ(cancelled->observation.messages[0].text, "one-first");
  auto replay = session->pendingLive("#one", 1, cancel);
  EXPECT_EQ(replay.readTicket, cancelled->readTicket);
  ASSERT_EQ(replay.observation.messages.size(), 1U);
  EXPECT_EQ(replay.observation.messages[0].text, "one-first");
  ASSERT_TRUE(session->acknowledgeRead(*cancelled->readTicket));
  auto second = session->pendingLive("#one", 1, cancel);
  ASSERT_EQ(second.observation.messages.size(), 1U);
  EXPECT_EQ(second.observation.messages[0].text, "one-second");
  ASSERT_TRUE(second.readTicket);
  ASSERT_TRUE(session->acknowledgeRead(*second.readTicket));
  auto first = session->pendingLive("#two", 1, cancel);
  ASSERT_EQ(first.observation.messages.size(), 1U);
  EXPECT_EQ(first.observation.messages[0].text, "two-first");
  ASSERT_TRUE(first.readTicket);
  ASSERT_TRUE(session->acknowledgeRead(*first.readTicket));
  auto other = session->pendingLive("#two", 1, cancel);
  ASSERT_EQ(other.observation.messages.size(), 1U);
  EXPECT_EQ(other.observation.messages[0].text, "two-second");
  ASSERT_TRUE(other.readTicket);
  ASSERT_TRUE(session->acknowledgeRead(*other.readTicket));
}

TEST(EvidenceIrcReadJobTest, AcknowledgeDoesNotWaitForAnInFlightSend) {
  std::atomic_bool sendReached{};
  std::atomic_bool releaseSend{};
  IrcFixture server;
  server.start([&](int fd) {
    if (!IrcFixture::handshake(fd, "echo-message")) return;
    if (IrcFixture::line(fd).empty()) return;
    IrcFixture::sendText(fd, ":guest!u@h JOIN :#room\r\n");
    IrcFixture::sendText(fd, ":alice!u@h PRIVMSG #room :retained\r\n");
    const auto command = IrcFixture::line(fd);
    sendReached        = command.starts_with("PRIVMSG #room :");
    const auto releaseDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!releaseSend.load() &&
           std::chrono::steady_clock::now() < releaseDeadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  });
  auto config                  = configFor(server.port());
  config.requestedCapabilities = {"echo-message"};
  auto session                 = std::make_shared<IrcSession>(config);
  std::atomic_bool cancel{};
  ASSERT_TRUE(session->connect(cancel).connected);
  auto revoked = std::make_shared<std::atomic_bool>(false);
  IrcJoinAuthority join{config.serverAuthority,
                        config.networkIncarnation,
                        "#room",
                        {"#room"},
                        std::chrono::steady_clock::now() +
                            std::chrono::seconds(5),
                        revoked};
  ASSERT_TRUE(session->join(join, cancel).joined);
  auto pending = session->pendingLive("#room", 1, cancel);
  ASSERT_TRUE(pending.readTicket);
  ASSERT_EQ(pending.observation.messages.size(), 1U);
  IrcSendReceipt receipt;
  const auto identity = session->identity();
  IrcSendAuthority send{config.serverAuthority,
                        config.networkIncarnation,
                        identity.connectionIncarnation,
                        "#room",
                        "hello",
                        {"#room"},
                        std::chrono::steady_clock::now() +
                            std::chrono::seconds(5),
                        revoked};
  std::jthread sender([&] { receipt = session->send(send, cancel); });
  const auto sendDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!sendReached.load() && std::chrono::steady_clock::now() < sendDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  ASSERT_TRUE(sendReached.load());
  const auto ackStart = std::chrono::steady_clock::now();
  EXPECT_TRUE(session->acknowledgeRead(*pending.readTicket));
  EXPECT_LT(std::chrono::steady_clock::now() - ackStart,
            std::chrono::milliseconds(100));
  releaseSend = true;
  sender.join();
  EXPECT_EQ(receipt.state, IrcSendState::WrittenUnknown);
}

} // namespace xanadu::evidence
