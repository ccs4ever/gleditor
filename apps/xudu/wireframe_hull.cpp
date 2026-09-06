/**
 * @file wireframe_hull.cpp
 * @brief Implementation of progressive 3D streaming wireframe hull and swarm
 * telemetry.
 */
#include "wireframe_hull.hpp"

#include <algorithm>
#include <cmath>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>

namespace xudu {

WireframeHullOverlay::WireframeHullOverlay(RendererRef renderer,
                                           std::string fontName)
    : renderer_(std::move(renderer)), fontName_(std::move(fontName)) {}

WireframeHullOverlay::~WireframeHullOverlay() = default;

void WireframeHullOverlay::deviceReady(render::RenderDevice &device,
                                       const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, false);
}

bool WireframeHullOverlay::busy() const {
  return !dissolvingHulls_.empty();
}

void WireframeHullOverlay::startLoading(const std::size_t docIndex,
                                        std::string title,
                                        std::string infoHash,
                                        const std::uint32_t totalPieces) {
  for (auto &ld : loadingDocs_) {
    if (ld.docIndex == docIndex) {
      ld.title        = std::move(title);
      ld.infoHash     = std::move(infoHash);
      ld.totalPieces  = (totalPieces > 0 ? totalPieces : 1U);
      ld.piecesFetched = 0;
      return;
    }
  }

  loadingDocs_.push_back(WireframeProgress{
      .docIndex      = docIndex,
      .title         = std::move(title),
      .infoHash      = std::move(infoHash),
      .piecesFetched = 0,
      .totalPieces   = (totalPieces > 0 ? totalPieces : 1U),
      .shimmerPhase  = 0.0F,
  });
}

void WireframeHullOverlay::updateProgress(const std::size_t docIndex,
                                          const std::uint32_t piecesFetched) {
  for (auto &ld : loadingDocs_) {
    if (ld.docIndex == docIndex) {
      ld.piecesFetched = std::min(piecesFetched, ld.totalPieces);
      if (ld.isComplete()) {
        finishLoading(docIndex);
      }
      return;
    }
  }
}

void WireframeHullOverlay::finishLoading(const std::size_t docIndex) {
  for (auto it = loadingDocs_.begin(); it != loadingDocs_.end(); ++it) {
    if (it->docIndex == docIndex) {
      loadingDocs_.erase(it);
      dissolvingHulls_.push_back(DissolvingHull{.docIndex = docIndex, .opacity = 1.0F});
      return;
    }
  }
}

bool WireframeHullOverlay::isLoading(const std::size_t docIndex) const noexcept {
  for (const auto &ld : loadingDocs_) {
    if (ld.docIndex == docIndex) return true;
  }
  return false;
}

void WireframeHullOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_ || (loadingDocs_.empty() && dissolvingHulls_.empty())) {
    return;
  }

  shimmerPhase_ += 0.05F;

  // Advance dissolving hulls
  for (auto it = dissolvingHulls_.begin(); it != dissolvingHulls_.end();) {
    it->opacity -= 0.05F;
    if (it->opacity <= 0.0F) {
      it = dissolvingHulls_.erase(it);
    } else {
      ++it;
    }
  }

  const auto width  = static_cast<float>(ctx.screenWidth);
  const auto height = static_cast<float>(ctx.screenHeight);
  const auto ortho  = glm::ortho(0.0F, width, 0.0F, height, -1.0F, 1.0F);

  canvas_->clear();
  canvas_->setTag(render::tagKindOverlay, 0);

  const float pulse = 0.5F + 0.5F * std::sin(shimmerPhase_ * 3.0F);
  const auto alphaByte =
      static_cast<std::uint32_t>(std::clamp(pulse * 255.0F, 80.0F, 255.0F));
  const auto cyanColour = 0x38BDF800U | (alphaByte & 0xFFU);

  for (const auto &ld : loadingDocs_) {
    float cx = width * 0.5F;
    float cy = height * 0.5F;

    if (ld.docIndex < ctx.state.docs.size()) {
      const auto &doc = ctx.state.docs[ld.docIndex];
      if (doc) {
        const auto origin4 =
            doc->modelMatrix() * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
        const auto clip = ctx.viewProjection * origin4;
        if (clip.w > 0.001F) {
          const auto ndc = glm::vec3(clip) / clip.w;
          cx = (ndc.x * 0.5F + 0.5F) * width;
          cy = (ndc.y * 0.5F + 0.5F) * height;
        }
      }
    }

    constexpr float halfW = 160.0F;
    constexpr float halfH = 220.0F;

    // 1. Ethereal Wireframe Hull Rectangles & Corner Brackets
    canvas_->addLine(cx - halfW, cy - halfH, cx + halfW, cy - halfH, 2.0F,
                     cyanColour);
    canvas_->addLine(cx + halfW, cy - halfH, cx + halfW, cy + halfH, 2.0F,
                     cyanColour);
    canvas_->addLine(cx + halfW, cy + halfH, cx - halfW, cy + halfH, 2.0F,
                     cyanColour);
    canvas_->addLine(cx - halfW, cy + halfH, cx - halfW, cy - halfH, 2.0F,
                     cyanColour);

    // Corner accents
    constexpr float corner = 18.0F;
    canvas_->addLine(cx - halfW, cy + halfH - corner, cx - halfW, cy + halfH,
                     3.5F, 0x38BDF8FF);
    canvas_->addLine(cx - halfW, cy + halfH, cx - halfW + corner, cy + halfH,
                     3.5F, 0x38BDF8FF);
    canvas_->addLine(cx + halfW - corner, cy + halfH, cx + halfW, cy + halfH,
                     3.5F, 0x38BDF8FF);
    canvas_->addLine(cx + halfW, cy + halfH, cx + halfW, cy + halfH - corner,
                     3.5F, 0x38BDF8FF);

    // 2. Telemetry Card
    constexpr float cardW = 260.0F;
    constexpr float cardH = 46.0F;
    canvas_->addRect(cx - (cardW * 0.5F), cy - (cardH * 0.5F), cardW, cardH,
                     0x0F172AF0);
    canvas_->addLine(cx - (cardW * 0.5F), cy - (cardH * 0.5F),
                     cx + (cardW * 0.5F), cy - (cardH * 0.5F), 1.5F,
                     0x38BDF8AA);

    // Progress bar inside card
    constexpr float barW = 240.0F;
    const float filledW  = barW * ld.progressFraction();
    canvas_->addRect(cx - 120.0F, cy - 14.0F, barW, 4.0F, 0x334155FF);
    if (filledW > 0.0F) {
      canvas_->addRect(cx - 120.0F, cy - 14.0F, filledW, 4.0F, 0x38BDF8FF);
    }

    // Telemetry text
    canvas_->addText(ctx.state, cx - 118.0F, cy + 2.0F, ld.progressStatusText(),
                     0xF8FAFCFF, 0);
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ortho);
}

} // namespace xudu
