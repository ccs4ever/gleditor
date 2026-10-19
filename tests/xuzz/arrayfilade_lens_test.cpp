#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/arrayfilade.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::enfilade {
namespace {

TEST(ArrayfiladeLensTest,
     PruningEfficiencyOverNinetyFivePercentOnOnePercentSelectivity) {
  // 10,000 cells with values 1.0 .. 10000.0
  constexpr std::size_t kTotalCells = 10000;
  std::vector<ArrayCellEntry> entries;
  entries.reserve(kTotalCells);
  for (std::size_t i = 1; i <= kTotalCells; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i), static_cast<std::int64_t>(i - 1),
        static_cast<double>(i)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);
  EXPECT_EQ(filade.count(), kTotalCells);

  // 1% Selectivity Query: between [5000.0, 5100.0] -> 101 cells matching out of
  // 10,000 (1.01%)
  const auto pred = Predicate::between(5000.0, 5100.0);
  QueryPlanStats stats{};
  const auto results = filade.planQueryWithStats(pred, stats);

  EXPECT_EQ(results.size(), 101U);
  EXPECT_GT(stats.subtreesExamined, 0U);

  // Over 95% of total cells/leaves are skipped completely
  const double leafPruningRate =
      1.0 - (static_cast<double>(stats.leavesExamined) /
             static_cast<double>(kTotalCells));
  EXPECT_GT(leafPruningRate, 0.95);
  EXPECT_GT(stats.subtreesPruned, 0U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(pred, entries));

  // Leaves examined must be a tiny fraction of total cells (< 500)
  EXPECT_LT(stats.leavesExamined, 500U);
}

TEST(ArrayfiladeLensTest, BloomFilterTextFunctorPruning) {
  std::vector<ArrayCellEntry> entries;
  entries.reserve(1000);
  for (std::size_t i = 0; i < 1000; ++i) {
    std::string tag = (i == 42) ? "target_process_special"
                                : "worker_pool_process_" + std::to_string(i);
    std::array<std::int64_t, MaxValence> coords{static_cast<std::int64_t>(i), 0,
                                                0, 0};
    entries.push_back(
        ArrayCellEntry::fromCell(static_cast<zigzag::CellRef>(1000 + i), coords,
                                 xanadu::ValueKind::None, 0, tag));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);

  const auto pred = Predicate::textEq("target_process_special");
  QueryPlanStats stats{};
  const auto matches = filade.planQueryWithStats(pred, stats);

  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches[0].cellRef, 1042U);
  EXPECT_GT(stats.subtreesPruned, 0U);
}

TEST(ArrayfiladeLensTest, ZeroCopySubEnfiladeSlicing) {
  constexpr std::size_t kSize = 1000;
  std::vector<ArrayCellEntry> entries;
  entries.reserve(kSize);
  for (std::size_t i = 0; i < kSize; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i + 1), static_cast<std::int64_t>(i),
        static_cast<double>(i * 10)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);

  // Slice [200 .. 299] (100 cells)
  const auto subFilade = filade.slice1D(200, 299);
  EXPECT_EQ(subFilade.count(), 100U);
  EXPECT_DOUBLE_EQ(subFilade.min(), 2000.0);
  EXPECT_DOUBLE_EQ(subFilade.max(), 2990.0);

  // Subscript into sub-enfilade
  const auto elem = subFilade.subscript1D(250);
  ASSERT_TRUE(elem.has_value());
  EXPECT_DOUBLE_EQ(elem->numericValue, 2500.0);
}

} // namespace
} // namespace xanadu::enfilade
