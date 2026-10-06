#pragma once

#include <cstddef>
#include <cstdint>
#include <gleditor/text/layout.hpp>
#include <memory>
#include <string_view>

namespace gleditor::text {

struct TextFit;
struct FittedText;

/// Bounded, caller-owned LRU cache for text layouts and fitted labels. Use on
/// one thread; callers choose the capacity to match their retained UI budget.
/// Cached font owners prevent address reuse from answering a different font.
/// Non-finite dimensions are refused so cache keys retain equality semantics.
class ShapingCache {
public:
  static constexpr std::size_t defaultCapacity = 512;

  struct Stats {
    std::size_t entries{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t evictions{};
  };

  /// Capacity must be positive.
  explicit ShapingCache(std::size_t capacity = defaultCapacity);
  ~ShapingCache();
  ShapingCache(const ShapingCache &)            = delete;
  ShapingCache &operator=(const ShapingCache &) = delete;
  ShapingCache(ShapingCache &&) noexcept;
  ShapingCache &operator=(ShapingCache &&) noexcept;

  /// References remain valid until their entry is evicted or the cache clears.
  [[nodiscard]] const PageShaping &page(std::string_view text,
                                        const FontFacePtr &font,
                                        const LayoutOptions &options);
  [[nodiscard]] const FittedText &fitted(std::string_view text,
                                         const FontFacePtr &font,
                                         const TextFit &constraints);
  [[nodiscard]] Stats stats() const noexcept;
  /// Drop entries and retained fonts; cumulative counters remain available.
  void clear() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gleditor::text
