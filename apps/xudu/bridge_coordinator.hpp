/**
 * @file bridge_coordinator.hpp
 * @brief Xudu composition boundary for an embedded Zigzag presentation.
 */
#ifndef XUDU_BRIDGE_COORDINATOR_HPP
#define XUDU_BRIDGE_COORDINATOR_HPP

#include <cstdint>
#include <functional>
#include <utility>

#include <gleditor/a11y/publisher.hpp>
#include <gleditor/renderer.hpp>

#include "beams.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"
#include "satelloid.hpp"

namespace xanadu {}
namespace xudu {
using namespace ::xanadu;

/**
 * @brief Connects a presentation surface to Xudu's renderer and LinkBeams.
 *
 * The coordinator owns no surface. The host owns the surface and must detach
 * it before either object is destroyed, making the renderer's non-owning
 * contributor pointers and callback lifetime explicit.
 */
class BridgeCoordinator {
public:
  using CellActivationHandler =
      std::function<void(zigzag::CellRef cell, bool altHeld)>;

  BridgeCoordinator(LinkBeams &links, RendererRef renderer,
                    gleditor::a11y::Publisher &accessibility) noexcept;
  ~BridgeCoordinator();

  BridgeCoordinator(const BridgeCoordinator &)            = delete;
  BridgeCoordinator &operator=(const BridgeCoordinator &) = delete;

  /// Attach and register @p surface. Returns false if already attached.
  bool attach(xanadu::ZigzagPresentationSurface &surface);
  /// Route satelloid selection into the currently attached surface.
  void connectSatelloidNavigation(SatelloidOverlay &overlay);
  void setCellActivationHandler(CellActivationHandler handler) {
    cellActivationHandler_ = std::move(handler);
  }
  /// Remove registrations and clear LinkBeams' cross-domain state.
  void detach() noexcept;
  /// Apply a changed surface revision at a host state-update boundary.
  void synchronize();

  [[nodiscard]] bool attached() const noexcept { return surface_ != nullptr; }
  [[nodiscard]] bool dirty() const noexcept { return dirty_; }
  [[nodiscard]] std::uint64_t synchronizedRevision() const noexcept {
    return synchronizedRevision_;
  }

private:
  LinkBeams &links_;
  RendererRef renderer_;
  gleditor::a11y::Publisher &accessibility_;
  xanadu::ZigzagPresentationSurface *surface_{nullptr};
  std::uint64_t synchronizedRevision_{0};
  bool dirty_{false};
  SatelloidOverlay *satelloidOverlay_{nullptr};
  CellActivationHandler cellActivationHandler_;
};

} // namespace xudu

#endif // XUDU_BRIDGE_COORDINATOR_HPP
