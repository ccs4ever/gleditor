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

#include <gleditor/ranges.hpp>

#include "xudu/link_context.hpp"

namespace xudu {

namespace {

std::optional<glm::vec3>
screenPointOnPlane(const glm::mat4 &inverseView, const glm::vec2 screen,
                   const float screenW, const float screenH, const float z) {
  const float x       = 2.0F * screen.x / screenW - 1.0F;
  const float y       = 2.0F * screen.y / screenH - 1.0F;
  const auto nearClip = inverseView * glm::vec4(x, y, -1.0F, 1.0F);
  const auto farClip  = inverseView * glm::vec4(x, y, 1.0F, 1.0F);
  if (std::abs(nearClip.w) < 1.0e-6F || std::abs(farClip.w) < 1.0e-6F)
    return std::nullopt;
  const glm::vec3 nearPoint = glm::vec3(nearClip) / nearClip.w;
  const glm::vec3 farPoint  = glm::vec3(farClip) / farClip.w;
  const float deltaZ        = farPoint.z - nearPoint.z;
  if (std::abs(deltaZ) < 1.0e-6F) return std::nullopt;
  return nearPoint + (z - nearPoint.z) / deltaZ * (farPoint - nearPoint);
}

} // namespace

SatelloidOverlay::SatelloidOverlay(RendererRef renderer, std::string fontName)
    : renderer_(std::move(renderer)), fontName_(std::move(fontName)) {}

SatelloidOverlay::~SatelloidOverlay() = default;

void SatelloidOverlay::setSatelloid(CellSatelloid satelloid) {
  if (satelloid.neighborhood.empty() && neighborhoodResolver_) {
    satelloid.neighborhood = neighborhoodResolver_(satelloid.cellRef);
    if (neighborhoodRevision_)
      satelloid.neighborhoodRevision = neighborhoodRevision_();
    if (!satelloid.neighborhood.empty()) {
      satelloid.text = satelloid.neighborhood.front().text;
    }
  }
  if (axisNameResolver_) satelloid.dimName = axisNameResolver_();
  for (auto &s : satelloids_) {
    if (!s.occurrence && s.cellRef == satelloid.cellRef &&
        s.linkId == satelloid.linkId) {
      s.originPos            = satelloid.originPos;
      s.targetPos            = satelloid.targetPos;
      s.width                = satelloid.width;
      s.height               = satelloid.height;
      s.text                 = satelloid.text;
      s.neighborhood         = std::move(satelloid.neighborhood);
      s.neighborhoodRevision = satelloid.neighborhoodRevision;
      s.dimName              = satelloid.dimName;
      s.accentColor          = satelloid.accentColor;
      s.active               = satelloid.active;
      s.currentPos           = satelloid.currentPos;
      s.targetAlpha          = satelloid.active ? 1.0F : 0.0F;
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
  device_   = &device;
  pipeline_ = pipeline;
  canvas_   = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(pipeline, true);
}

bool SatelloidOverlay::busy() const {
  return std::ranges::any_of(satelloids_, [](const auto &s) {
    return s.pulseAlpha > 0.01F || std::abs(s.alpha - s.targetAlpha) > 0.02F ||
           (s.screenPlaced &&
            glm::distance(s.screenPos, s.screenTarget) > 0.5F) ||
           std::abs(s.depth - s.targetDepth) > 0.02F;
  });
}

void SatelloidOverlay::synchronizeSelection() {
  for (auto &s : satelloids_) {
    if (s.occurrence) {
      s.active      = false;
      s.targetAlpha = 0.0F;
    }
  }
  if (!linkContext_) return;
  const auto selected = linkContext_->selection();
  if (!selected || !selected->occurrences) return;
  for (const auto side : {xanadu::LinkSide::Left, xanadu::LinkSide::Right}) {
    const auto &members = selected->occurrences->members(side);
    const auto &cursor  = selected->cursor(side);
    for (std::size_t member = 0; member < members.size(); ++member) {
      for (std::size_t occurrence = 0;
           occurrence < members[member].occurrences.size(); ++occurrence) {
        const auto *cell = std::get_if<xanadu::CellSite>(
            &members[member].occurrences[occurrence].site);
        if (!cell || (siteFilter_ && !siteFilter_(*cell))) continue;
        const SatelloidOccurrence id{selected->key.id,
                                     static_cast<std::uint32_t>(side),
                                     static_cast<std::uint32_t>(member),
                                     static_cast<std::uint32_t>(occurrence)};
        auto found = std::ranges::find_if(
            satelloids_, [&](const auto &s) { return s.occurrence == id; });
        if (found == satelloids_.end()) {
          CellSatelloid card;
          card.cellRef    = cell->cell;
          card.linkId     = selected->key.id;
          card.occurrence = id;
          if (neighborhoodResolver_) {
            card.neighborhood = neighborhoodResolver_(cell->cell);
            if (neighborhoodRevision_)
              card.neighborhoodRevision = neighborhoodRevision_();
          }
          if (axisNameResolver_) card.dimName = axisNameResolver_();
          if (!card.neighborhood.empty())
            card.text = card.neighborhood.front().text;
          card.alpha = 0.0F;
          satelloids_.push_back(std::move(card));
          found = std::prev(satelloids_.end());
        } else if (found->cellRef != cell->cell) {
          found->cellRef      = cell->cell;
          found->neighborhood = neighborhoodResolver_
                                    ? neighborhoodResolver_(cell->cell)
                                    : std::vector<SatelloidNeighbor>{};
          if (neighborhoodRevision_)
            found->neighborhoodRevision = neighborhoodRevision_();
        }
        found->selected = side == selected->active && cursor.member == member &&
                          cursor.occurrence == occurrence;
        found->active      = true;
        found->targetAlpha = 1.0F;
        if (anchorResolver_) {
          if (const auto anchor = anchorResolver_(cell->cell)) {
            found->currentPos = *anchor;
          }
        }
      }
    }
  }
}

void SatelloidOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!visible_ || !canvas_) {
    return;
  }

  synchronizeSelection();
  if (satelloids_.empty()) return;

  const auto now = std::chrono::steady_clock::now();
  const float dt = std::clamp(
      std::chrono::duration<float>(now - lastFrameTime_).count(), 0.0F, 0.1F);
  lastFrameTime_ = now;

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  if (screenW <= 1.0F || screenH <= 1.0F) {
    return;
  }
  if (neighborhoodResolver_ && neighborhoodRevision_) {
    const auto revision = neighborhoodRevision_();
    for (auto &s : satelloids_) {
      if (s.active && s.neighborhoodRevision != revision) {
        s.neighborhood         = neighborhoodResolver_(s.cellRef);
        s.neighborhoodRevision = revision;
        s.text                 = s.neighborhood.empty() ? std::string{}
                                                        : s.neighborhood.front().text;
        if (axisNameResolver_) s.dimName = axisNameResolver_();
      }
    }
  }

  constexpr float cardW  = 200.0F;
  constexpr float cardH  = 180.0F;
  constexpr float gutter = 18.0F;
  std::vector<std::size_t> occurrences;
  for (std::size_t i = 0; i < satelloids_.size(); ++i) {
    if (satelloids_[i].occurrence && satelloids_[i].active) {
      occurrences.push_back(i);
    }
  }
  const bool stacked = !occurrences.empty();
  std::size_t chosen = 0;
  for (std::size_t i = 0; i < occurrences.size(); ++i) {
    if (satelloids_[occurrences[i]].selected) chosen = i;
  }
  const auto anchor = stacked ? gleditor::spatial::projectToScreen(
                                    ctx.viewProjection,
                                    satelloids_[occurrences[chosen]].currentPos,
                                    screenW, screenH)
                              : glm::vec2(screenW * 0.5F, screenH * 0.5F);
  const float stackX =
      std::clamp(anchor.x, gutter, std::max(gutter, screenW - cardW - gutter));
  const float stackY = std::clamp(anchor.y - cardH * 0.5F, gutter,
                                  std::max(gutter, screenH - cardH - gutter));
  for (std::size_t i = 0; i < satelloids_.size(); ++i) {
    auto &s = satelloids_[i];
    if (s.occurrence && !s.active) {
      s.targetAlpha = 0.0F;
    }
    if (!s.occurrence && !stacked) s.targetAlpha = s.active ? 1.0F : 0.0F;
    if (!s.occurrence && stacked &&
        std::ranges::any_of(occurrences, [&](const auto at) {
          return satelloids_[at].cellRef == s.cellRef;
        })) {
      s.targetAlpha = 0.0F;
    }
    if (!s.occurrence) {
      s.targetDepth        = 0.0F;
      s.stackLayer         = 0;
      const auto projected = gleditor::spatial::projectToScreen(
          ctx.viewProjection, s.currentPos, screenW, screenH);
      s.screenTarget = {std::clamp(projected.x, gutter,
                                   std::max(gutter, screenW - cardW - gutter)),
                        std::clamp(projected.y - cardH * 0.5F, gutter,
                                   std::max(gutter, screenH - cardH - gutter))};
    }
    if (!s.screenPlaced) {
      s.screenPos    = s.screenTarget;
      s.screenPlaced = true;
    }
  }
  for (std::size_t i = 0; i < occurrences.size(); ++i) {
    auto &s           = satelloids_[occurrences[i]];
    s.stackLayer      = satelloidLayer(i, chosen, occurrences.size());
    const float layer = static_cast<float>(s.stackLayer);
    s.targetDepth     = -3.0F * layer;
    s.targetAlpha     = std::max(0.28F, 1.0F - layer * 0.18F);
    s.screenTarget    = {stackX - layer * 16.0F, stackY - layer * 18.0F};
    if (!s.screenPlaced) {
      s.screenPos    = s.screenTarget;
      s.screenPlaced = true;
    }
  }
  for (auto &s : satelloids_) s.updateDynamics(dt);
  std::erase_if(satelloids_, [](const CellSatelloid &s) {
    return !s.active && s.alpha <= 0.01F && s.pulseAlpha <= 0.01F;
  });

  drawnIndices_.clear();
  for (std::size_t index = 0; index < satelloids_.size(); ++index) {
    if (!satelloids_[index].selected) drawnIndices_.push_back(index);
  }
  for (std::size_t index = 0; index < satelloids_.size(); ++index) {
    if (satelloids_[index].selected) drawnIndices_.push_back(index);
  }
  std::stable_sort(drawnIndices_.begin(), drawnIndices_.end(),
                   [&](const auto left, const auto right) {
                     return satelloids_[left].stackLayer >
                            satelloids_[right].stackLayer;
                   });
  while (extraCanvases_.size() + 1 < drawnIndices_.size()) {
    auto next = std::make_unique<gleditor::Canvas>(device_, fontName_);
    next->createPipeline(pipeline_, true);
    extraCanvases_.push_back(std::move(next));
  }
  const auto inverseView = glm::inverse(ctx.viewProjection);
  const float frontZ =
      stacked ? satelloids_[occurrences[chosen]].currentPos.z + 18.0F : 1.0F;
  const auto frontOrigin = screenPointOnPlane(inverseView, {stackX, stackY},
                                              screenW, screenH, frontZ);
  const auto frontRight  = screenPointOnPlane(
      inverseView, {stackX + cardW, stackY}, screenW, screenH, frontZ);
  const auto frontUp = screenPointOnPlane(inverseView, {stackX, stackY + cardH},
                                          screenW, screenH, frontZ);
  if (!frontOrigin || !frontRight || !frontUp) return;
  const glm::vec3 worldX = (*frontRight - *frontOrigin) / cardW;
  const glm::vec3 worldY = (*frontUp - *frontOrigin) / cardH;
  for (std::size_t drawIndex = 0; drawIndex < drawnIndices_.size();
       ++drawIndex) {
    const auto &s = satelloids_[drawnIndices_[drawIndex]];
    if (s.alpha <= 0.01F && s.pulseAlpha <= 0.01F) {
      continue;
    }
    auto *cardCanvas =
        drawIndex == 0 ? canvas_.get() : extraCanvases_[drawIndex - 1].get();
    cardCanvas->clear();
    constexpr float x0 = 0.0F;
    constexpr float y0 = 0.0F;

    // Card background quad
    const auto bgA            = static_cast<std::uint8_t>(238.0F * s.alpha);
    const std::uint32_t bgCol = 0x20242B00U | bgA;
    // Tagged by position in this overlay's own list rather than by cell: a
    // cell reference is unbounded, and base-plus-cell ran into every other
    // overlay's tag range once a manifold passed a thousand cells.
    cardCanvas->setTag(render::tagKindOverlay,
                       kTagSatelloidBase +
                           static_cast<std::uint32_t>(drawIndex));
    cardCanvas->addRect(x0, y0, cardW, cardH, bgCol);

    const auto borderA = static_cast<std::uint8_t>(255.0F * s.alpha);
    const std::uint32_t borderCol =
        (s.selected ? 0xFACC1500U : 0x38BDF800U) | borderA;
    const float border = s.selected ? 3.0F : 2.0F;
    const std::uint32_t glowCol =
        (borderCol & 0xFFFFFF00U) |
        static_cast<std::uint32_t>(static_cast<float>(borderA) * 0.22F);
    cardCanvas->addLine(x0 - 3.0F, y0 - 3.0F, x0 + cardW + 3.0F, y0 - 3.0F,
                        6.0F, glowCol);
    cardCanvas->addLine(x0 + cardW + 3.0F, y0 - 3.0F, x0 + cardW + 3.0F,
                        y0 + cardH + 3.0F, 6.0F, glowCol);
    cardCanvas->addLine(x0 + cardW + 3.0F, y0 + cardH + 3.0F, x0 - 3.0F,
                        y0 + cardH + 3.0F, 6.0F, glowCol);
    cardCanvas->addLine(x0 - 3.0F, y0 + cardH + 3.0F, x0 - 3.0F, y0 - 3.0F,
                        6.0F, glowCol);
    cardCanvas->addLine(x0, y0, x0 + cardW, y0, border, borderCol);
    cardCanvas->addLine(x0 + cardW, y0, x0 + cardW, y0 + cardH, border,
                        borderCol);
    cardCanvas->addLine(x0 + cardW, y0 + cardH, x0, y0 + cardH, border,
                        borderCol);
    cardCanvas->addLine(x0, y0 + cardH, x0, y0, border, borderCol);

    // Header badge: "[#cellRef • dimName]"
    const std::string badgeText =
        "[#" + std::to_string(s.cellRef) + " \u2022 " + s.dimName + "]";
    cardCanvas->addText(ctx.state, x0 + 12.0F, y0 + cardH - 22.0F, badgeText,
                        borderCol, bgCol);
    if (s.neighborhood.empty()) {
      std::string preview = s.text;
      if (preview.size() > 32) preview = preview.substr(0, 29) + "...";
      cardCanvas->addText(ctx.state, x0 + 14.0F, y0 + cardH * 0.5F, preview,
                          0xE2E8F000U | borderA, bgCol);
    } else {
      std::uint32_t maxDepth = 1;
      for (const auto &node : s.neighborhood) {
        maxDepth = std::max(maxDepth, node.depth);
      }
      const float scale = std::min(30.0F, 62.0F / static_cast<float>(maxDepth));
      for (const auto &node : s.neighborhood) {
        const float nx = x0 + cardW * 0.5F + node.place.x * scale;
        const float ny = y0 + cardH * 0.46F + node.place.y * scale;
        const float falloff =
            node.depth == 0
                ? 1.0F
                : std::max(0.16F, 1.0F - static_cast<float>(node.depth) /
                                             static_cast<float>(maxDepth + 1));
        const auto nodeA = static_cast<std::uint8_t>(borderA * falloff);
        const std::uint32_t nodeBg =
            (node.depth == 0 ? 0x34415500U : 0x2A313900U) | nodeA;
        cardCanvas->addRect(nx - 24.0F, ny - 11.0F, 48.0F, 22.0F, nodeBg);
        std::string label =
            node.text.empty() ? "#" + std::to_string(node.cell) : node.text;
        if (label.size() > 7) label = label.substr(0, 6) + "...";
        cardCanvas->addText(ctx.state, nx - 21.0F, ny - 4.0F, label,
                            (node.depth == 0 ? borderCol : 0xCBD5E100U | nodeA),
                            nodeBg);
      }
    }

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
        const float x1 = cx + r * std::cos(theta1);
        const float y1 = cy + r * std::sin(theta1);
        const float x2 = cx + r * std::cos(theta2);
        const float y2 = cy + r * std::sin(theta2);
        cardCanvas->addLine(x1, y1, x2, y2, 2.0F, ringCol);
      }
    }
    const auto origin = screenPointOnPlane(inverseView, s.screenPos, screenW,
                                           screenH, frontZ + s.depth);
    if (!origin) continue;
    glm::mat4 model(1.0F);
    model[0] = glm::vec4(worldX, 0.0F);
    model[1] = glm::vec4(worldY, 0.0F);
    model[3] = glm::vec4(*origin, 1.0F);
    cardCanvas->commit();
    cardCanvas->draw(ctx.state, ctx.viewProjection * model);
  }
}

bool SatelloidOverlay::picked(const render::PickingResult &pick,
                              RenderState & /*state*/) {
  if (render::tagKindOverlay != pick.tag.kind ||
      pick.tag.clusterIndex < kTagSatelloidBase ||
      pick.tag.clusterIndex - kTagSatelloidBase >= drawnIndices_.size()) {
    return false;
  }
  const auto &card =
      satelloids_[drawnIndices_[pick.tag.clusterIndex - kTagSatelloidBase]];
  const auto cellRef = card.cellRef;
  triggerPulse(cellRef);
  if (card.occurrence && linkContext_) {
    const auto selected = linkContext_->selection();
    if (selected && selected->key.id == card.occurrence->link) {
      const auto side = card.occurrence->side == 0 ? xanadu::LinkSide::Left
                                                   : xanadu::LinkSide::Right;
      static_cast<void>(linkContext_->execute(
          xanadu::nav::EnterAt{.key        = selected->key,
                               .side       = side,
                               .member     = card.occurrence->member,
                               .occurrence = card.occurrence->occurrence}));
      return true;
    }
  }
  if (navigationCb_) {
    navigationCb_(cellRef, false);
  }
  return true;
}

} // namespace xudu
