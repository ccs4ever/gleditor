/**
 * @file bridge_coordinator.cpp
 * @brief Xudu composition boundary for an embedded Zigzag presentation.
 */
#include "bridge_coordinator.hpp"

#include <utility>

namespace xudu {

BridgeCoordinator::BridgeCoordinator(
    LinkBeams &links, RendererRef renderer,
    gleditor::a11y::Publisher &accessibility) noexcept
    : links_(links), renderer_(std::move(renderer)),
      accessibility_(accessibility) {}

BridgeCoordinator::~BridgeCoordinator() { detach(); }

bool BridgeCoordinator::attach(xanadu::ZigzagPresentationSurface &surface) {
  if (surface_ != nullptr) {
    return false;
  }

  surface_ = &surface;
  surface_->setCellRadius(config_.cellRadius);
  surface_->setBridgeInvalidationCallback(
      [this](const std::uint64_t) { dirty_ = true; });
  surface_->setCellActivationCallback(
      [this](const zigzag::CellRef cell) { activateCell(cell); });
  renderer_->addFrameContributor(surface_->frameContributor());
  renderer_->addPickObserver(surface_->pickObserver());
  accessibility_.addSource(surface_->accessibilitySource());
  synchronize();
  return true;
}

void BridgeCoordinator::connectSatelloidNavigation(SatelloidOverlay &overlay) {
  satelloidOverlay_ = &overlay;
  satelloidOverlay_->setNavigationCallback(
      [this](const zigzag::CellRef cell, const bool altHeld) {
        if (surface_ != nullptr) {
          surface_->focusCell(cell);
          dirty_ = true;
        }
        if (cellActivationHandler_) {
          cellActivationHandler_(cell, altHeld);
        }
      });
}

void BridgeCoordinator::onDocumentLinkActivated(const zigzag::CellRef cell) {
  if (surface_ != nullptr) {
    surface_->focusCell(cell);
    dirty_ = true;
  }
  if (satelloidOverlay_ != nullptr) {
    satelloidOverlay_->triggerPulse(cell);
  }
}

void BridgeCoordinator::activateCell(const zigzag::CellRef cell) {
  if (surface_ == nullptr) {
    return;
  }
  const auto &manifold = surface_->manifold();
  const auto spans     = manifold.contentOf(cell);
  PrimediaSpan primarySpan{};
  if (!spans.empty()) {
    primarySpan = spans.front();
  }
  if (documentFocusHandler_) {
    documentFocusHandler_(cell, primarySpan);
  }
}

bool BridgeCoordinator::isCellLocked(
    const zigzag::CellRef cell) const noexcept {
  return surface_ != nullptr && surface_->isCellLocked(cell);
}

std::optional<xanadu::TranscopyrightDescriptor>
BridgeCoordinator::cellRoyalty(const zigzag::CellRef cell) const noexcept {
  return surface_ != nullptr ? surface_->cellRoyalty(cell) : std::nullopt;
}

bool BridgeCoordinator::unlockCell(const zigzag::CellRef cell) {
  if (surface_ == nullptr) {
    return false;
  }
  const bool ok = surface_->unlockCell(cell);
  if (ok) {
    synchronize();
  }
  return ok;
}

void BridgeCoordinator::detach() noexcept {
  if (surface_ == nullptr) {
    return;
  }

  auto *const surface = surface_;
  if (satelloidOverlay_ != nullptr) {
    satelloidOverlay_->setNavigationCallback({});
    satelloidOverlay_ = nullptr;
  }
  cellActivationHandler_ = {};
  documentFocusHandler_  = {};
  surface->setBridgeInvalidationCallback({});
  surface->setCellActivationCallback({});
  renderer_->removeFrameContributor(surface->frameContributor());
  renderer_->removePickObserver(surface->pickObserver());
  accessibility_.removeSource(surface->accessibilitySource());
  links_.setManifoldViews({}, {});
  links_.setCellAnchorResolver({});
  surface_              = nullptr;
  synchronizedRevision_ = 0;
  dirty_                = false;
}

void BridgeCoordinator::synchronize() {
  if (surface_ == nullptr) {
    return;
  }

  const auto revision = surface_->bridgeRevision();
  if (!dirty_ && revision == synchronizedRevision_) {
    return;
  }

  links_.setManifoldViews({&surface_->manifold()}, {surface_->focusCell()});
  links_.setCellAnchorResolver(
      [surface = surface_](const zigzag::CellRef cell) {
        return surface->cellAnchor(cell);
      });
  synchronizedRevision_ = revision;
  dirty_                = false;
}

void BridgeCoordinator::applyConfig(xanadu::BridgeRuntimeConfig config) {
  config_ = config;
  links_.setCellRadius(config_.cellRadius);
  links_.setBridgeRuntimeConfig(config_);
  if (surface_ != nullptr) {
    surface_->setCellRadius(config_.cellRadius);
  }
  dirty_ = true;
}

} // namespace xudu
