/**
 * @file satelloid.cpp
 * @brief Flying Cell Satelloid proxy quads and focus ring overlay
 * implementation.
 */
#include "xudu/satelloid.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include <gleditor/spatial.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/ranges.hpp>

namespace xudu {

SatelloidOverlay::SatelloidOverlay(RendererRef renderer, std::string fontName)
    : renderer_(std::move(renderer)), fontName_(std::move(fontName)) {}

SatelloidOverlay::~SatelloidOverlay() = default;

void SatelloidOverlay::setSatelloid(CellSatelloid satelloid) {
  for (auto &s : satelloids_) {
    if (s.cellRef == satelloid.cellRef) {
      s.originPos   = satelloid.originPos;
      s.targetPos   = satelloid.targetPos;
      s.width       = satelloid.width;
      s.height      = satelloid.height;
      s.text        = satelloid.text;
      s.dimName     = satelloid.dimName;
      s.accentColor = satelloid.accentColor;
      s.active      = satelloid.active;
      s.targetAlpha = satelloid.active ? 1.0F : 0.0F;
      return;
    }
  }
  satelloid.targetAlpha = satelloid.active ? 1.0F : 0.0F;
  satelloids_.push_back(satelloid);
}

void SatelloidOverlay::removeSatelloid(const zigzag::CellRef cellRef) {
  std::erase_if(satelloids_, [cellRef](const CellSatelloid &s) {
    return s.cellRef == cellRef;
  });
}

void SatelloidOverlay::clear() { satelloids_.clear(); }

gleditor::cpp26::optional<const CellSatelloid &>
SatelloidOverlay::findSatelloid(const zigzag::CellRef cellRef) const {
  return gleditor::findRef(satelloids_, [cellRef](const CellSatelloid &s) {
    return s.cellRef == cellRef;
  });
}

gleditor::cpp26::optional<CellSatelloid &>
SatelloidOverlay::findSatelloid(const zigzag::CellRef cellRef) {
  return gleditor::findRef(satelloids_, [cellRef](const CellSatelloid &s) {
    return s.cellRef == cellRef;
  });
}

void SatelloidOverlay::triggerPulse(const zigzag::CellRef cellRef) {
  if (auto s = findSatelloid(cellRef)) {
    s->triggerPulse();
  }
}

void SatelloidOverlay::deviceReady(render::RenderDevice &device,
                                   const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool SatelloidOverlay::busy() const {
  return std::ranges::any_of(satelloids_, [](const auto &s) {
    return s.pulseAlpha > 0.01F || std::abs(s.alpha - s.targetAlpha) > 0.02F;
  });
}

void SatelloidOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || satelloids_.empty() || !canvas_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const float dt = std::clamp(
      std::chrono::duration<float>(now - lastFrameTime_).count(), 0.0F, 0.1F);
  lastFrameTime_ = now;

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  if (screenW <= 1.0F || screenH <= 1.0F) {
    return;
  }

  // Update animations and prune inactive settled satelloids
  for (auto &s : satelloids_) {
    s.updateDynamics(dt);
  }

  std::erase_if(satelloids_, [](const CellSatelloid &s) {
    return !s.active && s.alpha <= 0.01F && s.pulseAlpha <= 0.01F;
  });

  if (satelloids_.empty()) {
    return;
  }

  canvas_->clear();

  for (std::size_t index = 0; index < satelloids_.size(); ++index) {
    const auto &s = satelloids_[index];
    if (s.alpha <= 0.01F && s.pulseAlpha <= 0.01F) {
      continue;
    }

    const auto screenPos = gleditor::spatial::projectToScreen(
        ctx.viewProjection, s.currentPos, screenW, screenH);

    const float cardW = 160.0F;
    const float cardH = 54.0F;
    const float x0    = screenPos.x;
    const float y0    = screenPos.y - 0.5F * cardH;

    // Card background quad
    const auto bgA            = static_cast<std::uint8_t>(220.0F * s.alpha);
    const std::uint32_t bgCol = 0x0F172A00U | bgA;
    // Tagged by position in this overlay's own list rather than by cell: a
    // cell reference is unbounded, and base-plus-cell ran into every other
    // overlay's tag range once a manifold passed a thousand cells.
    canvas_->setTag(render::tagKindOverlay,
                    kTagSatelloidBase + static_cast<std::uint32_t>(index));
    canvas_->addRect(x0, y0, cardW, cardH, bgCol);

    // Border with primary dimension accent color
    const auto borderA = static_cast<std::uint8_t>(255.0F * s.alpha);
    const std::uint32_t borderCol =
        (s.accentColor & 0xFFFFFF00U) | static_cast<std::uint32_t>(borderA);
    canvas_->addLine(x0, y0, x0 + cardW, y0, 1.5F, borderCol);
    canvas_->addLine(x0 + cardW, y0, x0 + cardW, y0 + cardH, 1.5F, borderCol);
    canvas_->addLine(x0 + cardW, y0 + cardH, x0, y0 + cardH, 1.5F, borderCol);
    canvas_->addLine(x0, y0 + cardH, x0, y0, 1.5F, borderCol);

    // Header badge: "[#cellRef • dimName]"
    const std::string badgeText =
        "[#" + std::to_string(s.cellRef) + " \u2022 " + s.dimName + "]";
    canvas_->addText(ctx.state, x0 + 6.0F, y0 + cardH - 16.0F, badgeText,
                     borderCol, bgCol);

    // Text snippet preview
    std::string preview = s.text;
    if (preview.size() > 22) {
      preview = preview.substr(0, 20) + "...";
    }
    const std::uint32_t textCol =
        0xE2E8F000U | static_cast<std::uint32_t>(borderA);
    canvas_->addText(ctx.state, x0 + 6.0F, y0 + 12.0F, preview, textCol, bgCol);

    // Animated focus ring pulse
    if (s.pulseAlpha > 0.01F) {
      const auto pulseA = static_cast<std::uint8_t>(255.0F * s.pulseAlpha);
      const std::uint32_t ringCol =
          (s.accentColor & 0xFFFFFF00U) | static_cast<std::uint32_t>(pulseA);
      const float r  = s.pulseRadius;
      const float cx = x0 + 0.5F * cardW;
      const float cy = y0 + 0.5F * cardH;

      constexpr int kRingSegments = 16;
      for (int i = 0; i < kRingSegments; ++i) {
        const float theta1 = 2.0F * std::numbers::pi_v<float> *
                             static_cast<float>(i) /
                             static_cast<float>(kRingSegments);
        const float theta2 = 2.0F * std::numbers::pi_v<float> *
                             static_cast<float>(i + 1) /
                             static_cast<float>(kRingSegments);
        const float x1     = cx + r * std::cos(theta1);
        const float y1     = cy + r * std::sin(theta1);
        const float x2     = cx + r * std::cos(theta2);
        const float y2     = cy + r * std::sin(theta2);
        canvas_->addLine(x1, y1, x2, y2, 2.0F, ringCol);
      }
    }
  }

  const auto ortho = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);
  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

bool SatelloidOverlay::picked(const render::PickingResult &pick,
                              RenderState & /*state*/) {
  if (render::tagKindOverlay != pick.tag.kind ||
      pick.tag.clusterIndex < kTagSatelloidBase ||
      pick.tag.clusterIndex - kTagSatelloidBase >= satelloids_.size()) {
    return false;
  }
  const auto cellRef =
      satelloids_[pick.tag.clusterIndex - kTagSatelloidBase].cellRef;
  triggerPulse(cellRef);
  if (navigationCb_) {
    navigationCb_(cellRef, false);
  }
  return true;
}

} // namespace xudu
