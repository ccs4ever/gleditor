/**
 * @file layoutfilade.hpp
 * @brief The Layoutfilade: a 2D Coordinate & Height B-Enfilade.
 *
 * An in-memory, cache-conscious 2D coordinate index over a sequence of text
 * lines and rich-media boxes, providing O(log N) virtualized scrolling,
 * screen Y <-> byte coordinate mapping, and localized incremental reflow
 * without touching downstream entries. See
 * design/enfilade/layoutfilade-virtualized-scrolling.md for the full monoid
 * algebra and runtime mechanics this implements.
 */
#ifndef GLEDITOR_ENFILADE_LAYOUTFILADE_HPP
#define GLEDITOR_ENFILADE_LAYOUTFILADE_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include <gleditor/enfilade/crum_node.hpp>
#include <gleditor/layout_box.hpp>

namespace gleditor::enfilade {

struct LayoutWid;

/**
 * @struct LayoutDsp
 * @brief Relative displacement monoid along 2D text layout flow.
 */
struct LayoutDsp {
  std::uint32_t deltaBytes{0}; ///< Offset in UTF-8 bytes progression.
  float deltaYPx{0.0F};        ///< Vertical layout coordinate progression.
  std::uint32_t deltaLines{0}; ///< Logical flow item / line count progression.

  [[nodiscard]] LayoutDsp compose(const LayoutDsp &other) const noexcept {
    return LayoutDsp{
        .deltaBytes = deltaBytes + other.deltaBytes,
        .deltaYPx   = deltaYPx + other.deltaYPx,
        .deltaLines = deltaLines + other.deltaLines,
    };
  }

  [[nodiscard]] bool isIdentity() const noexcept {
    return 0 == deltaBytes && 0.0F == deltaYPx && 0 == deltaLines;
  }

  [[nodiscard]] static LayoutWid act(const LayoutWid &w) noexcept;

  bool operator==(const LayoutDsp &) const = default;
};

/**
 * @struct LayoutWid
 * @brief Subtree aggregate summary width monoid.
 */
struct LayoutWid {
  std::uint32_t totalBytes{0};    ///< Sum of UTF-8 bytes in subtree.
  float totalHeightPx{0.0F};      ///< Sum of vertical line/media heights.
  std::uint32_t lineCount{0};     ///< Total flow entities (lines + boxes).
  float maxLineWidthPx{0.0F};     ///< Bounding max line/media width.
  std::uint32_t mediaBoxCount{0}; ///< Number of rich media boxes.

  [[nodiscard]] bool isEmpty() const noexcept {
    return 0 == totalBytes && 0 == lineCount && totalHeightPx <= 0.0F &&
           0 == mediaBoxCount;
  }

  [[nodiscard]] LayoutWid combine(const LayoutWid &other) const noexcept {
    return LayoutWid{
        .totalBytes     = totalBytes + other.totalBytes,
        .totalHeightPx  = totalHeightPx + other.totalHeightPx,
        .lineCount      = lineCount + other.lineCount,
        .maxLineWidthPx = std::max(maxLineWidthPx, other.maxLineWidthPx),
        .mediaBoxCount  = mediaBoxCount + other.mediaBoxCount,
    };
  }

  bool operator==(const LayoutWid &) const = default;
};

inline LayoutWid LayoutDsp::act(const LayoutWid &w) noexcept {
  // Extent/metric properties are translation-invariant.
  return w;
}

static_assert(DisplacementMonoid<LayoutDsp>);
static_assert(WidthMonoid<LayoutWid>);
static_assert(EnfiladeAction<LayoutDsp, LayoutWid>);

/**
 * @enum LayoutEntryKind
 * @brief Typology of flow entities handled by the Layoutfilade.
 */
enum class LayoutEntryKind : std::uint8_t {
  TextLine,    ///< Standard shaped text line.
  BlockBox,    ///< Full-width rich media block (image, video, canvas).
  InlineBox,   ///< Text line carrying an oversized inline glyph/widget.
  FloatBox,    ///< Floating sidebar/figure with text flowing beside.
  ForcedBreak, ///< Zero-length page or column break.
};

/**
 * @struct LayoutEntry
 * @brief Exactly 32-byte leaf entry (2 entries per 64-byte cache line).
 */
struct LayoutEntry {
  std::uint32_t byteLength{0};  ///< UTF-8 bytes (3 for U+FFFC, or line).
  float heightPx{0.0F};         ///< Line or box height in layout px.
  float widthPx{0.0F};          ///< Line or box width in layout px.
  float marginPx{0.0F};         ///< Margin around box.
  float baselineOffsetPx{0.0F}; ///< Inline baseline offset.
  std::uint32_t boxId{0};       ///< Opaque media handle (LayoutBox::id).
  LayoutEntryKind kind{LayoutEntryKind::TextLine};
  std::uint8_t placement{0}; ///< gleditor::BoxPlacement.
  std::uint16_t flags{0};    ///< Bitflags (e.g. HasNewline).
  float effectiveHeightPx{
      0.0F}; ///< Cached total height (heightPx + 2.0F * marginPx).

  [[nodiscard]] bool isMediaBox() const noexcept {
    return LayoutEntryKind::BlockBox == kind ||
           LayoutEntryKind::InlineBox == kind ||
           LayoutEntryKind::FloatBox == kind;
  }

  [[nodiscard]] bool isForcedBreak() const noexcept {
    return LayoutEntryKind::ForcedBreak == kind;
  }

  bool operator==(const LayoutEntry &) const = default;
};

static_assert(sizeof(LayoutEntry) == 32,
              "LayoutEntry must be exactly 32 bytes for cache line packing");

/**
 * @struct LayoutCrum
 * @brief Routing node in the Layoutfilade tree (branching factor B = 16).
 */
struct alignas(kCacheLineBytes) LayoutCrum {
  static constexpr std::size_t BranchingFactor = 16;

  LayoutDsp dsp{};
  LayoutWid wid{};
  std::uint32_t firstChild{0};
  std::uint16_t childCount{0};
  std::uint32_t firstEntry{0};
  std::uint16_t entryCount{0};
  std::uint32_t parentIndex{0};
  bool isLeaf{true};
  std::uint8_t padding[11]{0};
};

static_assert(sizeof(LayoutCrum) == 64,
              "LayoutCrum must be exactly 64 bytes (1 cache line)");

/**
 * @struct LayoutHit
 * @brief Result of a point or offset query in the Layoutfilade.
 */
struct LayoutHit {
  std::size_t entryIndex{0};  ///< Global 0-based index of entry.
  std::uint32_t startByte{0}; ///< Document byte offset where entry starts.
  float startYPx{0.0F};       ///< Document Y coordinate of entry top.
  std::uint32_t intraByteOffset{0}; ///< Offset within entry (for byte queries).
  LayoutEntry entry{};              ///< Snapshot of entry metrics.
};

/**
 * @struct VisibleRange
 * @brief Result of a viewport range query for virtualized rendering.
 */
struct VisibleRange {
  std::size_t firstEntryIndex{0};
  std::size_t lastEntryIndex{0};    ///< Inclusive.
  std::uint32_t startByteOffset{0}; ///< Starting byte offset of first entry.
  std::uint32_t endByteOffset{0};   ///< Ending byte offset of last entry.
  float startYPx{0.0F};             ///< Top coordinate of first entry.
  float endYPx{0.0F};               ///< Bottom coordinate of last entry.
  std::size_t entryCount{0};        ///< Total entries visible in window.
  std::size_t mediaBoxCount{0}; ///< Total rich-media boxes visible in window.
  std::vector<std::size_t> visibleMediaBoxIndices; ///< Media box indices.
};

/**
 * @class Layoutfilade
 * @brief 2D Coordinate & Height B-Enfilade for sublinear layout and virtualized
 *        scrolling.
 */
class Layoutfilade {
public:
  Layoutfilade() = default;

  /**
   * @brief Construct a Layoutfilade by bulk-loading pre-measured entries in
   * O(N).
   */
  static Layoutfilade buildFromEntries(const std::vector<LayoutEntry> &entries);

  /**
   * @brief Construct a Layoutfilade from UTF-8 text and rich-media boxes.
   */
  static Layoutfilade
  fromTextAndBoxes(std::string_view text, float lineHeightPx,
                   float defaultCharWidthPx                      = 8.0F,
                   const std::vector<gleditor::LayoutBox> &boxes = {});

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const LayoutWid &metrics() const noexcept {
    return crums_.empty() ? emptyWid_ : crums_[rootIndex_].wid;
  }
  [[nodiscard]] const std::vector<LayoutEntry> &entries() const noexcept {
    return entries_;
  }

  /**
   * @brief Locate the line or media box at vertical screen coordinate Y in
   * O(log N).
   */
  [[nodiscard]] std::optional<LayoutHit> findEntryAtY(float targetYPx) const;

  /**
   * @brief Locate the 0-based entry index at vertical screen coordinate Y in
   * O(log N) without constructing a full LayoutHit snapshot.
   */
  [[nodiscard]] inline std::size_t
  findEntryIndexAtY(float targetYPx) const noexcept;

  /**
   * @brief Locate the line or media box containing byte offset in O(log N).
   */
  [[nodiscard]] std::optional<LayoutHit>
  findEntryAtByte(std::uint32_t targetByteOffset) const;

  /**
   * @brief Locate an entry by its 0-based index in O(log N).
   */
  [[nodiscard]] std::optional<LayoutHit>
  findEntryByIndex(std::size_t entryIndex) const;

  /**
   * @brief Determine the visible entries and media boxes within viewport in
   * O(log N).
   */
  [[nodiscard]] VisibleRange visibleRange(float viewportTopY,
                                          float viewportBottomY) const;

  /**
   * @brief Update metrics for an existing entry and bubble changes to root in
   * O(log N). False, changing nothing, for an index past the end.
   */
  [[nodiscard]] bool updateEntry(std::size_t entryIndex,
                                 const LayoutEntry &newEntry);

  /**
   * @brief Verifies enfilade queries exactly match linear scans.
   */
  [[nodiscard]] bool
  verifyAgainstLinearScan(const std::vector<LayoutEntry> &groundTruth) const;

private:
  std::vector<LayoutCrum> crums_;
  std::vector<LayoutEntry> entries_;
  std::size_t rootIndex_{0};

  static inline const LayoutWid emptyWid_{};

  void recomputeWid(std::size_t crumIdx);
  void buildTree();
};

inline std::size_t
Layoutfilade::findEntryIndexAtY(float targetYPx) const noexcept {
  if (entries_.empty() || crums_.empty()) {
    return 0;
  }

  std::size_t currCrumIdx = rootIndex_;

  while (!crums_[currCrumIdx].isLeaf) {
    const auto &crum      = crums_[currCrumIdx];
    const auto childCount = crum.childCount;
    if (childCount == 0) return 0;
    const LayoutCrum *childPtr = &crums_[crum.firstChild];

    std::size_t chosenChild = 0;
    if (__builtin_expect(childCount == 16, 1)) {
      if (targetYPx < childPtr[0].wid.totalHeightPx) {
        chosenChild = 0;
        goto descended_child;
      }
      targetYPx -= childPtr[0].wid.totalHeightPx;
      if (targetYPx < childPtr[1].wid.totalHeightPx) {
        chosenChild = 1;
        goto descended_child;
      }
      targetYPx -= childPtr[1].wid.totalHeightPx;
      if (targetYPx < childPtr[2].wid.totalHeightPx) {
        chosenChild = 2;
        goto descended_child;
      }
      targetYPx -= childPtr[2].wid.totalHeightPx;
      if (targetYPx < childPtr[3].wid.totalHeightPx) {
        chosenChild = 3;
        goto descended_child;
      }
      targetYPx -= childPtr[3].wid.totalHeightPx;
      if (targetYPx < childPtr[4].wid.totalHeightPx) {
        chosenChild = 4;
        goto descended_child;
      }
      targetYPx -= childPtr[4].wid.totalHeightPx;
      if (targetYPx < childPtr[5].wid.totalHeightPx) {
        chosenChild = 5;
        goto descended_child;
      }
      targetYPx -= childPtr[5].wid.totalHeightPx;
      if (targetYPx < childPtr[6].wid.totalHeightPx) {
        chosenChild = 6;
        goto descended_child;
      }
      targetYPx -= childPtr[6].wid.totalHeightPx;
      if (targetYPx < childPtr[7].wid.totalHeightPx) {
        chosenChild = 7;
        goto descended_child;
      }
      targetYPx -= childPtr[7].wid.totalHeightPx;
      if (targetYPx < childPtr[8].wid.totalHeightPx) {
        chosenChild = 8;
        goto descended_child;
      }
      targetYPx -= childPtr[8].wid.totalHeightPx;
      if (targetYPx < childPtr[9].wid.totalHeightPx) {
        chosenChild = 9;
        goto descended_child;
      }
      targetYPx -= childPtr[9].wid.totalHeightPx;
      if (targetYPx < childPtr[10].wid.totalHeightPx) {
        chosenChild = 10;
        goto descended_child;
      }
      targetYPx -= childPtr[10].wid.totalHeightPx;
      if (targetYPx < childPtr[11].wid.totalHeightPx) {
        chosenChild = 11;
        goto descended_child;
      }
      targetYPx -= childPtr[11].wid.totalHeightPx;
      if (targetYPx < childPtr[12].wid.totalHeightPx) {
        chosenChild = 12;
        goto descended_child;
      }
      targetYPx -= childPtr[12].wid.totalHeightPx;
      if (targetYPx < childPtr[13].wid.totalHeightPx) {
        chosenChild = 13;
        goto descended_child;
      }
      targetYPx -= childPtr[13].wid.totalHeightPx;
      if (targetYPx < childPtr[14].wid.totalHeightPx) {
        chosenChild = 14;
        goto descended_child;
      }
      chosenChild = 15;
    } else {
      const auto lastC = static_cast<std::size_t>(childCount - 1);
      chosenChild      = lastC;
      for (std::size_t c = 0; c < lastC; ++c) {
        const float h = childPtr[c].wid.totalHeightPx;
        if (targetYPx < h) {
          chosenChild = c;
          break;
        }
        targetYPx -= h;
      }
    }

  descended_child:
    currCrumIdx = crum.firstChild + static_cast<std::uint32_t>(chosenChild);
  }

  // At leaf crum
  const auto &leaf = crums_[currCrumIdx];
  const auto count = leaf.entryCount;
  if (count == 0) {
    return 0;
  }
  const LayoutEntry *entryPtr = &entries_[leaf.firstEntry];

  if (__builtin_expect(count == 16, 1)) {
    if (targetYPx < entryPtr[0].effectiveHeightPx) return leaf.firstEntry;
    targetYPx -= entryPtr[0].effectiveHeightPx;
    if (targetYPx < entryPtr[1].effectiveHeightPx) return leaf.firstEntry + 1;
    targetYPx -= entryPtr[1].effectiveHeightPx;
    if (targetYPx < entryPtr[2].effectiveHeightPx) return leaf.firstEntry + 2;
    targetYPx -= entryPtr[2].effectiveHeightPx;
    if (targetYPx < entryPtr[3].effectiveHeightPx) return leaf.firstEntry + 3;
    targetYPx -= entryPtr[3].effectiveHeightPx;
    if (targetYPx < entryPtr[4].effectiveHeightPx) return leaf.firstEntry + 4;
    targetYPx -= entryPtr[4].effectiveHeightPx;
    if (targetYPx < entryPtr[5].effectiveHeightPx) return leaf.firstEntry + 5;
    targetYPx -= entryPtr[5].effectiveHeightPx;
    if (targetYPx < entryPtr[6].effectiveHeightPx) return leaf.firstEntry + 6;
    targetYPx -= entryPtr[6].effectiveHeightPx;
    if (targetYPx < entryPtr[7].effectiveHeightPx) return leaf.firstEntry + 7;
    targetYPx -= entryPtr[7].effectiveHeightPx;
    if (targetYPx < entryPtr[8].effectiveHeightPx) return leaf.firstEntry + 8;
    targetYPx -= entryPtr[8].effectiveHeightPx;
    if (targetYPx < entryPtr[9].effectiveHeightPx) return leaf.firstEntry + 9;
    targetYPx -= entryPtr[9].effectiveHeightPx;
    if (targetYPx < entryPtr[10].effectiveHeightPx) return leaf.firstEntry + 10;
    targetYPx -= entryPtr[10].effectiveHeightPx;
    if (targetYPx < entryPtr[11].effectiveHeightPx) return leaf.firstEntry + 11;
    targetYPx -= entryPtr[11].effectiveHeightPx;
    if (targetYPx < entryPtr[12].effectiveHeightPx) return leaf.firstEntry + 12;
    targetYPx -= entryPtr[12].effectiveHeightPx;
    if (targetYPx < entryPtr[13].effectiveHeightPx) return leaf.firstEntry + 13;
    targetYPx -= entryPtr[13].effectiveHeightPx;
    if (targetYPx < entryPtr[14].effectiveHeightPx) return leaf.firstEntry + 14;
    return leaf.firstEntry + 15;
  }

  const auto lastI = static_cast<std::size_t>(count - 1);
  for (std::size_t i = 0; i < lastI; ++i) {
    const float h = entryPtr[i].effectiveHeightPx;
    if (targetYPx < h) {
      return leaf.firstEntry + i;
    }
    targetYPx -= h;
  }

  return leaf.firstEntry + lastI;
}

} // namespace gleditor::enfilade

#endif // GLEDITOR_ENFILADE_LAYOUTFILADE_HPP
