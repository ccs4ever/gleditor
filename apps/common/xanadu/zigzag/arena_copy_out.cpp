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
  for (const CellRef ref : cells) {
    if (!isEphemeral(ref) || !seen.insert(ref).second) continue;
    const auto dense = arena.denseOf(ref);
    if (!dense || *dense < floor.cellCount) continue;

    const auto refuse = [&](const CopyOutRefusal why) {
      return std::unexpected{CopyOutRefused{.reason = why, .cell = ref}};
    };
    if (arena.isProxy(ref)) return refuse(CopyOutRefusal::ProxyCell);
    if (arena.isQuoteOccurrence(ref) || arena.quotedContent(ref))
      return refuse(CopyOutRefusal::QuotedCell);
    if (std::ranges::find(dimensions, ref) != dimensions.end())
      return refuse(CopyOutRefusal::DimensionCell);
    const auto slot = arena.slot(ref);
    if (!slot) return refuse(CopyOutRefusal::QuotedCell);
    if (slot->linkCount != 0) return refuse(CopyOutRefusal::LinkedCell);
    const auto kind = static_cast<xanadu::ValueKind>(slot->valueKind);
    if (kind == xanadu::ValueKind::OpHandle ||
        kind == xanadu::ValueKind::ExternRef ||
        kind == xanadu::ValueKind::ForeignOpHandle)
      return refuse(CopyOutRefusal::HandleValue);

    Described described{.source    = ref,
                        .valueKind = slot->valueKind,
                        .valueBits = slot->valueBits};
    for (const auto &span : arena.contentOf(ref)) {
      if (span.scroll == xanadu::scratchScroll)
        described.run.push_back(
            Piece{.span      = span,
                  .scratch   = std::string{arena.scratchTextOf(span)},
                  .isScratch = true});
      else
        described.run.push_back(Piece{.span = span});
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
    const CellRef fresh = arena.makeCell();
    if (!described.run.empty()) {
      std::vector<xanadu::PrimediaSpan> spans;
      spans.reserve(described.run.size());
      for (const auto &piece : described.run)
        spans.push_back(piece.isScratch ? arena.intern(piece.scratch)
                                        : piece.span);
      expectWritten(arena.setContent(fresh, spans));
    }
    if (described.valueKind != 0 || described.valueBits != 0)
      expectWritten(arena.setValueBits(
          fresh, static_cast<xanadu::ValueKind>(described.valueKind),
          described.valueBits));
    replacements.emplace(described.source, fresh);
  }
  return replacements;
}

void applyCopyOut(const std::unordered_map<CellRef, CellRef> &replacements,
                  std::vector<CellRef> &cells) noexcept {
  for (auto &cell : cells)
    if (const auto found = replacements.find(cell); found != replacements.end())
      cell = found->second;
}

} // namespace zigzag
