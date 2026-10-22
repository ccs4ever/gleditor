/**
 * @file sqlite_backend_test.cpp
 * @brief Unit tests for SqliteBackend, the read-only SqlBackend port
 * (design/sql-to-zigzag.md §7.2). Fixtures are temp .db files created
 * through a separate writable connection, then reopened through the
 * backend's read-only gate.
 */
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "common/xanadu/zigzag/sqlite_backend.hpp"

namespace {

using namespace zigzag;

namespace fs = std::filesystem;

/// A temp .db populated through a writable connection and closed, so the
/// backend only ever sees it read-only. journalMode selects "wal" or
/// "delete" (the rollback journal).
struct FixtureDb {
  fs::path path;

  explicit FixtureDb(std::string_view journalMode, bool populate = true) {
    path        = fs::temp_directory_path() /
                  ("sqlite-backend-test-" + std::to_string(::getpid()) + "-" +
                   std::to_string(counter++) + ".db");
    sqlite3 *db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.string().c_str(), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                  SQLITE_OPEN_URI,
                              nullptr),
              SQLITE_OK);
    if (db == nullptr) {
      return; // open() will fail cleanly; gtest ASSERTs can't run in a ctor
    }
    const std::string jm = "PRAGMA journal_mode = " + std::string(journalMode);
    EXPECT_EQ(sqlite3_exec(db, jm.c_str(), nullptr, nullptr, nullptr),
              SQLITE_OK);
    if (populate) {
      const char *ddl =
          "CREATE TABLE invoices("
          "  id INTEGER PRIMARY KEY, customer TEXT NOT NULL,"
          "  amount REAL, note BLOB);"
          "INSERT INTO invoices VALUES"
          "  (1,'alice',10.5,zeroblob(4)),"
          "  (2,'bob',20.25,x'0102'),"
          "  (3,'carol',NULL,NULL);"
          "CREATE TABLE nopk(x TEXT, y INTEGER);"
          "INSERT INTO nopk VALUES ('a',1),('b',2);"
          "CREATE VIEW v_invoices AS SELECT id, customer FROM invoices;";
      EXPECT_EQ(sqlite3_exec(db, ddl, nullptr, nullptr, nullptr), SQLITE_OK);
    }
    sqlite3_close_v2(db);
  }

  ~FixtureDb() {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path.string() + "-wal", ec);
    fs::remove(path.string() + "-shm", ec);
    fs::remove(path.string() + "-journal", ec);
  }

  static std::size_t counter;
};

std::size_t FixtureDb::counter = 0;

TEST(SqliteBackendTest, SelectReturnsTypedValues) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  auto cur = (*be)->prepare("SELECT id, customer, amount, note FROM "
                            "invoices ORDER BY id",
                            {});
  ASSERT_TRUE(cur.has_value()) << cur.error();
  auto rows = (*cur)->fetchNext(16);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 3u);
  EXPECT_EQ(std::get<std::int64_t>((*rows)[0][0]), 1);
  EXPECT_EQ(std::get<std::string>((*rows)[0][1]), "alice");
  EXPECT_DOUBLE_EQ(std::get<double>((*rows)[0][2]), 10.5);
  EXPECT_EQ(std::get<std::vector<std::byte>>((*rows)[0][3]).size(), 4u);
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>((*rows)[2][2])); // NULL amount
}

TEST(SqliteBackendTest, WriteStatementsRefused) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  for (const char *sql :
       {"INSERT INTO invoices(id,customer) VALUES (9,'mallory')",
        "UPDATE invoices SET customer='x' WHERE id=1",
        "DELETE FROM invoices WHERE id=1", "CREATE TABLE t(x)",
        "DROP TABLE invoices", "REPLACE INTO invoices(id) VALUES(1)", "VACUUM",
        "REINDEX", "ANALYZE"}) {
    auto cur = (*be)->prepare(sql, {});
    EXPECT_FALSE(cur.has_value()) << "should refuse: " << sql;
  }
}

TEST(SqliteBackendTest, TransactionControlRefused) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  // stmt_readonly() passes these -- the statement-kind gate catches them.
  for (const char *sql : {"BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT s",
                          "ROLLBACK TO s", "RELEASE s", "END"}) {
    auto cur = (*be)->prepare(sql, {});
    EXPECT_FALSE(cur.has_value()) << "should refuse: " << sql;
  }
}

TEST(SqliteBackendTest, AttachDetachRefused) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  EXPECT_FALSE((*be)->prepare("ATTACH DATABASE 'x.db' AS x", {}).has_value());
  EXPECT_FALSE((*be)->prepare("DETACH DATABASE main", {}).has_value());
}

TEST(SqliteBackendTest, MultiStatementAndEmptyRefused) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  EXPECT_FALSE((*be)->prepare("SELECT 1; SELECT 2", {}).has_value());
  EXPECT_FALSE((*be)->prepare("SELECT 1; DROP TABLE nopk", {}).has_value());
  EXPECT_FALSE((*be)->prepare("", {}).has_value());
  EXPECT_FALSE((*be)->prepare("   -- only a comment\n", {}).has_value());
  EXPECT_FALSE((*be)->prepare("/* block comment */", {}).has_value());
}

TEST(SqliteBackendTest, ReadStatementKindsAllowed) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  {
    auto cur = (*be)->prepare("VALUES (1,'a'),(2,'b')", {});
    ASSERT_TRUE(cur.has_value()) << cur.error();
    auto rows = (*cur)->fetchNext(4);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 2u);
  }
  {
    auto cur = (*be)->prepare("EXPLAIN SELECT 1", {});
    ASSERT_TRUE(cur.has_value()) << cur.error();
    auto rows = (*cur)->fetchNext(64);
    ASSERT_TRUE(rows.has_value());
    EXPECT_GT(rows->size(), 0u);
  }
  {
    auto cur = (*be)->prepare("PRAGMA data_version", {});
    ASSERT_TRUE(cur.has_value()) << cur.error();
    auto rows = (*cur)->fetchNext(2);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 1u);
  }
  {
    // Comment-leading SQL still reads its kind correctly.
    auto cur = (*be)->prepare("-- lead\nSELECT x FROM nopk", {});
    ASSERT_TRUE(cur.has_value()) << cur.error();
  }
}

TEST(SqliteBackendTest, WriteFormPragmasRefused) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  // Dual-mode pragma with a value = a write; the authorizer denies it and
  // prepare fails.
  EXPECT_FALSE((*be)->prepare("PRAGMA query_only = 0", {}).has_value());
  EXPECT_FALSE((*be)->prepare("PRAGMA journal_mode = delete", {}).has_value());
}

TEST(SqliteBackendTest, DeniedFunctionsRefused) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  for (const char *sql :
       {"SELECT writefile('/tmp/x','x')", "SELECT readfile('/etc/passwd')",
        "SELECT load_extension('/tmp/evil.so')"}) {
    auto cur = (*be)->prepare(sql, {});
    // The function may not be compiled in at all (prepare fails with
    // "no such function"); where it exists, the authorizer denies at
    // step time. Either way the call must not succeed.
    if (cur.has_value()) {
      auto rows = (*cur)->fetchNext(1);
      EXPECT_FALSE(rows.has_value()) << "authorizer should deny: " << sql;
    }
  }
}

TEST(SqliteBackendTest, SchemaProjectsTablesColumnsAndKeys) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  auto tables = (*be)->schema();
  ASSERT_TRUE(tables.has_value()) << tables.error();
  const auto &ts = *tables;
  ASSERT_GE(ts.size(), 3u);
  const auto inv = std::ranges::find_if(
      ts, [](const SqlTable &t) { return t.name == "invoices"; });
  ASSERT_NE(inv, ts.end());
  EXPECT_FALSE(inv->isView);
  ASSERT_GE(inv->columns.size(), 4u);
  EXPECT_TRUE(inv->columns[0].keyColumn);
  EXPECT_EQ(inv->columns[0].keyPosition, 1u);
  EXPECT_FALSE(inv->columns[1].keyColumn);
  const auto vw = std::ranges::find_if(
      ts, [](const SqlTable &t) { return t.name == "v_invoices"; });
  ASSERT_NE(vw, ts.end());
  EXPECT_TRUE(vw->isView);
}

TEST(SqliteBackendTest, OneActiveWindowCursor) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  auto a = (*be)->prepare("SELECT id FROM invoices", {});
  ASSERT_TRUE(a.has_value()) << a.error();
  EXPECT_FALSE((*be)->prepare("SELECT x FROM nopk", {}).has_value());
  a->reset();
  auto b = (*be)->prepare("SELECT x FROM nopk", {});
  EXPECT_TRUE(b.has_value()) << b.error();
}

TEST(SqliteBackendTest, OrdinalReplayWrapsDeterministically) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  // Replays of the same window return identical rows.
  auto replay = [&](std::size_t off, std::size_t lim) {
    auto cur = (*be)->replayWindow(
        "SELECT id, customer FROM invoices ORDER BY id DESC", {}, {}, {}, off,
        lim);
    EXPECT_TRUE(cur.has_value()) << cur.error();
    auto rows = (*cur)->fetchNext(64);
    EXPECT_TRUE(rows.has_value()) << rows.error();
    return rows.value_or(std::vector<std::vector<SqlValue>>{});
  };
  auto w1 = replay(0, 2);
  auto w2 = replay(0, 2);
  ASSERT_EQ(w1.size(), 2u);
  ASSERT_EQ(w1.size(), w2.size());
  EXPECT_EQ(std::get<std::int64_t>(w1[0][0]), std::get<std::int64_t>(w2[0][0]));
  EXPECT_EQ(std::get<std::int64_t>(w1[0][0]), 3); // DESC => first is id 3
  // The second window continues deterministically.
  auto w3 = replay(2, 2);
  ASSERT_EQ(w3.size(), 1u);
  EXPECT_EQ(std::get<std::int64_t>(w3[0][0]), 1);
}

TEST(SqliteBackendTest, ReplayRejectsNonWrappableKinds) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  EXPECT_FALSE(
      (*be)->replayWindow("PRAGMA data_version", {}, {}, {}, 0, 4).has_value());
  EXPECT_FALSE(
      (*be)->replayWindow("EXPLAIN SELECT 1", {}, {}, {}, 0, 4).has_value());
  EXPECT_FALSE(
      (*be)
          ->replayWindow("INSERT INTO nopk VALUES('z',9)", {}, {}, {}, 0, 4)
          .has_value());
}

TEST(SqliteBackendTest, SnapshotRequiresWal) {
  {
    FixtureDb f("wal");
    auto be = openSqliteBackend(f.path.string());
    ASSERT_TRUE(be.has_value()) << be.error();
    auto snap = (*be)->snapshot();
    ASSERT_TRUE(snap.has_value()) << snap.error();
    EXPECT_TRUE((*be)->capabilities().snapshots);
    // A cursor inside the scope reads normally.
    auto cur = (*be)->prepare("SELECT id FROM invoices ORDER BY id", {});
    ASSERT_TRUE(cur.has_value()) << cur.error();
    auto rows = (*cur)->fetchNext(8);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3u);
    // A second snapshot while one is live is refused.
    EXPECT_FALSE((*be)->snapshot().has_value());
  }
  {
    FixtureDb f("delete");
    auto be = openSqliteBackend(f.path.string());
    ASSERT_TRUE(be.has_value()) << be.error();
    EXPECT_FALSE((*be)->capabilities().snapshots);
    EXPECT_FALSE((*be)->snapshot().has_value());
  }
}

TEST(SqliteBackendTest, BlobBudgetTruncatesButDoesNotFail) {
  FixtureDb f("wal");
  auto be =
      openSqliteBackend(f.path.string(), SqliteOpenOptions{.blobBytes = 2});
  ASSERT_TRUE(be.has_value()) << be.error();
  auto cur = (*be)->prepare("SELECT note FROM invoices WHERE id=1", {});
  ASSERT_TRUE(cur.has_value()) << cur.error();
  auto rows = (*cur)->fetchNext(1);
  ASSERT_TRUE(rows.has_value());
  ASSERT_EQ(rows->size(), 1u);
  // zeroblob(4) capped at the 2-byte budget.
  EXPECT_EQ(std::get<std::vector<std::byte>>((*rows)[0][0]).size(), 2u);
}

TEST(SqliteBackendTest, PathWithQueryCharsOpensCleanly) {
  // A literal '?'/'#' in a filename would parse as URI query/fragment
  // without the percent-encoding in open().
  const auto dir = fs::temp_directory_path();
  const auto odd = dir / ("sqlite-backend-test-odd?name#frag.db");
  {
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(odd.string().c_str(), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                  SQLITE_OPEN_URI,
                              nullptr),
              SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(db,
                           "CREATE TABLE t(x INTEGER);"
                           "INSERT INTO t VALUES(42);",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close_v2(db);
  }
  auto be = openSqliteBackend(odd.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  auto cur = (*be)->prepare("SELECT x FROM t", {});
  ASSERT_TRUE(cur.has_value()) << cur.error();
  auto rows = (*cur)->fetchNext(1);
  ASSERT_TRUE(rows.has_value());
  EXPECT_EQ(std::get<std::int64_t>((*rows)[0][0]), 42);
  std::error_code ec;
  fs::remove(odd, ec);
}

TEST(SqliteBackendTest, ColumnProvenanceWhenAvailable) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  auto cur = (*be)->prepare(
      "SELECT id, customer, amount FROM invoices ORDER BY id", {});
  ASSERT_TRUE(cur.has_value()) << cur.error();
  const auto &cols = (*cur)->columns();
  ASSERT_EQ(cols.size(), 3u);
  if ((*be)->capabilities().columnProvenance) {
    ASSERT_TRUE(cols[0].baseTable.has_value());
    EXPECT_EQ(*cols[0].baseTable, "invoices");
    EXPECT_TRUE(cols[0].keyColumn);
    EXPECT_EQ(cols[0].keyPosition, 1u);
    EXPECT_FALSE(cols[1].keyColumn);
  } else {
    GTEST_SKIP() << "sqlite built without SQLITE_ENABLE_COLUMN_METADATA";
  }
}

TEST(SqliteBackendTest, ParameterBindingTyped) {
  FixtureDb f("wal");
  auto be = openSqliteBackend(f.path.string());
  ASSERT_TRUE(be.has_value()) << be.error();
  // '1' bound as text must not equal 1 numeric under affinity --
  // typed binding is the whole point of SqlValue (spec §5.3).
  const std::vector<SqlValue> params = {SqlValue{std::string("1")},
                                        SqlValue{std::int64_t{1}}};
  auto cur = (*be)->prepare("SELECT ?1 IS ?2", params);
  ASSERT_TRUE(cur.has_value()) << cur.error();
  auto rows = (*cur)->fetchNext(1);
  ASSERT_TRUE(rows.has_value());
  EXPECT_EQ(std::get<std::int64_t>((*rows)[0][0]), 0); // '1' IS 1 -> false
}

} // namespace
