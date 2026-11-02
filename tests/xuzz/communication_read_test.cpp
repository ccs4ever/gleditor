#include "common/xanadu/evidence/communication_read.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {
using namespace std::chrono_literals;

class ReadServer {
public:
  explicit ReadServer(std::vector<std::string> replies,
                      std::chrono::milliseconds delay = 0ms)
      : replies_(std::move(replies)), delay_(delay) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("test socket failed");
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 4) != 0)
      throw std::runtime_error("test listener failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("test address failed");
    port_   = ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
  }
  ~ReadServer() {
    static_cast<void>(::shutdown(listener_, SHUT_RDWR));
    static_cast<void>(::close(listener_));
    if (worker_.joinable()) worker_.join();
  }
  ReadServer(const ReadServer &)            = delete;
  ReadServer &operator=(const ReadServer &) = delete;
  [[nodiscard]] std::string origin() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }
  void waitForRequest() {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, 3s, [&] { return requestSeen_; }))
      throw std::runtime_error("read request was not received");
  }

private:
  void serve() {
    for (const auto &reply : replies_) {
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) return;
      char buffer[1024];
      static_cast<void>(::recv(client, buffer, sizeof(buffer), 0));
      {
        const std::scoped_lock lock(mutex_);
        requestSeen_ = true;
      }
      condition_.notify_all();
      std::this_thread::sleep_for(delay_);
      const auto response = reply;
      std::size_t sent{};
      while (sent < response.size()) {
        const auto count = ::send(client, response.data() + sent,
                                  response.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
      }
      static_cast<void>(::shutdown(client, SHUT_RDWR));
      static_cast<void>(::close(client));
    }
  }
  int listener_{-1};
  std::uint16_t port_{};
  std::vector<std::string> replies_;
  std::chrono::milliseconds delay_{};
  std::mutex mutex_;
  std::condition_variable condition_;
  bool requestSeen_{};
  std::thread worker_;
};

std::string response(int status, std::string body, std::string headers = {}) {
  return "HTTP/1.1 " + std::to_string(status) + " Test\r\n" +
         "Connection: close\r\nContent-Type: application/json\r\n" +
         "Content-Length: " + std::to_string(body.size()) + "\r\n" + headers +
         "\r\n" + body;
}

FediverseReadRequest request(const ReadServer &server) {
  FediverseReadRequest result;
  result.instanceOrigin  = server.origin();
  result.accountId       = "a1";
  result.accountActorUri = "https://social.example/@alice";
  result.accountHandle   = "alice";
  result.limits.maxPages = 2;
  return result;
}

std::string quoteArgument(std::string_view text) {
  std::ostringstream out;
  out << std::quoted(std::string(text));
  return out.str();
}

bool waitForControllerRead(EvidenceController &controller) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (controller.poll()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

RecordId findRecord(const EvidenceController &controller,
                    std::string_view kind) {
  for (const auto &record : controller.snapshot().records)
    if (record.kind == kind) return record.id;
  throw std::runtime_error("expected controller record not found");
}

TEST(CommunicationReadJobTest, RunsNativeReadAsynchronouslyAndCanCancel) {
  ReadServer server({response(200, "[]")}, 150ms);
  CommunicationReadJob job({true});
  ASSERT_TRUE(job.start(request(server)));
  EXPECT_TRUE(job.busy());
  EXPECT_FALSE(job.start(request(server)));
  server.waitForRequest();
  job.cancel();
  std::optional<FediverseTransportResult> result;
  for (int i = 0; i < 1000 && !result; ++i) {
    result = job.poll();
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(result);
  EXPECT_EQ(result->observation.state, CaptureState::Cancelled);
  EXPECT_FALSE(job.busy());
}

TEST(CommunicationReadJobTest, ReturnsPrefixPageAndPaginationGap) {
  const auto first = response(200, "[]",
                              "Link: </api/v1/accounts/a1/statuses?max_id=1>; "
                              "rel=\"next\"\r\n");
  ReadServer server({first, response(503, "unavailable")});
  CommunicationReadJob job({true});
  ASSERT_TRUE(job.start(request(server)));
  std::optional<FediverseTransportResult> result;
  for (int i = 0; i < 1000 && !result; ++i) {
    result = job.poll();
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(result);
  EXPECT_EQ(result->observation.state, CaptureState::Partial);
  ASSERT_EQ(result->pages.size(), 2U);
  EXPECT_EQ(result->pages.front().body, "[]");
  EXPECT_FALSE(result->gaps.empty());
}

TEST(CommunicationReadRetentionTest,
     CombinedRecordAndByteLimitsRejectBeforeAppending) {
  const auto parent = std::filesystem::temp_directory_path() /
                      ("communication-read-preflight-" +
                       xanadu::UserPermascroll::generateUniqueDeviceId());
  const auto path = parent / "case";
  auto scroll     = std::make_shared<xanadu::UserPermascroll>();
  {
    EvidenceCase evidence(scroll);
    evidence.create(path, "communication read preflight");
    FediverseTransportResult result;
    result.observation.instanceAuthority = "https://social.example";
    result.observation.accountId         = "a1";
    result.observation.accountActorUri   = "https://social.example/@alice";
    result.observation.collectionUri =
        "https://social.example/api/v1/accounts/a1/statuses";
    result.pages.push_back(
        Page{.locator = "page", .status = 200, .body = "[]"});
    result.gaps.push_back({"next", "HTTP 503"});

    const auto originalBounds            = result.observation.bounds;
    result.observation.bounds.maxRecords = 2;
    const auto originalCount             = evidence.records().size();
    EXPECT_THROW(static_cast<void>(retainFediverseRead(evidence, result)),
                 std::invalid_argument);
    EXPECT_EQ(evidence.records().size(), originalCount);

    result.observation.bounds          = originalBounds;
    result.observation.bounds.maxBytes = 80;
    const auto beforeByteFailure       = evidence.records().size();
    EXPECT_THROW(static_cast<void>(retainFediverseRead(evidence, result)),
                 std::invalid_argument);
    EXPECT_EQ(evidence.records().size(), beforeByteFailure);
    evidence.close();
  }
  std::filesystem::remove_all(parent);
}

TEST(CommunicationReadRetentionTest,
     RetainsAnnotateablePageAndGapAcrossReopen) {
  const auto parent = std::filesystem::temp_directory_path() /
                      ("communication-read-retain-" +
                       xanadu::UserPermascroll::generateUniqueDeviceId());
  const auto path = parent / "case";
  auto scroll     = std::make_shared<xanadu::UserPermascroll>();
  RecordId pageId{};
  RecordId noteId{};
  {
    EvidenceCase evidence(scroll);
    evidence.create(path, "communication read retention");
    FediverseTransportResult result;
    result.observation.instanceAuthority = "https://social.example";
    result.observation.accountId         = "a1";
    result.observation.accountActorUri   = "https://social.example/@alice";
    result.observation.collectionUri =
        "https://social.example/api/v1/accounts/a1/statuses";
    result.observation.state    = CaptureState::Partial;
    result.observation.coverage = "pagination interrupted";
    result.pages.push_back(Page{.locator = "https://social.example/statuses",
                                .status  = 200,
                                .body    = "[]"});
    result.pages.push_back(Page{.locator  = "https://social.example/private",
                                .status   = 200,
                                .body     = "withheld marker",
                                .redacted = true});
    result.gaps.push_back({"https://social.example/next", "HTTP 503"});
    const auto retained = retainFediverseRead(evidence, result);
    pageId              = retained.records.at("page:0");
    EXPECT_EQ(evidence.inspect(retained.records.at("page:0")).text, "[]");
    EXPECT_EQ(evidence.inspect(retained.records.at("page:1")).text,
              "Response entity withheld by credential policy");
    EXPECT_EQ(evidence.inspect(retained.records.at("page:1"))
                  .attributes.at("artifact"),
              "withheld");
    noteId = evidence.annotate(pageId, "page review");
    evidence.save();
  }
  {
    EvidenceCase reopened(scroll);
    reopened.open(path);
    EXPECT_EQ(reopened.inspect(pageId).kind, "fediverse-response-page");
    EXPECT_EQ(reopened.inspect(noteId).kind, "note");
    EXPECT_EQ(reopened.inspect(noteId).text, "page review");
    reopened.close();
  }
  std::filesystem::remove_all(parent);
}

TEST(CommunicationReadControllerTest,
     ExplicitReadRetainsPageGapAndImmutableConnectionOffline) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("communication-read-controller-" +
                          xanadu::UserPermascroll::generateUniqueDeviceId());
  std::filesystem::create_directories(directory);
  const auto casePath = directory / "case";
  auto scroll         = std::make_shared<xanadu::UserPermascroll>();
  const auto first    = response(200, "[]",
                                 "Link: </api/v1/accounts/a1/statuses?max_id=1>; "
                                    "rel=\"next\"\r\n");
  ReadServer server({first, response(503, "unavailable"), response(200, "[]")});
  EvidenceController controller(scroll, {true});
  ASSERT_TRUE(controller.execute("new " + quoteArgument(casePath.string()) +
                                 " \"Fediverse reading\""));
  const auto read = "fediverse-read social " + quoteArgument(server.origin()) +
                    " a1 \"https://social.example/@alice\"";
  ASSERT_TRUE(controller.execute(read)) << controller.snapshot().status;
  ASSERT_TRUE(waitForControllerRead(controller));
  auto state = controller.snapshot();
  EXPECT_NE(state.status.find("partial"), std::string::npos);
  const auto connection = findRecord(controller, "communication-connection");
  const auto page       = findRecord(controller, "fediverse-response-page");
  EXPECT_TRUE(std::ranges::any_of(state.records, [](const auto &record) {
    return record.kind == "fediverse-read-gap";
  }));
  const auto noteCommand = "note " + std::to_string(page) + " \"page checked\"";
  ASSERT_TRUE(controller.execute(noteCommand));
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute("open " + quoteArgument(casePath.string())));
  EXPECT_EQ(controller.evidenceCase().inspect(page).kind,
            "fediverse-response-page");
  EXPECT_TRUE(std::ranges::any_of(
      controller.evidenceCase().records(), [](const auto &record) {
        return record.kind == "note" && record.text == "page checked";
      }));

  ASSERT_TRUE(controller.execute(read)) << controller.snapshot().status;
  ASSERT_TRUE(waitForControllerRead(controller));
  const auto records = controller.evidenceCase().records();
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const auto &record) {
                                    return record.kind ==
                                           "communication-connection";
                                  }),
            1);
  EXPECT_FALSE(controller.execute(
      "fediverse-read social " + quoteArgument(server.origin()) +
      " another-account \"https://social.example/@alice\""));
  EXPECT_NE(controller.snapshot().status.find("immutable"), std::string::npos);
  EXPECT_EQ(connection, findRecord(controller, "communication-connection"));
  ASSERT_TRUE(controller.execute("close"));
  std::filesystem::remove_all(directory);
}

TEST(CommunicationReadControllerTest,
     InvalidRemoteCleartextOriginIsRefusedBeforeConnectionPersistence) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("communication-read-invalid-origin-" +
                          xanadu::UserPermascroll::generateUniqueDeviceId());
  std::filesystem::create_directories(directory);
  const auto casePath = directory / "case";
  auto scroll         = std::make_shared<xanadu::UserPermascroll>();
  EvidenceController controller(scroll, {true});
  ASSERT_TRUE(controller.execute("new " + quoteArgument(casePath.string()) +
                                 " \"Invalid origin test\""));
  const auto count = controller.evidenceCase().records().size();
  EXPECT_FALSE(
      controller.execute("fediverse-read refused \"http://social.example\" a1 "
                         "\"https://social.example/@alice\""));
  EXPECT_EQ(controller.evidenceCase().records().size(), count);
  EXPECT_FALSE(std::ranges::any_of(
      controller.evidenceCase().records(), [](const auto &record) {
        return record.kind == "communication-connection";
      }));
  ASSERT_TRUE(controller.execute("close"));
  std::filesystem::remove_all(directory);
}

TEST(CommunicationReadControllerTest,
     ClosingAndOpeningReplacementDiscardsPendingNativeResult) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("communication-read-stale-" +
                          xanadu::UserPermascroll::generateUniqueDeviceId());
  std::filesystem::create_directories(directory);
  const auto originalPath    = directory / "original";
  const auto replacementPath = directory / "replacement";
  auto scroll                = std::make_shared<xanadu::UserPermascroll>();
  ReadServer server({response(200, "[]")}, 200ms);
  EvidenceController controller(scroll, {true});
  ASSERT_TRUE(controller.execute("new " + quoteArgument(originalPath.string()) +
                                 " \"Original\""));
  ASSERT_TRUE(controller.execute("fediverse-read pending " +
                                 quoteArgument(server.origin()) +
                                 " a1 \"https://social.example/@alice\""));
  server.waitForRequest();
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute(
      "new " + quoteArgument(replacementPath.string()) + " \"Replacement\""));
  ASSERT_TRUE(waitForControllerRead(controller));
  EXPECT_NE(controller.snapshot().status.find("Stale communication read"),
            std::string::npos);
  EXPECT_FALSE(std::ranges::any_of(
      controller.evidenceCase().records(), [](const auto &record) {
        return record.kind.starts_with("fediverse-") ||
               record.kind == "communication-acquisition";
      }));
  ASSERT_TRUE(controller.execute("close"));
  std::filesystem::remove_all(directory);
}

} // namespace
} // namespace xanadu::evidence
