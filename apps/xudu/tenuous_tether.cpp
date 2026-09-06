/**
 * @file tenuous_tether.cpp
 * @brief Implementation of tenuous elastic tether ribbons.
 */
#include "xudu/tenuous_tether.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <gleditor/render/types.hpp>

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
    if (t.docIndex == anchor.docIndex) {
      t = anchor;
      return;
    }
  }
  tethers_.push_back(anchor);
}

void TenuousTetherOverlay::removeTether(const std::size_t docIndex) {
  std::erase_if(tethers_, [docIndex](const FlyingTetherAnchor &t) {
    return t.docIndex == docIndex;
  });
}

void TenuousTetherOverlay::clear() {
  tethers_.clear();
}

bool TenuousTetherOverlay::hasActiveTethers() const noexcept {
  return std::any_of(tethers_.begin(), tethers_.end(),
                     [](const FlyingTetherAnchor &t) { return t.active; });
}

void TenuousTetherOverlay::deviceReady(render::RenderDevice &device,
                                      const render::PipelineDesc &) {
  device_ = &device;
  beams_  = std::make_unique<gleditor::Beams>(&device, 256);
  beams_->createPipeline("assets/shaders", "assets/shaders/vulkan", false);
}

void TenuousTetherOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || tethers_.empty() || !beams_ || !beams_->ready()) {
    return;
  }

  beams_->clear();

  constexpr int kSegments = 16;
  std::vector<glm::vec3> curve(kSegments + 1);

  for (const auto &t : tethers_) {
    if (!t.active) {
      continue;
    }

    const glm::vec3 ctrl = computeControlPoint(t.originPos, t.currentPos, 18.0F);

    for (int i = 0; i <= kSegments; ++i) {
      const float param = static_cast<float>(i) / static_cast<float>(kSegments);
      curve[i]          = evaluateBezier(t.originPos, ctrl, t.currentPos, param);
    }

    // 1. Tenuous connecting ribbon
    beams_->addPath(curve, 0.40F, t.colour, 0);

    // 2. Origin footprint blueprint quad outline in background plane
    const float halfW = 0.5F * t.width;
    const float halfH = 0.5F * t.height;
    const glm::vec3 p0 = t.originPos + glm::vec3(-halfW, -halfH, 0.0F);
    const glm::vec3 p1 = t.originPos + glm::vec3(halfW, -halfH, 0.0F);
    const glm::vec3 p2 = t.originPos + glm::vec3(halfW, halfH, 0.0F);
    const glm::vec3 p3 = t.originPos + glm::vec3(-halfW, halfH, 0.0F);

    const std::uint32_t footprintCol = (t.colour & 0xFFFFFF00U) | 0x2AU; // Faint alpha
    beams_->add(p0, p1, 0.25F, footprintCol, 0);
    beams_->add(p1, p2, 0.25F, footprintCol, 0);
    beams_->add(p2, p3, 0.25F, footprintCol, 0);
    beams_->add(p3, p0, 0.25F, footprintCol, 0);
  }

  beams_->commit();
  beams_->draw(ctx.state, ctx.viewProjection, 1.0F, 0);
}

} // namespace xudu
