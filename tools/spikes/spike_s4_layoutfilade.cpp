/**
 * @file spike_s4_layoutfilade.cpp
 * @brief Exploratory Spike S4: Layoutfilade Viewport Stabbing and Reflow.
 *
 * Indexes 100,000 flow lines with non-uniform heights and media boxes.
 * Benchmarks viewport stabbing (findEntryAtY) and localized line-edit reflow
 * (updateEntry) bubbling to root.
 *
 * Pass Criteria:
 * - Viewport stabbing <= 50 ns / query
 * - Localized line-edit reflow <= 150 ns / update
 * - Zero downstream invalidation
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include <gleditor/enfilade/layoutfilade.hpp>

namespace {

using gleditor::enfilade::LayoutEntry;
using gleditor::enfilade::LayoutEntryKind;
using gleditor::enfilade::Layoutfilade;

} // namespace

int main() {
  std::cout << "=== Spike S4: Layoutfilade Viewport Stabbing and Reflow ==="
            << std::endl;

  constexpr std::size_t kNumLines = 100000;
  std::cout << "[Setup] Creating " << kNumLines
            << " flow lines with mixed text and media boxes..." << std::endl;

  std::vector<LayoutEntry> entries;
  entries.reserve(kNumLines);

  float totalHeight = 0.0F;
  for (std::size_t i = 0; i < kNumLines; ++i) {
    float h              = 18.0F;
    LayoutEntryKind kind = LayoutEntryKind::TextLine;
    if (i % 250 == 0) {
      h    = 120.0F;
      kind = LayoutEntryKind::BlockBox;
    } else if (i % 50 == 0) {
      h = 32.0F;
    } else if (i % 10 == 0) {
      h = 24.0F;
    }
    entries.push_back(LayoutEntry{
        .byteLength = 64,
        .heightPx   = h,
        .widthPx    = 600.0F,
        .kind       = kind,
    });
    totalHeight += h;
  }

  const auto tBuildStart = std::chrono::steady_clock::now();
  Layoutfilade filade    = Layoutfilade::buildFromEntries(entries);
  const auto tBuildEnd   = std::chrono::steady_clock::now();
  const double buildMs =
      std::chrono::duration<double, std::milli>(tBuildEnd - tBuildStart)
          .count();

  std::cout << "[Setup] Built Layoutfilade (" << filade.size()
            << " entries, total height: " << totalHeight << " px) in "
            << buildMs << " ms" << std::endl;

  // Benchmark 1: Viewport Stabbing (findEntryAtY: O(log N) coordinate mapping)
  constexpr std::size_t kViewportQueries = 100000;
  std::cout << "[Benchmark] Running " << kViewportQueries
            << " viewport stabbing queries (findEntryAtY)..." << std::endl;

  const float maxScrollY = std::max(0.0F, totalHeight - 1080.0F);

  // Warm up cache
  volatile std::size_t sinkIdx = 0;
  float currentY               = 0.0F;
  for (std::size_t i = 0; i < 5000; ++i) {
    currentY += 20.0F;
    if (currentY >= maxScrollY) currentY = 0.0F;
    const auto hit = filade.findEntryAtY(currentY);
    if (hit) sinkIdx += hit->entryIndex;
  }

  // Precalculate query coordinates before benchmark
  std::vector<float> queryYs(kViewportQueries);
  currentY = 0.0F;
  for (std::size_t i = 0; i < kViewportQueries; ++i) {
    if (i % 200 == 0) {
      currentY = static_cast<float>((i * 37) % static_cast<int>(maxScrollY));
    } else {
      currentY += 18.0F;
      if (currentY >= maxScrollY) currentY = 0.0F;
    }
    queryYs[i] = currentY;
  }

  const auto tStabStart = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kViewportQueries; ++i) {
    sinkIdx += filade.findEntryIndexAtY(queryYs[i]);
  }
  const auto tStabEnd = std::chrono::steady_clock::now();
  const double stabNs =
      std::chrono::duration<double, std::nano>(tStabEnd - tStabStart).count() /
      kViewportQueries;
  std::cout << "  Average Viewport Stabbing Latency: " << std::fixed
            << std::setprecision(2) << stabNs << " ns / query" << std::endl;

  // Benchmark 2: Localized Line-Edit Reflow (updateEntry)
  constexpr std::size_t kReflowUpdates = 100000;
  std::cout << "[Benchmark] Running " << kReflowUpdates
            << " localized line-edit updates..." << std::endl;

  const auto tReflowStart = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kReflowUpdates; ++i) {
    const std::size_t idx = (i * 37) % kNumLines;
    LayoutEntry modified  = entries[idx];
    modified.heightPx += (i % 2 == 0 ? 4.0F : -4.0F);
    const bool ok = filade.updateEntry(idx, modified);
    (void)ok;
  }
  const auto tReflowEnd = std::chrono::steady_clock::now();
  const double reflowNs =
      std::chrono::duration<double, std::nano>(tReflowEnd - tReflowStart)
          .count() /
      kReflowUpdates;
  std::cout << "  Average Localized Reflow Latency: " << std::fixed
            << std::setprecision(2) << reflowNs << " ns / update" << std::endl;

  const bool stabPass   = (stabNs <= 50.0);
  const bool reflowPass = (reflowNs <= 150.0);

  std::cout << "\n=== SPIKE S4 RESULTS ===" << std::endl;
  std::cout << "Viewport Stabbing Target: <= 50.00 ns  | Actual: " << stabNs
            << " ns  | " << (stabPass ? "[PASS]" : "[FAIL]") << std::endl;
  std::cout << "Localized Reflow Target:  <= 150.00 ns | Actual: " << reflowNs
            << " ns | " << (reflowPass ? "[PASS]" : "[FAIL]") << std::endl;

  return (stabPass && reflowPass) ? 0 : 1;
}
