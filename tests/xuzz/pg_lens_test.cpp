/**
 * @file pg_lens_test.cpp
 * @brief PgBackend + SqlLens over a live PostgreSQL -- env-gated on
 *        GLEDITOR_PG_TEST_CONNINFO; without a server the suite skips.
 *
 *        A throwaway cluster is enough:
 *          initdb -D $T/data -U test -A trust
 *          pg_ctl -D $T/data -o "-k $T -F -p 54399" -w start
 *          psql "host=$T port=54399 user=test dbname=postgres" -c \
 *            'CREATE TABLE invoices(id int PRIMARY KEY, customer text,
 *             amount real, note text); INSERT INTO invoices VALUES
 *             (1,'''a''',10.5,'''one'''),(2,'''b''',20.25,'''two'''),
 *             (3,'''c''',30.0,NULL);
 *            CREATE VIEW v_inv AS SELECT id, customer FROM invoices;'
 *          GLEDITOR_PG_TEST_CONNINFO="host=$T port=54399 user=test
 *            dbname=postgres" ./build/xuzz_test --gtest_filter=Pg*
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/pg_backend.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::ExternalKey;
using zigzag::GenericDim;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::SqlLens;
using zigzag::SqlValue;
using zigzag::vortex::VortexCore;

/// The conninfo, or {} when no server is provisioned -- the suite
/// skips rather than failing on a host without postgres.
[[nodiscard]] std::string pgConninfo() {
  const char *env = std::getenv("GLEDITOR_PG_TEST_CONNINFO");
  return env != nullptr ? std::string{env} : std::string{};
}

struct PgDb {
  std::unique_ptr<zigzag::SqlBackend> be;
  /// Stub build (no GLEDITOR_HAVE_PG): openPgBackend refuses by design.
  bool stub{false};
  explicit PgDb() {
    auto b = zigzag::openPgBackend(pgConninfo());
    if (!b && b.error().find("no libpq") != std::string::npos) {
      stub = true;
      return;
    }
    EXPECT_TRUE(b.has_value()) << b.error();
    if (b) {
      be = std::move(*b);
    }
  }
  /// Skip this test unless a real backend is present.
  [[nodiscard]] bool require() const { return be != nullptr; }
};

TEST(PgBackendTest, SchemaEnumeratesBaseTablesAndViews) {
  if (pgConninfo().empty()) {
    GTEST_SKIP() << "GLEDITOR_PG_TEST_CONNINFO unset";
  }
  PgDb db;
  if (db.stub) {
    GTEST_SKIP() << "stub build -- no libpq";
  }
  ASSERT_NE(db.be, nullptr);
  const auto schema = db.be->schema();
  ASSERT_TRUE(schema.has_value()) << schema.error();
  bool invoices = false, vinv = false;
  for (const auto &t : *schema) {
    if (t.name == "invoices") {
      invoices = true;
      EXPECT_FALSE(t.isView);
      // The PK rung: invoices.id is declared.
      bool pk = false;
      for (const auto &c : t.columns) {
        if (c.name == "id" && c.keyColumn && c.keyPosition == 1) {
          pk = true;
        }
      }
      EXPECT_TRUE(pk) << "invoices.id must be keyColumn at position 1";
    }
    if (t.name == "v_inv") {
      vinv = true;
      EXPECT_TRUE(t.isView);
    }
  }
  EXPECT_TRUE(invoices);
  EXPECT_TRUE(vinv);
}

TEST(PgBackendTest, PrepareStreamsRowsInSingleRowMode) {
  if (pgConninfo().empty()) {
    GTEST_SKIP() << "GLEDITOR_PG_TEST_CONNINFO unset";
  }
  PgDb db;
  if (db.stub) {
    GTEST_SKIP() << "stub build -- no libpq";
  }
  auto cur = db.be->prepare("SELECT id, customer, amount, note "
                            "FROM invoices ORDER BY id",
                            {});
  ASSERT_TRUE(cur.has_value()) << cur.error();
  const auto rows = (*cur)->fetchNext(10);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 3u);
  EXPECT_EQ(std::get<std::int64_t>((*rows)[0][0]), 1);
  EXPECT_EQ(std::get<std::string>((*rows)[0][1]), "a");
  EXPECT_NEAR(std::get<double>((*rows)[0][2]), 10.5, 1e-9);
  EXPECT_TRUE(std::holds_alternative<std::monostate>((*rows)[2][3]));
}

TEST(PgBackendTest, QueryModeUnderTheRealBackend) {
  if (pgConninfo().empty()) {
    GTEST_SKIP() << "GLEDITOR_PG_TEST_CONNINFO unset";
  }
  PgDb db;
  if (db.stub) {
    GTEST_SKIP() << "stub build -- no libpq";
  }
  ArenaManifold arena;
  VortexCore core{arena};
  auto lens =
      std::make_unique<SqlLens>(std::move(db.be), "pgt", "pg", "testdb");
  zigzag::SqlLens *l = lens.get();
  ASSERT_TRUE(l->query("SELECT id, customer FROM invoices ORDER BY id", {})
                  .has_value());
  ProjectionSession session(std::move(lens), core);
  // q:-scoped rows materialized through single-row mode: query -> page
  // -> sightings on d.sighting/d.seq.
  int rowCells    = 0;
  const auto conn = session.roots()[0];
  auto qChild =
      arena.linked(conn, core.findOrMintDimension("d.contains"), Posward);
  // Find the query node (d.role sql.query) among conn children.
  while (qChild.has_value()) {
    auto role =
        arena.linked(*qChild, core.findOrMintDimension("d.role"), Posward);
    if (role.has_value() && arena.textOf(*role) == "sql.query") {
      break;
    }
    qChild = arena.linked(*qChild, core.findOrMintDimension("d.seq"), Posward);
  }
  ASSERT_TRUE(qChild.has_value());
  const auto page =
      arena.linked(*qChild, core.findOrMintDimension("d.contains"), Posward);
  ASSERT_TRUE(page.has_value());
  // Page -> sightings on d.contains; sightings chain on d.seq.
  for (auto s =
           arena.linked(*page, core.findOrMintDimension("d.contains"), Posward);
       s.has_value();
       s = arena.linked(*s, core.findOrMintDimension("d.seq"), Posward)) {
    ++rowCells;
  }
  EXPECT_EQ(rowCells, 3) << "3 invoice rows should project as sightings";
}

TEST(PgBackendTest, ReadOnlyCheckRefusesWrites) {
  if (pgConninfo().empty()) {
    GTEST_SKIP() << "GLEDITOR_PG_TEST_CONNINFO unset";
  }
  PgDb db;
  if (db.stub) {
    GTEST_SKIP() << "stub build -- no libpq";
  }
  EXPECT_FALSE(db.be->prepare("UPDATE invoices SET amount=0", {}).has_value());
  EXPECT_FALSE(db.be->prepare("DROP TABLE invoices", {}).has_value());
  // And a data-modifying CTE is caught by the keyword gate's WITH rule.
  EXPECT_FALSE(
      db.be
          ->prepare(
              "WITH d AS (DELETE FROM invoices RETURNING *) SELECT * FROM d",
              {})
          .has_value());
}

TEST(PgBackendTest, ReplayUnderSnapshot) {
  if (pgConninfo().empty()) {
    GTEST_SKIP() << "GLEDITOR_PG_TEST_CONNINFO unset";
  }
  PgDb db;
  if (db.stub) {
    GTEST_SKIP() << "stub build -- no libpq";
  }
  auto snap = db.be->snapshot();
  ASSERT_TRUE(snap.has_value()) << snap.error();
  auto cur = db.be->replayWindow("SELECT id FROM invoices ORDER BY id", {}, {},
                                 {}, 1, 2);
  ASSERT_TRUE(cur.has_value()) << cur.error();
  const auto rows = (*cur)->fetchNext(4);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ(std::get<std::int64_t>((*rows)[0][0]), 2);
}

} // namespace
