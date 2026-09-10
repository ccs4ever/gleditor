/**
 * @file compact_op.hpp
 * @brief 64-byte cache-line aligned POD representation of an operation.
 */
#ifndef XUDU_COMPACT_OP_HPP
#define XUDU_COMPACT_OP_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "ops.hpp"
#include "spool.hpp"

namespace xanadu {

/**
 * @struct CompactOpNode
 * @brief 64-byte cache-line aligned operation node in contiguous virtual
 * memory.
 */
struct alignas(64) CompactOpNode {
  // Tree topology & metadata (8 bytes)
  //
  // Only the edge pointing *up* lives here. firstChildIndex and
  // nextSiblingIndex used to sit beside parentIndex, which made appending a
  // child a write into the already-stored parent -- and sealed segments are
  // mapped PROT_READ. Both are derivable from parentIndex, so they moved to
  // SegmentedOpsSpool::tree, rebuilt on adopt. See design R10.
  std::uint32_t parentIndex{0};   ///< Index of parent node (0 for root).
  OpKind kind{OpKind::Insert};    ///< Operation kind.
  std::uint8_t flags{0};          ///< Reserved bit flags.
  std::uint16_t branchOrdinal{0}; ///< Branch ordinal (0 for continuation).

  // Position & geometry coordinates (24 bytes)
  std::uint32_t at{0};            ///< Position in version.
  std::uint32_t length{0};        ///< Delete/Rearrange length.
  std::uint32_t to{0};            ///< Rearrange destination.
  std::uint32_t sourceAt{0};      ///< Transclude source offset.
  std::uint32_t sourceLength{0};  ///< Transclude source length.
  std::uint32_t sourceOpIndex{0}; ///< Transclude source version index.

  // Content span, link reference & typed value (32 bytes)
  ScrollId scrollId{localScroll}; ///< Scroll ID of content span.
  std::uint32_t linkId{0};        ///< Link ID for OpKind::Link.
  std::uint64_t spanStart{0};     ///< Byte start in primedia scroll.
  std::uint64_t spanLength{0};    ///< Byte length in primedia scroll.
  /// The eight bytes the tree edges vacated, kept as a named field rather
  /// than left as tail padding so that what reaches disk is defined. Zero
  /// until R6's scalar cells give it a meaning.
  std::uint64_t value{0};

  [[nodiscard]] PrimediaSpan span() const {
    return PrimediaSpan{scrollId, spanStart, spanLength};
  }

  void setSpan(const PrimediaSpan &s) {
    scrollId   = s.scroll;
    spanStart  = s.start;
    spanLength = s.length;
  }

  [[nodiscard]] Op toOp(const MicroversionId &parentVersion,
                        const MicroversionId &sourceVersion) const {
    Op op;
    op.kind         = kind;
    op.parent       = parentVersion;
    op.at           = at;
    op.length       = length;
    op.to           = to;
    op.span         = span();
    op.source       = sourceVersion;
    op.sourceAt     = sourceAt;
    op.sourceLength = sourceLength;
    op.link         = linkId;
    op.flags        = flags;
    op.value        = value;
    return op;
  }

  /**
   * @brief The branch ordinal that reaches @p produces from its parent.
   *
   * Zero for a continuation, otherwise the ordinal the branch was filed under.
   * Not the last segment's branch field, which is a common way to get this
   * wrong: 1a2 continues 1a1 and so is ordinal zero, while its last segment
   * still says branch a. What settles it is whether the name is one step on
   * from the parent or a fork away from it.
   *
   * This is what lets a node's name be worked out from the tree rather than
   * stored beside it -- see SegmentedOpsSpool::adoptSegmentNodes(), which has
   * a file of nodes and no names at all.
   */
  static std::uint16_t branchOrdinalFor(const MicroversionId &produces) {
    const auto parent = produces.parent();
    if (produces == parent.next()) {
      return 0;
    }
    return static_cast<std::uint16_t>(produces.segments().back().branch);
  }

  static CompactOpNode fromOp(const Op &op, const std::uint32_t parentIdx,
                              const std::uint32_t sourceIdx = 0,
                              const std::uint16_t branchOrd = 0) {
    CompactOpNode node;
    node.parentIndex   = parentIdx;
    node.branchOrdinal = branchOrd;
    node.kind          = op.kind;
    node.at            = op.at;
    node.length        = op.length;
    node.to            = op.to;
    node.setSpan(op.span);
    node.sourceOpIndex = sourceIdx;
    node.sourceAt      = op.sourceAt;
    node.sourceLength  = op.sourceLength;
    // Not a cast. The node's field is 32 bits and R2 makes that load-bearing
    // -- a Structure op's linkId is a dimension's cell reference, which is an
    // ops-spool index -- so a link id that does not fit is a fact worth
    // stopping for rather than a silent change of which link is meant.
    if (op.link > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument(
          "link id " + std::to_string(op.link) +
          " does not fit the operation node's 32-bit link field");
    }
    node.linkId = static_cast<std::uint32_t>(op.link);
    node.flags  = op.flags;
    node.value  = op.value;
    return node;
  }

  bool operator==(const CompactOpNode &) const = default;
};

static_assert(sizeof(CompactOpNode) == 64,
              "CompactOpNode must be exactly 64 bytes (1 cache line)");
static_assert(alignof(CompactOpNode) == 64,
              "CompactOpNode must be cache-line aligned");
// alignas(64) would round a 56-byte struct up to 64 on its own, so the size
// assertion above cannot by itself catch a field going missing. This one can:
// it fails if the declared fields stop reaching offset 56.
static_assert(offsetof(CompactOpNode, value) == 56,
              "the eight bytes vacated by the tree edges must stay accounted "
              "for, not become tail padding");

} // namespace xanadu

#endif // XUDU_COMPACT_OP_HPP
