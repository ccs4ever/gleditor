#include "common/xanadu/vql/view_cells.hpp"

#include <algorithm>
#include <limits>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimVector;

std::optional<ViewDimensions>
findViewDimensions(const zigzag::vortex::VortexCore &core) {
  ViewDimensions dims;
  const std::array<std::pair<zigzag::DimRef *, std::string_view>, 10> names{{
      {&dims.marker, viewMarkerDimension},
      {&dims.of, viewOfDimension},
      {&dims.rule, viewRuleDimension},
      {&dims.kind, viewKindDimension},
      {&dims.to, viewToDimension},
      {&dims.neg, viewNegDimension},
      {&dims.bidir, viewBidirDimension},
      {&dims.dir, viewDirDimension},
      {&dims.base, viewBaseDimension},
      {&dims.epoch, viewEpochDimension},
  }};
  for (const auto &[slot, name] : names) {
    const auto found = core.findDimension(name);
    if (!found) return std::nullopt;
    *slot = *found;
  }
  return dims;
}

ViewDimensions ensureViewDimensions(zigzag::vortex::VortexCore &core) {
  return ViewDimensions{
      .marker = core.findOrMintDimension(viewMarkerDimension),
      .of     = core.findOrMintDimension(viewOfDimension),
      .rule   = core.findOrMintDimension(viewRuleDimension),
      .kind   = core.findOrMintDimension(viewKindDimension),
      .to     = core.findOrMintDimension(viewToDimension),
      .neg    = core.findOrMintDimension(viewNegDimension),
      .bidir  = core.findOrMintDimension(viewBidirDimension),
      .dir    = core.findOrMintDimension(viewDirDimension),
      .base   = core.findOrMintDimension(viewBaseDimension),
      .epoch  = core.findOrMintDimension(viewEpochDimension),
  };
}

bool carriesViewMarker(const zigzag::ArenaManifold &arena, const CellRef cell) {
  if (!arena.contains(cell)) return false;
  return std::ranges::any_of(
      arena.dimensionsOf(cell), [&](const zigzag::DimLink &link) {
        return (link.pos != zigzag::noCell || link.neg != zigzag::noCell) &&
               arena.textOf(link.dim) == viewMarkerDimension;
      });
}

bool carriesForeignViewMarker(const zigzag::ArenaManifold &arena,
                              const std::optional<ViewDimensions> &dims,
                              const CellRef cell) {
  if (!carriesViewMarker(arena, cell)) return false;
  return !dims || !arena.linked(cell, dims->marker, DimVector::POS);
}

std::optional<HandleParts> handleParts(const zigzag::ArenaManifold &arena,
                                       const ViewDimensions &dims,
                                       const CellRef cell) {
  if (!arena.contains(cell)) return std::nullopt;
  const auto sourceBox = arena.linked(cell, dims.marker, DimVector::POS);
  const auto viewBox   = arena.linked(cell, dims.of, DimVector::POS);
  if (!sourceBox || !viewBox) return std::nullopt;
  const auto cellNamedBy = [&](const CellRef box) -> std::optional<CellRef> {
    const auto number = arena.asInt64(box);
    if (!number || *number < 0 ||
        static_cast<std::uint64_t>(*number) >
            std::numeric_limits<CellRef>::max())
      return std::nullopt;
    const auto named = static_cast<CellRef>(*number);
    if (!arena.contains(named)) return std::nullopt;
    return named;
  };
  const auto source = cellNamedBy(*sourceBox);
  const auto view   = cellNamedBy(*viewBox);
  if (!source || !view) return std::nullopt;
  return HandleParts{.source    = *source,
                     .view      = *view,
                     .sourceBox = *sourceBox,
                     .viewBox   = *viewBox};
}

} // namespace xanadu::vql

namespace xanadu::vql {

namespace {

/// The cell a box names, without requiring that it still exists (a stale
/// number is what the witness is for).
std::optional<CellRef> numberIn(const zigzag::ArenaManifold &arena,
                                const CellRef box) {
  const auto number = arena.asInt64(box);
  if (!number || *number < 0 ||
      static_cast<std::uint64_t>(*number) > std::numeric_limits<CellRef>::max())
    return std::nullopt;
  return static_cast<CellRef>(*number);
}

} // namespace

void recordViewWitnesses(zigzag::ArenaManifold &arena,
                         const ViewDimensions &dims,
                         const std::span<const CellRef> cells) {
  for (const CellRef cell : cells) {
    if (!arena.contains(cell)) continue;
    if (arena.linked(cell, dims.epoch, DimVector::POS)) continue;
    const auto parts = handleParts(arena, dims, cell);
    if (!parts) continue;
    CellRef previous = cell;
    for (const CellRef named : {parts->source, parts->view}) {
      const auto born = arena.epochOfCell(named);
      if (!born) break;
      const CellRef witness =
          arena.makeScalarCell(static_cast<std::int64_t>(*born));
      zigzag::expectWritten(arena.link(previous, dims.epoch, false, witness));
      previous = witness;
    }
  }
}

std::optional<std::string> staleHandleReason(const zigzag::ArenaManifold &arena,
                                             const ViewDimensions &dims,
                                             const CellRef cell,
                                             const bool witnessed) {
  if (!arena.contains(cell)) return std::nullopt;
  const auto sourceBox = arena.linked(cell, dims.marker, DimVector::POS);
  const auto viewBox   = arena.linked(cell, dims.of, DimVector::POS);
  if (!sourceBox || !viewBox) return std::nullopt;
  const auto first = arena.linked(cell, dims.epoch, DimVector::POS);
  const auto second =
      first ? arena.linked(*first, dims.epoch, DimVector::POS) : std::nullopt;
  const auto number = [&](const std::optional<CellRef> witness) {
    return witness ? arena.asInt64(*witness) : std::nullopt;
  };
  if (witnessed && (!number(first) || !number(second)))
    return "VQL a dynamic view handle has no valid epoch witness: it was not "
           "made by a query of this session, so whether its source and view "
           "numbers still name their cells cannot be told";
  const auto check = [&](const CellRef box,
                         const std::optional<std::int64_t> recorded,
                         const char *what) -> std::optional<std::string> {
    const auto named = numberIn(arena, box);
    if (!named) return std::nullopt;
    const auto now = arena.epochOfCell(*named);
    if (!now)
      return std::string{"VQL a dynamic view handle's "} + what +
             " cell is no longer a cell: it was released";
    if (recorded && static_cast<std::int64_t>(*now) != *recorded)
      return std::string{"VQL a dynamic view handle's "} + what +
             " cell was released and its number reused: the handle no longer "
             "names the cell it was made with";
    return std::nullopt;
  };
  if (const auto why = check(*sourceBox, number(first), "source")) return why;
  return check(*viewBox, number(second), "view");
}

} // namespace xanadu::vql
