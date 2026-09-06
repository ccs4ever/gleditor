/**
 * @file transcopyright_overlay.cpp
 * @brief Implementation of 3D/2D Transcopyright paywall badges and withheld
 * redactions.
 */
#include "transcopyright_overlay.hpp"

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>

namespace xudu {

TranscopyrightOverlay::TranscopyrightOverlay(Session &session,
                                             RendererRef renderer,
                                             std::string fontName)
    : session_(session), renderer_(std::move(renderer)),
      fontName_(std::move(fontName)) {
  session_.setTranscopyrightUnlockedHandler(
      [this](const std::size_t docIdx, const PrimediaSpan &span,
             const std::uint64_t cost) {
        notifyUnlocked(docIdx, span, cost);
      });
}

TranscopyrightOverlay::~TranscopyrightOverlay() = default;

void TranscopyrightOverlay::deviceReady(render::RenderDevice &device,
                                        const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool TranscopyrightOverlay::busy() const {
  return !uncurlingAnims_.empty();
}

void TranscopyrightOverlay::notifyUnlocked(const std::size_t docIndex,
                                           const PrimediaSpan &span,
                                           const std::uint64_t /*cost*/) {
  float sx = 100.0F;
  float sy = 100.0F;
  float sw = 180.0F;
  float sh = 28.0F;

  for (const auto &b : activeBadges_) {
    if (b.docIndex == docIndex && b.span == span) {
      sx = b.screenX;
      sy = b.screenY;
      sw = b.width;
      sh = b.height;
      break;
    }
  }

  uncurlingAnims_.push_back(UncurlingAnim{
      .docIndex = docIndex,
      .span     = span,
      .progress = 0.0F,
      .screenX  = sx,
      .screenY  = sy,
      .width    = sw,
      .height   = sh,
  });
}

void TranscopyrightOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_) {
    return;
  }

  // 1. Advance uncurling animations
  for (auto it = uncurlingAnims_.begin(); it != uncurlingAnims_.end();) {
    it->progress += 0.05F; // ~20 frames (~300ms)
    if (it->progress >= 1.0F) {
      it = uncurlingAnims_.erase(it);
    } else {
      ++it;
    }
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas_->clear();
  activeBadges_.clear();

  // 2. Discover holes across active open views
  for (const auto &doc : ctx.state.docs) {
    if (!doc) continue;
    const auto docIdx = doc->documentIndex();
    if (docIdx >= session_.views().size()) continue;

    const auto holes = session_.holesForView(static_cast<std::uint32_t>(docIdx));
    for (const auto &hole : holes) {
      const auto startAnchor = doc->anchorFor(hole.charStart);
      if (!startAnchor) continue;
      const auto wStart = doc->worldPoint(*startAnchor);
      if (!wStart) continue;

      const glm::vec4 clipStart = ctx.viewProjection * glm::vec4(*wStart, 1.0F);
      if (clipStart.w <= 0.001F) continue;
      const glm::vec3 ndcStart = glm::vec3(clipStart) / clipStart.w;
      const float sxStart =
          (ndcStart.x * 0.5F + 0.5F) * static_cast<float>(ctx.screenWidth);
      const float syStart =
          (ndcStart.y * 0.5F + 0.5F) * static_cast<float>(ctx.screenHeight);

      float sxEnd = sxStart + 140.0F;
      const auto endAnchor =
          doc->anchorFor(hole.charEnd > 0 ? hole.charEnd - 1 : 0);
      if (endAnchor) {
        if (const auto wEnd = doc->worldPoint(*endAnchor)) {
          const glm::vec4 clipEnd = ctx.viewProjection * glm::vec4(*wEnd, 1.0F);
          if (clipEnd.w > 0.001F) {
            const glm::vec3 ndcEnd = glm::vec3(clipEnd) / clipEnd.w;
            sxEnd = (ndcEnd.x * 0.5F + 0.5F) *
                    static_cast<float>(ctx.screenWidth);
          }
        }
      }

      const float minX = std::min(sxStart, sxEnd);
      const float maxX = std::max(sxStart, sxEnd);
      const float boxW = std::max(120.0F, maxX - minX + 16.0F);
      const float boxH = 26.0F;
      const float boxY = syStart - 12.0F;
      const float boxX = minX - 4.0F;

      if (hole.isLocked()) {
        const auto tagId =
            kTagUnlockBase + static_cast<std::uint32_t>(activeBadges_.size());
        canvas_->setTag(render::tagKindOverlay, tagId);

        // Luminous Amber Gold acrylic background pill
        canvas_->addRect(boxX, boxY, boxW, boxH, 0xF59E0BEE);
        canvas_->addLine(boxX, boxY, boxX + boxW, boxY, 1.5F, 0xFBBF24FF);
        canvas_->addLine(boxX, boxY + boxH, boxX + boxW, boxY + boxH, 1.5F,
                         0xFDE68AFF);

        // Interactive label
        canvas_->addText(ctx.state, boxX + 8.0F, boxY + 6.0F, hole.badgeText(),
                         0xFFFFFFFFU, 0);

        activeBadges_.push_back(VisibleBadge{
            .tagId      = tagId,
            .docIndex   = hole.docIndex,
            .storeIndex = hole.storeIndex,
            .span       = hole.span,
            .screenX    = boxX,
            .screenY    = boxY,
            .width      = boxW,
            .height     = boxH,
            .text       = hole.badgeText(),
            .isLocked   = true,
            .color      = hole.color,
        });
      } else if (hole.isWithheld()) {
        canvas_->setTag(render::tagKindOverlay, 0);

        // Obsidian blackout quad with subtle etched border and cross-hatching
        canvas_->addRect(boxX, boxY, boxW, boxH, hole.color);
        canvas_->addLine(boxX, boxY, boxX + boxW, boxY, 1.0F, 0x475569AA);
        canvas_->addLine(boxX, boxY, boxX + boxW, boxY + boxH, 1.0F,
                         0x33415566);

        // Reason tag chip
        canvas_->addText(ctx.state, boxX + 6.0F, boxY + 6.0F,
                         "[" + hole.reasonLabel + "]", 0x94A3B8EE, 0);
      }
    }
  }

  // 3. Render active uncurling bloom animations
  canvas_->setTag(render::tagKindOverlay, 0);
  for (const auto &anim : uncurlingAnims_) {
    const float alpha       = std::clamp(1.0F - anim.progress, 0.0F, 1.0F);
    const auto alphaByte    = static_cast<std::uint32_t>(alpha * 220.0F);
    const auto glowColor    = 0xF59E0B00U | (alphaByte & 0xFFU);
    const float expansion   = anim.progress * 24.0F;

    canvas_->addRect(anim.screenX - expansion, anim.screenY - expansion,
                     anim.width + (expansion * 2.0F),
                     anim.height + (expansion * 2.0F), glowColor);
    canvas_->addLine(anim.screenX - expansion, anim.screenY - expansion,
                     anim.screenX + anim.width + expansion,
                     anim.screenY - expansion, 2.0F, 0xFDE68AFF);
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

bool TranscopyrightOverlay::picked(const render::PickingResult &pick,
                                   RenderState & /*state*/) {
  if (pick.tag.kind != render::tagKindOverlay ||
      pick.tag.clusterIndex < kTagUnlockBase) {
    return false;
  }

  const auto tagId = pick.tag.clusterIndex;
  for (const auto &badge : activeBadges_) {
    if (badge.tagId == tagId) {
      if (onUnlock_) {
        onUnlock_(badge.storeIndex, badge.span);
      }
      return true;
    }
  }

  return false;
}

} // namespace xudu
