#include <gtest/gtest.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/xanadu/evidence/capture.hpp"

namespace {

using namespace std::chrono_literals;
using xanadu::evidence::Binary;
using xanadu::evidence::CaptureJob;
using xanadu::evidence::CaptureLimits;
using xanadu::evidence::CaptureState;
using xanadu::evidence::Source;
using xanadu::evidence::SourceKind;
using xanadu::evidence::StorageClass;
using xanadu::evidence::Value;

class TemporaryDatabase {
public:
  TemporaryDatabase() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "gleditor-capture-XXXXXX")
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int descriptor = mkstemp(writable.data());
    if (descriptor < 0) throw std::runtime_error("mkstemp failed");
    close(descriptor);
    path_       = writable.data();
    sqlite3 *db = nullptr;
    if (sqlite3_open(path_.c_str(), &db) != SQLITE_OK) {
      const std::string error = db ? sqlite3_errmsg(db) : "SQLite open failed";
      if (db) sqlite3_close(db);
      throw std::runtime_error(error);
    }
    const char *sql =
        "CREATE TABLE sample(i INTEGER, txt TEXT, data BLOB);"
        "INSERT INTO sample VALUES(9223372036854775807, '', x'0000ff');"
        "INSERT INTO sample VALUES(NULL, 'tail', x'');";
    char *error               = nullptr;
    const int status          = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    const std::string message = error ? error : "fixture creation failed";
    sqlite3_free(error);
    sqlite3_close(db);
    if (status != SQLITE_OK) throw std::runtime_error(message);
  }

  ~TemporaryDatabase() { std::filesystem::remove(path_); }
  const std::string &path() const { return path_; }

private:
  std::string path_;
};

struct Response {
  int status{200};
  std::string headers;
  std::string body;
  std::chrono::milliseconds delay{};
};

class LocalHttpServer {
public:
  explicit LocalHttpServer(std::vector<Response> responses)
      : responses_(std::move(responses)) {
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) throw std::runtime_error("socket failed");
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (bind(socket_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0 ||
        listen(socket_, 8) != 0) {
      close(socket_);
      throw std::runtime_error("local HTTP listen failed");
    }
    socklen_t length = sizeof(address);
    if (getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &length) !=
        0) {
      close(socket_);
      throw std::runtime_error("getsockname failed");
    }
    port_   = ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
  }

  ~LocalHttpServer() {
    shutdown(socket_, SHUT_RDWR);
    close(socket_);
    if (worker_.joinable()) worker_.join();
  }

  std::string url(std::string_view path = "/") const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
  }
  std::size_t requests() const { return requests_.load(); }
  std::string authority() const {
    auto result = url();
    result.erase(0, std::string("http://").size());
    result.pop_back();
    return result;
  }

private:
  void serve() {
    for (const auto &response : responses_) {
      const int client = accept(socket_, nullptr, nullptr);
      if (client < 0) return;
      requests_.fetch_add(1);
      char request[4096];
      std::string received;
      while (received.find("\r\n\r\n") == std::string::npos) {
        const auto count = recv(client, request, sizeof(request), 0);
        if (count <= 0) break;
        received.append(request, static_cast<std::size_t>(count));
      }
      std::this_thread::sleep_for(response.delay);
      const auto reason = response.status >= 200 && response.status < 300 ? "OK"
                          : response.status == 302 ? "Found"
                                                   : "Failure";
      std::string headers                       = response.headers;
      constexpr std::string_view authorityToken = "$AUTHORITY";
      for (std::size_t offset = headers.find(authorityToken);
           offset != std::string::npos;
           offset = headers.find(authorityToken, offset)) {
        const auto value = authority();
        headers.replace(offset, authorityToken.size(), value);
        offset += value.size();
      }
      const std::string wire =
          "HTTP/1.1 " + std::to_string(response.status) + " " + reason +
          "\r\nContent-Length: " + std::to_string(response.body.size()) +
          "\r\nConnection: close\r\n" + headers + "\r\n" + response.body;
      std::size_t sent = 0;
      while (sent < wire.size()) {
        const auto count =
            send(client, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
      }
      shutdown(client, SHUT_RDWR);
      close(client);
    }
  }

  int socket_{-1};
  std::uint16_t port_{};
  std::vector<Response> responses_;
  std::atomic_size_t requests_{};
  std::thread worker_;
};

std::optional<xanadu::evidence::CaptureResult> waitFor(CaptureJob &job) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto result = job.poll()) return result;
    std::this_thread::sleep_for(1ms);
  }
  return {};
}

TEST(EvidenceCaptureTest, SqliteRetainsExactTypesAndIsReadOnly) {
  TemporaryDatabase database;
  Source source{.name    = "fixture",
                .kind    = SourceKind::SQLite,
                .locator = database.path(),
                .query   = "SELECT i, txt, data FROM sample ORDER BY rowid"};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Complete);
  ASSERT_TRUE(result->incarnation);
  EXPECT_NE(result->incarnation->find(database.path()), std::string::npos);
  ASSERT_EQ(result->columns.size(), 3u);
  ASSERT_EQ(result->rows.size(), 2u);
  EXPECT_EQ(result->rows[0].values[0].storage, StorageClass::Integer);
  EXPECT_EQ(std::get<std::int64_t>(result->rows[0].values[0].value),
            std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(result->rows[0].values[1].storage, StorageClass::Text);
  EXPECT_EQ(std::get<std::string>(result->rows[0].values[1].value), "");
  EXPECT_EQ(result->rows[0].values[2].storage, StorageClass::Blob);
  EXPECT_EQ(std::get<Binary>(result->rows[0].values[2].value).bytes,
            std::string("\0\0\xff", 3));
  EXPECT_EQ(result->rows[1].values[0].storage, StorageClass::Null);
  EXPECT_EQ(result->rows[1].values[2].storage, StorageClass::Blob);
  EXPECT_TRUE(std::get<Binary>(result->rows[1].values[2].value).bytes.empty());

  source.query = "INSERT INTO sample VALUES(1, 'bad', x'')";
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Failed);

  for (const auto query :
       {"PRAGMA user_version", "ATTACH ':memory:' AS extra"}) {
    source.query = query;
    ASSERT_TRUE(job.start(source));
    result = waitFor(job);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->state, CaptureState::Failed) << query;
  }

  sqlite3 *db = nullptr;
  ASSERT_EQ(sqlite3_open_v2(database.path().c_str(), &db, SQLITE_OPEN_READONLY,
                            nullptr),
            SQLITE_OK);
  sqlite3_stmt *count = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT count(*) FROM sample", -1, &count,
                               nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_step(count), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(count, 0), 2);
  sqlite3_finalize(count);
  sqlite3_close(db);
}

TEST(EvidenceCaptureTest, SqliteFileReplacementChangesIncarnationWitness) {
  TemporaryDatabase database;
  Source source{.name    = "fixture",
                .kind    = SourceKind::SQLite,
                .locator = database.path(),
                .query   = "SELECT i FROM sample ORDER BY rowid"};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto before = waitFor(job);
  ASSERT_TRUE(before);
  ASSERT_TRUE(before->incarnation);

  TemporaryDatabase replacement;
  std::filesystem::rename(replacement.path(), database.path());
  ASSERT_TRUE(job.start(source));
  auto after = waitFor(job);
  ASSERT_TRUE(after);
  ASSERT_TRUE(after->incarnation);
  EXPECT_NE(*before->incarnation, *after->incarnation);
}

TEST(EvidenceCaptureTest, SourceValidationIsPureAndDoesNotEchoCredentials) {
  Source source{.name    = "not-opened",
                .kind    = SourceKind::SQLite,
                .locator = "/a/path/that/need/not/exist",
                .query   = "SELECT 1"};
  EXPECT_FALSE(xanadu::evidence::sourceProblem(source));

  source.kind                  = SourceKind::Rest;
  source.locator               = "http://secret:password@example.invalid/path";
  const auto credentialProblem = xanadu::evidence::sourceProblem(source);
  ASSERT_TRUE(credentialProblem);
  EXPECT_NE(credentialProblem->find("credentials"), std::string::npos);
  EXPECT_EQ(credentialProblem->find("secret"), std::string::npos);
  EXPECT_EQ(credentialProblem->find("password"), std::string::npos);

  source.locator = std::string("https://example.invalid/path") +
                   std::string(1, '\0') + "suffix";
  EXPECT_TRUE(xanadu::evidence::sourceProblem(source));
  source.locator          = "https://example.invalid/path";
  source.limits.timeoutMs = std::numeric_limits<long>::max();
  EXPECT_TRUE(xanadu::evidence::sourceProblem(source));
  source.limits.timeoutMs = 5000;
  source.limits.maxRows   = std::numeric_limits<std::size_t>::max();
  EXPECT_TRUE(xanadu::evidence::sourceProblem(source));
  source.limits.maxRows  = 1000;
  source.limits.maxPages = 0;
  EXPECT_FALSE(xanadu::evidence::sourceProblem(source));
  source.limits.maxPages = 16;
  source.limits.maxBytes = 1;
  source.parameters      = {std::string("too large")};
  source.kind            = SourceKind::SQLite;
  source.locator         = "/not/opened";
  source.query           = "SELECT ?";
  EXPECT_TRUE(xanadu::evidence::sourceProblem(source));
  source.parameters.clear();
  source.limits.maxBytes = 1024;
  source.query = std::string("SELECT 1") + std::string(1, '\0') + "ignored";
  EXPECT_TRUE(xanadu::evidence::sourceProblem(source));
  source.query   = "SELECT 1";
  source.locator = std::string("/path") + std::string(1, '\0') + "suffix";
  EXPECT_TRUE(xanadu::evidence::sourceProblem(source));
}

TEST(EvidenceCaptureTest, SqliteBindsTypedParametersAndRejectsExtraStatements) {
  TemporaryDatabase database;
  Source source{.name       = "parameters",
                .kind       = SourceKind::SQLite,
                .locator    = database.path(),
                .query      = "SELECT ?1, ?2, ?3, ?4, ?5",
                .parameters = {std::int64_t{7}, std::string{},
                               Binary{std::string("a\0b", 3)}, std::monostate{},
                               1.25}};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Complete);
  ASSERT_EQ(result->rows.size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(result->rows[0].values[0].value), 7);
  EXPECT_EQ(std::get<std::string>(result->rows[0].values[1].value), "");
  EXPECT_EQ(std::get<Binary>(result->rows[0].values[2].value).bytes,
            std::string("a\0b", 3));
  EXPECT_EQ(result->rows[0].values[3].storage, StorageClass::Null);
  EXPECT_EQ(result->rows[0].values[4].storage, StorageClass::Real);
  EXPECT_DOUBLE_EQ(std::get<double>(result->rows[0].values[4].value), 1.25);

  source.query = "SELECT 1; SELECT 2";
  source.parameters.clear();
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Failed);
}

TEST(EvidenceCaptureTest, SqliteLimitsReportPartialCoverage) {
  TemporaryDatabase database;
  Source source{.name    = "bounded",
                .kind    = SourceKind::SQLite,
                .locator = database.path(),
                .query   = "SELECT i, txt, data FROM sample ORDER BY rowid",
                .limits  = CaptureLimits{.maxRows = 1}};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  EXPECT_EQ(result->rows.size(), 1u);

  source.limits.maxRows    = 100;
  source.limits.maxColumns = 1;
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  EXPECT_EQ(result->columns.size(), 1u);

  source.limits.maxColumns = 64;
  source.limits.maxBytes   = 20;
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
}

TEST(EvidenceCaptureTest,
     RestFollowsSameOriginNextAndKeepsPriorPagesOnFailure) {
  LocalHttpServer server({
      {.headers = "Content-Type: application/json\r\nLink: </next>; rel=\"prev "
                  "next\"\r\n",
       .body    = "{\"n\":1}"},
      {.headers = "Content-Type: application/json\r\n", .body = "{\"n\":2}"},
  });
  Source source{
      .name = "paged", .kind = SourceKind::Rest, .locator = server.url()};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Complete);
  ASSERT_EQ(result->pages.size(), 2u);
  EXPECT_EQ(result->pages[0].body, "{\"n\":1}");
  EXPECT_EQ(result->pages[1].body, "{\"n\":2}");
  EXPECT_EQ(server.requests(), 2u);

  LocalHttpServer failedPage({
      {.headers = "Link: </next>; rel=\"next\"\r\n", .body = "first"},
      {.status = 500, .body = "unavailable"},
  });
  source.locator = failedPage.url();
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  ASSERT_EQ(result->pages.size(), 2u);
  EXPECT_EQ(result->pages.front().body, "first");
  EXPECT_EQ(result->pages.back().status, 500);
}

TEST(EvidenceCaptureTest, RestPageAndOriginLimitsRemainIncomplete) {
  LocalHttpServer twoPages({
      {.headers = "Link: </next>; rel=\"next\"\r\n", .body = "one"},
      {.body = "two"},
  });
  Source source{.name    = "page-cap",
                .kind    = SourceKind::Rest,
                .locator = twoPages.url(),
                .limits  = CaptureLimits{.maxPages = 1}};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  EXPECT_EQ(result->pages.size(), 1u);
  EXPECT_EQ(twoPages.requests(), 1u);

  LocalHttpServer crossOrigin({
      {.headers = "Link: <http://example.invalid/next>; rel=\"next\"\r\n",
       .body    = "one"},
  });
  source.locator         = crossOrigin.url();
  source.limits.maxPages = 16;
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  EXPECT_EQ(result->pages.size(), 1u);
  EXPECT_EQ(result->pages.front().next, "[withheld: refused pagination link]");

  source.locator = "http://user:secret@127.0.0.1/";
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Failed);
  EXPECT_NE(result->error.find("credentials"), std::string::npos);
  EXPECT_EQ(result->error.find("secret"), std::string::npos);
  EXPECT_EQ(result->source.locator, "[withheld: invalid REST source]");
}

TEST(EvidenceCaptureTest, RestRefusesRedirectAndMarksByteCapPartial) {
  LocalHttpServer redirected({
      {.status  = 302,
       .headers = "Location: /elsewhere\r\n",
       .body    = "redirect"},
  });
  Source source{.name    = "redirect",
                .kind    = SourceKind::Rest,
                .locator = redirected.url()};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Failed);
  EXPECT_EQ(redirected.requests(), 1u);

  LocalHttpServer large({{.body = "0123456789"}});
  source.locator         = large.url();
  source.limits.maxBytes = 4;
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  EXPECT_EQ(result->pages.size(), 1u);
}

TEST(EvidenceCaptureTest, RestRelationTokensAndCredentialLinksAreSafe) {
  LocalHttpServer unrelated({
      {.headers = "Link: </ignored>; rel=nextish\r\n", .body = "one"},
      {.body = "must not be fetched"},
  });
  Source source{.name    = "rel-token",
                .kind    = SourceKind::Rest,
                .locator = unrelated.url()};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Complete);
  EXPECT_EQ(result->pages.size(), 1u);
  EXPECT_EQ(unrelated.requests(), 1u);

  LocalHttpServer credentials({
      {.headers = "Link: <http://user:secret@$AUTHORITY/next>; rel=next\r\n",
       .body    = "first"},
      {.body = "must not be fetched"},
  });
  source.locator = credentials.url();
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  ASSERT_EQ(result->pages.size(), 1u);
  EXPECT_EQ(result->pages.front().next, "[withheld: refused pagination link]");
  EXPECT_NE(result->pages.front().error.find("credentials"), std::string::npos);
  EXPECT_EQ(result->pages.front().next.find("secret"), std::string::npos);
  EXPECT_EQ(result->error.find("secret"), std::string::npos);
  EXPECT_EQ(credentials.requests(), 1u);

  LocalHttpServer malformed({{.headers = "Link: rel=next\r\n", .body = "one"}});
  source.locator = malformed.url();
  ASSERT_TRUE(job.start(source));
  result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Partial);
  EXPECT_EQ(result->pages.front().next, "[withheld: refused pagination link]");
}

TEST(EvidenceCaptureTest, CancellationAndPolledResultsOwnTheirData) {
  LocalHttpServer slow({{.body = "owned", .delay = 150ms}});
  Source source{
      .name = "owned", .kind = SourceKind::Rest, .locator = slow.url()};
  std::optional<xanadu::evidence::CaptureResult> owned;
  {
    CaptureJob job;
    ASSERT_TRUE(job.start(source));
    owned = waitFor(job);
  }
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->pages.front().body, "owned");

  LocalHttpServer cancelled({{.body = "late", .delay = 200ms}});
  source.locator = cancelled.url();
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  job.cancel();
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->state, CaptureState::Cancelled);
}

TEST(EvidenceCaptureTest, RestDeadlineStopsTheCaptureAsIncomplete) {
  LocalHttpServer slow({{.body = "too late", .delay = 250ms}});
  Source source{.name    = "deadline",
                .kind    = SourceKind::Rest,
                .locator = slow.url(),
                .limits  = CaptureLimits{.timeoutMs = 20}};
  CaptureJob job;
  ASSERT_TRUE(job.start(source));
  auto result = waitFor(job);
  ASSERT_TRUE(result);
  EXPECT_NE(result->state, CaptureState::Complete);
}

TEST(EvidenceCaptureTest, DisplayHelpersPreserveNullAndRenderBlobsAsHex) {
  using xanadu::evidence::CapturedValue;
  EXPECT_EQ(xanadu::evidence::stateName(CaptureState::Partial), "partial");
  EXPECT_EQ(xanadu::evidence::storageName(StorageClass::Blob), "blob");
  EXPECT_EQ(xanadu::evidence::displayValue(
                CapturedValue{.storage = StorageClass::Null, .value = {}}),
            "NULL");
  EXPECT_EQ(xanadu::evidence::displayValue(
                CapturedValue{.storage = StorageClass::Blob,
                              .value   = Binary{std::string("\0\x0f", 2)}}),
            "0x000f");
}

} // namespace
