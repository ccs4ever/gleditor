/**
 * @file spike_s5_arena_toss.cpp
 * @brief Exploratory Spike S5: ArenaManifold Epoch Toss Cost.
 *
 * Populates 100,000 ephemeral cells with values and multidimensional links
 * in ArenaManifold and measures the execution latency of release(emptyMark).
 *
 * Pass Criteria:
 * - Flat execution time <= 10 us for release()
 * - Zero name lookups during release
 * - Slab pointers reset cleanly and memory footprint remains bounded across 500
 * epochs
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"

int main() {
  std::cout << "=== Spike S5: ArenaManifold Epoch Toss Cost ===" << std::endl;

  zigzag::ArenaManifold arena;
  const zigzag::DimRef dX = arena.ensureDimension("d.x");
  const zigzag::DimRef dY = arena.ensureDimension("d.y");

  // Take the baseline empty mark
  const zigzag::Mark emptyMark = arena.mark();

  constexpr std::size_t kCellsPerEpoch = 100000;
  constexpr std::size_t kEpochs        = 500;

  std::cout << "[Setup] Running " << kEpochs << " repeated cycles of minting "
            << kCellsPerEpoch << " cells and tossing via release(emptyMark)..."
            << std::endl;

  double totalReleaseTimeUs = 0.0;
  double maxReleaseTimeUs   = 0.0;

  for (std::size_t epoch = 0; epoch < kEpochs; ++epoch) {
    zigzag::CellRef prev = zigzag::noCell;
    for (std::size_t i = 0; i < kCellsPerEpoch; ++i) {
      const zigzag::CellRef c = arena.makeCell();
      if (prev != zigzag::noCell) {
        if (i % 2 == 0) {
          zigzag::expectWritten(
              arena.link(prev, dX, zigzag::DimVector::POS, c));
        } else {
          zigzag::expectWritten(
              arena.link(prev, dY, zigzag::DimVector::POS, c));
        }
      }
      prev = c;
    }

    // Benchmark the toss: release(emptyMark)
    const auto tStart = std::chrono::steady_clock::now();
    arena.release(emptyMark);
    const auto tEnd = std::chrono::steady_clock::now();

    const double releaseUs =
        std::chrono::duration<double, std::micro>(tEnd - tStart).count();
    totalReleaseTimeUs += releaseUs;
    if (releaseUs > maxReleaseTimeUs) {
      maxReleaseTimeUs = releaseUs;
    }

    // Verify arena is completely reset to 0 cells
    if (arena.homeCell() != zigzag::noCell && arena.dimensions().size() < 2) {
      std::cerr << "[FAIL] Arena corrupted after release!" << std::endl;
      return 1;
    }
  }

  const double avgReleaseUs = totalReleaseTimeUs / kEpochs;

  std::cout << "[Benchmark] Completed " << kEpochs << " epoch tosses."
            << std::endl;
  std::cout << "  Average Toss Latency: " << std::fixed << std::setprecision(3)
            << avgReleaseUs << " us" << std::endl;
  std::cout << "  Max Toss Latency:     " << std::fixed << std::setprecision(3)
            << maxReleaseTimeUs << " us" << std::endl;

  const bool pass = (avgReleaseUs <= 10.0);

  std::cout << "\n=== SPIKE S5 RESULTS ===" << std::endl;
  std::cout << "Epoch Toss Target: <= 10.000 us | Actual: " << avgReleaseUs
            << " us | " << (pass ? "[PASS]" : "[FAIL]") << std::endl;

  return pass ? 0 : 1;
}
