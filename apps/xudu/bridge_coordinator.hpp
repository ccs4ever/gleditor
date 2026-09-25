/**
 * @file bridge_coordinator.hpp
 * @brief Xudu composition boundary for an embedded Zigzag presentation.
 */
#ifndef XUDU_BRIDGE_COORDINATOR_HPP
#define XUDU_BRIDGE_COORDINATOR_HPP

#include <cstdint>
#include <functional>
#include <span>
#include <utility>

#include <gleditor/a11y/publisher.hpp>
#include <gleditor/renderer.hpp>

#include "beams.hpp"
#include "common/xanadu/bridge_config.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"
#include "satelloid.hpp"

namespace xudu {

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
  /// Called with every span of the cell's content, in order: a cell's text is
  /// a run, and focusing only its first span would find the wrong passage.
  /// The span is the manifold's; copy it to keep it past the call.
  using DocumentFocusHandler = std::function<void(
      zigzag::CellRef cell, std::span<const PrimediaSpan> content)>;

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
  void setDocumentFocusHandler(DocumentFocusHandler handler) {
    documentFocusHandler_ = std::move(handler);
  }

  /// Activate a cell in Zigzag, resolving its span and notifying Xudu to focus.
  void activateCell(zigzag::CellRef cell);

  /// Notify that a document link to a Zigzag cell was activated.
  void onDocumentLinkActivated(zigzag::CellRef cell);

  /// The attached surface's manifold, or null when none is attached.
  [[nodiscard]] const zigzag::Manifold *manifold() const noexcept;

  [[nodiscard]] bool isCellLocked(zigzag::CellRef cell) const noexcept;
  [[nodiscard]] std::optional<xanadu::TranscopyrightDescriptor>
  cellRoyalty(zigzag::CellRef cell) const noexcept;
  bool unlockCell(zigzag::CellRef cell);

  /// Remove registrations and clear LinkBeams' cross-domain state.
  void detach() noexcept;
  /// Apply a changed surface revision at a host state-update boundary.
  void synchronize();

  /// Atomically apply bridge runtime configuration across LinkBeams and
  /// surface.
  void applyConfig(xanadu::BridgeRuntimeConfig config);
  [[nodiscard]] const xanadu::BridgeRuntimeConfig &config() const noexcept {
    return config_;
  }

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
  xanadu::BridgeRuntimeConfig config_{};
  SatelloidOverlay *satelloidOverlay_{nullptr};
  CellActivationHandler cellActivationHandler_;
  DocumentFocusHandler documentFocusHandler_;
};

} // namespace xudu

#endif // XUDU_BRIDGE_COORDINATOR_HPP
