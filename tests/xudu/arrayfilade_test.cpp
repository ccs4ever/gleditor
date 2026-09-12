/**
 * @file arrayfilade_test.cpp
 * @brief Comprehensive unit tests for the True Arrayfilade and VQL Query
 * Planner.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/arrayfilade.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::ValueKind;
using xanadu::enfilade::ArrayCellEntry;
using xanadu::enfilade::ArrayDsp;
using xanadu::enfilade::Arrayfilade;
using xanadu::enfilade::ArrayWid;
using xanadu::enfilade::DisplacementMonoid;
using xanadu::enfilade::EnfiladeAction;
using xanadu::enfilade::MaxValence;
using xanadu::enfilade::Predicate;
using xanadu::enfilade::PredicateOp;
using xanadu::enfilade::QueryPlanStats;
using xanadu::enfilade::WidthMonoid;
using zigzag::ArenaManifold;
using zigzag::DimVector;

// 1. Concept Verification
static_assert(DisplacementMonoid<ArrayDsp>);
static_assert(WidthMonoid<ArrayWid>);
static_assert(EnfiladeAction<ArrayDsp, ArrayWid>);

TEST(ArrayfiladeTest, MonoidAxiomsAndAction) {
  // Identity and composition
  ArrayDsp d0{};
  EXPECT_TRUE(d0.isIdentity());

  ArrayDsp d1{.valence = 2, .deltaCoords = {10, -5, 0, 0}};
  ArrayDsp d2{.valence = 2, .deltaCoords = {5, 15, 0, 0}};
  const auto d12 = d1.compose(d2);
  EXPECT_EQ(d12.deltaCoords[0], 15);
  EXPECT_EQ(d12.deltaCoords[1], 10);
  EXPECT_FALSE(d12.isIdentity());

  // Width monoid combination
  ArrayWid wEmpty{};
  EXPECT_TRUE(wEmpty.isEmpty());

  ArrayWid w1{.valence   = 2,
              .count     = 1,
              .minCoord  = {0, 0, 0, 0},
              .maxCoord  = {5, 5, 0, 0},
              .sum       = 10.0,
              .minVal    = 10.0,
              .maxVal    = 10.0,
              .product   = 10.0,
              .minScalar = 10.0,
              .maxScalar = 10.0};

  ArrayWid w2{.valence   = 2,
              .count     = 1,
              .minCoord  = {3, 2, 0, 0},
              .maxCoord  = {10, 8, 0, 0},
              .sum       = 25.0,
              .minVal    = 25.0,
              .maxVal    = 25.0,
              .product   = 25.0,
              .minScalar = 25.0,
              .maxScalar = 25.0};

  const auto wCombined = w1.combine(w2);
  EXPECT_FALSE(wCombined.isEmpty());
  EXPECT_EQ(wCombined.count, 2U);
  EXPECT_EQ(wCombined.minCoord[0], 0);
  EXPECT_EQ(wCombined.maxCoord[0], 10);
  EXPECT_EQ(wCombined.minCoord[1], 0);
  EXPECT_EQ(wCombined.maxCoord[1], 8);
  EXPECT_DOUBLE_EQ(wCombined.sum, 35.0);
  EXPECT_DOUBLE_EQ(wCombined.minVal, 10.0);
  EXPECT_DOUBLE_EQ(wCombined.maxVal, 25.0);
  EXPECT_DOUBLE_EQ(wCombined.product, 250.0);

  // Coordinate action
  const auto wActed = d1.act(w1);
  EXPECT_EQ(wActed.minCoord[0], 10);
  EXPECT_EQ(wActed.maxCoord[0], 15);
  EXPECT_EQ(wActed.minCoord[1], -5);
  EXPECT_EQ(wActed.maxCoord[1], 0);
  EXPECT_DOUBLE_EQ(wActed.sum, 10.0); // translation invariant
}

TEST(ArrayfiladeTest, VPLSubscriptingAndSlicing) {
  // 1D Array of 50 cells
  std::vector<ArrayCellEntry> entries1D{};
  for (std::int64_t i = 0; i < 50; ++i) {
    entries1D.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(100 + i), i, static_cast<double>(i * 2)));
  }

  const auto filade1D = Arrayfilade::fromEntries(entries1D, 1);
  EXPECT_EQ(filade1D.count(), 50U);
  EXPECT_TRUE(filade1D.verifySubscriptAgainstLinear(entries1D));

  // Subscripting
  const auto sub0 = filade1D.subscript1D(0);
  ASSERT_TRUE(sub0.has_value());
  EXPECT_DOUBLE_EQ(sub0->numericValue, 0.0);

  const auto sub25 = filade1D.subscript1D(25);
  ASSERT_TRUE(sub25.has_value());
  EXPECT_DOUBLE_EQ(sub25->numericValue, 50.0);

  const auto subOOB = filade1D.subscript1D(99);
  EXPECT_FALSE(subOOB.has_value());

  // 1D Slicing: [10..30]
  const auto slice1D = filade1D.slice1D(10, 30);
  EXPECT_EQ(slice1D.count(), 21U);
  EXPECT_TRUE(filade1D.verifySliceAgainstLinear({10, 0, 0, 0}, {30, 0, 0, 0},
                                                entries1D));

  // 2D Matrix (5x5 = 25 cells)
  std::vector<ArrayCellEntry> matrixEntries{};
  for (std::int64_t r = 0; r < 5; ++r) {
    for (std::int64_t c = 0; c < 5; ++c) {
      std::array<std::int64_t, MaxValence> coords{r, c, 0, 0};
      matrixEntries.push_back(ArrayCellEntry::fromCell(
          static_cast<zigzag::CellRef>(200 + r * 5 + c), coords,
          ValueKind::Double,
          std::bit_cast<std::uint64_t>(static_cast<double>(r * 10 + c))));
    }
  }

  const auto matrixFilade = Arrayfilade::fromEntries(matrixEntries, 2);
  EXPECT_EQ(matrixFilade.count(), 25U);
  EXPECT_EQ(matrixFilade.valence(), 2);
  EXPECT_TRUE(matrixFilade.verifySubscriptAgainstLinear(matrixEntries));

  const auto cell23 = matrixFilade.subscript({2, 3, 0, 0});
  ASSERT_TRUE(cell23.has_value());
  EXPECT_DOUBLE_EQ(cell23->numericValue, 23.0);

  // 2D Slicing: rows 1..3, cols 1..3 (3x3 = 9 cells)
  const auto slice2D = matrixFilade.slice({1, 1, 0, 0}, {3, 3, 0, 0});
  EXPECT_EQ(slice2D.count(), 9U);
  EXPECT_TRUE(matrixFilade.verifySliceAgainstLinear({1, 1, 0, 0}, {3, 3, 0, 0},
                                                    matrixEntries));
}

TEST(ArrayfiladeTest, VPLParallelReductionsO1) {
  // Numbers: [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]
  std::vector<ArrayCellEntry> entries{};
  for (std::int64_t i = 1; i <= 10; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i), i - 1, static_cast<double>(i)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);

  // Immediate O(1) Reductions
  EXPECT_DOUBLE_EQ(filade.sum(), 55.0);
  EXPECT_DOUBLE_EQ(filade.min(), 1.0);
  EXPECT_DOUBLE_EQ(filade.max(), 10.0);
  EXPECT_DOUBLE_EQ(filade.product(), 3628800.0);
  EXPECT_EQ(filade.count(), 10U);

  const auto shp = filade.shape();
  EXPECT_EQ(shp[0], 10);

  // R9 Verification against linear accumulation
  EXPECT_TRUE(filade.verifyReductionsAgainstLinear(entries));
}

TEST(ArrayfiladeTest, VQLPredicatePushdownNumeric) {
  // 1,000 cells with values 1.0 .. 1000.0
  std::vector<ArrayCellEntry> entries{};
  for (std::int64_t i = 1; i <= 1000; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i), i - 1, static_cast<double>(i)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);
  EXPECT_EQ(filade.count(), 1000U);

  // Query 1: > 900.0 (matches 100 cells)
  QueryPlanStats stats1{};
  const auto pred1    = Predicate::gt(900.0);
  const auto results1 = filade.planQueryWithStats(pred1, stats1);
  EXPECT_EQ(results1.size(), 100U);
  EXPECT_GT(stats1.subtreesPruned, 0U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(pred1, entries));

  // Query 2: < 50.0 (matches 49 cells)
  QueryPlanStats stats2{};
  const auto pred2    = Predicate::lt(50.0);
  const auto results2 = filade.planQueryWithStats(pred2, stats2);
  EXPECT_EQ(results2.size(), 49U);
  EXPECT_GT(stats2.subtreesPruned, 0U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(pred2, entries));

  // Query 3: Between 400.0 and 450.0 (matches 51 cells)
  QueryPlanStats stats3{};
  const auto pred3    = Predicate::between(400.0, 450.0);
  const auto results3 = filade.planQueryWithStats(pred3, stats3);
  EXPECT_EQ(results3.size(), 51U);
  EXPECT_GT(stats3.subtreesPruned, 0U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(pred3, entries));

  // Query 4: Exact equality == 777.0 (matches 1 cell)
  QueryPlanStats stats4{};
  const auto pred4    = Predicate::eq(777.0);
  const auto results4 = filade.planQueryWithStats(pred4, stats4);
  ASSERT_EQ(results4.size(), 1U);
  EXPECT_DOUBLE_EQ(results4[0].numericValue, 777.0);
  EXPECT_GT(stats4.subtreesPruned, 0U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(pred4, entries));

  // Query 5: Out of bounds > 2000.0 (matches 0 cells, prunes at root)
  QueryPlanStats stats5{};
  const auto pred5    = Predicate::gt(2000.0);
  const auto results5 = filade.planQueryWithStats(pred5, stats5);
  EXPECT_EQ(results5.size(), 0U);
  EXPECT_EQ(stats5.subtreesPruned, 1U);
  EXPECT_EQ(stats5.leavesExamined, 0U); // 100% pruned at root
}

TEST(ArrayfiladeTest, VQLPredicatePushdownBloom) {
  // Text cells with names
  const std::vector<std::string> names = {"Alice", "Bob",   "Charlie", "David",
                                          "Eve",   "Frank", "Grace",   "Heidi"};
  std::vector<ArrayCellEntry> textEntries{};
  for (std::size_t i = 0; i < names.size(); ++i) {
    std::array<std::int64_t, MaxValence> coords{static_cast<std::int64_t>(i), 0,
                                                0, 0};
    textEntries.push_back(
        ArrayCellEntry::fromCell(static_cast<zigzag::CellRef>(300 + i), coords,
                                 ValueKind::None, 0, names[i]));
  }

  const auto filade = Arrayfilade::fromEntries(textEntries, 1);
  EXPECT_EQ(filade.count(), names.size());

  // Search for Alice
  const auto alicePred = Predicate::textEq("Alice");
  const auto aliceRes  = filade.planQuery(alicePred);
  ASSERT_EQ(aliceRes.size(), 1U);
  EXPECT_EQ(aliceRes[0].cellRef, 300U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(alicePred, textEntries));

  // Search for non-existent name "Zara"
  QueryPlanStats stats{};
  const auto zaraPred = Predicate::textEq("Zara");
  const auto zaraRes  = filade.planQueryWithStats(zaraPred, stats);
  EXPECT_TRUE(zaraRes.empty());
  EXPECT_TRUE(filade.verifyAgainstLinearScan(zaraPred, textEntries));
}

TEST(ArrayfiladeTest, ManifoldRankAndMatrixIndexing) {
  ArenaManifold arena{};
  const auto dim1 = arena.makeCell("d.1");

  // Create a rank of 20 cells with integer values
  zigzag::CellRef head = zigzag::noCell;
  zigzag::CellRef prev = zigzag::noCell;

  for (int i = 1; i <= 20; ++i) {
    const auto cell = arena.makeCell();
    arena.setValueBits(
        cell, ValueKind::Int64,
        std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(i * 10)));
    if (zigzag::noCell == head) {
      head = cell;
    }
    if (zigzag::noCell != prev) {
      arena.link(prev, dim1, DimVector::POS, cell);
    }
    prev = cell;
  }

  const auto filade = Arrayfilade::fromArenaRank(arena, head, dim1);
  EXPECT_EQ(filade.count(), 20U);
  EXPECT_DOUBLE_EQ(filade.min(), 10.0);
  EXPECT_DOUBLE_EQ(filade.max(), 200.0);
  EXPECT_DOUBLE_EQ(filade.sum(), 2100.0);

  // VQL query: d.1[value > 150]
  const auto pred    = Predicate::gt(150.0);
  const auto matched = filade.planQuery(pred);
  EXPECT_EQ(matched.size(), 5U); // 160, 170, 180, 190, 200

  for (const auto &m : matched) {
    EXPECT_GT(m.numericValue, 150.0);
  }
}

} // namespace
