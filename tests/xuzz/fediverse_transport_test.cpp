#include "common/xanadu/evidence/fediverse_transport.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace xanadu::evidence;
using namespace std::chrono_literals;

struct HttpReply {
  int status{200};
  std::string body;
  std::string headers;
  bool disconnect{};
  std::chrono::milliseconds delay{};
};

class FediverseServer {
public:
  explicit FediverseServer(std::vector<HttpReply> replies)
      : replies_(std::move(replies)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("test socket failed");
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
        ::listen(fd_, 8))
      throw std::runtime_error("test listen failed");
    socklen_t length = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &length))
      throw std::runtime_error("test address failed");
    port_   = ntohs(addr.sin_port);
    worker_ = std::thread([this] { serve(); });
  }
  ~FediverseServer() {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    if (worker_.joinable()) worker_.join();
  }
  std::string origin() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }
  std::vector<std::string> requests() const {
    const std::scoped_lock lock(mutex_);
    return requests_;
  }

private:
  void serve() {
    for (const auto &reply : replies_) {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) return;
      timeval timeout{2, 0};
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      std::string bytes;
      char buffer[4096];
      std::size_t expected{};
      for (;;) {
        const auto n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        bytes.append(buffer, static_cast<std::size_t>(n));
        const auto end = bytes.find("\r\n\r\n");
        if (end == bytes.npos) continue;
        const auto length = bytes.find("Content-Length:");
        if (length != bytes.npos)
          expected = std::stoull(bytes.substr(length + 15));
        if (bytes.size() >= end + 4 + expected) break;
      }
      {
        const std::scoped_lock lock(mutex_);
        requests_.push_back(bytes);
      }
      std::this_thread::sleep_for(reply.delay);
      if (!reply.disconnect) {
        auto headers = reply.headers;
        for (auto pos = headers.find("$ORIGIN"); pos != headers.npos;
             pos      = headers.find("$ORIGIN"))
          headers.replace(pos, 7, origin());
        auto wire = "HTTP/1.1 " + std::to_string(reply.status) + " Test\r\n" +
                    "Connection: close\r\nContent-Type: application/json\r\n" +
                    "Content-Length: " + std::to_string(reply.body.size()) +
                    "\r\n" + headers + "\r\n" + reply.body;
        for (std::size_t sent = 0; sent < wire.size();) {
          const auto n = ::send(client, wire.data() + sent, wire.size() - sent,
                                MSG_NOSIGNAL);
          if (n <= 0) break;
          sent += static_cast<std::size_t>(n);
        }
      }
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }
  int fd_{};
  std::uint16_t port_{};
  std::vector<HttpReply> replies_;
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread worker_;
};

constexpr std::string_view actor = "https://social.example/users/alice";
constexpr std::string_view publicAudience =
    "https://www.w3.org/ns/activitystreams#Public";
const std::string status =
    R"({"id":"s1","uri":"https://social.example/objects/1","content":"retained body","visibility":"public","created_at":"2026-10-22T12:00:00Z","account":{"id":"a1","url":"https://social.example/@alice","acct":"alice","display_name":"Alice"},"mentions":[{"url":"https://social.example/@bob"}]})";

FediverseReadRequest readRequest(const FediverseServer &server) {
  return {.instanceOrigin  = server.origin(),
          .accountId       = "a1",
          .accountActorUri = std::string(actor)};
}
FediversePostRequest postRequest(const FediverseServer &server) {
  FediversePostRequest r;
  r.instanceOrigin  = server.origin();
  r.accountId       = "a1";
  r.accountActorUri = actor;
  r.text            = "literal + & = body";
  r.audience        = {std::string(publicAudience)};
  r.idempotencyKey  = "review-1";
  return r;
}
template <class Request>
std::shared_ptr<FediverseAuthority> grant(const Request &r,
                                          FediverseAction action) {
  std::vector<std::string> audience;
  if constexpr (std::is_same_v<Request, FediversePostRequest>)
    audience = r.audience;
  return std::make_shared<FediverseAuthority>(
      r.instanceOrigin, r.accountId, action, fediverseRequestFingerprint(r),
      audience, std::chrono::steady_clock::now() + 10s);
}
std::shared_ptr<FediverseCredentialLease>
credential(const FediverseServer &server) {
  return std::make_shared<FediverseCredentialLease>(
      server.origin(), "a1", "synthetic-token",
      std::chrono::steady_clock::now() + 10s);
}

TEST(FediverseTransportTest,
     ReadsNativeHttpWithoutInventingActorOrAudienceIdentity) {
  FediverseServer server({{.body = "[" + status + "]"}});
  auto request            = readRequest(server);
  request.limits.maxPages = 1;
  std::atomic_bool cancel{};
  const auto result = FediverseTransport({true}).read(request, {}, {}, cancel);
  ASSERT_EQ(result.observation.state, CaptureState::Complete) << result.error;
  ASSERT_EQ(result.observation.objects.size(), 1U);
  EXPECT_EQ(result.observation.objects[0].actorUri, actor);
  ASSERT_EQ(result.observation.actors.size(), 1U);
  EXPECT_EQ(result.observation.actors[0].identityBasis,
            "configured-account-correspondence");
  EXPECT_EQ(result.observation.actors[0].profileUrl,
            "https://social.example/@alice");
  EXPECT_TRUE(result.observation.objects[0].audience.empty());
  ASSERT_EQ(result.pages.size(), 1U);
  EXPECT_EQ(result.pages[0].body, "[" + status + "]");
  ASSERT_EQ(server.requests().size(), 1U);
  EXPECT_EQ(server.requests()[0].find("Authorization:"), std::string::npos);
}

TEST(FediverseTransportTest, RetainsPrefixAndGapWhenLaterPageFails) {
  FediverseServer server(
      {{.body    = "[" + status + "]",
        .headers = "Link: <$ORIGIN/api/v1/accounts/a1/statuses?max_id=1>; "
                   "rel=\"next\"\r\n"},
       {.status = 503, .body = "unavailable"}});
  std::atomic_bool cancel{};
  const auto result =
      FediverseTransport({true}).read(readRequest(server), {}, {}, cancel);
  EXPECT_EQ(result.observation.state, CaptureState::Partial);
  EXPECT_EQ(result.observation.objects.size(), 1U);
  EXPECT_EQ(result.pages.size(), 2U);
  EXPECT_FALSE(result.gaps.empty());
}

TEST(FediverseTransportTest,
     RefusesCrossOriginPaginationAndRedirectWithoutFollowing) {
  for (bool redirect : {false, true}) {
    FediverseServer server(
        {{.status  = redirect ? 302 : 200,
          .body    = redirect ? "" : "[" + status + "]",
          .headers = redirect ? "Location: https://elsewhere.example/\r\n"
                              : "Link: "
                                "<https://elsewhere.example/api/v1/accounts/a1/"
                                "statuses>; rel=\"next\"\r\n"}});
    std::atomic_bool cancel{};
    const auto result =
        FediverseTransport({true}).read(readRequest(server), {}, {}, cancel);
    EXPECT_NE(result.observation.state, CaptureState::Complete);
    EXPECT_FALSE(result.gaps.empty());
    EXPECT_EQ(server.requests().size(), 1U);
  }
}

TEST(FediverseTransportTest,
     PaginationDoesNotRetainOrForwardEchoedCredentials) {
  for (const std::string &token :
       {std::string("synthetic-token"), std::string("synthetic-%74oken")}) {
    FediverseServer server(
        {{.body    = "[]",
          .headers = "Link: <$ORIGIN/api/v1/accounts/a1/statuses?cursor=" +
                     token + ">; rel=\"next\"\r\n"}});
    auto request          = readRequest(server);
    request.authenticated = true;
    std::atomic_bool cancel{};
    const auto result = FediverseTransport({true}).read(
        request, credential(server),
        grant(request, FediverseAction::ReadStatuses), cancel);
    EXPECT_NE(result.observation.state, CaptureState::Complete);
    ASSERT_EQ(result.gaps.size(), 1U);
    EXPECT_EQ(result.gaps[0].locator.find("synthetic"), std::string::npos);
    EXPECT_EQ(result.error.find("synthetic"), std::string::npos);
    EXPECT_EQ(server.requests().size(), 1U);
  }
}

TEST(FediverseTransportTest, BoundsResponseBytesHeadersAndDeadline) {
  for (int mode = 0; mode < 3; ++mode) {
    FediverseServer server(
        {{.body = mode == 0 ? std::string(1024, 'x') : "[]",
          .headers =
              mode == 1 ? "X-Long: " + std::string(1024, 'x') + "\r\n" : "",
          .delay = mode == 2 ? 100ms : 0ms}});
    auto request = readRequest(server);
    if (mode == 0) request.limits.records.maxBytes = 16;
    if (mode == 1) request.limits.maxHeaderBytes = 64;
    if (mode == 2) request.limits.timeoutMs = 10;
    std::atomic_bool cancel{};
    auto result = FediverseTransport({true}).read(request, {}, {}, cancel);
    EXPECT_NE(result.observation.state, CaptureState::Complete);
    EXPECT_FALSE(result.error.empty());
  }
}

TEST(FediverseTransportTest, CancelAndRevocationRefuseBeforeAnyRequest) {
  FediverseServer server({});
  auto request          = readRequest(server);
  request.authenticated = true;
  auto lease            = credential(server);
  auto authority        = grant(request, FediverseAction::ReadStatuses);
  lease->revoke();
  std::atomic_bool cancel{};
  auto result =
      FediverseTransport({true}).read(request, lease, authority, cancel);
  EXPECT_EQ(result.observation.state, CaptureState::Failed);
  cancel                = true;
  request.authenticated = false;
  result = FediverseTransport({true}).read(request, {}, {}, cancel);
  EXPECT_EQ(result.observation.state, CaptureState::Cancelled);
  EXPECT_TRUE(server.requests().empty());
}

TEST(FediverseTransportTest, PostBindsPayloadAndReportsLocalAcceptanceOnly) {
  FediverseServer server({{.body = status}});
  auto request         = postRequest(server);
  const auto authority = grant(request, FediverseAction::PublishStatus);
  std::atomic_bool cancel{};
  const auto result = FediverseTransport({true}).post(
      request, credential(server), authority, cancel);
  ASSERT_TRUE(result.accepted) << result.error;
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_EQ(result.objectUri, "https://social.example/objects/1");
  EXPECT_EQ(result.actorUri, actor);
  EXPECT_EQ(result.actorIdentityBasis, "configured-account-correspondence");
  EXPECT_FALSE(result.activityUri);
  EXPECT_FALSE(result.federationDeliveryConfirmed);
  ASSERT_EQ(server.requests().size(), 1U);
  EXPECT_NE(server.requests()[0].find("Idempotency-Key: review-1"),
            std::string::npos);
  EXPECT_NE(
      server.requests()[0].find("status=literal%20%2B%20%26%20%3D%20body"),
      std::string::npos);
  EXPECT_EQ(result.error.find("synthetic-token"), std::string::npos);
}

TEST(FediverseTransportTest,
     ChangedOrRestrictedAudienceCannotUsePriorAuthority) {
  FediverseServer server({});
  auto request         = postRequest(server);
  const auto authority = grant(request, FediverseAction::PublishStatus);
  std::atomic_bool cancel{};
  request.text = "edited after review";
  EXPECT_FALSE(FediverseTransport({true})
                   .post(request, credential(server), authority, cancel)
                   .mayHaveExecuted);
  request            = postRequest(server);
  request.visibility = "direct";
  request.audience   = {"https://social.example/users/bob"};
  EXPECT_FALSE(FediverseTransport({true})
                   .post(request, credential(server),
                         grant(request, FediverseAction::PublishStatus), cancel)
                   .mayHaveExecuted);
  EXPECT_TRUE(server.requests().empty());
}

TEST(FediverseTransportTest,
     DisconnectAfterPostRetainsUncertaintyWithoutResending) {
  FediverseServer server({{.disconnect = true}});
  auto request = postRequest(server);
  std::atomic_bool cancel{};
  const auto result = FediverseTransport({true}).post(
      request, credential(server),
      grant(request, FediverseAction::PublishStatus), cancel);
  EXPECT_FALSE(result.accepted);
  EXPECT_TRUE(result.mayHaveExecuted);
  EXPECT_NE(result.error.find("unknown"), std::string::npos);
  EXPECT_EQ(server.requests().size(), 1U);
}

TEST(FediverseTransportTest,
     WithholdsCredentialEchoBeforeReturningOwnedEvidence) {
  for (const std::string &echo : {std::string("synthetic-token"),
                                  std::string(R"(synthetic-\u0074oken)")}) {
    const auto body = "{\"credential\":\"" + echo + "\"}";
    FediverseServer server({{.body = body}, {.body = "[" + body + "]"}});
    std::atomic_bool cancel{};
    auto post         = postRequest(server);
    const auto posted = FediverseTransport({true}).post(
        post, credential(server), grant(post, FediverseAction::PublishStatus),
        cancel);
    EXPECT_TRUE(posted.accepted);
    EXPECT_TRUE(posted.responseRedacted);
    EXPECT_EQ(posted.responseBody.find("synthetic"), std::string::npos);
    EXPECT_FALSE(posted.objectUri);
    EXPECT_FALSE(posted.actorUri);
    auto read           = readRequest(server);
    read.authenticated  = true;
    const auto observed = FediverseTransport({true}).read(
        read, credential(server), grant(read, FediverseAction::ReadStatuses),
        cancel);
    ASSERT_EQ(observed.pages.size(), 1U);
    EXPECT_TRUE(observed.pages[0].redacted);
    EXPECT_EQ(observed.pages[0].body.find("synthetic"), std::string::npos);
    EXPECT_TRUE(observed.observation.objects.empty());
    EXPECT_NE(observed.observation.state, CaptureState::Complete);
  }
}

TEST(FediverseTransportTest,
     ProductionRefusesCleartextAndCredentialHeaderInjection) {
  FediverseServer server({});
  auto request = readRequest(server);
  std::atomic_bool cancel{};
  EXPECT_EQ(
      FediverseTransport().read(request, {}, {}, cancel).observation.state,
      CaptureState::Failed);
  EXPECT_THROW(FediverseCredentialLease(server.origin(), "a1",
                                        "token\r\nX-Injected: yes",
                                        std::chrono::steady_clock::now() + 1s),
               std::invalid_argument);
  EXPECT_TRUE(server.requests().empty());
}
} // namespace
