/**
 * @file universal_link_endpoint.hpp
 * @brief Universal link endpoints and cache-aligned transclusion structures
 *        unifying linear documents and multidimensional Zigzag cells.
 */
#ifndef COMMON_XANADU_UNIVERSAL_LINK_ENDPOINT_HPP
#define COMMON_XANADU_UNIVERSAL_LINK_ENDPOINT_HPP

#include <cstdint>
#include <vector>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {

/**
 * @enum LinkTargetKind
 * @brief Discriminator for whether an endpoint targets a 2D document concatext
 *        range or a 3D Zigzag manifold cell.
 */
enum class LinkTargetKind : std::uint8_t {
  Document   = 0, ///< 2D Doc concatext byte range
  ZigzagCell = 1, ///< 3D Zigzag CellRef + intra-cell character range
};

/**
 * @struct UniversalLinkEnd
 * @brief One end of a link, addressing either an open document concatext range
 *        or a Zigzag manifold cell.
 *
 * Designed at exactly 16 bytes (alignas(4)). Under the System V AMD64 ABI,
 * this integer aggregate is classified as INTEGER, INTEGER and passed across
 * hot loops in two 64-bit general-purpose registers (%rsi, %rdx) with zero
 * stack spills.
 *
 * Member field ordering {targetId/doc, start, end, spanIndex, kind, flags}
 * places 'end' in the third position, preserving complete aggregate
 * initialization compatibility with legacy LinkEnd{doc, start, end} call sites
 * and avoiding silent offset-length corruption. An anonymous union over
 * targetId and doc guarantees 100% source compatibility with legacy .doc member
 * accesses.
 */
struct alignas(4) UniversalLinkEnd {
  union {
    std::uint32_t targetId{
        0};            ///< docIndex if Document, or CellRef if ZigzagCell
    std::uint32_t doc; ///< Legacy document index alias
  };
  std::uint32_t start{0};     ///< byte offset in doc concatext or cell text
  std::uint32_t end{0};       ///< end offset (start + length)
  std::uint16_t spanIndex{0}; ///< index in cell's content run (0 for doc)
  std::uint8_t kind{0};       ///< LinkTargetKind (0: Document, 1: Cell)
  std::uint8_t flags{0};      ///< bit 0: isWithheld, bit 1: isEphemeral

  [[nodiscard]] constexpr std::uint32_t length() const noexcept {
    return end >= start ? (end - start) : 0U;
  }
  [[nodiscard]] constexpr bool isDocument() const noexcept {
    return kind == static_cast<std::uint8_t>(LinkTargetKind::Document);
  }
  [[nodiscard]] constexpr bool isCell() const noexcept {
    return kind == static_cast<std::uint8_t>(LinkTargetKind::ZigzagCell);
  }
  [[nodiscard]] constexpr bool isWithheld() const noexcept {
    return (flags & 1U) != 0;
  }
  [[nodiscard]] constexpr bool isEphemeral() const noexcept {
    return (flags & 2U) != 0;
  }

  [[nodiscard]] constexpr zigzag::CellRef cell() const noexcept {
    return isCell() ? static_cast<zigzag::CellRef>(targetId) : zigzag::noCell;
  }

  static constexpr UniversalLinkEnd
  forDocument(std::uint32_t docIndex, std::uint32_t startOffset,
              std::uint32_t endOffset) noexcept {
    return UniversalLinkEnd{
        .targetId  = docIndex,
        .start     = startOffset,
        .end       = endOffset,
        .spanIndex = 0,
        .kind      = static_cast<std::uint8_t>(LinkTargetKind::Document),
        .flags     = 0,
    };
  }

  static constexpr UniversalLinkEnd
  forCell(zigzag::CellRef cellRef, std::uint32_t startOffset,
          std::uint32_t endOffset, std::uint16_t spanIdx = 0,
          std::uint8_t cellFlags = 0) noexcept {
    return UniversalLinkEnd{
        .targetId  = static_cast<std::uint32_t>(cellRef),
        .start     = startOffset,
        .end       = endOffset,
        .spanIndex = spanIdx,
        .kind      = static_cast<std::uint8_t>(LinkTargetKind::ZigzagCell),
        .flags     = cellFlags,
    };
  }

  constexpr bool operator==(const UniversalLinkEnd &other) const noexcept {
    return targetId == other.targetId && start == other.start &&
           end == other.end && spanIndex == other.spanIndex &&
           kind == other.kind && flags == other.flags;
  }
};
static_assert(sizeof(UniversalLinkEnd) == 16);
static_assert(alignof(UniversalLinkEnd) == 4);

/**
 * @struct UniversalTransclusionPair
 * @brief An emergent transclusion where identical primedia spans appear across
 *        open views (documents or cells).
 *
 * Exactly 64 bytes (alignas(8)). Every instance occupies exactly one 64-byte
 * CPU cache line, eliminating cache-line straddling and split loads across
 * worker threads while retaining full backward compatibility for .from, .to,
 * and .span field accesses.
 */
struct alignas(8) UniversalTransclusionPair {
  UniversalLinkEnd from;
  UniversalLinkEnd to;
  PrimediaSpan span;
  std::uint64_t _reserved{0}; ///< Pads to exactly 64 bytes (1 cache line)

  [[nodiscard]] constexpr std::uint32_t fromTargetId() const noexcept {
    return from.targetId;
  }
  [[nodiscard]] constexpr std::uint32_t fromOffset() const noexcept {
    return from.start;
  }
  [[nodiscard]] constexpr std::uint32_t toTargetId() const noexcept {
    return to.targetId;
  }
  [[nodiscard]] constexpr std::uint32_t toOffset() const noexcept {
    return to.start;
  }
  [[nodiscard]] constexpr std::uint32_t length() const noexcept {
    return from.length();
  }
  [[nodiscard]] constexpr ScrollId scrollId() const noexcept {
    return span.scroll;
  }
  [[nodiscard]] constexpr std::uint64_t spanStart() const noexcept {
    return span.start;
  }

  bool operator==(const UniversalTransclusionPair &other) const noexcept {
    return from == other.from && to == other.to && span == other.span;
  }
};
static_assert(sizeof(UniversalTransclusionPair) == 64);
static_assert(alignof(UniversalTransclusionPair) == 8);

/**
 * @struct CompactTransclusionPair
 * @brief High-density 32-byte representation of an emergent transclusion pair.
 *
 * Packs two pairs into a single 64-byte cache line for streaming serialization.
 */
struct alignas(8) CompactTransclusionPair {
  std::uint32_t fromTargetId{0}; ///< docIndex or CellRef
  std::uint32_t fromOffset{0};   ///< byte offset in doc concatext or cell
  std::uint32_t toTargetId{0};   ///< docIndex or CellRef
  std::uint32_t toOffset{0};     ///< byte offset in doc concatext or cell
  std::uint32_t length{0};       ///< transcluded span byte length
  ScrollId scrollId{0};          ///< primedia scroll ID
  std::uint64_t spanStart{0};    ///< primedia scroll start coordinate

  [[nodiscard]] UniversalLinkEnd from() const noexcept {
    return UniversalLinkEnd{
        .targetId  = fromTargetId,
        .start     = fromOffset,
        .end       = fromOffset + length,
        .spanIndex = 0,
        .kind      = 0,
        .flags     = 0,
    };
  }
  [[nodiscard]] UniversalLinkEnd to() const noexcept {
    return UniversalLinkEnd{
        .targetId  = toTargetId,
        .start     = toOffset,
        .end       = toOffset + length,
        .spanIndex = 0,
        .kind      = 0,
        .flags     = 0,
    };
  }
  [[nodiscard]] PrimediaSpan span() const noexcept {
    return PrimediaSpan{scrollId, spanStart, length};
  }

  [[nodiscard]] static CompactTransclusionPair
  fromUniversal(const UniversalTransclusionPair &u) noexcept {
    return CompactTransclusionPair{
        .fromTargetId = u.from.targetId,
        .fromOffset   = u.from.start,
        .toTargetId   = u.to.targetId,
        .toOffset     = u.to.start,
        .length       = u.length(),
        .scrollId     = u.span.scroll,
        .spanStart    = u.span.start,
    };
  }

  [[nodiscard]] UniversalTransclusionPair toUniversal() const noexcept {
    return UniversalTransclusionPair{
        .from      = from(),
        .to        = to(),
        .span      = span(),
        ._reserved = 0,
    };
  }

  bool operator==(const CompactTransclusionPair &) const = default;
};
static_assert(sizeof(CompactTransclusionPair) == 32);
static_assert(alignof(CompactTransclusionPair) == 8);

/**
 * @struct UniversalLinkedPair
 * @brief A link whose two ends landed in open views: one connection to draw.
 */
struct UniversalLinkedPair {
  std::uint64_t link{0};
  LinkType type{LinkType::Comment};
  ProminenceTier tier{ProminenceTier::Author};
  UniversalLinkEnd from; ///< Left end list.
  UniversalLinkEnd to;   ///< Right end list.

  [[nodiscard]] constexpr std::uint64_t linkId() const noexcept { return link; }
  bool operator==(const UniversalLinkedPair &) const = default;
};

/**
 * @struct UniversalHalfLink
 * @brief A link with one end in an open view and the other in none.
 */
struct UniversalHalfLink {
  std::uint64_t link{0};
  LinkType type{LinkType::Comment};
  ProminenceTier tier{ProminenceTier::Author};
  UniversalLinkEnd here;
  std::vector<PrimediaSpan> elsewhere;

  [[nodiscard]] constexpr std::uint64_t linkId() const noexcept { return link; }
  bool operator==(const UniversalHalfLink &) const = default;
};

} // namespace xanadu

#endif // COMMON_XANADU_UNIVERSAL_LINK_ENDPOINT_HPP
