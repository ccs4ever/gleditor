/**
 * @file projection_to_zz.cpp
 * @brief projectionToZz: ProjectionSession -> ZzStructureDocument.
 */

#include "common/xanadu/zigzag/projection_to_zz.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace zigzag {

namespace {

/// The generic-dim projection dims project under their own names; the
/// slice view reads containment as X and sequence as Y.
std::string isoNow() {
  const auto t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[24];
  const auto n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return {buf, buf + n};
}

} // namespace

ZzStructureDocument projectionToZz(const std::string_view label,
                                   ProjectionSession &session,
                                   ArenaManifold &arena) {
  ZzStructureDocument doc;
  doc.meta.name    = std::string{label};
  doc.meta.created = isoNow();
  doc.meta.tags    = {"lens"};
  doc.focus        = session.roots().empty() ? CellID{0} : session.roots()[0];
  doc.view         = {.x_dimension = "d.contains",
                      .y_dimension = "d.seq",
                      .z_dimension = "d.role"};
  doc.scene.cell_radius         = 0.30F;
  doc.scene.neighborhood_radius = 3;

  const auto slots = arena.cells();
  doc.cells.reserve(slots.size());
  for (const auto &slot : slots) {
    if (slot.birthOp == 0) {
      continue; // slot 0 is the manifold's own head, not a cell
    }
    const CellRef ref{slot.birthOp};
    Cell c;
    c.id   = ref;
    c.data = arena.textOf(ref);
    if (const auto role =
            arena.linked(ref, arena.dimensionNamed("d.role"), Posward);
        role.has_value()) {
      c.role = arena.textOf(*role);
    }
    // dimensionsOf hands back the cell's links per dim -- the whole
    // rank topology in one span.
    for (const auto &link : arena.dimensionsOf(ref)) {
      if (link.pos == noCell && link.neg == noCell) {
        continue;
      }
      c.dimensions[arena.textOf(link.dim)] = LinkPairs{
          .pos = link.pos == noCell ? 0 : static_cast<CellID>(link.pos),
          .neg = link.neg == noCell ? 0 : static_cast<CellID>(link.neg)};
    }
    doc.cells.emplace(ref, std::move(c));
  }
  for (const auto &[dim, gen] : session.lens().dimensions()) {
    if (gen.has_value()) {
      const auto name                       = genericDimName(*gen);
      doc.dimension_meta[std::string{name}] = {.label       = std::string{name},
                                               .description = "generic dim"};
    }
    doc.dimension_meta[dim] = {.label = dim, .description = "lens dim"};
  }
  return doc;
}

} // namespace zigzag
