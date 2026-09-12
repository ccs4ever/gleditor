/**
 * @file spanfilade.hpp
 * @brief The True Spanfilade: 1D Interval B-Enfilade for Transclusion
 * Discovery.
 *
 * Provides sublinear O(log N + K) range stabbing over primedia scroll
 * coordinates [start, start + length), eliminating linear O(N) scans in
 * Version::occurrencesOf() and O(D * N) pairwise comparisons in LinkBeams /
 * placeTransclusions().
 *
 * Strict Compliance:
 * - Ruling R8: Derived replay product in memory. Indexing and range stabbing
 *   NEVER mint operations on disk.
 * - Ruling R9: Mathematical equivalence guaranteed with raw linear scans and
 *   placeTransclusions() via verifyAgainstLinearScan().
 * - Ruling R12 & U3: CellSlot remains strictly 32 bytes. Manifold content runs
 *   in contentArena are indexed to (CellRef, SpanIndex).
 * - Ruling V3: Ephemeral derived index with wholesale rebuild-on-demand policy.
 */
#ifndef XANADU_ENFILADE_SPANFILADE_HPP
#define XANADU_ENFILADE_SPANFILADE_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/xanadu/enfilade/crum_node.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/version.hpp"

namespace zigzag {
class Manifold;
} // namespace zigzag

namespace xanadu::enfilade {

struct SpanWid;

/**
 * @struct SpanDsp
 * @brief Relative coordinate displacement monoid along a primedia scroll.
 */
struct SpanDsp {
  int64_t delta{0};

  [[nodiscard]] SpanDsp compose(const SpanDsp &other) const noexcept {
    return SpanDsp{delta + other.delta};
  }
  [[nodiscard]] bool isIdentity() const noexcept { return 0 == delta; }
  [[nodiscard]] SpanWid act(const SpanWid &w) const noexcept;
  bool operator==(const SpanDsp &) const = default;
};

/**
 * @struct SpanWid
 * @brief Bounding hull interval width monoid [minStart, maxEnd).
 */
struct SpanWid {
  uint64_t minStart{std::numeric_limits<uint64_t>::max()};
  uint64_t maxEnd{0};
  uint32_t count{0};

  [[nodiscard]] bool isEmpty() const noexcept {
    return minStart >= maxEnd || 0 == count;
  }

  [[nodiscard]] SpanWid combine(const SpanWid &other) const noexcept {
    if (isEmpty()) {
      return other;
    }
    if (other.isEmpty()) {
      return *this;
    }
    return SpanWid{std::min(minStart, other.minStart),
                   std::max(maxEnd, other.maxEnd), count + other.count};
  }

  [[nodiscard]] bool overlaps(const uint64_t start,
                              const uint64_t end) const noexcept {
    if (isEmpty() || start >= end) {
      return false;
    }
    return !(end <= minStart || start >= maxEnd);
  }

  bool operator==(const SpanWid &) const = default;
};

inline SpanWid SpanDsp::act(const SpanWid &w) const noexcept {
  if (w.isEmpty() || 0 == delta) {
    return w;
  }
  const auto newMin = (delta >= 0)
                          ? (w.minStart + static_cast<uint64_t>(delta))
                          : (w.minStart > static_cast<uint64_t>(-delta)
                                 ? w.minStart - static_cast<uint64_t>(-delta)
                                 : 0ULL);
  const auto newMax = (delta >= 0)
                          ? (w.maxEnd + static_cast<uint64_t>(delta))
                          : (w.maxEnd > static_cast<uint64_t>(-delta)
                                 ? w.maxEnd - static_cast<uint64_t>(-delta)
                                 : 0ULL);
  return SpanWid{newMin, newMax, w.count};
}

static_assert(DisplacementMonoid<SpanDsp>);
static_assert(WidthMonoid<SpanWid>);
static_assert(EnfiladeAction<SpanDsp, SpanWid>);

/**
 * @struct SpanEntry
 * @brief 32-byte cache-conscious leaf payload descriptor.
 *
 * Exactly two SpanEntry instances fit in a single 64-byte cache line.
 */
struct SpanEntry {
  uint64_t start{0};     ///< Primedia scroll start address.
  uint64_t length{0};    ///< Span byte length.
  uint32_t docId{0};     ///< Document or view index.
  uint32_t docOffset{0}; ///< Offset in document concatext.
  uint32_t cellDense{0}; ///< denseOf(CellRef) if zzstructure (Ruling U3).
  uint16_t spanIndex{0}; ///< Index within cell's content run (Ruling U3).
  uint16_t flags{0};     ///< Bit 0: isCell, Bit 1: isWithheld.

  [[nodiscard]] uint64_t end() const noexcept { return start + length; }
  [[nodiscard]] bool empty() const noexcept { return 0 == length; }
  [[nodiscard]] bool isCell() const noexcept { return (flags & 1U) != 0; }
  [[nodiscard]] bool isWithheld() const noexcept { return (flags & 2U) != 0; }
  bool operator==(const SpanEntry &) const = default;
};
static_assert(sizeof(SpanEntry) == 32);

/// 64-byte aligned B-enfilade crum node with branching factor 8.
using SpanCrum = CrumNode<SpanDsp, SpanWid, 8>;

/**
 * @class ScrollSpanfilade
 * @brief Single-scroll 1D interval B-enfilade.
 */
class ScrollSpanfilade {
public:
  ScrollSpanfilade() = default;

  void clear();
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<SpanEntry> &entries() const noexcept {
    return entries_;
  }

  /// Insert a single entry.
  void insert(const SpanEntry &entry);

  /// Bulk-load and construct a perfectly balanced B-tree in O(N log N).
  void bulkLoad(std::vector<SpanEntry> newEntries);

  /// Sublinear O(log N + K) range stabbing query.
  void stab(uint64_t qStart, uint64_t qEnd,
            std::vector<SpanEntry> &results) const;

  /// Find all entries overlapping @p qStart to @p qEnd.
  [[nodiscard]] std::vector<SpanEntry> query(uint64_t qStart,
                                             uint64_t qEnd) const {
    std::vector<SpanEntry> results;
    stab(qStart, qEnd, results);
    return results;
  }

private:
  void stabNode(uint32_t nodeIdx, int64_t parentDsp, uint64_t qStart,
                uint64_t qEnd, std::vector<SpanEntry> &results) const;

  std::vector<SpanCrum> nodes_;
  std::vector<SpanEntry> entries_;
  uint32_t rootIndex_{0};
};

/**
 * @class Spanfilade
 * @brief Universal multi-scroll transclusion enfilade.
 */
class Spanfilade {
public:
  Spanfilade() = default;

  void clear();
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t totalSpans() const noexcept;
  [[nodiscard]] std::size_t scrollCount() const noexcept {
    return scrolls_.size();
  }

  /// Index a single span entry on @p scroll.
  void indexSpan(ScrollId scroll, const SpanEntry &entry);

  /// Index all non-empty primedia pieces from @p ver into the Spanfilade.
  void indexVersion(uint32_t docId, const Version &ver);

  /// Bulk-index multiple open views.
  void indexViews(const std::vector<const Version *> &views);

  /// Index all cell content runs in @p manifold (Ruling U3).
  void indexManifold(const zigzag::Manifold &manifold);

  /// Rebuild/balance all per-scroll interval enfilades.
  void build();

  /// Sublinear O(log N + K) range stabbing across all indexed documents and
  /// cells.
  [[nodiscard]] std::vector<SpanEntry>
  findIntersections(const PrimediaSpan &span) const;

  /**
   * @brief Transclusion occurrences query matching Version::occurrencesOf().
   *
   * Finds everywhere in document @p docId that @p span's content appears,
   * merging contiguous adjacent runs into seamless extents (Ruling R9).
   */
  [[nodiscard]] std::vector<Extent> occurrencesOf(const PrimediaSpan &span,
                                                  uint32_t docId = 0) const;

  /**
   * @brief Compute pairwise transclusion bands across open views.
   *
   * Matches the output of link_layout::placeTransclusions().
   */
  void placeTransclusions(const std::vector<const Version *> &views,
                          std::vector<TransclusionPair> &pairs) const;

  /**
   * @brief Verify that Spanfilade query produces the exact same extents as
   *        Version::occurrencesOf() (Ruling R9).
   */
  [[nodiscard]] bool verifyAgainstLinearScan(const PrimediaSpan &query,
                                             const Version &ver,
                                             uint32_t docId = 0) const;

  // Static Factory Helpers
  static Spanfilade fromVersion(const Version &ver, uint32_t docId = 0);
  static Spanfilade fromViews(const std::vector<const Version *> &views);
  static Spanfilade fromManifold(const zigzag::Manifold &manifold);

private:
  std::unordered_map<ScrollId, ScrollSpanfilade> scrolls_;
};

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_SPANFILADE_HPP
