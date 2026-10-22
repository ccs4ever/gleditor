/**
 * @file sql_lens_test.cpp
 * @brief Browse-mode tests for SqlLens (design/sql-to-zigzag.md §3-§5):
 * schema projection, sightings vs subjects, the pk:/rid:/h: key ladder,
 * keyset/ordinal paging, NULL-vs-empty, epoch-stable keys, rewind, and a
 * VQL gate over the projected slice.
 */
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/sql_keys.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"

namespace {

namespace fs = std::filesystem;
using namespace zigzag;
using xanadu::MultiStoreCoordinator;
using xanadu::vql::VQLEngine;

/// A temp .db in WAL mode, populated through a writable connection.
struct FixtureDb {
  fs::path path;

  explicit FixtureDb() {
    path        = fs::temp_directory_path() /
                  ("sql-lens-test-" + std::to_string(::getpid()) + ".db");
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
    const char *ddl = "CREATE TABLE invoices("
                      "  id INTEGER PRIMARY KEY, customer TEXT NOT NULL,"
                      "  amount REAL, note TEXT);"
                      "INSERT INTO invoices VALUES"
                      "  (1,'alice',10.5,'one'),(2,'bob',20.25,'two'),"
                      "  (3,'carol',NULL,NULL),(4,'dan',1.0,''),"
                      "  (5,'eve',5.5,'five');"
                      "CREATE TABLE nopk(x TEXT, y INTEGER);"
                      "INSERT INTO nopk VALUES ('a',1),('b',2),('c',3);"
                      "CREATE VIEW v_inv AS SELECT id, customer FROM invoices;";
    EXPECT_EQ(sqlite3_exec(db, ddl, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close_v2(db);
  }

  /// Apply a write through a fresh RW connection (the lens's own handle
  /// stays read-only; WAL admits the concurrent writer).
  void write(std::string_view sql) {
    sqlite3 *db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.string().c_str(), &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, nullptr),
              SQLITE_OK);
    if (db == nullptr) {
      return;
    }
    std::string s(sql);
    EXPECT_EQ(sqlite3_exec(db, s.c_str(), nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close_v2(db);
  }

  ~FixtureDb() {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path.string() + "-wal", ec);
    fs::remove(path.string() + "-shm", ec);
  }
};

/// One projected source: arena + core + coordinator + session + engine.
struct ProjectedDb {
  FixtureDb db;
  SqlLens *lens{nullptr};
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coord{core};
  VQLEngine engine{coord};
  std::unique_ptr<ProjectionSession> session;

  explicit ProjectedDb(std::size_t pageSize = 64) {
    auto l = openSqliteLens(db.path.string(), pageSize);
    EXPECT_TRUE(l.has_value()) << l.error();
    if (!l) {
      return;
    }
    lens    = l->get();
    session = std::make_unique<ProjectionSession>(std::move(*l), core);
  }

  DimRef dim(std::string_view n) { return core.findOrMintDimension(n); }

  /// +d.contains-> firstborn.
  [[nodiscard]] OptionalCell child(CellRef c) {
    return core.arena().linked(c, dim("d.contains"), Posward);
  }
  /// +d.seq-> next sibling.
  [[nodiscard]] OptionalCell next(CellRef c) {
    return core.arena().linked(c, dim("d.seq"), Posward);
  }
  /// Every posward sibling starting at c (inclusive).
  std::vector<CellRef> siblings(CellRef c) {
    std::vector<CellRef> out{c};
    for (auto n = next(c); n.has_value(); n = next(*n)) {
      out.push_back(*n);
    }
    return out;
  }
  /// The attr text on a rank, or "".
  std::string attr(CellRef c, std::string_view rank) {
    const auto a = core.arena().linked(c, dim(rank), Posward);
    return a.has_value() ? core.arena().textOf(*a) : "";
  }
  /// The conn's child carrying d.role == @p role (query nodes share the
  /// sibling rank with schema tables).
  [[nodiscard]] OptionalCell childWithRole(std::string_view role) {
    const auto first = child(session->roots()[0]);
    if (!first.has_value()) {
      return std::nullopt;
    }
    for (const auto c : siblings(*first)) {
      if (attr(c, "d.role") == role) {
        return c;
      }
    }
    return std::nullopt;
  }
  /// Walk +d.subject-> from a sighting to its row.
  CellRef subjectOf(CellRef sight) {
    auto at = sight;
    while (attr(at, "d.role") == "sql.sight") {
      at = *core.arena().linked(at, dim("d.subject"), Posward);
    }
    return at;
  }
};

TEST(SqlLensTest, SchemaAndConnProjectAtRoots) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_EQ(p.session->roots().size(), 1u);
  const auto conn = p.session->roots()[0];
  EXPECT_EQ(p.attr(conn, "d.role"), "sql.conn");
  // The conn cell's firstborn is a schema table; all schema objects sit on
  // the shared sibling rank.
  const auto first = p.child(conn);
  ASSERT_TRUE(first.has_value());
  const auto kids = p.siblings(*first);
  std::vector<std::string> names;
  for (const auto c : kids) {
    names.push_back(p.core.arena().textOf(c));
  }
  EXPECT_NE(std::find(names.begin(), names.end(), "invoices"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "v_inv"), names.end());
  // A table cell's firstborn is its first column.
  for (const auto c : kids) {
    if (p.core.arena().textOf(c) == "invoices") {
      const auto col0 = p.child(c);
      ASSERT_TRUE(col0.has_value());
      EXPECT_EQ(p.attr(*col0, "d.role"), "sql.col");
      EXPECT_EQ(p.core.arena().textOf(*col0), "id");
      EXPECT_EQ(p.attr(*col0, "d.pk"), "1");
      const auto cols = p.siblings(*col0);
      EXPECT_EQ(cols.size(), 4u);
      EXPECT_EQ(p.attr(cols[3], "d.type"), "TEXT");
    }
  }
}

TEST(SqlLensTest, BrowseProjectsPagesSightsSubjectsValues) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();

  const auto conn  = p.session->roots()[0];
  const auto query = p.childWithRole("sql.query");
  ASSERT_TRUE(query.has_value());
  EXPECT_EQ(p.attr(*query, "d.qhash").size(), 64u);

  // query -> page0 -> sight0.
  const auto page = p.child(*query);
  ASSERT_TRUE(page.has_value());
  EXPECT_EQ(p.attr(*page, "d.role"), "sql.page");
  EXPECT_EQ(p.attr(*page, "d.state"), "live");
  EXPECT_EQ(p.attr(*page, "d.wincount"), "5");
  EXPECT_EQ(p.attr(*page, "d.winhi"), "k:i:5");
  EXPECT_TRUE(p.attr(*page, "d.winlo").empty());

  const auto sight0 = p.child(*page);
  ASSERT_TRUE(sight0.has_value());
  EXPECT_EQ(p.attr(*sight0, "d.role"), "sql.sight");
  EXPECT_EQ(p.attr(*sight0, "d.ordinal"), "0");
  EXPECT_EQ(p.siblings(*sight0).size(), 5u);

  // A sighting walks posward d.subject -> its row.
  const auto row = p.subjectOf(*sight0);
  EXPECT_EQ(p.attr(row, "d.role"), "sql.row");
  EXPECT_EQ(p.attr(row, "d.keystab"), "pk");
  const auto extkey = p.attr(row, "d.extkey");
  EXPECT_TRUE(extkey.find("pk:i:1") != std::string::npos) << extkey;
  // Identity is stable in ExternalKey space -- the cell's key IS the row's.
  const auto rowKey = p.session->keyFor(row);
  ASSERT_TRUE(rowKey.has_value());
  EXPECT_EQ(*rowKey, extkey);

  // The row's values chain on contains/seq; a value walks +d.ofcol -> col.
  const auto v0 = p.child(row);
  ASSERT_TRUE(v0.has_value());
  EXPECT_EQ(p.attr(*v0, "d.role"), "sql.val");
  auto colHead = *p.core.arena().linked(*v0, p.dim("d.ofcol"), Posward);
  while (p.attr(colHead, "d.role") == "sql.val") {
    colHead = *p.core.arena().linked(colHead, p.dim("d.ofcol"), Posward);
  }
  EXPECT_EQ(p.attr(colHead, "d.role"), "sql.col");

  // The subject sits in the ever-seen index under conn.
  EXPECT_TRUE(p.core.arena().rankContains(conn, row, p.dim("d.subjects")));
}

TEST(SqlLensTest, KeysetPagesAdvanceAndBindCorrectly) {
  ProjectedDb p(/*pageSize=*/2);
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();

  const auto query = p.childWithRole("sql.query");
  ASSERT_TRUE(query.has_value());
  const auto page0 = p.child(*query);
  ASSERT_TRUE(page0.has_value());
  EXPECT_EQ(p.attr(*page0, "d.wincount"), "2");
  const auto pages = p.siblings(*page0);
  ASSERT_EQ(pages.size(), 2u); // page0 + stub
  const auto stub = pages[1];
  EXPECT_EQ(p.attr(stub, "d.state"), "deferred");
  // The stub's key IS the page it becomes: p:k:i:2 -- the exclusive bound.
  const auto stubKey = p.session->keyFor(stub);
  ASSERT_TRUE(stubKey.has_value());
  EXPECT_TRUE(stubKey->find(":p:k:i:2") != std::string::npos) << *stubKey;

  ASSERT_TRUE(p.lens->materializePage(*stubKey).has_value());
  (void)p.session->refresh();

  const auto page1 = p.core.arena().linked(*page0, p.dim("d.seq"), Posward);
  ASSERT_TRUE(page1.has_value());
  EXPECT_EQ(p.attr(*page1, "d.state"), "live");
  EXPECT_EQ(p.attr(*page1, "d.wincount"), "2");
  EXPECT_EQ(p.attr(*page1, "d.winlo"), "k:i:2");
  // Rows continue at id 3 -- the keyset predicate bound correctly.
  const auto row = p.subjectOf(*p.child(*page1));
  EXPECT_TRUE(p.attr(row, "d.extkey").find("pk:i:3") != std::string::npos);
  // One more page completes the table; no further stub appears.
  const auto pages2 = p.siblings(*page0);
  ASSERT_EQ(pages2.size(), 3u); // page0, page1, stub2
  ASSERT_TRUE(
      p.lens->materializePage(*p.session->keyFor(pages2[2])).has_value());
  (void)p.session->refresh();
  const auto page2 = p.core.arena().linked(*page1, p.dim("d.seq"), Posward);
  ASSERT_TRUE(page2.has_value());
  EXPECT_EQ(p.attr(*page2, "d.wincount"), "1");
  const auto tail = p.siblings(*page2);
  EXPECT_EQ(tail.size(), 1u); // the horizon closed -- no new stub
}

TEST(SqlLensTest, NoPkTableGetsRidKeys) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("nopk").has_value());
  (void)p.session->refresh();
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  const auto row = p.subjectOf(*p.child(*p.child(*q)));
  EXPECT_EQ(p.attr(row, "d.keystab"), "rid");
  EXPECT_TRUE(p.attr(row, "d.extkey").find("rid:i:1") != std::string::npos);
}

TEST(SqlLensTest, ViewGetsOrdinalWindowsAndHashKeys) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("v_inv").has_value());
  (void)p.session->refresh();
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  const auto page = *p.child(*q);
  EXPECT_TRUE(p.session->keyFor(page)->find(":p:o:0") != std::string::npos);
  const auto row = p.subjectOf(*p.child(page));
  EXPECT_EQ(p.attr(row, "d.keystab"), "hash");
  EXPECT_TRUE(p.attr(row, "d.extkey").find(":h:") != std::string::npos);
}

TEST(SqlLensTest, NullCarriesDNullEmptyStringDoesNot) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  const auto sight0 = *p.child(*p.child(*q));
  bool sawNull = false, sawEmpty = false;
  for (const auto s : p.siblings(sight0)) {
    const auto row = p.subjectOf(s);
    for (auto v = p.child(row); v.has_value(); v = p.next(*v)) {
      if (p.attr(*v, "d.column") != "note") {
        continue;
      }
      const bool isNull = !p.attr(*v, "d.null").empty();
      if (isNull) {
        sawNull = true;
      } else if (p.core.arena().textOf(*v).empty()) {
        sawEmpty = true;
      }
    }
  }
  EXPECT_TRUE(sawNull);  // carol's NULL note carries d.null
  EXPECT_TRUE(sawEmpty); // dan's '' does not
}

TEST(SqlLensTest, ScalarValuesAreTypedCells) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  const auto row = p.subjectOf(*p.child(*p.child(*q)));
  bool sawInt = false, sawReal = false;
  for (auto v = p.child(row); v.has_value(); v = p.next(*v)) {
    if (p.attr(*v, "d.column") == "id") {
      EXPECT_EQ(p.core.arena().asInt64(*v).value_or(0), 1);
      sawInt = true;
    }
    if (p.attr(*v, "d.column") == "amount") {
      EXPECT_DOUBLE_EQ(p.core.arena().asDouble(*v).value_or(0.0), 10.5);
      sawReal = true;
    }
  }
  EXPECT_TRUE(sawInt && sawReal);
}

TEST(SqlLensTest, RewindReBrowseKeepsSubjectKeysRestatesValues) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  const auto cp = p.session->checkpoint();
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  p.db.write("UPDATE invoices SET note='one-v2' WHERE id=1");
  ASSERT_TRUE(p.session->rewind(cp).has_value());
  // Re-browse: the pk: key is epoch-stable, so the same subject key
  // returns carrying the new value.
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  const auto row = p.subjectOf(*p.child(*p.child(*q)));
  EXPECT_TRUE(p.attr(row, "d.extkey").find("pk:i:1") != std::string::npos);
  bool sawNote = false;
  for (auto v = p.child(row); v.has_value(); v = p.next(*v)) {
    if (p.attr(*v, "d.column") == "note") {
      EXPECT_EQ(p.core.arena().textOf(*v), "one-v2");
      sawNote = true;
    }
  }
  EXPECT_TRUE(sawNote);
}

TEST(SqlLensTest, RewindDropsMaterializedPagesAndBuffer) {
  ProjectedDb p(/*pageSize=*/2);
  ASSERT_NE(p.lens, nullptr);
  const auto cp = p.session->checkpoint();
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  // Materialize page 1 as well, then rewind the whole browse.
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  const auto stub = *p.next(*p.child(*q));
  ASSERT_TRUE(p.lens->materializePage(*p.session->keyFor(stub)).has_value());
  (void)p.session->refresh();
  ASSERT_TRUE(p.session->rewind(cp).has_value());
  // The query and its pages are forgotten from the identity map.
  EXPECT_FALSE(p.session->keyFor(*q).has_value());
  EXPECT_EQ(p.session->roots().size(), 1u);
  // And the lens's buffer dropped them: re-browsing mints fresh pages.
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  const auto q2 = p.childWithRole("sql.query");
  ASSERT_TRUE(q2.has_value());
  EXPECT_EQ(p.attr(*p.child(*q2), "d.wincount"), "2");
}

TEST(SqlLensTest, VqlTraversesSqlProjection) {
  ProjectedDb p;
  ASSERT_NE(p.lens, nullptr);
  ASSERT_TRUE(p.lens->browse("invoices").has_value());
  (void)p.session->refresh();
  const auto q = p.childWithRole("sql.query");
  ASSERT_TRUE(q.has_value());
  p.engine.setVariable("q", *q);
  // query -> page -> sight: canonical ranks carry the whole way.
  const auto roles = p.engine.execute("$q/d.contains/d.contains/d.role");
  ASSERT_EQ(roles.size(), 1u);
  EXPECT_EQ(p.core.arena().textOf(roles[0]), "sql.sight");
  // sight -> subject on the membership rank.
  const auto rowRoles =
      p.engine.execute("$q/d.contains/d.contains/d.subject/d.role");
  ASSERT_EQ(rowRoles.size(), 1u);
  EXPECT_EQ(p.core.arena().textOf(rowRoles[0]), "sql.row");
}

} // namespace
