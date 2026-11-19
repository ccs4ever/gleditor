/**
 * @file arena_release_epoch_test.cpp
 * @brief ArenaManifold::releaseEpoch / epochOfCell: a cell number held as a
 * bare Int64 is validated by the epoch the cell was minted in, because
 * release() truncates cells and then hands the same numbers out again.
 */
#include <gtest/gtest.h>

#include <cstdint>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;

TEST(ArenaReleaseEpochTest, FreshCellsAreBornInTheFirstEpoch) {
  ArenaManifold arena;
  const auto a = arena.makeCell("a");
  EXPECT_EQ(arena.releaseEpoch(), 0u);
  EXPECT_EQ(arena.epochOfCell(a), std::optional<std::uint64_t>{0});
}

TEST(ArenaReleaseEpochTest, ANumberNoCellHoldsHasNoEpoch) {
  ArenaManifold arena;
  const auto a = arena.makeCell("a");
  EXPECT_EQ(arena.epochOfCell(ArenaManifold::refOf(1)), std::nullopt);
  EXPECT_NE(a, ArenaManifold::refOf(1));
}

TEST(ArenaReleaseEpochTest, ReusedNumberAnswersALaterEpochThanTheOldCell) {
  ArenaManifold arena;
  const auto below = arena.makeCell("below");
  const auto m     = arena.mark();
  const auto gone  = arena.makeCell("gone");
  const auto held  = arena.epochOfCell(gone);
  ASSERT_TRUE(held.has_value());
  ASSERT_TRUE(arena.release(m));
  EXPECT_EQ(arena.epochOfCell(gone), std::nullopt);
  const auto reused = arena.makeCell("reused");
  ASSERT_EQ(reused, gone);
  EXPECT_NE(arena.epochOfCell(reused), held);
  EXPECT_GT(*arena.epochOfCell(reused), *held);
  EXPECT_EQ(arena.epochOfCell(below), std::optional<std::uint64_t>{0});
}

TEST(ArenaReleaseEpochTest, ReleaseThatTruncatesNothingDoesNotAdvance) {
  ArenaManifold arena;
  arena.makeCell("a");
  const auto m = arena.mark();
  ASSERT_TRUE(arena.release(m));
  EXPECT_EQ(arena.releaseEpoch(), 0u);
}

TEST(ArenaReleaseEpochTest, LiveCellKeepsItsEpochAcrossReleasesAboveIt) {
  ArenaManifold arena;
  const auto m1    = arena.mark();
  const auto first = arena.makeCell("first");
  const auto m2    = arena.mark();
  arena.makeCell("scratch");
  ASSERT_TRUE(arena.release(m2));
  const auto second = arena.makeCell("second");
  const auto m3     = arena.mark();
  arena.makeCell("scratch");
  ASSERT_TRUE(arena.release(m3));
  EXPECT_EQ(arena.epochOfCell(first), std::optional<std::uint64_t>{0});
  EXPECT_EQ(arena.epochOfCell(second), std::optional<std::uint64_t>{1});
  EXPECT_EQ(arena.releaseEpoch(), 2u);
  const auto third = arena.makeCell("third");
  EXPECT_EQ(arena.epochOfCell(third), std::optional<std::uint64_t>{2});
  ASSERT_TRUE(arena.release(m1));
  EXPECT_EQ(arena.releaseEpoch(), 3u);
  EXPECT_EQ(arena.epochOfCell(first), std::nullopt);
  EXPECT_EQ(arena.epochOfCell(arena.makeCell("again")),
            std::optional<std::uint64_t>{3});
}

TEST(ArenaReleaseEpochTest, DiscardKeepsCellsAndEpoch) {
  ArenaManifold arena;
  const auto m    = arena.mark();
  const auto kept = arena.makeCell("kept");
  ASSERT_TRUE(arena.discard(m));
  EXPECT_EQ(arena.releaseEpoch(), 0u);
  EXPECT_EQ(arena.epochOfCell(kept), std::optional<std::uint64_t>{0});
}

TEST(ArenaReleaseEpochTest, CompactionMovesRunsNotCells) {
  ArenaManifold arena;
  const auto a = arena.makeCell("a");
  const auto m = arena.mark();
  arena.makeCell("scratch");
  ASSERT_TRUE(arena.release(m));
  const auto b = arena.makeCell("b");
  ASSERT_TRUE(arena.compact());
  EXPECT_EQ(arena.epochOfCell(a), std::optional<std::uint64_t>{0});
  EXPECT_EQ(arena.epochOfCell(b), std::optional<std::uint64_t>{1});
  EXPECT_EQ(arena.releaseEpoch(), 1u);
}

TEST(ArenaReleaseEpochTest, HydratingAPrefixRenumbersEveryCell) {
  ArenaManifold from;
  from.makeCell("a");
  const auto m        = from.mark();
  const auto snapshot = from.exportPrefix(m);
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(from.release(m));

  ArenaManifold into;
  const auto before = into.makeCell("x");
  ASSERT_EQ(into.epochOfCell(before), std::optional<std::uint64_t>{0});
  ASSERT_TRUE(into.hydratePrefix(*snapshot));
  EXPECT_NE(into.epochOfCell(before), std::optional<std::uint64_t>{0});
  EXPECT_EQ(into.epochOfCell(before),
            std::optional<std::uint64_t>{into.releaseEpoch()});
}

TEST(ArenaReleaseEpochTest, ACopyCarriesTheWitnessesOfTheMomentItWasTaken) {
  ArenaManifold arena;
  const auto m = arena.mark();
  arena.makeCell("scratch");
  ASSERT_TRUE(arena.release(m));
  const auto a             = arena.makeCell("a");
  const ArenaManifold copy = arena;
  EXPECT_EQ(copy.epochOfCell(a), arena.epochOfCell(a));
  EXPECT_EQ(copy.releaseEpoch(), arena.releaseEpoch());
}

TEST(ArenaReleaseEpochTest, NestedReleasesPopTheRunsAboveTheirCut) {
  ArenaManifold arena;
  const auto m1 = arena.mark();
  const auto a  = arena.makeCell("a");
  const auto m2 = arena.mark();
  const auto b  = arena.makeCell("b");
  ASSERT_TRUE(arena.release(m2));
  const auto c = arena.makeCell("c");
  ASSERT_EQ(c, b);
  ASSERT_EQ(arena.epochOfCell(c), std::optional<std::uint64_t>{1});
  ASSERT_TRUE(arena.release(m1));
  EXPECT_EQ(arena.epochOfCell(a), std::nullopt);
  EXPECT_EQ(arena.epochOfCell(c), std::nullopt);
  const auto d = arena.makeCell("d");
  const auto e = arena.makeCell("e");
  ASSERT_EQ(d, a);
  ASSERT_EQ(e, b);
  // The epoch-1 run that described c died with it: e is not epoch 1.
  EXPECT_EQ(arena.epochOfCell(d), std::optional<std::uint64_t>{2});
  EXPECT_EQ(arena.epochOfCell(e), std::optional<std::uint64_t>{2});
}

TEST(ArenaReleaseEpochTest, AssignmentMovesTheEpochStrictlyForward) {
  ArenaManifold arena;
  const auto m = arena.mark();
  arena.makeCell("scratch");
  ASSERT_TRUE(arena.release(m));
  const auto witnessed = arena.makeCell("witnessed");
  const auto witness   = arena.epochOfCell(witnessed);
  const auto before    = arena.releaseEpoch();

  arena = ArenaManifold{};
  EXPECT_GT(arena.releaseEpoch(), before);
  const auto fresh = arena.makeCell("fresh");
  ASSERT_EQ(fresh, witnessed);
  EXPECT_NE(arena.epochOfCell(fresh), witness);

  const auto afterMove = arena.releaseEpoch();
  const ArenaManifold other;
  arena = other;
  EXPECT_GT(arena.releaseEpoch(), afterMove);
  EXPECT_GT(arena.releaseEpoch(), other.releaseEpoch());
}

TEST(ArenaReleaseEpochTest,
     ASurvivingWitnessFromTheSourceArenaDoesNotValidate) {
  ArenaManifold source;
  const auto m = source.mark();
  source.makeCell("scratch");
  ASSERT_TRUE(source.release(m));
  source.makeCell("x");
  const auto witness = source.epochOfCell(ArenaManifold::refOf(0));
  ArenaManifold target;
  target.makeCell("y");
  target = source;
  EXPECT_NE(target.epochOfCell(ArenaManifold::refOf(0)), witness);
}

TEST(ArenaReleaseEpochTest, ABaseCellAndItsShadowShareTheFloorEpoch) {
  auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store{scroll};
  const auto head = store.sliceGenesis(xanadu::MicroversionId{});
  const auto dim  = store.makeDimension(head, "d.c").dim;
  const auto home = store.homeCell().value();
  const auto base = store.rebuildManifold(store.latest());

  ArenaManifold arena{&base};
  const auto floor = arena.releaseEpoch();
  ASSERT_EQ(arena.epochOfCell(home), std::optional<std::uint64_t>{floor});
  const auto m     = arena.mark();
  const auto other = arena.makeCell("scratch");
  // Writing the base cell shadows it; the shadow is the same cell.
  ASSERT_TRUE(arena.link(home, dim, zigzag::DimVector::POS, other));
  EXPECT_EQ(arena.epochOfCell(home), std::optional<std::uint64_t>{floor});
  ASSERT_TRUE(arena.release(m));
  EXPECT_GT(arena.releaseEpoch(), floor);
  EXPECT_EQ(arena.epochOfCell(home), std::optional<std::uint64_t>{floor});
}

} // namespace
