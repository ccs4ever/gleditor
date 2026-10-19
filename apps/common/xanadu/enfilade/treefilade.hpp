/**
 * @file treefilade.hpp
 * @brief Statistical summary enfilade (Treefilade) for large-scale fleet
 * telemetry and quantile aggregation.
 *
 * Implements mergeable summary monoids (StatWid, StatDsp) over B-enfilades:
 * - Exact count, sum, sum of squares (mean & stddev) in O(1).
 * - Exact min, max, and DDSketch logarithmic bucket histogram for quantiles
 * (p50, p90, p99).
 * - HyperLogLog cardinality estimation and anomaly bitflags.
 * - Subtree pruning for rapid anomaly detection across 50,000+ nodes.
 */
#ifndef COMMON_XANADU_ENFILADE_TREEFILADE_HPP
#define COMMON_XANADU_ENFILADE_TREEFILADE_HPP

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xanadu::enfilade {

/**
 * @brief Anomaly bitflags for telemetry tracking.
 */
enum TelemetryAnomaly : std::uint32_t {
  AnomalyNone        = 0,
  AnomalyHighLatency = 1U << 0, // Latency exceeds SLA (e.g. > 100ms)
  AnomalyErrorSpike  = 1U << 1, // Error rate threshold exceeded
  AnomalySaturation  = 1U << 2  // Host CPU / Memory saturation
};

/**
 * @struct StatWid
 * @brief Statistical summary monoid for fleet telemetry aggregation.
 *
 * Combines in O(1) via monoid multiplication (associative & commutative):
 * - Exact count, sum, sum of squares (mean & stddev)
 * - Exact min, max
 * - DDSketch logarithmic bucket histogram for mergeable quantile estimation
 * (p50, p90, p99)
 * - HyperLogLog cardinality mask
 * - Bitwise-OR anomaly flags
 */
struct StatWid {
  static constexpr std::size_t kBuckets = 64;
  static constexpr double kMinVal       = 1e-3;
  static constexpr double kMaxVal       = 1e5;

  std::size_t count{0};
  double sum{0.0};
  double sumSquares{0.0};
  double minVal{std::numeric_limits<double>::infinity()};
  double maxVal{-std::numeric_limits<double>::infinity()};
  std::array<std::uint32_t, kBuckets> buckets{};
  std::uint32_t hllMask{0};
  std::uint32_t anomalyFlags{0};

  static std::size_t bucketIndex(double v) noexcept {
    if (v <= kMinVal) return 0;
    if (v >= kMaxVal) return kBuckets - 1;
    // Log scale from 1e-3 to 1e5 across 64 buckets
    const double ratio = std::log(v / kMinVal) / std::log(kMaxVal / kMinVal);
    const auto idx =
        static_cast<std::size_t>(ratio * static_cast<double>(kBuckets));
    return std::min(idx, kBuckets - 1);
  }

  static double bucketRepresentative(std::size_t idx) noexcept {
    const double ratio =
        (static_cast<double>(idx) + 0.5) / static_cast<double>(kBuckets);
    return kMinVal * std::exp(ratio * std::log(kMaxVal / kMinVal));
  }

  void add(double val, std::uint32_t flags = AnomalyNone) noexcept {
    ++count;
    sum += val;
    sumSquares += (val * val);
    minVal = std::min(minVal, val);
    maxVal = std::max(maxVal, val);
    buckets[bucketIndex(val)] += 1;
    anomalyFlags |= flags;
    // Update HLL register mask
    const auto h = std::hash<double>{}(val);
    hllMask |= (1U << (h % 32));
  }

  [[nodiscard]] double mean() const noexcept {
    return (count == 0) ? 0.0 : (sum / static_cast<double>(count));
  }

  [[nodiscard]] double stddev() const noexcept {
    if (count <= 1) return 0.0;
    const double m        = mean();
    const double variance = (sumSquares / static_cast<double>(count)) - (m * m);
    return std::sqrt(std::max(0.0, variance));
  }

  [[nodiscard]] double quantile(double q) const noexcept {
    if (count == 0) return 0.0;
    const auto target =
        static_cast<std::size_t>(std::ceil(q * static_cast<double>(count)));
    std::size_t running = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      running += buckets[i];
      if (running >= target) {
        return bucketRepresentative(i);
      }
    }
    return maxVal;
  }

  [[nodiscard]] static StatWid combine(const StatWid &a,
                                       const StatWid &b) noexcept {
    StatWid res{};
    res.count      = a.count + b.count;
    res.sum        = a.sum + b.sum;
    res.sumSquares = a.sumSquares + b.sumSquares;
    res.minVal     = std::min(a.minVal, b.minVal);
    res.maxVal     = std::max(a.maxVal, b.maxVal);
    for (std::size_t i = 0; i < kBuckets; ++i) {
      res.buckets[i] = a.buckets[i] + b.buckets[i];
    }
    res.hllMask      = a.hllMask | b.hllMask;
    res.anomalyFlags = a.anomalyFlags | b.anomalyFlags;
    return res;
  }
};

/**
 * @class TreefiladeNode
 * @brief Node in the 4-level Treefilade telemetry aggregation hierarchy.
 */
class TreefiladeNode {
public:
  explicit TreefiladeNode(std::string name);

  [[nodiscard]] const std::string &name() const noexcept { return name_; }
  [[nodiscard]] const StatWid &summary() const noexcept { return summary_; }

  TreefiladeNode *getOrCreateChild(std::string_view childName);
  void recordMetric(double val, std::uint32_t flags = AnomalyNone);
  void recompute() noexcept;

  [[nodiscard]] const std::unordered_map<std::string,
                                         std::unique_ptr<TreefiladeNode>> &
  children() const noexcept {
    return children_;
  }

private:
  std::string name_;
  StatWid summary_{};
  std::unordered_map<std::string, std::unique_ptr<TreefiladeNode>> children_;
};

/**
 * @class Treefilade
 * @brief 4-level fleet telemetry aggregator (Fleet -> Region -> Zone -> Host).
 *
 * Implements Milestone M5 Package D2:
 * - Aggregates 50,000+ host metrics into a single root summary crum.
 * - Instant O(1) p50, p90, p99 quantile queries at any level of the hierarchy.
 * - Reactive anomaly detection along d.alerting.
 */
class Treefilade {
public:
  Treefilade();

  void recordMetric(std::string_view region, std::string_view zone,
                    std::string_view host, double latencyMs,
                    std::uint32_t flags = AnomalyNone);

  [[nodiscard]] double fleetQuantile(double q) const noexcept {
    return root_.summary().quantile(q);
  }

  [[nodiscard]] double fleetMean() const noexcept {
    return root_.summary().mean();
  }

  [[nodiscard]] std::size_t totalMetrics() const noexcept {
    return root_.summary().count;
  }

  [[nodiscard]] bool hasFleetAnomaly(std::uint32_t flagMask) const noexcept {
    return (root_.summary().anomalyFlags & flagMask) != 0U;
  }

  /**
   * @brief Collects all alert messages for active anomalies down the hierarchy.
   */
  void collectAlerts(std::vector<std::string> &outAlerts) const;

  [[nodiscard]] const TreefiladeNode &root() const noexcept { return root_; }

private:
  TreefiladeNode root_{"fleet"};
};

} // namespace xanadu::enfilade

#endif // COMMON_XANADU_ENFILADE_TREEFILADE_HPP
