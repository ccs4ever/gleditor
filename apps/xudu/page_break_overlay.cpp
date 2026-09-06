/**
 * @file apps/xudu/page_break_overlay.cpp
 * @brief Inter-paragraph hover gap affordance and page break overlay.
 */
#include "page_break_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

#include "session.hpp"

namespace xudu {

PageBreakOverlay::PageBreakOverlay(Session &session, RendererRef renderer,
                                   std::string fontName)
    : session_(session), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)) {}

PageBreakOverlay::~PageBreakOverlay() = default;

void PageBreakOverlay::deviceReady(render::RenderDevice &device,
                                   const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool PageBreakOverlay::busy() const { return false; }

void PageBreakOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_ || ctx.state.docs.empty()) {
    hasHover_ = false;
    return;
  }
  canvas_->clear();
  hasHover_ = false;

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  if (screenW <= 1.0F || screenH <= 1.0F) {
    return;
  }

  // Mouse in window coordinates (bottom-up for canvas ortho space)
  float mouseX = 0.0F;
  float mouseY = 0.0F;
  if (renderer_ && renderer_->appState()) {
    mouseX = static_cast<float>(renderer_->appState()->mouseX);
    mouseY = screenH - static_cast<float>(renderer_->appState()->mouseY);
  }

  // Search foreground documents for inter-paragraph boundaries
  for (std::size_t d = 0; d < ctx.state.docs.size(); ++d) {
    const auto &doc = ctx.state.docs[d];
    if (!doc || doc->currentOpacity() <= 0.01F) {
      continue;
    }
    // Skip background documents
    if (glm::vec3(doc->getModel()[3]).z < 0.0F) {
      continue;
    }

    if (d >= session_.views().size()) {
      continue;
    }
    const auto &openView = session_.views()[d];
    const auto text      = session_.store(openView.storeIndex).textOf(openView.version);
    if (text.empty()) {
      continue;
    }

    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '\n') {
        continue;
      }
      if (i + 1 >= text.size()) {
        continue;
      }
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        i++;
      }

      const auto splitOffset = static_cast<std::uint32_t>(i + 1);
      const auto anchor      = doc->anchorFor(splitOffset);
      if (!anchor) {
        continue;
      }
      const auto *const p = doc->page(anchor->pageIndex);
      if (!p) {
        continue;
      }

      const float halfW = p->widthPixels() * 0.5F;
      const auto leftWorld =
          doc->worldPoint(anchor->pageIndex, -halfW + 36.0F, anchor->y);
      const auto rightWorld =
          doc->worldPoint(anchor->pageIndex, halfW - 36.0F, anchor->y);
      if (!leftWorld || !rightWorld) {
        continue;
      }

      const auto screenLeft = gleditor::spatial::projectToScreen(
          ctx.viewProjection, *leftWorld, screenW, screenH);
      const auto screenRight = gleditor::spatial::projectToScreen(
          ctx.viewProjection, *rightWorld, screenW, screenH);

      const float gapY  = screenLeft.y;
      const float gapX0 = std::min(screenLeft.x, screenRight.x);
      const float gapX1 = std::max(screenLeft.x, screenRight.x);

      bool isTarget = false;
      if (sampleForceVisible_ && d == sampleDocIdx_) {
        if (sampleGapOffset_ == 0 || splitOffset == sampleGapOffset_) {
          isTarget = true;
        }
      } else if (mouseX >= gapX0 - 24.0F && mouseX <= gapX1 + 24.0F &&
                 std::abs(mouseY - gapY) <= 18.0F) {
        isTarget = true;
      }

      if (isTarget) {
        hasHover_        = true;
        hoverDoc_        = static_cast<std::uint32_t>(d);
        hoverCharOffset_ = splitOffset;

        // Draw horizontal dashed line across the page width
        constexpr float dashLen    = 10.0F;
        constexpr float gapLen     = 6.0F;
        constexpr uint32_t dashCol = 0x06B6D4EE; // Luminous cyan

        float curX = gapX0;
        while (curX < gapX1) {
          const float nextX = std::min(curX + dashLen, gapX1);
          canvas_->addLine(curX, gapY, nextX, gapY, 1.5F, dashCol);
          curX += dashLen + gapLen;
        }

        // Draw centered button: [+ Split to New Page (Ctrl+Ret)]
        const std::string btnLabel = "+ Split to New Page (Ctrl+Ret)";
        constexpr float btnW       = 236.0F;
        constexpr float btnH       = 26.0F;
        const float btnX           = 0.5F * (gapX0 + gapX1) - (0.5F * btnW);
        const float btnY           = gapY - (0.5F * btnH);

        buttonMin_ = glm::vec2(btnX, btnY);
        buttonMax_ = glm::vec2(btnX + btnW, btnY + btnH);

        canvas_->setTag(render::tagKindOverlay, kTagPageBreakAffordance);

        // Translucent card background
        constexpr uint32_t cardBg = 0x0F172AEE;
        canvas_->addRect(btnX, btnY, btnW, btnH, cardBg);
        // Cyan border
        canvas_->addLine(btnX, btnY, btnX + btnW, btnY, 1.5F, 0x06B6D4FF);
        canvas_->addLine(btnX, btnY + btnH, btnX + btnW, btnY + btnH, 1.5F,
                         0x06B6D4FF);
        canvas_->addLine(btnX, btnY, btnX, btnY + btnH, 1.5F, 0x06B6D4FF);
        canvas_->addLine(btnX + btnW, btnY, btnX + btnW, btnY + btnH, 1.5F,
                         0x06B6D4FF);

        // Centered button label
        canvas_->addText(ctx.state, btnX + 14.0F, btnY + btnH - 6.0F, btnLabel,
                         0x38BDF8FF, cardBg);

        canvas_->setTag(render::tagKindOverlay, 0);
        break;
      }
    }
    if (hasHover_) {
      break;
    }
  }

  if (hasHover_) {
    const auto ortho =
        glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);
    canvas_->commit();
    canvas_->draw(ctx.state, ortho);
  }
}

bool PageBreakOverlay::picked(const render::PickingResult &pick,
                              RenderState &/*state*/) {
  if (pick.tag.kind != render::tagKindOverlay ||
      pick.tag.clusterIndex != kTagPageBreakAffordance) {
    return false;
  }
  if (hasHover_ && onSplit_) {
    onSplit_(hoverDoc_, hoverCharOffset_);
    return true;
  }
  return false;
}

} // namespace xudu
