/**
 * @file layoutfilade_benchmark_test.cpp
 * @brief Performance benchmarks for Layoutfilade O(log N) coordinate mapping.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "common/xanadu/enfilade/layoutfilade.hpp"

namespace {

using xanadu::enfilade::LayoutEntry;
using xanadu::enfilade::LayoutEntryKind;
using xanadu::enfilade::Layoutfilade;

TEST(LayoutfiladeBenchmarkTest, ScaledCoordinateDescentSpeedup) {
  constexpr std::size_t kNumLines = 10000;
  std::vector<LayoutEntry> entries;
  entries.reserve(kNumLines);

  float totalHeight = 0.0F;
  for (std::size_t i = 0; i < kNumLines; ++i) {
    const float h = 18.0F + static_cast<float>(i % 6);
    entries.push_back(LayoutEntry{
        .byteLength = 60,
        .heightPx   = h,
        .widthPx    = 500.0F,
        .kind       = LayoutEntryKind::TextLine,
    });
    totalHeight += h;
  }

  const auto filade = Layoutfilade::buildFromEntries(entries);
  ASSERT_EQ(filade.size(), kNumLines);

  constexpr int kQueryIterations = 10000;
  volatile std::size_t sink      = 0;

  // 1. Linear scan benchmark: sequential search for target Y
  const auto t0 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kQueryIterations; ++iter) {
    const float queryY =
        static_cast<float>((iter * 17) % static_cast<int>(totalHeight));
    float currentY       = 0.0F;
    std::size_t foundIdx = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (queryY < currentY + entries[i].heightPx || i == entries.size() - 1) {
        foundIdx = i;
        break;
      }
      currentY += entries[i].heightPx;
    }
    sink += foundIdx;
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto linearElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 2. Layoutfilade O(log N) descent benchmark
  const auto t2 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kQueryIterations; ++iter) {
    const float queryY =
        static_cast<float>((iter * 17) % static_cast<int>(totalHeight));
    const auto hit = filade.findEntryAtY(queryY);
    if (hit) {
      sink += hit->entryIndex;
    }
  }
  const auto t3 = std::chrono::steady_clock::now();
  const auto filadeElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

  const double speedup = (filadeElapsedUs > 0)
                             ? static_cast<double>(linearElapsedUs) /
                                   static_cast<double>(filadeElapsedUs)
                             : 1.0;

  std::cout << "Layoutfilade Benchmark [N=" << kNumLines << " lines, "
            << kQueryIterations << " queries]:\n"
            << "  Linear scan Y descent : " << linearElapsedUs << " us\n"
            << "  Layoutfilade O(log N) : " << filadeElapsedUs << " us\n"
            << "  Speedup factor        : " << speedup << "x\n";

  EXPECT_GT(speedup, 2.0);
}

TEST(LayoutfiladeBenchmarkTest, IncrementalResizeLatency) {
  constexpr std::size_t kNumLines = 10000;
  std::vector<LayoutEntry> entries;
  entries.reserve(kNumLines);

  for (std::size_t i = 0; i < kNumLines; ++i) {
    entries.push_back(LayoutEntry{
        .byteLength = 60,
        .heightPx   = 20.0F,
        .widthPx    = 500.0F,
        .kind       = LayoutEntryKind::TextLine,
    });
  }

  auto filade = Layoutfilade::buildFromEntries(entries);

  constexpr int kUpdateIterations = 10000;

  const auto t0 = std::chrono::steady_clock::now();
  for (int iter = 0; iter < kUpdateIterations; ++iter) {
    const std::size_t targetIdx = (iter * 37) % kNumLines;
    auto modEntry               = entries[targetIdx];
    modEntry.heightPx           = 25.0F + static_cast<float>(iter % 10);
    filade.updateEntry(targetIdx, modEntry);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto updateElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  const double nsPerUpdate =
      (static_cast<double>(updateElapsedUs) * 1000.0) / kUpdateIterations;

  std::cout << "Layoutfilade Incremental Update [N=" << kNumLines << " lines, "
            << kUpdateIterations << " updates]:\n"
            << "  Total update time : " << updateElapsedUs << " us\n"
            << "  Average latency   : " << nsPerUpdate << " ns/update\n";

  EXPECT_LT(nsPerUpdate, 1000.0); // Sub-microsecond updates
}

} // namespace
