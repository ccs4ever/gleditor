/**
 * @file tenuous_tether.cpp
 * @brief Implementation of tenuous elastic tether ribbons.
 */
#include "common/ui/xanadoc/tenuous_tether.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <gleditor/paths.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

namespace xanadu {

TenuousTetherOverlay::TenuousTetherOverlay(RendererRef renderer,
                                           render::RenderDevice *device)
    : renderer_(std::move(renderer)), device_(device) {
  if (device_ != nullptr) {
    beams_ = std::make_unique<gleditor::Beams>(device_, 256);
  }
}

TenuousTetherOverlay::~TenuousTetherOverlay() = default;

void TenuousTetherOverlay::setTether(FlyingTetherAnchor anchor) {
  for (auto &t : tethers_) {
    if (t.targetKind == anchor.targetKind && t.targetId == anchor.targetId) {
      t = anchor;
      return;
    }
  }
  tethers_.push_back(anchor);
}

void TenuousTetherOverlay::removeTether(const std::size_t targetId,
                                        const LinkTargetKind kind) {
  std::erase_if(tethers_, [targetId, kind](const FlyingTetherAnchor &t) {
    return t.targetKind == kind && t.targetId == targetId;
  });
}

void TenuousTetherOverlay::clear() { tethers_.clear(); }

bool TenuousTetherOverlay::hasActiveTethers() const noexcept {
  return std::ranges::any_of(
      tethers_, [](const FlyingTetherAnchor &t) { return t.active; });
}

void TenuousTetherOverlay::deviceReady(
    render::RenderDevice &device,
    [[maybe_unused]] const render::PipelineDesc &documentPipeline) {
  device_ = &device;
  beams_  = std::make_unique<gleditor::Beams>(&device, 256);
  // Through assetDir(), as every other pipeline is: a path relative to the
  // working directory found the shaders only when run from the source tree.
  beams_->createPipeline(gleditor::assetPath("shaders"),
                         gleditor::assetPath("shaders/vulkan"), false);
}

void TenuousTetherOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!ctx.state.documentsVisible || !visible_ || tethers_.empty() || !beams_ ||
      !beams_->ready()) {
    return;
  }

  beams_->clear();

  const std::size_t segments =
      segments_ > 0 ? segments_ : defaultTessellationSegments;

  for (const auto &t : tethers_) {
    if (!t.active) {
      continue;
    }

    const glm::vec3 ctrl =
        computeControlPoint(t.originPos, t.currentPos, controlDepth_);

    const std::array poles{t.originPos, ctrl, t.currentPos};
    const std::array weights{1.F, 1.F, 1.F};
    const std::array knots{0.F, 0.F, 0.F, 1.F, 1.F, 1.F};
    beams_->addNurbs(gleditor::NurbsPath(poles, weights, knots, 2),
                     static_cast<unsigned>(std::min<std::size_t>(
                         segments, gleditor::NurbsSamples::maxSegments)),
                     .40F, t.colour, 0, 0, gleditor::Beams::Surface::Filament);

    // 2. Origin footprint blueprint quad outline in background plane
    const float halfW  = 0.5F * t.width;
    const float halfH  = 0.5F * t.height;
    const glm::vec3 p0 = t.originPos + glm::vec3(-halfW, -halfH, 0.0F);
    const glm::vec3 p1 = t.originPos + glm::vec3(halfW, -halfH, 0.0F);
    const glm::vec3 p2 = t.originPos + glm::vec3(halfW, halfH, 0.0F);
    const glm::vec3 p3 = t.originPos + glm::vec3(-halfW, halfH, 0.0F);

    const std::uint32_t footprintCol =
        (t.colour & 0xFFFFFF00U) | 0x2AU; // Faint alpha
    beams_->add(p0, p1, 0.25F, footprintCol, 0);
    beams_->add(p1, p2, 0.25F, footprintCol, 0);
    beams_->add(p2, p3, 0.25F, footprintCol, 0);
    beams_->add(p3, p0, 0.25F, footprintCol, 0);
  }

  beams_->commit();
  beams_->draw(ctx.state, ctx.viewProjection, 1.0F, 0);
}

} // namespace xanadu
