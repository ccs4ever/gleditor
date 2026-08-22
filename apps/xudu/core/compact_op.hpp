/**
 * @file compact_op.hpp
 * @brief 64-byte cache-line aligned POD representation of an operation.
 */
#ifndef XUDU_COMPACT_OP_HPP
#define XUDU_COMPACT_OP_HPP

#include <cstdint>

#include "ops.hpp"
#include "spool.hpp"

namespace xudu {

/**
 * @struct CompactOpNode
 * @brief 64-byte cache-line aligned operation node in contiguous virtual
 * memory.
 */
struct alignas(64) CompactOpNode {
  // Tree topology & metadata (16 bytes)
  std::uint32_t parentIndex{0};      ///< Index of parent node (0 for root).
  std::uint32_t firstChildIndex{0};  ///< First branch or continuation child.
  std::uint32_t nextSiblingIndex{0}; ///< Sibling branch off the same parent.
  OpKind kind{OpKind::Insert};       ///< Operation kind.
  std::uint8_t flags{0};             ///< Reserved bit flags.
  std::uint16_t branchOrdinal{0};    ///< Branch ordinal (0 for continuation).

  // Position & geometry coordinates (24 bytes)
  std::uint32_t at{0};           ///< Position in version.
  std::uint32_t length{0};       ///< Delete/Rearrange length.
  std::uint32_t to{0};           ///< Rearrange destination.
  std::uint32_t sourceAt{0};     ///< Transclude source offset.
  std::uint32_t sourceLength{0}; ///< Transclude source length.
  std::uint32_t sourceOpIndex{0};///< Transclude source version index.

  // Content span & link reference (24 bytes)
  ScrollId scrollId{localScroll}; ///< Scroll ID of content span.
  std::uint32_t linkId{0};        ///< Link ID for OpKind::Link.
  std::uint64_t spanStart{0};     ///< Byte start in primedia scroll.
  std::uint64_t spanLength{0};    ///< Byte length in primedia scroll.

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
    return op;
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
    node.linkId        = static_cast<std::uint32_t>(op.link);
    return node;
  }

  bool operator==(const CompactOpNode &) const = default;
};

static_assert(sizeof(CompactOpNode) == 64,
              "CompactOpNode must be exactly 64 bytes (1 cache line)");

} // namespace xudu

#endif // XUDU_COMPACT_OP_HPP
