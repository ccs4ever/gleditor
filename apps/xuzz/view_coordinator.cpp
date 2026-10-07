/**
 * @file view_coordinator.cpp
 * @brief Dynamic presentation coordinator for Unified, Xanadoc, and Zigzag view
 * modes.
 */
#include "view_coordinator.hpp"

#include <iostream>

namespace xuzz {

ViewCoordinator::ViewCoordinator(
    xanadu::Views &views, std::shared_ptr<zigzag::ZigzagVisualizer> visualizer,
    xanadu::BridgeCoordinator &bridgeCoordinator, RendererRef renderer,
    AppStateRef state)
    : views_(views), visualizer_(std::move(visualizer)),
      bridgeCoordinator_(bridgeCoordinator), renderer_(std::move(renderer)),
      state_(std::move(state)) {}

void ViewCoordinator::setViewMode(const ViewMode mode) {
  if (mode_ == mode) {
    return;
  }
  mode_ = mode;
  apply();
}

void ViewCoordinator::cycleViewMode() {
  switch (mode_) {
  case ViewMode::Unified:
    setViewMode(ViewMode::XanadocOnly);
    break;
  case ViewMode::XanadocOnly:
    setViewMode(ViewMode::ZigzagOnly);
    break;
  case ViewMode::ZigzagOnly:
    setViewMode(ViewMode::Unified);
    break;
  }
}

void ViewCoordinator::apply() {
  if (!visualizer_) {
    return;
  }

  switch (mode_) {
  case ViewMode::Unified: {
    visualizer_->setPresentationVisible(true);
    visualizer_->setPresentationTransformResolver(
        [this] { return views_.presentationTransform(); });
    if (!bridgeCoordinator_.attached()) {
      bridgeCoordinator_.attach(*visualizer_);
    }
    if (state_) {
      state_->usesDocPages = true;
      state_->showDialog(render::DiagnosticSeverity::Info, "View Mode: Unified",
                         "Unified Xanadoc & ZigZag bridge view active.");
    }
    std::cout << "xuzz: view mode -> Unified (Xanadoc + ZigZag bridge)\n";
    break;
  }

  case ViewMode::XanadocOnly: {
    visualizer_->setPresentationVisible(false);
    visualizer_->setPresentationTransformResolver({});
    if (state_) {
      state_->usesDocPages = true;
      state_->showDialog(
          render::DiagnosticSeverity::Info, "View Mode: Xanadoc",
          "Pure Xanadoc hypertext view active (ZigZag suspended).");
    }
    std::cout << "xuzz: view mode -> Xanadoc Only (ZigZag suspended)\n";
    break;
  }

  case ViewMode::ZigzagOnly: {
    visualizer_->setPresentationVisible(true);
    visualizer_->setPresentationTransformResolver({});
    visualizer_->setPresentationOrigin(glm::vec3{0.0F, 0.0F, 0.0F});
    if (state_) {
      state_->usesDocPages = false;
      state_->showDialog(render::DiagnosticSeverity::Info, "View Mode: ZigZag",
                         "Pure ZigZag multidimensional hypergrid view active.");
    }
    std::cout << "xuzz: view mode -> ZigZag Only (Hypergrid centered)\n";
    break;
  }
  }
}

} // namespace xuzz
