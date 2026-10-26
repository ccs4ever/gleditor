/**
 * @file projection_surface.hpp
 * @brief The arena/core/session triple a live projection needs, bundled so a
 *        lens swap rebuilds all three in one reset.
 *
 * A ProjectionSession's CellRefs are ArenaManifold addresses and its
 * VortexCore references that same arena -- so a lens swap cannot mutate the
 * session in place: the whole bundle is torn down (session, then core, then
 * arena) and rebuilt. A host holds one per projected view and calls reset()
 * when the source changes; the LensSurfaceContributor in xuzz is that host.
 */
#ifndef COMMON_XANADU_ZIGZAG_PROJECTION_SURFACE_HPP
#define COMMON_XANADU_ZIGZAG_PROJECTION_SURFACE_HPP

#include <memory>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace zigzag {

class ProjectionSurface {
public:
  explicit ProjectionSurface(std::unique_ptr<ProjectionLens> lens) {
    reset(std::move(lens));
  }

  /// Rebuild the bundle under @p lens. The fresh arena, core and session
  /// are fully constructed before the old ones are released, so a lens
  /// whose projection throws leaves the previous surface intact; after a
  /// successful reset every CellRef the old surface minted is stale.
  ProjectionSurface *reset(std::unique_ptr<ProjectionLens> lens) {
    auto arena   = std::make_unique<ArenaManifold>();
    auto core    = std::make_unique<vortex::VortexCore>(*arena);
    auto session = std::make_unique<ProjectionSession>(std::move(lens), *core);
    // Teardown order mirrors the reference chain: the session first, then
    // the core, then the arena its CellRefs address.
    session_ = std::move(session);
    core_    = std::move(core);
    arena_   = std::move(arena);
    return this;
  }

  [[nodiscard]] ArenaManifold &arena() noexcept { return *arena_; }
  [[nodiscard]] const ArenaManifold &arena() const noexcept { return *arena_; }
  [[nodiscard]] vortex::VortexCore &core() noexcept { return *core_; }
  [[nodiscard]] const vortex::VortexCore &core() const noexcept {
    return *core_;
  }
  [[nodiscard]] ProjectionSession &session() noexcept { return *session_; }
  [[nodiscard]] const ProjectionSession &session() const noexcept {
    return *session_;
  }

private:
  std::unique_ptr<ArenaManifold> arena_;
  std::unique_ptr<vortex::VortexCore> core_;
  std::unique_ptr<ProjectionSession> session_;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_PROJECTION_SURFACE_HPP
