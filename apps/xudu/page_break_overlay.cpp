/**
 * @file apps/xudu/page_break_overlay.cpp
 * @brief Inter-paragraph hover gap affordance and page break overlay.
 */
#include "page_break_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

#include "session.hpp"

namespace xanadu {

PageBreakOverlay::PageBreakOverlay(Session &session, RendererRef renderer,
                                   std::string fontName)
    : session_(session), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)) {}

PageBreakOverlay::~PageBreakOverlay() = default;

void PageBreakOverlay::deviceReady(render::RenderDevice &device,
                                   const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(
      &device,
      gleditor::ui::UiMetrics{}.fontDescription(gleditor::ui::FontRole::Label,
                                                gleditor::ui::defaultTheme()));
  presentation_.deviceReady(device, pipeline);
  canvas_->createPipeline(pipeline, false);
}

bool PageBreakOverlay::busy() const {
  const std::scoped_lock lock(actionMutex_);
  return pendingSplit_.has_value();
}

void PageBreakOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_ || ctx.state.docs.empty()) {
    hasHover_ = false;
    selectTarget(std::nullopt);
    drainActions();
    return;
  }
  hasHover_ = false;

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  if (screenW <= 1.0F || screenH <= 1.0F) {
    selectTarget(std::nullopt);
    drainActions();
    return;
  }

  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;

  // Mouse in window coordinates (bottom-up for canvas ortho space)
  float mouseX = 0.0F;
  float mouseY = 0.0F;
  if (renderer_ && renderer_->appState()) {
    mouseX = static_cast<float>(renderer_->appState()->mouseX);
    mouseY = screenH - static_cast<float>(renderer_->appState()->mouseY);
  }

  // A scaled button can extend beyond the gap's discovery band. Keep its
  // original target while the pointer remains in the drawn control.
  const auto previous = presentation_.snapshot();
  const bool insideControl =
      target_ && previous &&
      previous->layout.hitTest(mouseX, mouseY) != nullptr;

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
    if (boundaries_.size() <= d) boundaries_.resize(d + 1);
    auto &cache = boundaries_[d];
    const Target boundaryStamp{
        .document   = static_cast<std::uint32_t>(d),
        .store      = openView.storeIndex,
        .operations = session_.store(openView.storeIndex).opCount(),
        .generation = session_.generation(),
        .version    = openView.version};
    if (cache.stamp != boundaryStamp) {
      cache.offsets.clear();
      const auto text =
          session_.store(openView.storeIndex).textOf(openView.version);
      for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != '\n') continue;
        if (text[i + 1] == '\n') ++i;
        cache.offsets.push_back(static_cast<std::uint32_t>(i + 1));
      }
      cache.stamp = boundaryStamp;
    }

    for (const auto splitOffset : cache.offsets) {
      const auto anchor = doc->anchorFor(splitOffset);
      if (!anchor) {
        continue;
      }
      const auto p = doc->page(anchor->pageIndex);
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
      if (insideControl) {
        isTarget = target_->document == d && target_->offset == splitOffset;
      } else if (sampleForceVisible_ && d == sampleDocIdx_) {
        if (sampleGapOffset_ == 0 || splitOffset == sampleGapOffset_) {
          isTarget = true;
        }
      } else if (mouseX >=
                     gapX0 - metrics.px(ctx.theme.type.minTouchPx * 0.5F) &&
                 mouseX <=
                     gapX1 + metrics.px(ctx.theme.type.minTouchPx * 0.5F) &&
                 std::abs(mouseY - gapY) <=
                     metrics.px(ctx.theme.type.minTouchPx * 0.5F)) {
        isTarget = true;
      }

      if (isTarget) {
        hasHover_        = true;
        hoverDoc_        = static_cast<std::uint32_t>(d);
        hoverCharOffset_ = splitOffset;

        selectTarget(
            Target{.document   = hoverDoc_,
                   .offset     = splitOffset,
                   .store      = openView.storeIndex,
                   .operations = session_.store(openView.storeIndex).opCount(),
                   .generation = session_.generation(),
                   .version    = openView.version});
        std::ignore = presentation_.prepareGap({gapX0, gapY, gapX1 - gapX0, 0},
                                               metrics, ctx.theme, fontName_);
        const std::array<float, 3> gap{gapX0, gapX1, gapY};
        if (drawnGap_ != gap ||
            drawnRevision_ != presentation_.layoutRevision()) {
          canvas_->clear();
          canvas_->pushClip(metrics.pixelSafeArea());
          canvas_->setTag(render::tagKindOverlay, 0);
          const float dashLen = metrics.px(ctx.theme.type.minTouchPx * 0.25F);
          const float gapLen  = metrics.px(ctx.theme.type.minTouchPx * 0.125F);
          const auto safe     = metrics.pixelSafeArea();
          float curX          = std::max(gapX0, safe.left);
          const float endX    = std::min(gapX1, safe.left + safe.width);
          while (dashLen + gapLen > 0 && curX < endX) {
            canvas_->addLine(curX, gapY, std::min(curX + dashLen, endX), gapY,
                             metrics.px(1),
                             gleditor::ui::rgba(ctx.theme.colours.accent));
            curX += dashLen + gapLen;
          }
          canvas_->popClip();
          canvas_->commit();
          drawnGap_      = gap;
          drawnRevision_ = presentation_.layoutRevision();
        }
        break;
      }
    }
    if (hasHover_) {
      break;
    }
  }

  if (!hasHover_) selectTarget(std::nullopt);
  drainActions();
  if (hasHover_) {
    const auto ortho = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);
    canvas_->draw(ctx.state, ortho);
    presentation_.drawPrepared(ctx);
  } else {
    selectTarget(std::nullopt);
  }
}

bool PageBreakOverlay::picked(const render::PickingResult &pick,
                              RenderState &state) {
  return presentation_.picked(pick, state);
}

void PageBreakOverlay::selectTarget(std::optional<Target> next) {
  if (next == target_) return;
  target_ = next;
  presentation_.select(next ? std::optional{++targetRevision_} : std::nullopt);
  presentation_.setActionHandler([this, next](const auto &) {
    const std::scoped_lock lock(actionMutex_);
    pendingSplit_ = next;
  });
}

void PageBreakOverlay::drainActions() {
  std::optional<Target> next;
  {
    const std::scoped_lock lock(actionMutex_);
    next = std::exchange(pendingSplit_, std::nullopt);
  }
  if (!next || next != target_ || next->document >= session_.views().size())
    return;
  const auto &view = session_.views()[next->document];
  if (view.storeIndex != next->store || view.version != next->version ||
      session_.generation() != next->generation ||
      session_.store(view.storeIndex).opCount() != next->operations ||
      !view.uncommittedLog.empty())
    return;
  if (onSplit_) onSplit_(next->document, next->offset);
}

} // namespace xanadu
