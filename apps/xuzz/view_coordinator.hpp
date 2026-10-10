/**
 * @file view_coordinator.hpp
 * @brief Dynamic presentation coordinator for Unified, Xanadoc, and Zigzag view
 * modes.
 */
#ifndef XUZZ_VIEW_COORDINATOR_HPP
#define XUZZ_VIEW_COORDINATOR_HPP

#include <memory>
#include <utility>

#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>

#include "cli.hpp"
#include "common/ui/view/slice_presentation.hpp"
#include "common/ui/view/slice_presentation_switch.hpp"
#include "common/ui/xanadoc/bridge_coordinator.hpp"
#include "common/ui/xanadoc/views.hpp"

namespace xuzz {

class ViewCoordinator {
public:
  ViewCoordinator(xanadu::Views &views,
                  std::shared_ptr<xanadu::view::SlicePresentation> slice,
                  xanadu::BridgeCoordinator &bridgeCoordinator,
                  RendererRef renderer, AppStateRef state);

  void setViewMode(ViewMode mode);
  [[nodiscard]] ViewMode viewMode() const noexcept { return mode_; }

  void cycleViewMode();
  void apply();

  /// The presentations the slice can be shown by, the legacy visualizer
  /// among them.
  void setPresentations(
      std::shared_ptr<xanadu::view::SlicePresentationSwitch> presentations) {
    presentations_ = std::move(presentations);
  }
  /**
   * @brief Show the slice bound by the presentation its
   *        slice.<id>.presentation setting in @p layout names
   *        (view-system-implementation-plan.md §4.6, U2).
   *
   * The legacy visualizer is the default, and what shows a slice whose
   * setting names a view this program does not have.
   */
  void applySlicePresentation(const xanadu::Store &layout);

private:
  xanadu::Views &views_;
  std::shared_ptr<xanadu::view::SlicePresentation> slice_;
  std::shared_ptr<xanadu::view::SlicePresentationSwitch> presentations_;
  xanadu::BridgeCoordinator &bridgeCoordinator_;
  RendererRef renderer_;
  AppStateRef state_;
  ViewMode mode_{ViewMode::Unified};
};

} // namespace xuzz

#endif // XUZZ_VIEW_COORDINATOR_HPP
