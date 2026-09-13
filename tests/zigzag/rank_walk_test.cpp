/**
 * @file rank_walk_test.cpp
 * @brief Unit tests for consolidated walkRank template helpers.
 */
#include <gtest/gtest.h>

#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace zigzag;

TEST(RankWalkTest, ArenaManifoldLinearForwardWalk) {
  ArenaManifold arena;
  const DimRef d1  = arena.makeCell("d.1");
  const CellRef c1 = arena.makeCell("first");
  const CellRef c2 = arena.makeCell("second");
  const CellRef c3 = arena.makeCell("third");

  arena.link(c1, d1, DimVector::POS, c2);
  arena.link(c2, d1, DimVector::POS, c3);

  std::vector<CellRef> visited;
  std::vector<std::size_t> steps;

  arena.walkRank(c1, d1, [&](CellRef cell, std::size_t step) {
    visited.push_back(cell);
    steps.push_back(step);
  });

  ASSERT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], c1);
  EXPECT_EQ(visited[1], c2);
  EXPECT_EQ(visited[2], c3);

  ASSERT_EQ(steps.size(), 3U);
  EXPECT_EQ(steps[0], 0U);
  EXPECT_EQ(steps[1], 1U);
  EXPECT_EQ(steps[2], 2U);
}

TEST(RankWalkTest, ArenaManifoldLinearBackwardWalk) {
  ArenaManifold arena;
  const DimRef d1  = arena.makeCell("d.1");
  const CellRef c1 = arena.makeCell("first");
  const CellRef c2 = arena.makeCell("second");
  const CellRef c3 = arena.makeCell("third");

  arena.link(c1, d1, DimVector::POS, c2);
  arena.link(c2, d1, DimVector::POS, c3);

  std::vector<CellRef> visited;
  arena.walkRank(c3, d1, DimVector::NEG,
                 [&](CellRef cell) { visited.push_back(cell); });

  ASSERT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], c3);
  EXPECT_EQ(visited[1], c2);
  EXPECT_EQ(visited[2], c1);
}

TEST(RankWalkTest, ArenaManifoldEarlyTermination) {
  ArenaManifold arena;
  const DimRef d1  = arena.makeCell("d.1");
  const CellRef c1 = arena.makeCell("first");
  const CellRef c2 = arena.makeCell("second");
  const CellRef c3 = arena.makeCell("third");

  arena.link(c1, d1, DimVector::POS, c2);
  arena.link(c2, d1, DimVector::POS, c3);

  std::vector<CellRef> visited;
  arena.walkRank(c1, d1, [&](CellRef cell) {
    visited.push_back(cell);
    return cell != c2; // Stop when c2 is reached
  });

  ASSERT_EQ(visited.size(), 2U);
  EXPECT_EQ(visited[0], c1);
  EXPECT_EQ(visited[1], c2);
}

TEST(RankWalkTest, ArenaManifoldCycleProtection) {
  ArenaManifold arena;
  const DimRef d1  = arena.makeCell("d.1");
  const CellRef c1 = arena.makeCell("A");
  const CellRef c2 = arena.makeCell("B");
  const CellRef c3 = arena.makeCell("C");

  // Create circular ring: c1 -> c2 -> c3 -> c1
  arena.link(c1, d1, DimVector::POS, c2);
  arena.link(c2, d1, DimVector::POS, c3);
  arena.link(c3, d1, DimVector::POS, c1);

  std::vector<CellRef> visited;
  arena.walkRank(c1, d1, [&](CellRef cell) { visited.push_back(cell); });

  // Cycle check must stop when next == start, visiting all 3 cells once
  ASSERT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], c1);
  EXPECT_EQ(visited[1], c2);
  EXPECT_EQ(visited[2], c3);
}

TEST(RankWalkTest, ManifoldRankWalk) {
  xanadu::Store store;
  auto at = store.sliceGenesis(xanadu::MicroversionId{});

  at            = store.makeCell(at, "cell_A");
  const auto cA = store.cellRefOf(at);

  at            = store.makeCell(at, "cell_B");
  const auto cB = store.cellRefOf(at);

  at            = store.makeCell(at, "cell_C");
  const auto cC = store.cellRefOf(at);

  const auto dSeq = store.rebuildManifold(at).dimensions().front();

  at = store.setLink(at, cA, dSeq, DimVector::POS, cB);
  at = store.setLink(at, cB, dSeq, DimVector::POS, cC);

  const auto manifold = store.rebuildManifold(at);

  std::vector<CellRef> visited;
  manifold.walkRank(cA, dSeq, [&](CellRef c) { visited.push_back(c); });

  ASSERT_EQ(visited.size(), 3U);
  EXPECT_EQ(visited[0], cA);
  EXPECT_EQ(visited[1], cB);
  EXPECT_EQ(visited[2], cC);
}

} // namespace
