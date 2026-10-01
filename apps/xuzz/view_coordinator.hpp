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
#include "xudu/bridge_coordinator.hpp"
#include "xudu/views.hpp"
#include "zigzag/zigzag_visualizer.hpp"

namespace xuzz {

class ViewCoordinator {
public:
  ViewCoordinator(xudu::Views &views,
                  std::shared_ptr<zigzag::ZigzagVisualizer> visualizer,
                  xudu::BridgeCoordinator &bridgeCoordinator,
                  RendererRef renderer, AppStateRef state);

  void setViewMode(ViewMode mode);
  [[nodiscard]] ViewMode viewMode() const noexcept { return mode_; }

  void cycleViewMode();
  void apply();

private:
  xudu::Views &views_;
  std::shared_ptr<zigzag::ZigzagVisualizer> visualizer_;
  xudu::BridgeCoordinator &bridgeCoordinator_;
  RendererRef renderer_;
  AppStateRef state_;
  ViewMode mode_{ViewMode::Unified};
};

} // namespace xuzz

#endif // XUZZ_VIEW_COORDINATOR_HPP
