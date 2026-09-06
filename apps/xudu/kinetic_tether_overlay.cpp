/**
 * @file apps/xudu/kinetic_tether_overlay.cpp
 * @brief Visual overlay implementation for Hookean spring tether and floating
 * blueprint quad.
 */
#include "kinetic_tether_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>

#include <gleditor/render/types.hpp>

namespace xudu {

KineticTetherOverlay::KineticTetherOverlay(KineticTetherEngine &engine,
                                           std::string fontName)
    : engine_(engine), fontName_(std::move(fontName)) {}

KineticTetherOverlay::~KineticTetherOverlay() = default;

void KineticTetherOverlay::deviceReady(render::RenderDevice &device,
                                       const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool KineticTetherOverlay::busy() const {
  return engine_.state() == TetherState::SnappingBack;
}

void KineticTetherOverlay::drawTether(gleditor::Canvas &canvas, RenderState &,
                                      const glm::vec2 &p0, const glm::vec2 &p1,
                                      const bool detached) {
  const float dist    = glm::length(p1 - p0);
  const float sag     = std::min(40.0F, dist * 0.18F);
  const glm::vec2 mid = (p0 + p1) * 0.5F + glm::vec2(0.0F, -sag);

  const std::uint32_t col =
      detached ? 0xFFD700FF : 0xF59E0BCC; // Identity Gold vs Amber
  const float thickness = detached ? 2.5F : 1.5F;

  // Origin anchor point pip
  canvas.setTag(render::tagKindOverlay, 0);
  canvas.addRect(p0.x - 3.0F, p0.y - 3.0F, 6.0F, 6.0F, col);

  // Subdivided quadratic Bezier
  constexpr int kSegments = 16;
  glm::vec2 prevPt        = p0;
  for (int i = 1; i <= kSegments; ++i) {
    const float t   = static_cast<float>(i) / static_cast<float>(kSegments);
    const float inv = 1.0F - t;
    const glm::vec2 pt =
        (inv * inv * p0) + (2.0F * inv * t * mid) + (t * t * p1);
    canvas.addLine(prevPt.x, prevPt.y, pt.x, pt.y, thickness, col);
    prevPt = pt;
  }

  // Cursor tether end pip
  canvas.addRect(p1.x - 3.0F, p1.y - 3.0F, 6.0F, 6.0F, col);
}

void KineticTetherOverlay::drawBlueprintQuad(gleditor::Canvas &canvas,
                                             RenderState &state,
                                             const glm::vec2 &pos,
                                             const bool detached) {
  constexpr float quadW = 190.0F;
  constexpr float quadH = 88.0F;
  const float quadX     = pos.x - (quadW * 0.5F);
  const float quadY     = pos.y + 16.0F; // Floats above cursor

  canvas.setTag(render::tagKindOverlay, 0);

  // 1. Translucent Blueprint Fill (Ethereal slate)
  canvas.addRect(quadX, quadY, quadW, quadH, 0x0A101DDE);

  // 2. Glowing Blueprint Border
  const std::uint32_t borderCol = detached ? 0xFFD700EE : 0x06B6D4CC;
  const float borderThick       = detached ? 2.0F : 1.5F;
  canvas.addLine(quadX, quadY, quadX + quadW, quadY, borderThick, borderCol);
  canvas.addLine(quadX + quadW, quadY, quadX + quadW, quadY + quadH,
                 borderThick, borderCol);
  canvas.addLine(quadX + quadW, quadY + quadH, quadX, quadY + quadH,
                 borderThick, borderCol);
  canvas.addLine(quadX, quadY + quadH, quadX, quadY, borderThick, borderCol);

  // Corner Bracket Accents
  constexpr float bracketLen = 8.0F;
  canvas.addLine(quadX, quadY + quadH, quadX + bracketLen, quadY + quadH, 2.5F,
                 0xFFFFFFFF);
  canvas.addLine(quadX, quadY + quadH, quadX, quadY + quadH - bracketLen, 2.5F,
                 0xFFFFFFFF);
  canvas.addLine(quadX + quadW, quadY + quadH, quadX + quadW - bracketLen,
                 quadY + quadH, 2.5F, 0xFFFFFFFF);
  canvas.addLine(quadX + quadW, quadY + quadH, quadX + quadW,
                 quadY + quadH - bracketLen, 2.5F, 0xFFFFFFFF);

  // 3. Header
  const std::string header =
      detached ? "⟦ SPAWN XANADOC ⟧" : "⟦ BLUEPRINT CARD ⟧";
  canvas.addText(state, quadX + 8.0F, quadY + quadH - 4.0F, header, borderCol,
                 0);

  // 4. Preview Text Snippet
  std::string snippet = engine_.payload().previewText;
  if (snippet.size() > 22) {
    snippet = snippet.substr(0, 19) + "...";
  }
  canvas.addText(state, quadX + 8.0F, quadY + quadH - 24.0F, snippet,
                 0xF8FAFCFF, 0);

  // 5. Action Status Prompt
  const std::string statusPrompt =
      detached ? "[ RELEASE TO MATERIALIZE ]" : "[ SNAP-BACK ZONE (< 120px) ]";
  const std::uint32_t statusCol = detached ? 0x10B981FF : 0xEF4444FF;
  canvas.addText(state, quadX + 8.0F, quadY + 16.0F, statusPrompt, statusCol,
                 0);
}

void KineticTetherOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_ || !engine_.busy()) {
    return;
  }

  engine_.stepPhysics();

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas_->clear();

  const bool detached = engine_.isDetached();
  drawTether(*canvas_, ctx.state, engine_.originPos(), engine_.currentPos(),
             detached);
  drawBlueprintQuad(*canvas_, ctx.state, engine_.currentPos(), detached);

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

} // namespace xudu
