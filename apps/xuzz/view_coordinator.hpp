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

private:
  xanadu::Views &views_;
  std::shared_ptr<xanadu::view::SlicePresentation> slice_;
  xanadu::BridgeCoordinator &bridgeCoordinator_;
  RendererRef renderer_;
  AppStateRef state_;
  ViewMode mode_{ViewMode::Unified};
};

} // namespace xuzz

#endif // XUZZ_VIEW_COORDINATOR_HPP
