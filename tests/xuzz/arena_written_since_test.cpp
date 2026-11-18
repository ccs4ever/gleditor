/**
 * @file arena_written_since_test.cpp
 * @brief ArenaManifold::wroteOlderCellSince says whether release() of a mark
 * would undo a write to a cell that existed when the mark was taken, which is
 * what lets a caller keep a run's effects and drop only its scratch cells.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::DimVector;

struct Fixture {
  ArenaManifold arena;
  zigzag::CellRef older{arena.makeScalarCell(std::int64_t{1})};
  zigzag::CellRef other{arena.makeScalarCell(std::int64_t{2})};
  zigzag::DimRef dim{arena.makeCell()};
};

TEST(ArenaWrittenSinceTest, NothingWrittenIsFalse) {
  Fixture f;
  const auto m = f.arena.mark();
  EXPECT_FALSE(f.arena.wroteOlderCellSince(m));
  ASSERT_TRUE(f.arena.release(m));
}

TEST(ArenaWrittenSinceTest, CellsMintedUnderTheMarkDoNotCount) {
  Fixture f;
  const auto m = f.arena.mark();
  const auto a = f.arena.makeCell(std::string_view{"a"});
  const auto b = f.arena.makeScalarCell(std::int64_t{3});
  zigzag::expectWritten(f.arena.link(a, f.dim, DimVector::POS, b));
  zigzag::expectWritten(f.arena.setContent(a, f.arena.contentOf(a)));
  EXPECT_FALSE(f.arena.wroteOlderCellSince(m));
  ASSERT_TRUE(f.arena.release(m));
}

TEST(ArenaWrittenSinceTest, ALinkOrValueOrContentWriteToAnOlderCellCounts) {
  {
    Fixture f;
    const auto m = f.arena.mark();
    zigzag::expectWritten(
        f.arena.link(f.older, f.dim, DimVector::POS, f.other));
    EXPECT_TRUE(f.arena.wroteOlderCellSince(m));
    ASSERT_TRUE(f.arena.release(m));
  }
  {
    Fixture f;
    const auto m = f.arena.mark();
    zigzag::expectWritten(f.arena.setValueBits(
        f.older, xanadu::ValueKind::Int64, std::uint64_t{9}));
    EXPECT_TRUE(f.arena.wroteOlderCellSince(m));
    ASSERT_TRUE(f.arena.release(m));
  }
  {
    Fixture f;
    const auto m    = f.arena.mark();
    const auto text = f.arena.intern("x");
    const std::array spans{text};
    zigzag::expectWritten(f.arena.setContent(f.older, spans));
    EXPECT_TRUE(f.arena.wroteOlderCellSince(m));
    ASSERT_TRUE(f.arena.release(m));
  }
}

TEST(ArenaWrittenSinceTest, AWriteBeforeTheMarkDoesNotCount) {
  Fixture f;
  zigzag::expectWritten(f.arena.link(f.older, f.dim, DimVector::POS, f.other));
  const auto m = f.arena.mark();
  EXPECT_FALSE(f.arena.wroteOlderCellSince(m));
  ASSERT_TRUE(f.arena.release(m));
}

TEST(ArenaWrittenSinceTest, ARereadAfterReleaseIsFalseAgain) {
  Fixture f;
  const auto m = f.arena.mark();
  zigzag::expectWritten(f.arena.link(f.older, f.dim, DimVector::POS, f.other));
  ASSERT_TRUE(f.arena.release(m));
  const auto again = f.arena.mark();
  EXPECT_FALSE(f.arena.wroteOlderCellSince(again));
  ASSERT_TRUE(f.arena.release(again));
}

TEST(ArenaWrittenSinceTest, NestedMarksSeeWritesTheInnerOneKept) {
  Fixture f;
  const auto outer = f.arena.mark();
  const auto inner = f.arena.mark();
  zigzag::expectWritten(f.arena.link(f.older, f.dim, DimVector::POS, f.other));
  ASSERT_TRUE(f.arena.discard(inner));
  EXPECT_TRUE(f.arena.wroteOlderCellSince(outer));
  ASSERT_TRUE(f.arena.release(outer));
}

TEST(ArenaWrittenSinceTest, AWriteToACellBetweenTheMarksCountsForTheInnerOnly) {
  Fixture f;
  const auto outer  = f.arena.mark();
  const auto middle = f.arena.makeCell();
  const auto inner  = f.arena.mark();
  const auto fresh  = f.arena.makeCell();
  zigzag::expectWritten(f.arena.link(middle, f.dim, DimVector::POS, fresh));
  EXPECT_TRUE(f.arena.wroteOlderCellSince(inner));
  EXPECT_FALSE(f.arena.wroteOlderCellSince(outer));
  ASSERT_TRUE(f.arena.release(inner));
  ASSERT_TRUE(f.arena.release(outer));
}

TEST(ArenaWrittenSinceTest, SetUpWritesBeforeTheSecondMarkAreNotTheRuns) {
  Fixture f;
  const auto outer = f.arena.mark();
  // The set-up writes an older cell on purpose, as attaching a program does.
  zigzag::expectWritten(f.arena.link(f.older, f.dim, DimVector::POS, f.other));
  const auto run = f.arena.mark();
  EXPECT_TRUE(f.arena.wroteOlderCellSince(outer));
  EXPECT_FALSE(f.arena.wroteOlderCellSince(outer, run));
  // The same cell written again after the second mark is the run's write,
  // although the first mark had already copied it out.
  zigzag::expectWritten(
      f.arena.link(f.older, f.dim, DimVector::POS, std::nullopt));
  EXPECT_TRUE(f.arena.wroteOlderCellSince(outer, run));
  ASSERT_TRUE(f.arena.discard(run));
  ASSERT_TRUE(f.arena.release(outer));
}

TEST(ArenaWrittenSinceTest, CellsMintedInTheSetUpDoNotCountForTheRun) {
  Fixture f;
  const auto outer  = f.arena.mark();
  const auto minted = f.arena.makeCell("set-up");
  const auto run    = f.arena.mark();
  zigzag::expectWritten(
      f.arena.link(minted, f.dim, DimVector::POS, f.arena.makeCell()));
  EXPECT_FALSE(f.arena.wroteOlderCellSince(outer, run));
  ASSERT_TRUE(f.arena.discard(run));
  ASSERT_TRUE(f.arena.release(outer));
}

} // namespace
