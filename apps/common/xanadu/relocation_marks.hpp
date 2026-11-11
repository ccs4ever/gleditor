#ifndef COMMON_XANADU_RELOCATION_MARKS_HPP
#define COMMON_XANADU_RELOCATION_MARKS_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu {

/// Resolve a dimension without creating it. The core's d.dims walk is
/// authoritative when available; link evidence also finds rank-only dims in
/// null-base scratch arenas, which are intentionally absent from arenaDims_.
[[nodiscard]] inline std::optional<zigzag::DimRef>
findDimensionByName(const zigzag::ArenaManifold &arena,
                    const std::string_view name,
                    const zigzag::vortex::VortexCore *core = nullptr) {
  if (core) {
    if (const auto found = core->findDimension(name)) return found;
  }

  for (const auto dim : arena.dimensions())
    if (arena.textOf(dim) == name) return dim;

  for (const auto &slot : arena.cells()) {
    for (const auto &edge : arena.dimensionsOf(slot.birthOp)) {
      if (arena.textOf(edge.dim) == name) return edge.dim;
      if (arena.textOf(edge.dim) != "d.dims") continue;
      for (const auto neighbor : std::array{edge.pos, edge.neg})
        if (neighbor != zigzag::noCell && arena.textOf(neighbor) == name)
          return neighbor;
    }
  }
  return std::nullopt;
}

/// Decode a slot already declared a reference by a marker or wing position.
/// Integer shape alone does not declare a reference.
[[nodiscard]] inline std::optional<zigzag::CellRef>
relocationTarget(const zigzag::ArenaManifold &arena,
                 const zigzag::CellRef slot) {
  std::optional<zigzag::CellRef> target;
  switch (arena.valueKindOf(slot)) {
  case ValueKind::Int64:
    if (const auto value = arena.asInt64(slot);
        value && *value >= 0 &&
        static_cast<std::uint64_t>(*value) <=
            std::numeric_limits<zigzag::CellRef>::max())
      target = static_cast<zigzag::CellRef>(*value);
    break;
  case ValueKind::OpHandle:
    target = arena.handleTarget(slot);
    break;
  default:
    break;
  }
  return target && arena.contains(*target) ? target : std::nullopt;
}

/// Add a fresh relocation marker to a cell. Markers are unique because link()
/// gives each marker cell one owner under the dimension's two-degree topology.
inline zigzag::ArenaManifold *
markCellRelocation(zigzag::ArenaManifold &arena,
                   zigzag::vortex::VortexCore &core,
                   const zigzag::CellRef cell) {
  const auto marker = arena.makeCell(cellRelocationMarker);
  zigzag::expectWritten(
      arena.link(cell, core.findOrMintDimension(cellRelocationDimension),
                 zigzag::DimVector::POS, marker));
  return &arena;
}

/// A ref-valued Int64 cell with its relocation declaration.
[[nodiscard]] inline zigzag::CellRef refCell(zigzag::ArenaManifold &arena,
                                             zigzag::vortex::VortexCore &core,
                                             const zigzag::CellRef ref) {
  const auto cell = arena.makeScalarCell(static_cast<std::int64_t>(ref));
  markCellRelocation(arena, core, cell);
  return cell;
}

/// Cells carrying the exact d.mesh.relocation -> "cell" declaration.
[[nodiscard]] inline std::vector<zigzag::CellRef>
markedRelocationSlots(const zigzag::ArenaManifold &arena) {
  const auto markerDim = findDimensionByName(arena, cellRelocationDimension);
  std::vector<zigzag::CellRef> slots;
  if (!markerDim) return slots;

  for (const auto &slot : arena.cells()) {
    const auto cell = slot.birthOp;
    if (!arena.holdsOwn(cell)) continue;
    const auto marker = arena.linked(cell, *markerDim, zigzag::DimVector::POS);
    if (marker && arena.textOf(*marker) == cellRelocationMarker)
      slots.push_back(cell);
  }
  return slots;
}

/// Referents that must be included as extra promotion roots.
[[nodiscard]] inline std::vector<zigzag::CellRef>
harvestRelocationTargets(const zigzag::ArenaManifold &arena) {
  std::vector<zigzag::CellRef> targets;
  for (const auto slot : markedRelocationSlots(arena))
    if (const auto target = relocationTarget(arena, slot);
        target && zigzag::isEphemeral(*target))
      targets.push_back(*target);
  return targets;
}

} // namespace xanadu

#endif // COMMON_XANADU_RELOCATION_MARKS_HPP
