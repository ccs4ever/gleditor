#pragma once

#include <cstddef>
#include <cstdint>

namespace gleditor::text {

struct ShapingStats {
  std::uint64_t layoutCalls{};
  /// Includes cluster shaping on glyph-cache misses as well as text layout.
  std::uint64_t harfbuzzCalls{};
  std::uint64_t fallbackCalls{};
  /// Bytes submitted to layoutPage, before its height-budgeted slicing.
  std::uint64_t inputBytes{};

  friend bool operator==(const ShapingStats &, const ShapingStats &) = default;
};

namespace detail {
void recordLayout(std::size_t bytes);
void recordHarfBuzz(bool fallback);
} // namespace detail

/// Opt-in, allocation-free capture on the calling thread. Background document
/// layout is deliberately excluded from render-thread frame measurements.
/// Nested captures record into the innermost enabled scope; destroying it
/// restores the previous capture. A disabled scope leaves that capture alone.
class ShapingStatsScope {
public:
  explicit ShapingStatsScope(bool enabled = true);
  ~ShapingStatsScope();

  ShapingStatsScope(const ShapingStatsScope &)            = delete;
  ShapingStatsScope &operator=(const ShapingStatsScope &) = delete;
  ShapingStatsScope(ShapingStatsScope &&)                 = delete;
  ShapingStatsScope &operator=(ShapingStatsScope &&)      = delete;

  [[nodiscard]] ShapingStats stats() const { return collected; }

private:
  friend void detail::recordLayout(std::size_t bytes);
  friend void detail::recordHarfBuzz(bool fallback);
  ShapingStats collected{};
  ShapingStatsScope *previous{};
  bool enabled{};
};

} // namespace gleditor::text
