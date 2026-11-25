#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <sqlite3.h>
#include <unistd.h>

#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/vortex/lens_adapters.hpp"
#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/git_lens.hpp"
#include "common/xanadu/zigzag/log_lens.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"

namespace {
using namespace xanadu::vortex;
namespace fs = std::filesystem;

constexpr AdapterKey logKey{10, 1};

CellRequest requestFor(AdapterKey key, std::uint32_t specification) {
  return CellRequest{.key           = key,
                     .capability    = CapabilityCell{},
                     .specification = specification};
}

struct CapturingSink final : CompletionSink {
  void progress(PrivateRequestToken, IoProgress) noexcept override {}
  void terminalCandidate(PrivateRequestToken, BackendResult result,
                         std::optional<PayloadLease>) noexcept override {
    candidate = std::move(result);
  }
  void releaseAcknowledged(BackendReleaseAck ack) noexcept override {
    released = ack;
  }
  std::optional<BackendResult> candidate;
  std::optional<BackendReleaseAck> released;
};

fs::path writeLogFile(std::string_view content) {
  const auto path =
      fs::temp_directory_path() /
      fs::path("io_lens_adapters_test_" + std::to_string(::getpid()) + ".log");
  std::ofstream out(path);
  out << content;
  out.close();
  return path;
}

BackendContext makeContext(PayloadResolver &payloads) {
  return BackendContext{.stop = {}, .payloads = payloads, .budget = {1 << 20}};
}

// -- LogLens: Browse bumps the adapter's fetch generation -------------------

TEST(LogLensSourceAdapterTest, BrowseSucceedsAndBumpsGeneration) {
  const auto path = writeLogFile("line-0\nline-1\nline-2\n");
  zigzag::LogLens lens(path.string(), 2);
  LogLensSourceAdapter adapter(lens);
  OwnerContext owner;
  PayloadResolver payloads;

  const auto startGeneration = adapter.generation();
  const auto key             = adapter.stage(
      LensOpSpec{.kind = LensOpKind::Browse, .expectedGeneration = 0}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);

  CapturingSink sink;
  auto context = makeContext(payloads);
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  EXPECT_GT(adapter.generation(), startGeneration);

  const auto published = adapter.publish(
      NativeCompletion{{1}, *sink.candidate, {}, *sink.released}, owner);
  ASSERT_TRUE(published);
  EXPECT_TRUE(std::holds_alternative<Complete>(published->outcome));

  fs::remove(path);
}

TEST(LogLensSourceAdapterTest, PrepareRefusesStaleGenerationBeforeAdmission) {
  const auto path = writeLogFile("line-0\n");
  zigzag::LogLens lens(path.string(), 2);
  LogLensSourceAdapter adapter(lens);
  OwnerContext owner;

  const auto staleGeneration = adapter.generation();
  // A browse that succeeds bumps the adapter past staleGeneration.
  const auto bumpKey = adapter.stage(LensOpSpec{.kind = LensOpKind::Browse}).value();
  const auto bumpPrepared = adapter.prepare(requestFor(logKey, bumpKey), owner);
  ASSERT_TRUE(bumpPrepared);
  PayloadResolver payloads;
  auto context = makeContext(payloads);
  CapturingSink bumpSink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *bumpPrepared},
                                            context, bumpSink);
  ASSERT_TRUE(bumpSink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(bumpSink.candidate->outcome));

  const auto key      = adapter.stage(LensOpSpec{
           .kind = LensOpKind::Browse, .expectedGeneration = staleGeneration}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_FALSE(prepared);
  EXPECT_EQ(prepared.error().code, IoErrorCode::Stale);

  fs::remove(path);
}

TEST(LogLensSourceAdapterTest,
     ExecuteReportsConflictWhenAnotherFetchLandsAfterAdmission) {
  const auto path = writeLogFile("line-0\n");
  zigzag::LogLens lens(path.string(), 2);
  LogLensSourceAdapter adapter(lens);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key      = adapter.stage(LensOpSpec{
           .kind = LensOpKind::Browse, .expectedGeneration = adapter.generation()}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);

  // A second, already-admitted browse lands on this adapter between this
  // request's admission and its worker turn.
  const auto raceKey = adapter.stage(LensOpSpec{.kind = LensOpKind::Browse}).value();
  const auto racePrepared = adapter.prepare(requestFor(logKey, raceKey), owner);
  ASSERT_TRUE(racePrepared);
  CapturingSink raceSink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{2}, *racePrepared},
                                            context, raceSink);
  ASSERT_TRUE(raceSink.candidate);
  ASSERT_TRUE(std::holds_alternative<Complete>(raceSink.candidate->outcome));

  CapturingSink sink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Conflict>(sink.candidate->outcome));

  fs::remove(path);
}

// -- SqlLens: Browse fetches a table off-owner and bumps generation --------

/// A temp .db in WAL mode with one browsable table (the sql_lens_test
/// FixtureDb pattern, trimmed to what the adapter needs).
struct AdapterDb {
  fs::path path;

  explicit AdapterDb() {
    path = fs::temp_directory_path() /
           ("io_lens_sql_" + std::to_string(::getpid()) + ".db");
    sqlite3 *db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.string().c_str(), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                  SQLITE_OPEN_URI,
                              nullptr),
              SQLITE_OK);
    if (db == nullptr) {
      return;
    }
    EXPECT_EQ(sqlite3_exec(db, "PRAGMA journal_mode = WAL", nullptr, nullptr,
                           nullptr),
              SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(db,
                           "CREATE TABLE invoices("
                           "  id INTEGER PRIMARY KEY, customer TEXT NOT NULL);"
                           "INSERT INTO invoices VALUES (1,'alice'),(2,'bob');",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close_v2(db);
  }

  ~AdapterDb() {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path.string() + "-wal", ec);
    fs::remove(path.string() + "-shm", ec);
  }
};

TEST(SqlLensSourceAdapterTest, BrowseFetchesAndBumpsGeneration) {
  AdapterDb db;
  auto lens = zigzag::openSqliteLens(db.path.string(), 8);
  ASSERT_TRUE(lens.has_value()) << lens.error();
  SqlLensSourceAdapter adapter(**lens);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto startGeneration = adapter.generation();
  const auto key             = adapter.stage(
      LensOpSpec{.kind = LensOpKind::Browse, .argument = "invoices"}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);

  CapturingSink sink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  EXPECT_GT(adapter.generation(), startGeneration);

  const auto published = adapter.publish(
      NativeCompletion{{1}, *sink.candidate, {}, *sink.released}, owner);
  ASSERT_TRUE(published);
  EXPECT_TRUE(std::holds_alternative<Complete>(published->outcome));
}

TEST(SqlLensSourceAdapterTest, ProbeDoesNotBumpGeneration) {
  AdapterDb db;
  auto lens = zigzag::openSqliteLens(db.path.string(), 8);
  ASSERT_TRUE(lens.has_value()) << lens.error();
  SqlLensSourceAdapter adapter(**lens);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto startGeneration = adapter.generation();
  const auto key      = adapter.stage(LensOpSpec{.kind = LensOpKind::Probe}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  EXPECT_EQ(adapter.generation(), startGeneration);
}

// -- GitLens: Browse over a real scratch repo -------------------------------

/// A scratch repo with two empty commits (the git_lens_test GitFixture
/// pattern, trimmed to what the adapter needs).
struct AdapterRepo {
  fs::path repo;

  explicit AdapterRepo(std::size_t n) {
    repo = fs::temp_directory_path() /
           ("io_lens_git_" + std::to_string(::getpid()));
    fs::remove_all(repo);
    fs::create_directories(repo);
    run("git init -q -b main " + repo.string());
    run("git -C " + repo.string() + " config user.email t@t && git -C " +
        repo.string() + " config user.name t");
    for (std::size_t i = 1; i <= n; ++i) {
      run("git -C " + repo.string() + " commit -q --allow-empty -m c" +
          std::to_string(i));
    }
  }
  ~AdapterRepo() { fs::remove_all(repo); }
  static void run(const std::string &cmd) {
    EXPECT_EQ(::system(cmd.c_str()), 0) << cmd;
  }
};

TEST(GitLensSourceAdapterTest, BrowseFetchesAndBumpsGeneration) {
  AdapterRepo repo{2};
  zigzag::GitLens lens(repo.repo.string(), 8);
  GitLensSourceAdapter adapter(lens);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto startGeneration = adapter.generation();
  const auto key      = adapter.stage(LensOpSpec{.kind = LensOpKind::Browse}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);

  CapturingSink sink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  EXPECT_GT(adapter.generation(), startGeneration);
}

TEST(GitLensSourceAdapterTest, MaterializeOnUnknownWindowRefuses) {
  AdapterRepo repo{2};
  zigzag::GitLens lens(repo.repo.string(), 8);
  GitLensSourceAdapter adapter(lens);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto key = adapter.stage(LensOpSpec{.kind = LensOpKind::Materialize,
                                            .windowKey = "git:no-such-window"}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *failed = std::get_if<Failed>(&sink.candidate->outcome);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::BackendFailure);
}

// -- ConfigLens: Probe only, no generation bump on Probe ---------------------

TEST(ConfigLensSourceAdapterTest,
     ProbeReportsUnchangedWithoutBumpingGeneration) {
  auto lensResult = zigzag::ConfigLens::fromText("cfg.json", "{\"a\":1}");
  ASSERT_TRUE(lensResult);
  ConfigLensSourceAdapter adapter(**lensResult);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto startGeneration = adapter.generation();
  const auto key      = adapter.stage(LensOpSpec{.kind = LensOpKind::Probe}).value();
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink;
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(sink.candidate->outcome));
  EXPECT_EQ(adapter.generation(), startGeneration);
}

TEST(ConfigLensSourceAdapterTest, MaterializeIsRefusedAtPrepareTime) {
  // ConfigLens has no windowed materializePage: the refusal happens at
  // prepare so no worker turn is spent reporting Unsupported.
  auto lensResult = zigzag::ConfigLens::fromText("cfg.json", "{\"a\":1}");
  ASSERT_TRUE(lensResult);
  ConfigLensSourceAdapter adapter(**lensResult);
  OwnerContext owner;

  const auto key = adapter.stage(
      LensOpSpec{.kind = LensOpKind::Materialize, .windowKey = "cfg:no-such"}).value();
  ASSERT_NE(key, 0U);
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(prepared.error().code, IoErrorCode::Unsupported);
}

TEST(ConfigLensSourceAdapterTest,
     StageAtKeySpaceSkipsReservedAndDuplicateKeys) {
  auto lensResult = zigzag::ConfigLens::fromText("cfg.json", "{\"a\":1}");
  ASSERT_TRUE(lensResult);
  ConfigLensSourceAdapter adapter(**lensResult);
  const LensOpSpec spec{.kind = LensOpKind::Probe};

  // Key 0 is the table-full sentinel; an entry there would be found by a
  // request whose specification names nothing.
  EXPECT_FALSE(adapter.stageAt(0, spec).has_value());
  const auto pinned = adapter.stageAt(1, spec);
  ASSERT_TRUE(pinned.has_value());
  EXPECT_EQ(*pinned, 1U);
  // Silently overwriting a live key would redirect an in-flight request.
  EXPECT_FALSE(adapter.stageAt(1, spec).has_value());

  // Auto keys skip still-live stageAt'd keys rather than collide.
  const auto autoKey = adapter.stage(spec).value();
  EXPECT_NE(autoKey, 0U);
  EXPECT_NE(autoKey, 1U);

  OwnerContext owner;
  EXPECT_TRUE(adapter.prepare(requestFor(logKey, 1), owner).has_value());
  EXPECT_TRUE(adapter.prepare(requestFor(logKey, autoKey), owner).has_value());
}

TEST(ConfigLensSourceAdapterTest, AbandonedStageFailsPrepare) {
  auto lensResult = zigzag::ConfigLens::fromText("cfg.json", "{\"a\":1}");
  ASSERT_TRUE(lensResult);
  ConfigLensSourceAdapter adapter(**lensResult);
  OwnerContext owner;

  const auto key = adapter.stage(LensOpSpec{.kind = LensOpKind::Probe}).value();
  ASSERT_NE(key, 0U);
  adapter.abandon(key);
  const auto prepared = adapter.prepare(requestFor(logKey, key), owner);
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(prepared.error().code, IoErrorCode::InvalidRequest);
}

} // namespace
