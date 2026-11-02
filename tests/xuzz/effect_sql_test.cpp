#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/xanadu/evidence/effect_adapter.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;

struct DbCloser {
  void operator()(sqlite3 *db) const { sqlite3_close_v2(db); }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;

class SqlPatchTest : public ::testing::Test {
protected:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("evidence-sql-patch-" +
       xanadu::UserPermascroll::generateUniqueDeviceId() + ".db");

  void SetUp() override {
    sqlite3 *raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    Db db(raw);
    ASSERT_EQ(sqlite3_exec(db.get(),
                           "CREATE TABLE item(id INTEGER PRIMARY KEY, value, "
                           "UNIQUE(value));"
                           "INSERT INTO item VALUES(1, 10);"
                           "INSERT INTO item VALUES(2, 20);",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
  }

  void TearDown() override {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".old");
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
    std::filesystem::remove(path.string() + "-journal");
  }

  EffectRequest request(std::string sql,
                        std::vector<Value> parameters = {}) const {
    EffectRequest result;
    result.kind       = EffectKind::SqlPatch;
    result.target     = path.string();
    result.sql        = std::move(sql);
    result.parameters = std::move(parameters);
    result.guard      = SqlGuard{"SELECT 1", {}, std::int64_t{1}};
    return result;
  }

  EffectResult run(const EffectRequest &effect,
                   const std::optional<SqlGuard> &guard = {}) const {
    auto withGuard = effect;
    if (guard) withGuard.guard = guard;
    EffectAuthority authority;
    authority.operation         = EffectKind::SqlPatch;
    authority.canonicalTarget   = canonicalEffectTarget(withGuard);
    authority.targetFingerprint = effectTargetFingerprint(withGuard);
    std::atomic_bool cancel{};
    return executeSqlPatch(withGuard, authority, cancel);
  }

  std::int64_t integerAt(const std::string &sql) const {
    sqlite3 *raw = nullptr;
    EXPECT_EQ(
        sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr),
        SQLITE_OK);
    Db db(raw);
    sqlite3_stmt *statement = nullptr;
    EXPECT_EQ(
        sqlite3_prepare_v2(db.get(), sql.c_str(), -1, &statement, nullptr),
        SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
        statement, sqlite3_finalize);
    EXPECT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    return sqlite3_column_int64(stmt.get(), 0);
  }
};

TEST_F(SqlPatchTest, StaleTypedGuardRollsBackWithoutMutation) {
  auto effect = request("UPDATE item SET value=?1 WHERE id=?2",
                        {std::int64_t{11}, std::int64_t{1}});
  SqlGuard guard{
      "SELECT value FROM item WHERE id=?1", {std::int64_t{1}}, std::int64_t{9}};
  const auto result = run(effect, guard);
  EXPECT_EQ(result.outcome, EffectOutcome::Rejected);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 10);
}

TEST_F(SqlPatchTest, AffectedRowMismatchRollsBackTheUpdate) {
  auto effect                 = request("UPDATE item SET value=?1 WHERE id=?2",
                                        {std::int64_t{11}, std::int64_t{1}});
  effect.expectedAffectedRows = 2;
  const auto result           = run(effect);
  EXPECT_EQ(result.outcome, EffectOutcome::Rejected);
  EXPECT_EQ(result.affectedRows, 1U);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 10);
}

TEST_F(SqlPatchTest, ConstraintFailureRollsBackTheUpdate) {
  auto effect       = request("UPDATE item SET value=?1 WHERE id=?2",
                              {std::int64_t{20}, std::int64_t{1}});
  const auto result = run(effect);
  EXPECT_EQ(result.outcome, EffectOutcome::Rejected);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 10);
}

TEST_F(SqlPatchTest, RevokedAndCancelledAuthorityDoNotMutate) {
  auto effect = request("UPDATE item SET value=?1 WHERE id=?2",
                        {std::int64_t{11}, std::int64_t{1}});
  EffectAuthority revoked;
  revoked.operation         = EffectKind::SqlPatch;
  revoked.canonicalTarget   = canonicalEffectTarget(effect);
  revoked.targetFingerprint = effectTargetFingerprint(effect);
  revoked.revoked.store(true);
  std::atomic_bool cancel{};
  EXPECT_EQ(executeSqlPatch(effect, revoked, cancel).outcome,
            EffectOutcome::Cancelled);
  EffectAuthority authority;
  authority.operation         = EffectKind::SqlPatch;
  authority.canonicalTarget   = canonicalEffectTarget(effect);
  authority.targetFingerprint = effectTargetFingerprint(effect);
  cancel.store(true);
  EXPECT_EQ(executeSqlPatch(effect, authority, cancel).outcome,
            EffectOutcome::Cancelled);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 10);
}

TEST_F(SqlPatchTest, ReplacedTargetDoesNotMatchPinnedAuthority) {
  const auto effect = request("UPDATE item SET value=?1 WHERE id=?2",
                              {std::int64_t{11}, std::int64_t{1}});
  EffectAuthority authority;
  authority.operation         = EffectKind::SqlPatch;
  authority.canonicalTarget   = canonicalEffectTarget(effect);
  authority.targetFingerprint = effectTargetFingerprint(effect);
  const auto replacedPath     = path.string() + ".old";
  std::filesystem::rename(path, replacedPath);
  sqlite3 *raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  Db replacement(raw);
  ASSERT_EQ(sqlite3_exec(replacement.get(),
                         "CREATE TABLE item(id INTEGER PRIMARY KEY, value);"
                         "INSERT INTO item VALUES(1, 30);",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  replacement.reset();

  std::atomic_bool cancel{};
  EXPECT_EQ(executeSqlPatch(effect, authority, cancel).outcome,
            EffectOutcome::Rejected);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 30);
}

TEST_F(SqlPatchTest, CancellationInterruptsWaitingForWriteLock) {
  sqlite3 *raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  Db lockDb(raw);
  ASSERT_EQ(
      sqlite3_exec(lockDb.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr),
      SQLITE_OK);

  const auto effect = request("UPDATE item SET value=?1 WHERE id=?2",
                              {std::int64_t{11}, std::int64_t{1}});
  EffectAuthority authority;
  authority.operation         = EffectKind::SqlPatch;
  authority.canonicalTarget   = canonicalEffectTarget(effect);
  authority.targetFingerprint = effectTargetFingerprint(effect);
  std::atomic_bool cancel{};
  auto running = std::async(std::launch::async, [&] {
    return executeSqlPatch(effect, authority, cancel);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  cancel.store(true);
  EXPECT_EQ(running.get().outcome, EffectOutcome::Cancelled);
  ASSERT_EQ(sqlite3_exec(lockDb.get(), "ROLLBACK", nullptr, nullptr, nullptr),
            SQLITE_OK);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 10);
}

TEST_F(SqlPatchTest, ReadOnlyAndMultipleStatementsAreRejected) {
  auto read = request("SELECT value FROM item WHERE id=?1", {std::int64_t{1}});
  EXPECT_EQ(run(read).outcome, EffectOutcome::Rejected);
  auto multiple =
      request("UPDATE item SET value=11 WHERE id=1; DELETE FROM item");
  EXPECT_EQ(run(multiple).outcome, EffectOutcome::Rejected);
  for (const auto *sql :
       {"ATTACH DATABASE 'other.db' AS other", "PRAGMA user_version=8",
        "CREATE TABLE extra(id INTEGER)", "SELECT load_extension('missing')"}) {
    EXPECT_EQ(run(request(sql)).outcome, EffectOutcome::Rejected) << sql;
  }
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 10);
}

TEST_F(SqlPatchTest, GuardComparesNullBlobAndLargeIntegerWithoutCoercion) {
  sqlite3 *raw = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
  Db db(raw);
  ASSERT_EQ(
      sqlite3_exec(db.get(),
                   "ALTER TABLE item ADD COLUMN nullable;"
                   "ALTER TABLE item ADD COLUMN payload;"
                   "UPDATE item SET nullable=NULL, payload=x'00ff' WHERE id=1;",
                   nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db.get(),
                         "UPDATE item SET value=9007199254740993 WHERE id=1;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  db.reset();

  auto effect = request("UPDATE item SET value=?1 WHERE id=?2",
                        {std::int64_t{9007199254740994LL}, std::int64_t{1}});
  SqlGuard guard{"SELECT value FROM item WHERE id=?1",
                 {std::int64_t{1}},
                 std::int64_t{9007199254740993LL}};
  EXPECT_EQ(run(effect, guard).outcome, EffectOutcome::Completed);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 9007199254740994LL);

  effect = request("UPDATE item SET value=?1 WHERE id=?2",
                   {std::int64_t{9007199254740995LL}, std::int64_t{1}});
  SqlGuard nullGuard{"SELECT nullable FROM item WHERE id=?1",
                     {std::int64_t{1}},
                     std::monostate{}};
  EXPECT_EQ(run(effect, nullGuard).outcome, EffectOutcome::Completed);

  effect = request("UPDATE item SET value=?1 WHERE id=?2",
                   {std::int64_t{9007199254740996LL}, std::int64_t{1}});
  SqlGuard blobGuard{"SELECT payload FROM item WHERE id=?1",
                     {std::int64_t{1}},
                     Binary{std::string("\0\xff", 2)}};
  EXPECT_EQ(run(effect, blobGuard).outcome, EffectOutcome::Completed);
  EXPECT_EQ(integerAt("SELECT value FROM item WHERE id=1"), 9007199254740996LL);
}

} // namespace
