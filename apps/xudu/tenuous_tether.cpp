/**
 * @file tenuous_tether.cpp
 * @brief Implementation of tenuous elastic tether ribbons.
 */
#include "xudu/tenuous_tether.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include <gleditor/render/types.hpp>

#include "common/cpp26_inplace_vector.hpp"

namespace xudu {

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
  beams_->createPipeline("assets/shaders", "assets/shaders/vulkan", false);
}

void TenuousTetherOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || tethers_.empty() || !beams_ || !beams_->ready()) {
    return;
  }

  beams_->clear();

  const std::size_t segments =
      segments_ > 0 ? segments_ : defaultTessellationSegments;

  // The default curve fits inline; larger user-configured curves retain the
  // dynamic path rather than changing their tessellation.
  common::cpp26::inplace_vector<glm::vec3, defaultTessellationSegments + 1>
      inlineCurve;
  std::vector<glm::vec3> largeCurve;
  std::span<glm::vec3> curve;
  if (segments <= defaultTessellationSegments) {
    inlineCurve.resize(segments + 1);
    curve = std::span(inlineCurve.data(), inlineCurve.size());
  } else {
    largeCurve.resize(segments + 1);
    curve = std::span(largeCurve.data(), largeCurve.size());
  }

  for (const auto &t : tethers_) {
    if (!t.active) {
      continue;
    }

    const glm::vec3 ctrl =
        computeControlPoint(t.originPos, t.currentPos, controlDepth_);

    for (std::size_t i = 0; i <= segments; ++i) {
      const float param = static_cast<float>(i) / static_cast<float>(segments);
      curve[i] = evaluateBezier(t.originPos, ctrl, t.currentPos, param);
    }

    // 1. Tenuous connecting ribbon
    beams_->addPath(curve, 0.40F, t.colour, 0);

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

} // namespace xudu
