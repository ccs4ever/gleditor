/**
 * @file cell_deletion_test.cpp
 * @brief The one cell-deletion algorithm (P7): the plan, both modes, the
 * arena's expression of it under marks and over a base, and the cursor
 * move-off rule.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_deletion.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;
using zigzag::ArenaManifold;
using zigzag::ArenaRefusal;
using zigzag::CellRef;
using zigzag::DeletionMode;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::LinkEdit;
using zigzag::Neighbourhood;
using zigzag::noCell;
using zigzag::RankNeighbours;

struct Arena {
  ArenaManifold m;
  DimRef x{noCell};
  DimRef y{noCell};
  DimRef clone{noCell};

  Arena() {
    x     = m.makeCell("d.x");
    y     = m.makeCell("d.y");
    clone = m.makeCell("d.clone");
  }
  CellRef cell(const std::string_view text) { return m.makeCell(text); }
  CellRef chain(const DimRef dim, const std::vector<CellRef> &cells) {
    EXPECT_TRUE(m.linkChain(cells, dim));
    return cells.front();
  }
  auto inc(const CellRef ref) { return m.incarnationOf(ref).value(); }
};

// -- the plan ---------------------------------------------------------------

TEST(CellDeletionPlanTest,
     repairIsOneWritePerDimensionAndBrokenClearsBothSides) {
  const RankNeighbours x{.dim = 10, .negward = 1, .posward = 2};
  const RankNeighbours y{.dim = 11, .negward = std::nullopt, .posward = 3};
  const Neighbourhood hood{.cell = 5, .ranks = {x, y}};
  std::vector<LinkEdit> repair;
  zigzag::planDeletion(5, hood, DeletionMode::Repair,
                       [&](const LinkEdit &e) { repair.push_back(e); });
  EXPECT_THAT(repair,
              testing::ElementsAre(
                  LinkEdit{1, 10, DimVector::POS, std::optional<CellRef>{2}},
                  LinkEdit{5, 11, DimVector::POS, std::nullopt}));
  std::vector<LinkEdit> broken;
  zigzag::planDeletion(5, hood, DeletionMode::LeaveBroken,
                       [&](const LinkEdit &e) { broken.push_back(e); });
  EXPECT_THAT(broken, testing::ElementsAre(
                          LinkEdit{5, 10, DimVector::POS, std::nullopt},
                          LinkEdit{5, 10, DimVector::NEG, std::nullopt},
                          LinkEdit{5, 11, DimVector::POS, std::nullopt}));
}

TEST(CellDeletionPlanTest, aRankOfTwoIsClearedNotJoinedToItself) {
  const Neighbourhood hood{
      .cell  = 5,
      .ranks = {RankNeighbours{.dim = 10, .negward = 1, .posward = 1}}};
  std::vector<LinkEdit> edits;
  zigzag::planDeletion(5, hood, DeletionMode::Repair,
                       [&](const LinkEdit &e) { edits.push_back(e); });
  EXPECT_THAT(edits, testing::ElementsAre(
                         LinkEdit{5, 10, DimVector::POS, std::nullopt},
                         LinkEdit{5, 10, DimVector::NEG, std::nullopt}));
}

// -- the arena --------------------------------------------------------------

TEST(ArenaDeleteCellTest, leaveBrokenCutsEveryConnectionAndFreesTheCell) {
  Arena a;
  const auto p = a.cell("p"), q = a.cell("q"), r = a.cell("r"), s = a.cell("s");
  a.chain(a.x, {p, q, r});
  a.chain(a.y, {s, q});
  const auto done = a.m.deleteCell(q, a.inc(q), DeletionMode::LeaveBroken);
  ASSERT_TRUE(done);
  EXPECT_TRUE(done->freed);
  EXPECT_FALSE(a.m.contains(q));
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::POS), std::nullopt);
  EXPECT_EQ(a.m.linked(r, a.x, DimVector::NEG), std::nullopt);
  EXPECT_EQ(a.m.linked(s, a.y, DimVector::POS), std::nullopt);
  EXPECT_EQ(done->broken.ranks.size(), 2U);
  EXPECT_EQ(done->broken.along(a.x),
            (RankNeighbours{.dim = a.x, .negward = p, .posward = r}));
}

TEST(ArenaDeleteCellTest, repairJoinsEachDimensionsNeighboursAndFreesTheCell) {
  Arena a;
  const auto p = a.cell("p"), q = a.cell("q"), r = a.cell("r"), s = a.cell("s");
  a.chain(a.x, {p, q, r});
  a.chain(a.y, {s, q});
  const auto done = a.m.deleteCell(q, a.inc(q), DeletionMode::Repair);
  ASSERT_TRUE(done);
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::POS), r);
  EXPECT_EQ(a.m.linked(r, a.x, DimVector::NEG), p);
  EXPECT_EQ(a.m.linked(s, a.y, DimVector::POS), std::nullopt);
  EXPECT_TRUE(done->freed);
  // The freed slot is reusable and the stale handle is refused.
  EXPECT_EQ(a.m.deleteCell(q, 1, DeletionMode::Repair).error(),
            ArenaRefusal::UnknownCell);
}

TEST(ArenaDeleteCellTest, aCellWithNoLinksIsJustFreed) {
  Arena a;
  const auto lone = a.cell("lone");
  const auto live = a.m.liveCells();
  const auto done = a.m.deleteCell(lone, a.inc(lone), DeletionMode::Repair);
  ASSERT_TRUE(done);
  EXPECT_TRUE(done->broken.isolated());
  EXPECT_TRUE(done->freed);
  EXPECT_EQ(a.m.liveCells(), live - 1);
}

TEST(ArenaDeleteCellTest, aRankOfTwoLeavesTheSurvivorUnlinked) {
  Arena a;
  const auto p = a.cell("p"), q = a.cell("q");
  a.chain(a.x, {p, q});
  ASSERT_TRUE(a.m.deleteCell(q, a.inc(q), DeletionMode::Repair));
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::POS), std::nullopt);
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::NEG), std::nullopt);
}

TEST(ArenaDeleteCellTest, aCloneMasterAndAClone) {
  Arena a;
  const auto master = a.cell("m"), v1 = a.cell("v1"), v2 = a.cell("v2");
  a.chain(a.clone, {master, v1, v2});
  // Deleting a view repairs the rank; the master still answers.
  ASSERT_TRUE(a.m.deleteCell(v1, a.inc(v1), DeletionMode::Repair));
  EXPECT_EQ(a.m.cloneMaster(v2, a.clone), master);
  // Deleting the master with Repair promotes the first remaining view.
  ASSERT_TRUE(a.m.deleteCell(
      master, a.inc(master), DeletionMode::Repair,
      zigzag::CloneGuard{.dim = a.clone, .promoteFirstView = true}));
  EXPECT_EQ(a.m.cloneMaster(v2, a.clone), v2);
  EXPECT_EQ(a.m.linked(v2, a.clone, DimVector::NEG), std::nullopt);
}

TEST(ArenaDeleteCellTest,
     aCloneMasterWithViewsIsRefusedUnlessPromotionIsChosen) {
  // M7-VQL gate r2, vortex minor 5: the first view would become the master
  // without the master's content, so the guarded deletion refuses by name
  // and writes nothing, in either mode.
  for (const auto mode : {DeletionMode::Repair, DeletionMode::LeaveBroken}) {
    Arena a;
    const auto master = a.cell("m"), v1 = a.cell("v1");
    a.chain(a.clone, {master, v1});
    const auto guard   = zigzag::CloneGuard{.dim = a.clone};
    const auto refused = a.m.deleteCell(master, a.inc(master), mode, guard);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), ArenaRefusal::HoldsViews);
    EXPECT_TRUE(a.m.contains(master));
    EXPECT_EQ(a.m.linked(master, a.clone, DimVector::POS), v1);
    // A view, and a master with no views, are never refused.
    EXPECT_TRUE(a.m.deleteCell(v1, a.inc(v1), mode, guard));
    EXPECT_TRUE(a.m.deleteCell(master, a.inc(master), mode, guard));
  }
}

TEST(ArenaDeleteCellTest, deletionUnderAMarkIsUndoneByRelease) {
  Arena a;
  const auto p = a.cell("p"), q = a.cell("q"), r = a.cell("r");
  a.chain(a.x, {p, q, r});
  const auto live = a.m.liveCells();
  const auto mark = a.m.mark();
  ASSERT_TRUE(a.m.deleteCell(q, a.inc(q), DeletionMode::Repair));
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::POS), r);
  ASSERT_TRUE(a.m.release(mark));
  EXPECT_TRUE(a.m.contains(q));
  EXPECT_EQ(a.m.liveCells(), live);
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::POS), q);
  EXPECT_EQ(a.m.linked(q, a.x, DimVector::POS), r);
  EXPECT_EQ(a.m.linked(r, a.x, DimVector::NEG), q);
}

TEST(ArenaDeleteCellTest,
     aShadowedBaseCellIsDetachedNotFreedAndBaseIsUntouched) {
  Store store;
  auto at           = store.sliceGenesis(MicroversionId{});
  const auto minted = store.makeDimension(at, "d.step");
  at                = minted.version;
  at                = store.makeCell(at, "first");
  const auto first  = store.cellRefOf(at).value();
  at                = store.makeCell(at, "second");
  const auto second = store.cellRefOf(at).value();
  at                = store.makeCell(at, "third");
  const auto third  = store.cellRefOf(at).value();
  at = store.setLink(at, first, minted.dim, DimVector::POS, second);
  at = store.setLink(at, second, minted.dim, DimVector::POS, third);
  const auto base = store.rebuildManifold(at);
  ArenaManifold arena(&base, &store);
  const auto mark = arena.mark();

  // A base cell has no incarnation of its own until a write shadows it.
  ASSERT_TRUE(arena.link(second, minted.dim, DimVector::POS, third));
  const auto done = arena.deleteCell(
      second, arena.incarnationOf(second).value(), DeletionMode::Repair);
  ASSERT_TRUE(done);
  EXPECT_FALSE(done->freed);
  EXPECT_EQ(arena.linked(first, minted.dim, DimVector::POS), third);
  EXPECT_EQ(arena.linked(second, minted.dim, DimVector::POS), std::nullopt);
  EXPECT_EQ(base.linked(first, minted.dim, DimVector::POS), second)
      << "the base manifold is never written";
  ASSERT_TRUE(arena.release(mark));
  EXPECT_EQ(arena.linked(first, minted.dim, DimVector::POS), second);
}

TEST(ArenaDeleteCellTest, theHomeAndNamedCellsAreRefusedBeforeAnyWrite) {
  Store store;
  const auto genesis = store.sliceGenesis(MicroversionId{});
  const auto base    = store.rebuildManifold(genesis);
  ArenaManifold arena(&base, &store);
  const auto home = base.home().value();
  const auto dims = arena.dimensionNamed("d.dims").value();
  ASSERT_TRUE(arena.link(home, dims, DimVector::POS, std::nullopt));
  EXPECT_EQ(arena
                .deleteCell(home, arena.incarnationOf(home).value(),
                            DeletionMode::Repair)
                .error(),
            ArenaRefusal::NotReleasable);
  Arena a;
  const auto dim = a.m.ensureDimension("d.named");
  EXPECT_EQ(a.m.deleteCell(dim, a.inc(dim), DeletionMode::Repair).error(),
            ArenaRefusal::NotReleasable);
}

TEST(ArenaDeleteCellTest, detachFromRankLeavesOtherRanksAlone) {
  Arena a;
  const auto p = a.cell("p"), q = a.cell("q"), r = a.cell("r"), s = a.cell("s");
  a.chain(a.x, {p, q, r});
  a.chain(a.y, {s, q});
  const auto ward = a.m.detachFromRank(q, a.x, DeletionMode::Repair);
  ASSERT_TRUE(ward);
  EXPECT_EQ(ward->posward, r);
  EXPECT_EQ(a.m.linked(p, a.x, DimVector::POS), r);
  EXPECT_EQ(a.m.linked(q, a.y, DimVector::NEG), s);
  EXPECT_EQ(a.m.linked(q, a.x, DimVector::POS), std::nullopt);
}

// -- the cursor rule --------------------------------------------------------

TEST(CursorMoveOffTest, theFirstBoundDimensionWinsPosOverNeg) {
  const Neighbourhood hood{.cell  = 9,
                           .ranks = {{.dim = 20, .negward = 1, .posward = 2},
                                     {.dim = 21, .negward = 3, .posward = 4}}};
  const std::vector<DimRef> bound{21, 20};
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, 100), 4U);
  const std::vector<DimRef> reversed{20, 21};
  EXPECT_EQ(zigzag::cursorMoveOff(hood, reversed, 100), 2U);
}

TEST(CursorMoveOffTest, negwardIsTheFallbackWhenNothingIsPosward) {
  const Neighbourhood hood{
      .cell = 9, .ranks = {{.dim = 20, .negward = 1, .posward = std::nullopt}}};
  const std::vector<DimRef> bound{20};
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, 100), 1U);
}

TEST(CursorMoveOffTest, unboundDimensionsAreSkippedThenUsedThenHome) {
  const Neighbourhood hood{.cell  = 9,
                           .ranks = {{.dim = 30, .negward = 7, .posward = 8}}};
  const std::vector<DimRef> bound{20, 21};
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, 100), 8U)
      << "no bound dimension connects, so the remaining connections are used";
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, 100,
                                  [](DimRef, CellRef) { return false; }),
            100U)
      << "nothing eligible lands on home";
}

TEST(CursorMoveOffTest, anIsolatedCellSendsTheCursorHome) {
  const Neighbourhood hood{.cell = 9, .ranks = {}};
  const std::vector<DimRef> bound{20};
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, 100), 100U);
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, std::nullopt), std::nullopt);
}

TEST(CursorMoveOffTest, aCellLinkedToItselfIsNeverTheLanding) {
  const Neighbourhood hood{.cell  = 9,
                           .ranks = {{.dim = 20, .negward = 9, .posward = 9}}};
  const std::vector<DimRef> bound{20};
  EXPECT_EQ(zigzag::cursorMoveOff(hood, bound, 100), 100U);
}

} // namespace
