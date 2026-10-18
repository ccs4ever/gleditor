/**
 * @file chronofilade_benchmark_test.cpp
 * @brief Performance and asymptotic scalability tests for the Osmic
 * Chronofilade.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/chronofilade.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Store;

TEST(ChronofiladeBenchmarkTest, ScalabilityAndSpeedup) {
  Store store;
  MicroversionId current{};

  constexpr int totalOps = 500;
  std::vector<MicroversionId> history;
  history.reserve(totalOps);

  for (int i = 0; i < totalOps; ++i) {
    const auto text = "word" + std::to_string(i) + " ";
    current = store.insert(current, store.rebuild(current).length(), text);
    history.push_back(current);
  }

  // Benchmark timeline scrubbing at the end of history (worst-case for linear
  // replay)
  const auto targetId  = history.back();
  const auto targetIdx = store.segmentedOps().indexOf(targetId);

  // 1. Raw linear replay from State 0
  const auto startRaw      = std::chrono::high_resolution_clock::now();
  constexpr int iterations = 100;
  for (int i = 0; i < iterations; ++i) {
    auto path = store.segmentedOps().ancestralPath(targetIdx);
    xanadu::Version rawBuilt;
    for (const auto idx : path) {
      if (const auto *const node = store.getCompactOp(idx); nullptr != node) {
        // Manually apply without chronofilade
        switch (node->kind) {
        case xanadu::OpKind::Insert:
          rawBuilt.insert(node->at, node->span());
          break;
        case xanadu::OpKind::Delete:
          rawBuilt.remove(node->at, node->length);
          break;
        case xanadu::OpKind::Rearrange:
          rawBuilt.rearrange(node->at, node->length, node->to);
          break;
        case xanadu::OpKind::PageBreak:
          rawBuilt.insertBreak(node->at);
          break;
        default:
          break;
        }
      }
    }
  }
  const auto endRaw = std::chrono::high_resolution_clock::now();
  const auto rawDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(endRaw - startRaw)
          .count() /
      static_cast<double>(iterations);

  // 2. Chronofilade rebuild
  const auto startChrono = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const auto chronoBuilt = store.rebuild(targetId);
    (void)chronoBuilt;
  }
  const auto endChrono = std::chrono::high_resolution_clock::now();
  const auto chronoDurationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(endChrono -
                                                            startChrono)
          .count() /
      static_cast<double>(iterations);

  std::cout << "[ BENCHMARK ] Operations: " << totalOps
            << " | Raw Replay: " << rawDurationUs << " us"
            << " | Chronofilade: " << chronoDurationUs << " us"
            << " | Speedup: "
            << (rawDurationUs / std::max(chronoDurationUs, 0.001)) << "x"
            << std::endl;

  // Confirm R9 mathematical identity
  EXPECT_TRUE(store.verifyAgainstFullRebuild(targetId));
  EXPECT_LT(chronoDurationUs, rawDurationUs);
}

} // namespace
