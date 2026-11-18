#include "common/xanadu/zigzag/arena_copy_out.hpp"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace zigzag {

std::string toString(const CopyOutRefused &refused) {
  return std::format("result cell {} cannot be copied out of the program: {}",
                     refused.cell, toString(refused.reason));
}

std::expected<CopyOut, CopyOutRefused>
CopyOut::capture(const ArenaManifold &arena, const Mark &floor,
                 std::span<const CellRef> cells) {
  CopyOut out;
  std::unordered_set<CellRef> seen;
  const auto dimensions = arena.dimensions();
  // Cells to describe: the ones asked for, then whatever they link to that the
  // program minted. Index loop, since describing a cell can append to it.
  std::vector<CellRef> pending;
  const auto enqueue = [&](const CellRef ref) {
    if (isEphemeral(ref) && seen.insert(ref).second) pending.push_back(ref);
  };
  for (const CellRef ref : cells) enqueue(ref);

  for (std::size_t at = 0; at < pending.size(); ++at) {
    const CellRef ref = pending[at];
    const auto dense  = arena.denseOf(ref);
    if (!dense || *dense < floor.cellCount) continue;

    const auto refuse = [&](const CopyOutRefusal why) {
      return std::unexpected{CopyOutRefused{.reason = why, .cell = ref}};
    };
    if (arena.isProxy(ref)) return refuse(CopyOutRefusal::ProxyCell);
    if (arena.isQuoteOccurrence(ref)) return refuse(CopyOutRefusal::QuotedCell);
    if (std::ranges::find(dimensions, ref) != dimensions.end())
      return refuse(CopyOutRefusal::DimensionCell);
    const auto slot = arena.slot(ref);
    if (!slot) return refuse(CopyOutRefusal::QuotedCell);
    const auto kind = static_cast<xanadu::ValueKind>(slot->valueKind);
    if (kind == xanadu::ValueKind::OpHandle ||
        kind == xanadu::ValueKind::ExternRef ||
        kind == xanadu::ValueKind::ForeignOpHandle)
      return refuse(CopyOutRefusal::HandleValue);

    Described described{.source    = ref,
                        .valueKind = slot->valueKind,
                        .valueBits = slot->valueBits};
    if (const auto quote = arena.quotedContent(ref)) {
      // Bytes quoted from a store stay where they are, so the copy names the
      // same space; a space attached since the mark is detached by release().
      for (std::uint32_t id = 1; id <= floor.spaceCount; ++id)
        if (const auto space = arena.spaceAt(id);
            space && space->store == quote->store) {
          described.quoteSpace = id;
          break;
        }
      if (!described.quoteSpace) return refuse(CopyOutRefusal::QuotedCell);
      for (const auto &span : quote->spans)
        described.run.push_back(Piece{.span = span});
    } else {
      for (const auto &span : arena.contentOf(ref)) {
        if (span.scroll == xanadu::scratchScroll)
          described.run.push_back(
              Piece{.span      = span,
                    .scratch   = std::string{arena.scratchTextOf(span)},
                    .isScratch = true});
        else
          described.run.push_back(Piece{.span = span});
      }
    }
    for (const auto &link : arena.dimensionsOf(ref)) {
      // A dimension minted under the mark is un-named by release(); an edge
      // on one cannot be written back.
      const auto dimDense = arena.denseOf(link.dim);
      if (!dimDense || *dimDense >= floor.cellCount)
        return refuse(CopyOutRefusal::LinkedCell);
      for (const DimVector dir : {DimVector::POS, DimVector::NEG}) {
        const auto neighbour = link.neighbor(dir);
        if (!neighbour) continue;
        // A neighbour that predates the mark would need a write the release
        // undoes; the run's older-cell check keeps this unreachable, and it
        // stays a refusal rather than an assumption.
        const auto neighbourDense = arena.denseOf(*neighbour);
        if (!neighbourDense || *neighbourDense < floor.cellCount)
          return refuse(CopyOutRefusal::LinkedCell);
        described.edges.push_back(
            Edge{.dim = link.dim, .dir = dir, .target = *neighbour});
        enqueue(*neighbour);
      }
    }
    out.cells_.push_back(std::move(described));
  }
  return out;
}

std::unordered_map<CellRef, CellRef>
CopyOut::materialize(ArenaManifold &arena) const {
  std::unordered_map<CellRef, CellRef> replacements;
  replacements.reserve(cells_.size());
  for (const auto &described : cells_) {
    std::vector<xanadu::PrimediaSpan> spans;
    spans.reserve(described.run.size());
    for (const auto &piece : described.run)
      spans.push_back(piece.isScratch ? arena.intern(piece.scratch)
                                      : piece.span);
    const CellRef fresh = described.quoteSpace
                              ? arena.makeQuote(*described.quoteSpace, spans)
                              : arena.makeCell();
    if (!described.quoteSpace && !spans.empty())
      expectWritten(arena.setContent(fresh, spans));
    if (described.valueKind != 0 || described.valueBits != 0)
      expectWritten(arena.setValueBits(
          fresh, static_cast<xanadu::ValueKind>(described.valueKind),
          described.valueBits));
    replacements.emplace(described.source, fresh);
  }
  // Links last: an edge needs both of its ends to exist.
  for (const auto &described : cells_)
    for (const auto &edge : described.edges)
      expectWritten(arena.link(replacements.at(described.source), edge.dim,
                               edge.dir, replacements.at(edge.target)));
  return replacements;
}

void applyCopyOut(const std::unordered_map<CellRef, CellRef> &replacements,
                  std::vector<CellRef> &cells) noexcept {
  for (auto &cell : cells)
    if (const auto found = replacements.find(cell); found != replacements.end())
      cell = found->second;
}

} // namespace zigzag
