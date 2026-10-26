/**
 * @file rankfilade_test.cpp
 * @brief Unit tests for the Rankfilade 1D B-enfilade.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "common/xanadu/enfilade/rankfilade.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::enfilade::RankDsp;
using xanadu::enfilade::Rankfilade;
using xanadu::enfilade::RankWid;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

TEST(RankfiladeTest, MonoidAxiomsAndAction) {
  // Identity
  const RankDsp idDsp{};
  EXPECT_TRUE(idDsp.isIdentity());

  const RankWid emptyWid{};
  EXPECT_TRUE(emptyWid.isEmpty());

  // Composition
  const RankDsp d1{5};
  const RankDsp d2{10};
  const RankDsp d3 = d1.compose(d2);
  EXPECT_EQ(d3.delta, 15);

  // Combination
  const RankWid w1{.count = 42};
  const RankWid w2{.count = 58};
  const RankWid w3 = w1.combine(w2);
  EXPECT_EQ(w3.count, 100);

  // Action
  const RankWid acted = d1.act(w1);
  EXPECT_EQ(acted.count, 42);
}

TEST(RankfiladeTest, EmptyRankfilade) {
  Rankfilade filade;
  EXPECT_TRUE(filade.empty());
  EXPECT_EQ(filade.size(), 0);
  EXPECT_EQ(filade.cellAtOrdinal(0), std::nullopt);
  EXPECT_EQ(filade.ordinalOfCell(1), std::nullopt);
  EXPECT_FLOAT_EQ(filade.scrollbarThumbRatio(1), 0.0F);
}

TEST(RankfiladeTest, SingleCellRankfilade) {
  const std::vector<CellRef> cells = {101};
  const Rankfilade filade(cells);

  EXPECT_FALSE(filade.empty());
  EXPECT_EQ(filade.size(), 1);
  EXPECT_EQ(filade.cellAtOrdinal(0), 101);
  EXPECT_EQ(filade.cellAtOrdinal(1), std::nullopt);
  EXPECT_EQ(filade.ordinalOfCell(101), 0);
  EXPECT_EQ(filade.ordinalOfCell(999), std::nullopt);
  EXPECT_FLOAT_EQ(filade.scrollbarThumbRatio(101), 0.0F);
  EXPECT_TRUE(filade.verifyAgainstLinearScan());
}

TEST(RankfiladeTest, CoordinateDescentAndAscentSmallRank) {
  constexpr std::size_t kCount = 200;
  std::vector<CellRef> cells;
  cells.reserve(kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    cells.push_back(static_cast<CellRef>(1000 + i));
  }

  const Rankfilade filade(cells);
  EXPECT_EQ(filade.size(), kCount);
  EXPECT_TRUE(filade.verifyAgainstLinearScan());

  // Check boundary ordinals
  EXPECT_EQ(filade.cellAtOrdinal(0), 1000);
  EXPECT_EQ(filade.cellAtOrdinal(kCount - 1), 1000 + kCount - 1);
  EXPECT_EQ(filade.cellAtOrdinal(kCount), std::nullopt);

  // Check scrollbar thumb ratio
  EXPECT_FLOAT_EQ(filade.scrollbarThumbRatio(1000), 0.0F);
  EXPECT_FLOAT_EQ(filade.scrollbarThumbRatio(1000 + kCount - 1), 1.0F);
  EXPECT_NEAR(filade.scrollbarThumbRatio(1000 + kCount / 2), 0.5F, 0.01F);
}

TEST(RankfiladeTest, BuildFromArenaManifoldRank) {
  ArenaManifold arena;
  const DimRef dRank = arena.ensureDimension("d.rank");

  constexpr std::size_t kCount = 500;
  std::vector<CellRef> cells;
  cells.reserve(kCount);

  CellRef prev = zigzag::noCell;
  for (std::size_t i = 0; i < kCount; ++i) {
    const CellRef c = arena.makeCell();
    cells.push_back(c);
    if (prev != zigzag::noCell) {
      zigzag::expectWritten(arena.link(prev, dRank, DimVector::POS, c));
    }
    prev = c;
  }

  const Rankfilade filade =
      Rankfilade::buildFromRank(arena, cells.front(), dRank);
  EXPECT_EQ(filade.size(), kCount);
  EXPECT_TRUE(filade.verifyAgainstLinearScan());

  for (std::size_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(filade.cellAtOrdinal(i), cells[i]);
    EXPECT_EQ(filade.ordinalOfCell(cells[i]), i);
  }
}

TEST(RankfiladeTest, LargeScaleDescentAndAscent100k) {
  constexpr std::size_t kCount = 100000;
  std::vector<CellRef> cells;
  cells.reserve(kCount);
  for (std::size_t i = 0; i < kCount; ++i) {
    cells.push_back(static_cast<CellRef>(i + 1));
  }

  const Rankfilade filade(cells);
  EXPECT_EQ(filade.size(), kCount);

  // Sample check every 100th element
  for (std::size_t i = 0; i < kCount; i += 100) {
    const auto c = filade.cellAtOrdinal(i);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(*c, cells[i]);

    const auto ord = filade.ordinalOfCell(*c);
    ASSERT_TRUE(ord.has_value());
    EXPECT_EQ(*ord, i);
  }
}

} // namespace

TEST(RankfiladeParentTest, ParentZeroIsDistinctFromAnAbsentParent) {
  using Crum = gleditor::enfilade::CrumNode<RankDsp, RankWid, 8>;
  Crum root;
  EXPECT_FALSE(root.parentIndex);
  Crum child;
  child.parentIndex = 0;
  ASSERT_TRUE(child.parentIndex);
  EXPECT_EQ(*child.parentIndex, 0U);
  static_assert(sizeof(Crum) == 128);
}
