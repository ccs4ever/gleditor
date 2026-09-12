/**
 * @file arrayfilade_benchmark_test.cpp
 * @brief Performance and asymptotic benchmarks for the True Arrayfilade.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "common/xanadu/enfilade/arrayfilade.hpp"

namespace {

using xanadu::enfilade::ArrayCellEntry;
using xanadu::enfilade::Arrayfilade;
using xanadu::enfilade::Predicate;
using xanadu::enfilade::QueryPlanStats;

TEST(ArrayfiladeBenchmarkTest, Subscripting10000Cells) {
  constexpr std::size_t CellCount = 10000;
  std::vector<ArrayCellEntry> entries{};
  entries.reserve(CellCount);

  for (std::size_t i = 0; i < CellCount; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i + 1), static_cast<std::int64_t>(i),
        static_cast<double>(i * 3)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);
  EXPECT_EQ(filade.count(), CellCount);

  constexpr std::size_t QueryCount = 2000;
  std::vector<std::int64_t> queryIndices{};
  queryIndices.reserve(QueryCount);
  for (std::size_t q = 0; q < QueryCount; ++q) {
    queryIndices.push_back(static_cast<std::int64_t>((q * 7919) % CellCount));
  }

  // 1. Linear Scan Baseline
  const auto t0               = std::chrono::steady_clock::now();
  std::uint64_t linearMatches = 0;
  for (const auto idx : queryIndices) {
    for (const auto &entry : entries) {
      if (entry.coords[0] == idx) {
        linearMatches++;
        break;
      }
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto linearDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. Arrayfilade O(log N) Descent
  const auto t2               = std::chrono::steady_clock::now();
  std::uint64_t filadeMatches = 0;
  for (const auto idx : queryIndices) {
    const auto res = filade.subscript1D(idx);
    if (res.has_value()) {
      filadeMatches++;
    }
  }
  const auto t3 = std::chrono::steady_clock::now();
  const auto filadeDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  EXPECT_EQ(linearMatches, QueryCount);
  EXPECT_EQ(filadeMatches, QueryCount);

  const double speedup =
      static_cast<double>(linearDurationUs) /
      static_cast<double>(std::max<std::int64_t>(1, filadeDurationUs));

  std::cout << "[ BENCHMARK ] Subscripting " << QueryCount
            << " lookups over 10,000 cells:"
            << "\n  Linear scan:       " << linearDurationUs << " us"
            << "\n  Arrayfilade O(log N): " << filadeDurationUs << " us"
            << "\n  Speedup factor:    " << speedup << "x\n";

  EXPECT_GE(speedup, 5.0);
}

TEST(ArrayfiladeBenchmarkTest, O1ReductionVsLinearScan) {
  constexpr std::size_t CellCount = 10000;
  std::vector<ArrayCellEntry> entries{};
  entries.reserve(CellCount);

  for (std::size_t i = 1; i <= CellCount; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i), static_cast<std::int64_t>(i - 1),
        static_cast<double>(i)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);

  constexpr std::size_t Iterations = 50000;

  // 1. Linear scan reduction
  const auto t0    = std::chrono::steady_clock::now();
  double linearSum = 0.0;
  for (std::size_t it = 0; it < 100; ++it) {
    for (const auto &c : entries) {
      linearSum += c.numericValue;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto linearDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. O(1) Arrayfilade root summary reduction
  const auto t2    = std::chrono::steady_clock::now();
  double filadeSum = 0.0;
  for (std::size_t it = 0; it < Iterations; ++it) {
    filadeSum += filade.sum();
  }
  const auto t3 = std::chrono::steady_clock::now();
  const auto filadeDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  const double perQueryNs = (static_cast<double>(filadeDurationUs) * 1000.0) /
                            static_cast<double>(Iterations);

  std::cout << "[ BENCHMARK ] Parallel Reduction:"
            << "\n  Linear 100 iterations: " << linearDurationUs << " us"
            << "\n  Arrayfilade 50,000 queries: " << filadeDurationUs << " us"
            << "\n  Per-reduction latency: " << perQueryNs << " ns\n";

  EXPECT_GT(linearSum, 0.0);
  EXPECT_GT(filadeSum, 0.0);
  EXPECT_LT(perQueryNs, 50.0); // O(1) memory lookup < 50 ns
}

TEST(ArrayfiladeBenchmarkTest, VQLPredicatePushdownPruning) {
  constexpr std::size_t CellCount = 10000;
  std::vector<ArrayCellEntry> entries{};
  entries.reserve(CellCount);

  for (std::size_t i = 1; i <= CellCount; ++i) {
    entries.push_back(ArrayCellEntry::fromDouble(
        static_cast<zigzag::CellRef>(i), static_cast<std::int64_t>(i - 1),
        static_cast<double>(i)));
  }

  const auto filade = Arrayfilade::fromEntries(entries, 1);

  // Selective query: value > 9,800 (matches 200 / 10,000 cells)
  const auto pred = Predicate::gt(9800.0);

  // 1. Linear scan
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<ArrayCellEntry> linearResults{};
  for (const auto &c : entries) {
    if (pred.matches(c)) {
      linearResults.push_back(c);
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto linearDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. Query planning with predicate pushdown
  QueryPlanStats stats{};
  const auto t2             = std::chrono::steady_clock::now();
  const auto plannedResults = filade.planQueryWithStats(pred, stats);
  const auto t3             = std::chrono::steady_clock::now();
  const auto planDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  EXPECT_EQ(linearResults.size(), plannedResults.size());
  EXPECT_EQ(plannedResults.size(), 200U);
  EXPECT_GT(stats.subtreesPruned, 0U);

  const double speedup =
      static_cast<double>(linearDurationUs) /
      static_cast<double>(std::max<std::int64_t>(1, planDurationUs));

  std::cout << "[ BENCHMARK ] VQL Predicate Pushdown (val > 9800 on 10k cells):"
            << "\n  Linear scan duration: " << linearDurationUs << " us"
            << "\n  Planned scan duration: " << planDurationUs << " us"
            << "\n  Subtrees examined:   " << stats.subtreesExamined
            << "\n  Subtrees pruned:     " << stats.subtreesPruned
            << "\n  Leaves examined:     " << stats.leavesExamined
            << "\n  Leaves matched:      " << stats.leavesMatched
            << "\n  Planning speedup:    " << speedup << "x\n";

  EXPECT_GT(stats.subtreesPruned, 0U);
  EXPECT_LE(stats.leavesExamined, CellCount / 10);
  EXPECT_GE(speedup, 3.0);
}

} // namespace
