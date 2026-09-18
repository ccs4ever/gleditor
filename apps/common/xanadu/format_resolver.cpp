/**
 * @file format_resolver.cpp
 * @brief Universal content-addressed formatting extraction implementation.
 */
#include "format_resolver.hpp"

#include <utility>

#include "store.hpp"
#include "version.hpp"

namespace xanadu {

namespace {

struct Extent {
  std::uint32_t start{0};
  std::uint32_t end{0};
  [[nodiscard]] bool empty() const noexcept { return start >= end; }
};

std::vector<Extent> findOccurrences(const std::span<const PrimediaSpan> spans,
                                    const PrimediaSpan &target) {
  std::vector<Extent> found;
  if (target.empty() || spans.empty()) {
    return found;
  }
  std::uint32_t seen = 0;
  for (const auto &run : spans) {
    const auto shared = run.intersect(target);
    if (!shared.empty()) {
      const auto into = static_cast<std::uint32_t>(shared.start - run.start);
      found.push_back(Extent{.start = seen + into,
                             .end = seen + into +
                                    static_cast<std::uint32_t>(shared.length)});
    }
    seen += static_cast<std::uint32_t>(run.length);
  }

  std::vector<Extent> merged;
  for (const auto &extent : found) {
    if (!merged.empty() && merged.back().end == extent.start) {
      merged.back().end = extent.end;
    } else {
      merged.push_back(extent);
    }
  }
  return merged;
}

} // namespace

FormatResolver::FormatResolver(const Store &store) noexcept {
  for (const auto &[linkId, link] : store.links()) {
    if (LinkType::Format != link.type) {
      continue;
    }
    const auto attrOpt = store.formatAttributeOf(link);
    if (!attrOpt) {
      continue;
    }
    formatLinks_.push_back(CachedFormatLink{
        .attribute  = *attrOpt,
        .targets    = link.left,
        .decoration = decorationFromFormatAttribute(*attrOpt),
        .align      = textAlignFromFormatAttribute(*attrOpt),
    });
  }
}

FormatResolver::FormattingResult
FormatResolver::resolveSpans(const std::span<const PrimediaSpan> spans) const {
  FormattingResult result;
  if (spans.empty() || formatLinks_.empty()) {
    return result;
  }

  for (const auto &link : formatLinks_) {
    if (link.decoration) {
      const auto mask = gleditor::decorationBit(*link.decoration);
      for (const auto &target : link.targets) {
        for (const auto &extent : findOccurrences(spans, target)) {
          if (!extent.empty()) {
            result.decoratedRanges.push_back(gleditor::DecoratedRange{
                .start       = extent.start,
                .end         = extent.end,
                .decorations = mask,
            });
          }
        }
      }
    } else if (link.align) {
      for (const auto &target : link.targets) {
        for (const auto &extent : findOccurrences(spans, target)) {
          if (!extent.empty()) {
            result.blockStyles.push_back(gleditor::BlockStyleRange{
                .start = extent.start,
                .end   = extent.end,
                // link is const-bound with no reassignment since the
                // link.align guard above; findOccurrences doesn't touch it.
                .align =
                    *link.align, // NOLINT(bugprone-unchecked-optional-access)
            });
          }
        }
      }
    }
  }

  return result;
}

FormatResolver::FormattingResult
FormatResolver::resolveVersion(const Version &version) const {
  return resolveSpans(version.pieces());
}

FormatResolver::FormattingResult
FormatResolver::resolveCell(const zigzag::Manifold &manifold,
                            const zigzag::CellRef cell) const {
  return resolveSpans(manifold.contentOf(cell));
}

std::uint16_t FormatResolver::computeFormatFlags(
    const std::span<const PrimediaSpan> spans) const {
  std::uint16_t flags = 0;
  if (spans.empty() || formatLinks_.empty()) {
    return 0;
  }
  for (const auto &link : formatLinks_) {
    for (const auto &target : link.targets) {
      bool intersects = false;
      for (const auto &span : spans) {
        if (!span.intersect(target).empty()) {
          intersects = true;
          break;
        }
      }
      if (intersects) {
        flags |= static_cast<std::uint16_t>(
            1U << static_cast<std::uint8_t>(link.attribute));
        break;
      }
    }
  }
  return flags;
}

std::uint16_t
FormatResolver::computeCellFormatFlags(const zigzag::Manifold &manifold,
                                       const zigzag::CellRef cell) const {
  return computeFormatFlags(manifold.contentOf(cell));
}

void FormatResolver::updateManifoldFormatFlags(
    zigzag::Manifold &manifold) const {
  for (const auto &cell : manifold.cells()) {
    const auto flags = computeCellFormatFlags(manifold, cell.birthOp);
    manifold.setFormatFlags(cell.birthOp, flags);
  }
}

} // namespace xanadu
