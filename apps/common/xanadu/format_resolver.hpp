/**
 * @file format_resolver.hpp
 * @brief Universal content-addressed formatting extraction service.
 *
 * Resolves presentation attributes (bold, italic, alignment, etc.) attached via
 * LinkType::Format links across both Xudu documents and Zigzag manifold cells.
 */
#ifndef XUDU_FORMAT_RESOLVER_HPP
#define XUDU_FORMAT_RESOLVER_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gleditor/glyphcache/types.hpp>
#include <gleditor/layout_box.hpp>

#include "format.hpp"
#include "spool.hpp"
#include "zigzag/manifold.hpp"

namespace xanadu {

class Store;
class Version;

/**
 * @class FormatResolver
 * @brief Zero-copy formatting extraction service for documents and cells.
 */
class FormatResolver {
public:
  struct FormattingResult {
    std::vector<gleditor::DecoratedRange> decoratedRanges;
    std::vector<gleditor::BlockStyleRange> blockStyles;
  };

  explicit FormatResolver(const Store &store) noexcept;

  /// Resolve formatting over an arbitrary sequence of primedia spans.
  [[nodiscard]] FormattingResult
  resolveSpans(std::span<const PrimediaSpan> spans) const;

  /// Resolve formatting over a document Version.
  [[nodiscard]] FormattingResult resolveVersion(const Version &version) const;

  /// Resolve formatting over a Zigzag manifold cell.
  [[nodiscard]] FormattingResult resolveCell(const zigzag::Manifold &manifold,
                                             zigzag::CellRef cell) const;

  /// Compute the 16-bit format attribute bitmask for a manifold cell.
  [[nodiscard]] std::uint16_t
  computeCellFormatFlags(const zigzag::Manifold &manifold,
                         zigzag::CellRef cell) const;

  /// Compute the 16-bit format attribute bitmask for an arbitrary sequence of
  /// spans.
  [[nodiscard]] std::uint16_t
  computeFormatFlags(std::span<const PrimediaSpan> spans) const;

  /// Batch update cached formatFlags on all cells in @p manifold.
  void updateManifoldFormatFlags(zigzag::Manifold &manifold) const;

private:
  struct CachedFormatLink {
    FormatAttribute attribute;
    std::vector<PrimediaSpan> targets;
    std::optional<gleditor::Decoration> decoration;
    std::optional<gleditor::TextAlign> align;
  };

  std::vector<CachedFormatLink> formatLinks_;
};

} // namespace xanadu

#endif // XUDU_FORMAT_RESOLVER_HPP
