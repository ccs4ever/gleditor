/**
 * @file spike_s6_treefilade.cpp
 * @brief Exploratory Spike S6: Treefilade DDSketch Fleet Rollup.
 *
 * Simulates a fleet of 50,000 heterogeneous hosts streaming latency metrics.
 * Aggregates DDSketch mergeable quantile sketches up a 4-level Treefilade
 * (B=16) to evaluate fleet-wide p99 latency and single-node update propagation.
 *
 * Pass Criteria:
 * - Root p99 query <= 1 ms (evaluated directly in O(1) from root crum)
 * - Single-node metric update bubbling to root <= 15 ms
 * - Total memory footprint <= 64 MB
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include <gleditor/enfilade/crum_node.hpp>

namespace xanadu::enfilade {

struct StatDsp {
  std::uint32_t topologyHop{0};

  [[nodiscard]] StatDsp compose(const StatDsp &other) const noexcept {
    return StatDsp{topologyHop + other.topologyHop};
  }
  [[nodiscard]] bool isIdentity() const noexcept { return 0 == topologyHop; }
  bool operator==(const StatDsp &) const = default;
};

struct alignas(64) StatWid {
  static constexpr std::size_t kBuckets = 16;

  std::uint64_t count{0};
  double sum{0.0};
  double minVal{std::numeric_limits<double>::infinity()};
  double maxVal{-std::numeric_limits<double>::infinity()};
  double sumSquares{0.0};
  std::uint32_t alertFlags{0};

  // Logarithmic bucket sketch (DDSketch / t-digest compact representation)
  // Buckets partition ranges from 1ms to 10000ms logarithmically
  std::array<std::uint32_t, kBuckets> sketchBuckets{};

  [[nodiscard]] bool isEmpty() const noexcept { return 0 == count; }

  static std::size_t bucketForVal(double v) noexcept {
    if (v <= 1.0) return 0;
    // Log scale from 1ms to ~10,000ms: bucket = floor(log2(v)) clamped to [0,
    // 15]
    const int b = static_cast<int>(std::log2(v));
    return static_cast<std::size_t>(
        std::clamp(b, 0, static_cast<int>(kBuckets - 1)));
  }

  void addSample(double val) noexcept {
    ++count;
    sum += val;
    sumSquares += val * val;
    minVal = std::min(minVal, val);
    maxVal = std::max(maxVal, val);
    ++sketchBuckets[bucketForVal(val)];
    if (val > 1000.0) { // Alert on latency > 1000ms
      alertFlags |= 0x1;
    }
  }

  [[nodiscard]] StatWid combine(const StatWid &other) const noexcept {
    if (isEmpty()) return other;
    if (other.isEmpty()) return *this;

    StatWid out;
    out.count      = count + other.count;
    out.sum        = sum + other.sum;
    out.sumSquares = sumSquares + other.sumSquares;
    out.minVal     = std::min(minVal, other.minVal);
    out.maxVal     = std::max(maxVal, other.maxVal);
    out.alertFlags = alertFlags | other.alertFlags;

    for (std::size_t i = 0; i < kBuckets; ++i) {
      out.sketchBuckets[i] = sketchBuckets[i] + other.sketchBuckets[i];
    }
    return out;
  }

  // Instantaneous O(1) p99 quantile query from sketch
  [[nodiscard]] double estimateQuantile(double q) const noexcept {
    if (0 == count) return 0.0;
    const auto targetCount =
        static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count)));
    std::uint64_t accumulated = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      accumulated += sketchBuckets[i];
      if (accumulated >= targetCount) {
        // Return mid-range estimate for bucket 2^i
        const double lower = std::pow(2.0, static_cast<double>(i));
        const double upper = std::pow(2.0, static_cast<double>(i + 1));
        return (lower + upper) * 0.5;
      }
    }
    return maxVal;
  }
};

inline StatWid act(const StatDsp &, const StatWid &w) noexcept { return w; }

} // namespace xanadu::enfilade

namespace {

using xanadu::enfilade::StatDsp;
using xanadu::enfilade::StatWid;
using TreeCrum = gleditor::enfilade::CrumNode<StatDsp, StatWid, 16>;

class TreefiladeFleet {
public:
  static constexpr std::size_t B = 16;

  explicit TreefiladeFleet(const std::vector<StatWid> &hostMetrics) {
    leafMetrics_        = hostMetrics;
    const std::size_t n = hostMetrics.size();

    // 1. Level 1 (Zones): aggregate host leaves
    std::vector<std::uint32_t> currentLevel;
    currentLevel.reserve((n + B - 1) / B);

    for (std::size_t i = 0; i < n; i += B) {
      const auto crumIdx = static_cast<std::uint32_t>(crums_.size());
      TreeCrum crum{};
      crum.isLeaf             = 1;
      const std::size_t chunk = std::min(B, n - i);
      crum.childCount         = static_cast<std::uint8_t>(chunk);
      for (std::size_t s = 0; s < chunk; ++s) {
        crum.children[s] = static_cast<std::uint32_t>(i + s);
        crum.wids[s]     = hostMetrics[i + s];
      }
      crums_.push_back(crum);
      currentLevel.push_back(crumIdx);
    }

    // 2. Interior Levels up to Root
    while (currentLevel.size() > 1) {
      std::vector<std::uint32_t> nextLevel;
      nextLevel.reserve((currentLevel.size() + B - 1) / B);

      for (std::size_t i = 0; i < currentLevel.size(); i += B) {
        const auto parentIdx = static_cast<std::uint32_t>(crums_.size());
        TreeCrum parent{};
        parent.isLeaf           = 0;
        const std::size_t chunk = std::min(B, currentLevel.size() - i);
        parent.childCount       = static_cast<std::uint8_t>(chunk);

        for (std::size_t s = 0; s < chunk; ++s) {
          const std::uint32_t childIdx = currentLevel[i + s];
          crums_[childIdx].parentIndex = parentIdx;
          parent.children[s]           = childIdx;

          StatWid subWid{};
          for (std::size_t k = 0; k < crums_[childIdx].childCount; ++k) {
            subWid = subWid.combine(crums_[childIdx].wids[k]);
          }
          parent.wids[s] = subWid;
        }
        crums_.push_back(parent);
        nextLevel.push_back(parentIdx);
      }
      currentLevel = std::move(nextLevel);
    }

    rootIndex_ = currentLevel.front();
  }

  // Instantaneous O(1) p99 query from Fleet Root Crum
  [[nodiscard]] double queryFleetP99() const noexcept {
    StatWid rootWid{};
    const TreeCrum &root = crums_[rootIndex_];
    for (std::size_t s = 0; s < root.childCount; ++s) {
      rootWid = rootWid.combine(root.wids[s]);
    }
    return rootWid.estimateQuantile(0.99);
  }

  [[nodiscard]] std::uint64_t totalTelemetryEvents() const noexcept {
    StatWid rootWid{};
    const TreeCrum &root = crums_[rootIndex_];
    for (std::size_t s = 0; s < root.childCount; ++s) {
      rootWid = rootWid.combine(root.wids[s]);
    }
    return rootWid.count;
  }

  // Update a single host's metric and bubble 4 levels up to root
  void updateHostMetric(std::size_t hostIdx, const StatWid &newWid) noexcept {
    leafMetrics_[hostIdx]         = newWid;
    const std::size_t zoneCrumIdx = hostIdx / B;
    const std::size_t intraSlot   = hostIdx % B;

    crums_[zoneCrumIdx].wids[intraSlot] = newWid;

    // Bubble up to root
    std::uint32_t curr = static_cast<std::uint32_t>(zoneCrumIdx);
    while (crums_[curr].parentIndex) {
      const std::uint32_t parent = *crums_[curr].parentIndex;
      TreeCrum &p                = crums_[parent];

      // Recompute width summary for child 'curr'
      StatWid subWid{};
      for (std::size_t k = 0; k < crums_[curr].childCount; ++k) {
        subWid = subWid.combine(crums_[curr].wids[k]);
      }

      for (std::size_t s = 0; s < p.childCount; ++s) {
        if (p.children[s] == curr) {
          p.wids[s] = subWid;
          break;
        }
      }
      curr = parent;
    }
  }

  [[nodiscard]] std::size_t totalMemoryBytes() const noexcept {
    return crums_.size() * sizeof(TreeCrum) +
           leafMetrics_.size() * sizeof(StatWid);
  }

  [[nodiscard]] std::size_t crumCount() const noexcept { return crums_.size(); }

private:
  std::vector<TreeCrum> crums_;
  std::vector<StatWid> leafMetrics_;
  std::uint32_t rootIndex_{0};
};

} // namespace

int main() {
  std::cout << "=== Spike S6: Treefilade DDSketch Fleet Rollup ==="
            << std::endl;

  constexpr std::size_t kFleetSize = 50000;
  std::cout << "[Setup] Simulating telemetry feeds for " << kFleetSize
            << " hosts..." << std::endl;

  std::mt19937 rng(777);
  // Log-normal distribution: median ~ 25 ms, tail extending past 500 ms
  std::lognormal_distribution<double> latDist(3.2, 0.8);

  std::vector<StatWid> hostMetrics(kFleetSize);
  for (std::size_t h = 0; h < kFleetSize; ++h) {
    // Each host records 100 requests
    for (int req = 0; req < 100; ++req) {
      hostMetrics[h].addSample(latDist(rng));
    }
  }

  const auto tBuildStart = std::chrono::steady_clock::now();
  TreefiladeFleet fleet(hostMetrics);
  const auto tBuildEnd = std::chrono::steady_clock::now();
  const double buildMs =
      std::chrono::duration<double, std::milli>(tBuildEnd - tBuildStart)
          .count();

  const std::size_t memBytes = fleet.totalMemoryBytes();
  const double memMB = static_cast<double>(memBytes) / (1024.0 * 1024.0);

  std::cout << "[Setup] Built 4-level Treefilade (" << fleet.crumCount()
            << " crums) in " << buildMs << " ms | Total Memory: " << std::fixed
            << std::setprecision(2) << memMB << " MB" << std::endl;

  // Benchmark 1: Instantaneous Fleet-Wide p99 Query from Root
  constexpr std::size_t kQueryIterations = 100000;
  std::cout << "[Benchmark] Running " << kQueryIterations
            << " root p99 queries..." << std::endl;

  const auto tQueryStart = std::chrono::steady_clock::now();
  volatile double p99Val = 0.0;
  for (std::size_t i = 0; i < kQueryIterations; ++i) {
    p99Val = fleet.queryFleetP99();
  }
  const auto tQueryEnd = std::chrono::steady_clock::now();
  const double queryNs =
      std::chrono::duration<double, std::nano>(tQueryEnd - tQueryStart)
          .count() /
      kQueryIterations;
  const double queryMs = queryNs / 1000000.0;

  std::cout << "  Fleet p99 Estimate: " << std::fixed << std::setprecision(2)
            << p99Val << " ms" << std::endl;
  std::cout << "  Root p99 Query Latency: " << std::fixed
            << std::setprecision(3) << queryNs << " ns / query (" << queryMs
            << " ms)" << std::endl;

  // Benchmark 2: Host Telemetry Update bubbling 4 levels up to root
  constexpr std::size_t kUpdateIterations = 100000;
  std::cout << "[Benchmark] Running " << kUpdateIterations
            << " single-host metric updates bubbling to root..." << std::endl;

  std::uniform_int_distribution<std::size_t> hostPicker(0, kFleetSize - 1);
  std::vector<std::size_t> targetHosts(kUpdateIterations);
  for (auto &th : targetHosts) th = hostPicker(rng);

  const auto tUpdateStart = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < kUpdateIterations; ++i) {
    StatWid updated{};
    updated.addSample(35.0);
    fleet.updateHostMetric(targetHosts[i], updated);
  }
  const auto tUpdateEnd = std::chrono::steady_clock::now();
  const double updateNs =
      std::chrono::duration<double, std::nano>(tUpdateEnd - tUpdateStart)
          .count() /
      kUpdateIterations;
  const double updateMs = updateNs / 1000000.0;

  std::cout << "  Tree Update Latency: " << std::fixed << std::setprecision(3)
            << updateNs << " ns / update (" << updateMs << " ms)" << std::endl;

  const bool p99Pass    = (queryMs <= 1.0);
  const bool updatePass = (updateMs <= 15.0);
  const bool memPass    = (memMB <= 64.0);

  std::cout << "\n=== SPIKE S6 RESULTS ===" << std::endl;
  std::cout << "Root p99 Query Target: <= 1.000 ms  | Actual: " << queryMs
            << " ms | " << (p99Pass ? "[PASS]" : "[FAIL]") << std::endl;
  std::cout << "Tree Update Target:    <= 15.000 ms | Actual: " << updateMs
            << " ms | " << (updatePass ? "[PASS]" : "[FAIL]") << std::endl;
  std::cout << "Memory Target:         <= 64.00 MB  | Actual: " << memMB
            << " MB | " << (memPass ? "[PASS]" : "[FAIL]") << std::endl;

  return (p99Pass && updatePass && memPass) ? 0 : 1;
}
