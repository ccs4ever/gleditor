/**
 * @file apps/xudu/collaborator_overlay.cpp
 * @brief 3D multi-author remote carets, selection highlights, and nameplate
 * badges.
 */
#include "collaborator_overlay.hpp"

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>
#include <utility>

#include "session.hpp"

namespace xudu {

const CollaboratorNameplatePresentation &CollaboratorNameplate::prepare(
    std::string_view name, std::string_view identity, glm::vec2 anchor,
    const gleditor::ui::UiMetrics &metrics, const gleditor::ui::Theme &theme,
    std::string_view fontOverride) {
  namespace ui           = gleditor::ui;
  const bool textChanged = name_ != name || identity_ != identity;
  if (textChanged) {
    name_                         = name;
    identity_                     = identity;
    presentation_.accessibleLabel = name_ + " ✦ " + identity_;
  }
  const auto description = ui::scaledFontDescription(
      fontOverride, ui::FontRole::Caption, metrics, theme);
  const bool fontChanged =
      !font_ || presentation_.fontDescription != description;
  if (fontChanged) {
    presentation_.fontDescription = description;
    font_ = gleditor::text::FontManager::instance().getFont(description);
  }
  if (textChanged || fontChanged) constraints_.reset();
  const auto safe = metrics.pixelSafeArea();
  if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y) || safe.width <= 0 ||
      safe.height <= 0) {
    presentation_.bounds = presentation_.content = {};
    return presentation_;
  }
  const float padding =
      std::min(std::max(2.F, font_->metrics().lineHeight * .25F),
               std::min(safe.width, safe.height) * .25F);
  const float height =
      std::min(safe.height, font_->metrics().lineHeight + 2 * padding);
  const gleditor::text::TextFit fit{.maxWidthPx  = safe.width - 2 * padding,
                                    .maxHeightPx = height - 2 * padding,
                                    .at = gleditor::text::EllipsisAt::Middle};
  if (textChanged || fontChanged || !constraints_ || *constraints_ != fit) {
    presentation_.fitted =
        shaping_.fitted(presentation_.accessibleLabel, font_, fit);
    constraints_ = fit;
  }
  const float width =
      std::min(safe.width, presentation_.fitted.widthPx + 2 * padding);
  presentation_.bounds = ui::clampToSafeArea(
      {anchor.x - padding, anchor.y + padding, width, height}, safe);
  const auto &box       = presentation_.bounds;
  presentation_.content = {box.left + padding, box.bottom + padding,
                           std::max(0.F, box.width - 2 * padding),
                           std::max(0.F, box.height - 2 * padding)};
  return presentation_;
}

CollaboratorCaretOverlay::CollaboratorCaretOverlay(Session &session,
                                                   RendererRef renderer,
                                                   std::string fontName)
    : session_(session), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)) {}

CollaboratorCaretOverlay::~CollaboratorCaretOverlay() = default;

void CollaboratorCaretOverlay::deviceReady(
    render::RenderDevice &device, const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline);
}

bool CollaboratorCaretOverlay::busy() const { return false; }

void CollaboratorCaretOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!enabled_ || !canvas_) {
    return;
  }
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  canvas_->setFontDescription(gleditor::ui::scaledFontDescription(
      fontName_, gleditor::ui::FontRole::Caption, metrics, ctx.theme));
  canvas_->clear();

  // 1. Drain pending live operations arriving over BEP 10 swarm wire
  if (auto *sw = session_.swarm()) {
    const auto pending = sw->takePendingLiveOps();
    for (const auto &b : pending) {
      session_.applyRemoteLiveOp(b, ctx.state.docs, ctx.state.caret);
    }
    const auto sealedNotifications = sw->takePendingScrollSealed();
    for (const auto &sealed : sealedNotifications) {
      session_.applyRemoteScrollSealed(sealed);
    }
  }

  // 2. Broadcast local caret movement on the main render thread
  if (ctx.state.caret && ctx.state.caret->active()) {
    const auto curDoc    = ctx.state.caret->documentIndex();
    const auto curOffset = ctx.state.caret->byteOffset();
    const auto curSelLen = ctx.state.caret->hasSelection()
                               ? (ctx.state.caret->selectionEnd() -
                                  ctx.state.caret->selectionStart())
                               : 0U;
    if (!lastCaretActive_ || curDoc != lastBroadcastDoc_ ||
        curOffset != lastBroadcastOffset_ ||
        curSelLen != lastBroadcastSelLen_) {
      lastCaretActive_     = true;
      lastBroadcastDoc_    = curDoc;
      lastBroadcastOffset_ = curOffset;
      lastBroadcastSelLen_ = curSelLen;
      session_.broadcastLocalCaret(curDoc, curOffset, curSelLen);
    }
  } else if (lastCaretActive_) {
    lastCaretActive_ = false;
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

  const auto &collaborators = session_.collaborators();
  std::erase_if(visuals_, [&](const auto &entry) {
    const auto found = collaborators.find(entry.first);
    return found == collaborators.end() ||
           now - found->second.lastSeen >= std::chrono::seconds{15};
  });
  std::size_t renderedCount = 0;
  for (const auto &[key, collab] : collaborators) {
    if (renderedCount >= 16) {
      break;
    }
    const float idleSec =
        std::chrono::duration<float>(now - collab.lastSeen).count();
    if (idleSec >= 15.0F) {
      continue;
    }
    float targetAlpha = 1.0F;
    if (idleSec >= 10.0F) {
      targetAlpha = std::clamp(1.0F - (idleSec - 10.0F) / 5.0F, 0.0F, 1.0F);
    }

    if (collab.docIndex >= ctx.state.docs.size()) {
      continue;
    }
    const auto &doc = ctx.state.docs[collab.docIndex];
    if (!doc || doc->currentOpacity() <= 0.01F) {
      continue;
    }

    const auto anchor = doc->anchorFor(collab.caretOffset);
    if (!anchor) {
      continue;
    }

    const auto bottomWorld =
        doc->worldPoint(anchor->pageIndex, anchor->x, anchor->y);
    const auto topWorld = doc->worldPoint(anchor->pageIndex, anchor->x,
                                          anchor->y + anchor->height);
    if (!bottomWorld || !topWorld) {
      continue;
    }

    const auto targetBottom = gleditor::spatial::projectToScreen(
        ctx.viewProjection, *bottomWorld, screenW, screenH);
    const auto targetTop = gleditor::spatial::projectToScreen(
        ctx.viewProjection, *topWorld, screenW, screenH);

    auto &vis = visuals_[key];
    if (!vis.initialized) {
      vis.bottom      = targetBottom;
      vis.top         = targetTop;
      vis.alpha       = targetAlpha;
      vis.initialized = true;
    } else {
      const float lerpFactor = std::clamp(dt * 25.0F, 0.0F, 1.0F);
      vis.bottom             = glm::mix(vis.bottom, targetBottom, lerpFactor);
      vis.top                = glm::mix(vis.top, targetTop, lerpFactor);
      vis.alpha              = glm::mix(vis.alpha, targetAlpha, lerpFactor);
    }

    if (vis.alpha <= 0.01F) {
      continue;
    }

    // Remote selection highlight quads
    if (collab.selectionLength > 0) {
      const auto endAnchor =
          doc->anchorFor(collab.caretOffset + collab.selectionLength);
      if (endAnchor && endAnchor->pageIndex == anchor->pageIndex) {
        const auto selStartWorld =
            doc->worldPoint(anchor->pageIndex, anchor->x, anchor->y);
        const auto selEndWorld =
            doc->worldPoint(endAnchor->pageIndex, endAnchor->x,
                            endAnchor->y + endAnchor->height);
        if (selStartWorld && selEndWorld) {
          const auto selStartScreen = gleditor::spatial::projectToScreen(
              ctx.viewProjection, *selStartWorld, screenW, screenH);
          const auto selEndScreen = gleditor::spatial::projectToScreen(
              ctx.viewProjection, *selEndWorld, screenW, screenH);
          const float x0  = std::min(selStartScreen.x, selEndScreen.x);
          const float x1  = std::max(selStartScreen.x, selEndScreen.x);
          const float y0  = std::min(selStartScreen.y, selEndScreen.y);
          const float y1  = std::max(selStartScreen.y, selEndScreen.y);
          const auto selA = static_cast<std::uint8_t>(55.0F * vis.alpha);
          const std::uint32_t selCol = (collab.colorRgba & 0xFFFFFF00) | selA;
          canvas_->addRect(x0, y0, std::max(x1 - x0, 2.0F),
                           std::max(y1 - y0, 14.0F), selCol);
        }
      }
    }

    // Caret glowing vertical bar
    const auto a                = static_cast<std::uint8_t>(255.0F * vis.alpha);
    const auto glowA            = static_cast<std::uint8_t>(90.0F * vis.alpha);
    const std::uint32_t coreCol = (collab.colorRgba & 0xFFFFFF00) | a;
    const std::uint32_t glowCol = (collab.colorRgba & 0xFFFFFF00) | glowA;

    // Subtle outer glow
    canvas_->addLine(vis.bottom.x, vis.bottom.y, vis.top.x, vis.top.y, 6.0F,
                     glowCol);
    // Sharp inner core
    canvas_->addLine(vis.bottom.x, vis.bottom.y, vis.top.x, vis.top.y, 2.5F,
                     coreCol);

    const auto identity   = !collab.fingerprint.empty()
                                ? std::string_view{collab.fingerprint}
                            : !collab.authorScrollKey.empty()
                                ? std::string_view{collab.authorScrollKey}
                                : std::string_view{"anon"};
    const auto &nameplate = vis.nameplate.prepare(
        collab.name, identity, vis.top, metrics, ctx.theme, fontName_);
    const auto &box           = nameplate.bounds;
    const auto bgA            = static_cast<std::uint8_t>(220.0F * vis.alpha);
    const std::uint32_t bgCol = 0x0F172A00 | bgA;
    if (box.width > 0 && box.height > 0) {
      canvas_->pushClip(box);
      canvas_->addRect(box.left, box.bottom, box.width, box.height, bgCol);
      canvas_->addLine(box.left, box.bottom, box.left + box.width, box.bottom,
                       1.F, coreCol);
      canvas_->addLine(box.left, box.bottom + box.height, box.left + box.width,
                       box.bottom + box.height, 1.F, coreCol);
      canvas_->addLine(box.left, box.bottom, box.left, box.bottom + box.height,
                       1.F, coreCol);
      canvas_->addLine(box.left + box.width, box.bottom, box.left + box.width,
                       box.bottom + box.height, 1.F, coreCol);
      canvas_->addText(ctx.state, nameplate.content, nameplate.fitted, coreCol,
                       bgCol);
      canvas_->popClip();
    }

    renderedCount++;
  }

  if (renderedCount > 0) {
    const auto ortho = glm::ortho(0.0F, screenW, 0.0F, screenH, -1.0F, 1.0F);
    ctx.state.glyphCache.flush();
    canvas_->commit();
    canvas_->draw(ctx.state, ortho);
  }
}

} // namespace xudu
