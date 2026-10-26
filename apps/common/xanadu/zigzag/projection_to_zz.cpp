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
  if (!session.roots().empty()) {
    doc.focus = session.roots()[0];
  }
  doc.view         = {.x_dimension = "d.contains",
                      .y_dimension = "d.seq",
                      .z_dimension = "d.role"};
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
    if (const auto roleDim = arena.dimensionNamed("d.role")) {
      if (const auto role = arena.linked(ref, *roleDim, Posward)) {
        c.role = arena.textOf(*role);
      }
    }
    // dimensionsOf hands back the cell's links per dim -- the whole
    // rank topology in one span.
    for (const auto &link : arena.dimensionsOf(ref)) {
      auto pos = arena.linked(ref, link.dim, Posward);
      auto neg = arena.linked(ref, link.dim, Negward);
      if (pos || neg) {
        c.dimensions[arena.textOf(link.dim)] = {.pos = pos, .neg = neg};
      }
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
