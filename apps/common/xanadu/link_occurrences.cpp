/**
 * @file link_occurrences.cpp
 * @brief Exact per-member occurrences of one link.
 */
#include "common/xanadu/link_occurrences.hpp"

#include <gleditor/logging.hpp>

#include "common/xanadu/store.hpp"

namespace xanadu {

std::vector<PieceMatch> exactOccurrences(std::span<const PrimediaSpan> pieces,
                                         const PrimediaSpan &member) {
  // One run in progress: where it sits in the text, and which addresses of the
  // member it has covered so far.
  struct Run {
    Extent range;
    std::uint64_t firstAddress{};
    std::uint64_t endAddress{};
  };
  std::vector<Run> runs;
  if (member.empty()) {
    return {};
  }

  std::uint32_t seen = 0;
  for (const auto &piece : pieces) {
    const auto shared = piece.intersect(member);
    if (!shared.empty()) {
      const auto at =
          seen + static_cast<std::uint32_t>(shared.start - piece.start);
      const auto end = at + static_cast<std::uint32_t>(shared.length);
      // Same scroll is already guaranteed by intersecting one member; what
      // makes two pieces one quotation is that the text and the addresses
      // both carry straight on.
      if (!runs.empty() && runs.back().range.end == at &&
          runs.back().endAddress == shared.start) {
        runs.back().range.end  = end;
        runs.back().endAddress = shared.end();
      } else {
        runs.push_back(Run{.range        = Extent{.start = at, .end = end},
                           .firstAddress = shared.start,
                           .endAddress   = shared.end()});
      }
    }
    seen += static_cast<std::uint32_t>(piece.length);
  }

  std::vector<PieceMatch> matches;
  matches.reserve(runs.size());
  for (const auto &run : runs) {
    const bool whole =
        run.firstAddress == member.start && run.endAddress == member.end();
    matches.push_back(
        PieceMatch{.range    = run.range,
                   .coverage = whole ? Coverage::Full : Coverage::Partial});
  }
  return matches;
}

namespace {

std::vector<LinkMember> resolveSide(const LinkSide side,
                                    const std::vector<PrimediaSpan> &spans,
                                    std::span<const DocumentView> documents,
                                    std::span<const CellView> cells) {
  std::vector<LinkMember> members;
  members.reserve(spans.size());
  for (std::uint32_t index = 0; index < spans.size(); ++index) {
    LinkMember member{.side = side, .index = index, .span = spans[index]};
    for (const auto &document : documents) {
      for (const auto &match :
           exactOccurrences(document.text.pieces(), member.span)) {
        member.occurrences.push_back(
            Occurrence{.site     = DocumentSite{.store   = document.store,
                                                .version = document.version,
                                                .range   = match.range},
                       .coverage = match.coverage});
      }
    }
    for (const auto &view : cells) {
      for (const auto cell : view.cells) {
        for (const auto &match :
             exactOccurrences(view.manifold.contentOf(cell), member.span)) {
          member.occurrences.push_back(
              Occurrence{.site     = CellSite{.store   = view.store,
                                              .version = view.version,
                                              .cell    = cell,
                                              .range   = match.range},
                         .coverage = match.coverage});
        }
      }
    }
    members.push_back(std::move(member));
  }
  return members;
}

} // namespace

std::expected<LinkOccurrences, LinkQueryError>
resolveLinkOccurrences(const Store &store, const zigzag::CellRef id,
                       std::span<const DocumentView> documents,
                       std::span<const CellView> cells) {
  const auto found = store.links().find(id);
  if (store.links().end() == found) {
    GLEDITOR_LOG_DEBUG("xudu.links", "no link {} to resolve", id);
    return std::unexpected(LinkQueryError::LinkNotFound);
  }
  const auto &link = found->second;
  return LinkOccurrences{
      .key   = LinkKey{.authority = store.documentId(), .id = id},
      .link  = link,
      .left  = resolveSide(LinkSide::Left, link.left, documents, cells),
      .right = resolveSide(LinkSide::Right, link.right, documents, cells)};
}

} // namespace xanadu
