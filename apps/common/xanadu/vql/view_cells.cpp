#include "common/xanadu/vql/view_cells.hpp"

#include <algorithm>
#include <limits>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimVector;

std::optional<ViewDimensions>
findViewDimensions(const zigzag::vortex::VortexCore &core) {
  ViewDimensions dims;
  const std::array<std::pair<zigzag::DimRef *, std::string_view>, 9> names{{
      {&dims.marker, viewMarkerDimension},
      {&dims.of, viewOfDimension},
      {&dims.rule, viewRuleDimension},
      {&dims.kind, viewKindDimension},
      {&dims.to, viewToDimension},
      {&dims.neg, viewNegDimension},
      {&dims.bidir, viewBidirDimension},
      {&dims.dir, viewDirDimension},
      {&dims.base, viewBaseDimension},
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
