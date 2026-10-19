/**
 * @file projection_session.hpp
 * @brief The source-agnostic framework that drives a ProjectionLens into a
 *        ZigZag arena and owns the ExternalKey <-> CellRef identity map
 *        (design/anything-to-zigzag-lens.md §4.4, §9A.2).
 *
 * A session projects a lens into a VortexCore's ArenaManifold -- the SAME arena
 * the query/logic runtimes read -- so VQL, Vlog/vprolog and vlace all see the
 * projected cells (§9A.2's project-into-bound-arena contract). It mints the
 * lens's dimensions, then its nodes (recording key -> CellRef), then its edges
 * (resolving both ends through the map), then attaches each node's attribute
 * ranks. The identity map is exposed both ways so a query can recover the
 * source key from a bound cell (the extkey/2 predicate, §9A.3).
 */
#ifndef ZIGZAG_PROJECTION_SESSION_HPP
#define ZIGZAG_PROJECTION_SESSION_HPP

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

class ProjectionSession {
public:
  /// Project @p lens into @p core's arena. Mints dimensions, nodes, edges and
  /// attribute ranks, recording the identity map. Deterministic given a
  /// deterministic lens. The projected cells are immediately visible to any
  /// query or logic goal run against @p core.
  ProjectionSession(std::unique_ptr<ProjectionLens> lens,
                    zigzag::vortex::VortexCore &core);

  /// The cell for a source key, if projected.
  [[nodiscard]] std::optional<CellRef>
  cellFor(const ExternalKey &key) const noexcept;

  /// The source key for a cell, if it is a projected node (not an attribute
  /// cell). Backs the extkey/2 predicate (§9A.3).
  [[nodiscard]] std::optional<ExternalKey> keyFor(CellRef cell) const noexcept;

  /// The root cells the lens projected, in mint order (a source may be a
  /// forest). The first is the natural entry point for a walk.
  [[nodiscard]] const std::vector<CellRef> &roots() const noexcept {
    return roots_;
  }

  /// How many cells (nodes + attribute cells) the projection minted.
  [[nodiscard]] std::size_t cellCount() const noexcept {
    return keyToCell_.size();
  }

  [[nodiscard]] const ProjectionLens &lens() const noexcept { return *lens_; }

private:
  std::unique_ptr<ProjectionLens> lens_;
  zigzag::vortex::VortexCore &core_;
  std::unordered_map<ExternalKey, CellRef> keyToCell_;
  std::unordered_map<CellRef, ExternalKey> cellToKey_;
  std::vector<CellRef> roots_;

  void project();
};

} // namespace zigzag

#endif // ZIGZAG_PROJECTION_SESSION_HPP
