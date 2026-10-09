/**
 * @file satelloid.cpp
 * @brief Flying Cell Satelloid proxy quads and focus ring overlay
 * implementation.
 */
#include "common/ui/xanadoc/satelloid.hpp"
#include "world_card_presentation.hpp"
#include <gleditor/render_state.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>
#include <utility>

#include <gleditor/spatial.hpp>

#include <gleditor/ranges.hpp>

#include "common/ui/xanadoc/link_context.hpp"

namespace xanadu {
struct SatelloidOverlay::Presentation {
  struct Slot {
    gleditor::ui::WorldPanel panel;
    std::unique_ptr<gleditor::Canvas> pulse;
    std::string text, dimension;
    std::vector<SatelloidNeighbor> neighbors;
    zigzag::CellRef cell{zigzag::noCell};
    gleditor::ui::UiMetrics metrics;
    gleditor::ui::Theme theme;
    glm::mat4 matrix{1};
    std::uint32_t id{};
    bool ready{}, drawn{};
  };
  WorldCardConfig config;
  std::vector<std::unique_ptr<Slot>> slots;
  std::vector<std::size_t> occurrences;
  std::vector<std::uint32_t> ids;
  std::shared_ptr<const std::vector<std::uint32_t>> targets;
  std::vector<std::uint32_t> pending;
  std::uint32_t next{1}, scope{};
  std::uint64_t selectionRevision{}, revision{1};
  world_cards::Style style;
  gleditor::ui::UiMetrics metrics;
  gleditor::ui::Theme theme;
  gleditor::ui::Size viewport;
  bool ready{};
};

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
    : presentation_(std::make_unique<Presentation>()),
      renderer_(std::move(renderer)), fontName_(std::move(fontName)) {}

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
  for (auto &slot : presentation_->slots)
    if (slot->cell == cellRef) slot->drawn = false;
  ++presentation_->revision;
  std::erase_if(satelloids_, [cellRef](const CellSatelloid &s) {
    return s.cellRef == cellRef;
  });
}

void SatelloidOverlay::clear() {
  satelloids_.clear();
  for (auto &slot : presentation_->slots) slot->drawn = false;
  ++presentation_->revision;
}

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
  for (auto &slot : presentation_->slots) {
    slot->panel.deviceReady(device, pipeline, true);
    slot->pulse.reset();
  }
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
        found->active   = true;
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
  if (!visible_ || !device_) {
    return;
  }

  auto &presentation = *presentation_;
  if (!linkContext_ ||
      presentation.selectionRevision != linkContext_->revision()) {
    synchronizeSelection();
    if (linkContext_) {
      presentation.selectionRevision = linkContext_->revision();
      for (auto &card : satelloids_)
        if (card.occurrence) card.presentationId = 0;
    }
  }
  for (const auto id : presentation.pending) {
    render::PickingResult action;
    action.tag.kind        = render::tagKindOverlay;
    action.tag.docIndex    = presentation.scope;
    action.overlayWidgetId = id;
    (void)picked(action, ctx.state);
  }
  presentation.pending.clear();
  if (satelloids_.empty()) {
    for (auto &slot : presentation.slots) {
      if (slot->drawn) ++presentation.revision;
      slot->drawn = false;
    }
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
  if (neighborhoodResolver_ && neighborhoodRevision_) {
    const auto revision = neighborhoodRevision_();
    for (auto &s : satelloids_) {
      if (s.active && s.neighborhoodRevision != revision) {
        s.neighborhood         = neighborhoodResolver_(s.cellRef);
        s.neighborhoodRevision = revision;
        s.text = s.neighborhood.empty() ? std::string{}
                                        : s.neighborhood.front().text;
        if (axisNameResolver_) s.dimName = axisNameResolver_();
      }
    }
  }

  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  if (!presentation.ready || presentation.metrics != metrics ||
      presentation.theme != ctx.theme) {
    presentation.metrics = metrics;
    presentation.theme   = ctx.theme;
    presentation.style   = world_cards::style(
        metrics, ctx.theme, presentation.config,
        gleditor::ui::FontRole::Caption, fontName_,
        static_cast<float>(presentation.config.maxLines) + 2);
    presentation.ready = true;
    ++presentation.revision;
  }
  const auto cardW  = presentation.style.size.width,
             cardH  = presentation.style.size.height;
  const auto safe   = metrics.pixelSafeArea();
  auto &occurrences = presentation.occurrences;
  occurrences.clear();
  for (auto &s : satelloids_) {
    if (!s.presentationId) s.presentationId = presentation.next++;
    if (s.occurrence && anchorResolver_)
      if (const auto anchor = anchorResolver_(s.cellRef))
        s.currentPos = *anchor;
  }
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
  const auto anchor  = stacked ? gleditor::spatial::projectToScreen(
                                     ctx.viewProjection,
                                     satelloids_[occurrences[chosen]].currentPos,
                                     screenW, screenH)
                               : glm::vec2(screenW * 0.5F, screenH * 0.5F);
  const float stackX = std::clamp(
      anchor.x, safe.left, std::max(safe.left, safe.left + safe.width - cardW));
  const float stackY =
      std::clamp(anchor.y - cardH * .5F, safe.bottom,
                 std::max(safe.bottom, safe.bottom + safe.height - cardH));
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
      s.screenTarget = {
          std::clamp(projected.x, safe.left,
                     std::max(safe.left, safe.left + safe.width - cardW)),
          std::clamp(projected.y - cardH * .5F, safe.bottom,
                     std::max(safe.bottom, safe.bottom + safe.height - cardH))};
    }
    if (!s.screenPlaced) {
      s.screenPos    = s.screenTarget;
      s.screenPlaced = true;
    }
  }
  for (std::size_t i = 0; i < occurrences.size(); ++i) {
    auto &s                  = satelloids_[occurrences[i]];
    s.stackLayer             = satelloidLayer(i, chosen, occurrences.size());
    const float layer        = static_cast<float>(s.stackLayer);
    s.targetDepth            = -3.0F * layer;
    s.targetAlpha            = std::max(0.28F, 1.0F - layer * 0.18F);
    const auto stackedBounds = gleditor::ui::clampToSafeArea(
        {stackX - layer * metrics.px(16), stackY - layer * metrics.px(18),
         cardW, cardH},
        safe);
    s.screenTarget = {stackedBounds.left, stackedBounds.bottom};
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
  std::sort(drawnIndices_.begin(), drawnIndices_.end(),
            [&](const auto left, const auto right) {
              const auto &a = satelloids_[left], &b = satelloids_[right];
              if (a.stackLayer != b.stackLayer)
                return a.stackLayer > b.stackLayer;
              if (a.selected != b.selected) return !a.selected;
              return left < right;
            });
  while (presentation.slots.size() < drawnIndices_.size()) {
    auto slot = std::make_unique<Presentation::Slot>();
    slot->panel.deviceReady(*device_, pipeline_, true);
    presentation.slots.push_back(std::move(slot));
  }
  for (auto &slot : presentation.slots) slot->drawn = false;
  presentation.viewport = {screenW, screenH};
  if (!presentation.scope)
    presentation.scope = ctx.state.allocatePersistentOverlayPickScope();
  presentation.ids.clear();
  for (const auto index : drawnIndices_)
    presentation.ids.push_back(satelloids_[index].presentationId);
  if (!presentation.targets || *presentation.targets != presentation.ids)
    presentation.targets =
        std::make_shared<const std::vector<std::uint32_t>>(presentation.ids);
  ctx.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, presentation.scope, 0),
      presentation.targets);
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
    auto &slot        = *presentation.slots[drawIndex];
    const auto origin = screenPointOnPlane(inverseView, s.screenPos, screenW,
                                           screenH, frontZ + s.depth);
    if (!origin) continue;
    glm::mat4 model(1);
    model[0]          = glm::vec4(worldX, 0);
    model[1]          = glm::vec4(worldY, 0);
    model[3]          = glm::vec4(*origin, 1);
    const auto matrix = ctx.viewProjection * model;
    auto theme        = presentation.style.theme;
    theme.colours.border =
        s.selected ? glm::vec4(1, .8F, .1F, 1) : theme.colours.accent;
    if (!slot.ready || slot.metrics != metrics || slot.theme != theme ||
        slot.text != s.text || slot.dimension != s.dimName ||
        slot.cell != s.cellRef || slot.neighbors != s.neighborhood) {
      slot.text      = s.text;
      slot.dimension = s.dimName;
      slot.cell      = s.cellRef;
      slot.neighbors = s.neighborhood;
      slot.metrics   = metrics;
      slot.theme     = theme;
      slot.ready     = true;
      namespace ui   = gleditor::ui;
      ui::Widget root{
          .id = 1, .model = ui::Panel{}, .fontRole = ui::FontRole::Caption};
      root.children.push_back(
          {.id = 2,
           .model =
               ui::Label{"#" + std::to_string(s.cellRef) + " · " + s.dimName,
                         ui::TextPurpose::Identifier},
           .preferred = {0, metrics.logical(presentation.style.line)},
           .fontRole  = ui::FontRole::Caption,
           .maxLines  = 1});
      if (s.neighborhood.empty())
        root.children.push_back(
            {.id       = 3,
             .model    = ui::Label{s.text, ui::TextPurpose::Description},
             .fontRole = ui::FontRole::Caption,
             .maxLines = presentation.config.maxLines});
      else {
        const auto p      = presentation.style.padding;
        const auto innerW = std::max(0.F, cardW - 4 * p),
                   innerH = std::max(0.F, cardH - presentation.style.line -
                                              4 * p - presentation.style.gap);
        ui::Widget nodes{.id        = 3,
                         .model     = ui::PositionedPanel{},
                         .preferred = {0, metrics.logical(innerH)},
                         .fontRole  = ui::FontRole::Caption};
        auto &placed   = std::get<ui::PositionedPanel>(nodes.model);
        float maximumX = 1, maximumY = 1;
        for (const auto &node : s.neighborhood) {
          maximumX = std::max(maximumX, std::abs(node.place.x));
          maximumY = std::max(maximumY, std::abs(node.place.y));
        }
        const auto availableH = std::max(0.F, innerH - 2 * p);
        const auto w =
            std::min(innerW / (2 * maximumX + 1), presentation.style.line * 4);
        const auto h     = std::min(availableH / (2 * maximumY + 1),
                                    presentation.style.line + 2 * p);
        const auto stepX = std::max(0.F, (innerW - w) / (2 * maximumX));
        const auto stepY = std::max(0.F, (availableH - h) / (2 * maximumY));
        std::uint32_t id = 100;
        for (const auto &node : s.neighborhood) {
          nodes.children.push_back(
              {.id = id++,
               .model =
                   ui::Badge{node.text.empty() ? "#" + std::to_string(node.cell)
                                               : node.text,
                             node.depth == 0 ? ui::Tone::Accent
                                             : ui::Tone::Muted},
               .fontRole = ui::FontRole::Caption,
               .maxLines = 1});
          placed.childBounds.push_back(
              {metrics.logical((innerW - w) * .5F + node.place.x * stepX),
               metrics.logical((availableH - h) * .5F + node.place.y * stepY),
               metrics.logical(w), metrics.logical(h)});
        }
        root.children.push_back(std::move(nodes));
      }
      slot.panel.setBounds({0, 0, cardW, cardH});
      slot.panel.setModel(std::move(root));
      slot.panel.setLabelLod({0, theme.type.minFontPx});
      (void)slot.panel.prepare(metrics, theme);
      ++presentation.revision;
    }
    if (slot.matrix != matrix || slot.id != s.presentationId)
      ++presentation.revision;
    slot.matrix = matrix;
    slot.id     = s.presentationId;
    slot.drawn  = true;
    slot.panel.draw(ctx.state, matrix, presentation.viewport,
                    {.kind         = render::tagKindOverlay,
                     .docIndex     = presentation.scope,
                     .clusterIndex = static_cast<std::uint32_t>(drawIndex + 1)},
                    s.alpha);
    if (s.pulseAlpha > .01F) {
      if (!slot.pulse) {
        slot.pulse = std::make_unique<gleditor::Canvas>(
            device_, gleditor::ui::scaledFontDescription(
                         fontName_, gleditor::ui::FontRole::Body, {},
                         gleditor::ui::defaultTheme()));
        slot.pulse->createPipeline(pipeline_, true);
      }
      slot.pulse->clear();
      slot.pulse->setTag(render::tagKindNone, 0);
      constexpr int segments = 16;
      for (int segment = 0; segment < segments; ++segment) {
        const auto a = 2 * std::numbers::pi_v<float> *
                       static_cast<float>(segment) / segments;
        const auto b = 2 * std::numbers::pi_v<float> *
                       static_cast<float>(segment + 1) / segments;
        slot.pulse->addLine(cardW * .5F + s.pulseRadius * std::cos(a),
                            cardH * .5F + s.pulseRadius * std::sin(a),
                            cardW * .5F + s.pulseRadius * std::cos(b),
                            cardH * .5F + s.pulseRadius * std::sin(b),
                            metrics.px(2), s.accentColor);
      }
      slot.pulse->commit();
      slot.pulse->draw(ctx.state, matrix, s.pulseAlpha);
    }
  }
}

bool SatelloidOverlay::picked(const render::PickingResult &pick,
                              RenderState & /*state*/) {
  auto &p = *presentation_;
  if (pick.tag.kind != render::tagKindOverlay || pick.tag.docIndex != p.scope ||
      !p.scope)
    return false;
  const auto id = pick.overlayWidgetId;
  if (!id) return true;
  const auto found =
      std::ranges::find(satelloids_, *id, &CellSatelloid::presentationId);
  if (found == satelloids_.end() || !found->active) return true;
  const auto &card   = *found;
  const auto cellRef = card.cellRef;
  triggerPulse(cellRef);
  if (card.occurrence && linkContext_) {
    const auto selected = linkContext_->selection();
    if (selected && selected->key.id == card.occurrence->link) {
      const auto side = card.occurrence->side == 0 ? xanadu::LinkSide::Left
                                                   : xanadu::LinkSide::Right;
      std::ignore     = linkContext_->execute(
          xanadu::nav::EnterAt{.key        = selected->key,
                               .side       = side,
                               .member     = card.occurrence->member,
                               .occurrence = card.occurrence->occurrence});
      return true;
    }
    return true;
  }
  if (navigationCb_) {
    navigationCb_(cellRef, false);
  }
  return true;
}

void SatelloidOverlay::setConfig(const WorldCardConfig &config) {
  presentation_->config = config;
  presentation_->ready  = false;
  for (auto &slot : presentation_->slots) slot->ready = false;
}
std::uint64_t SatelloidOverlay::accessibilityRevision() const {
  return presentation_->revision;
}
std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>>
SatelloidOverlay::snapshots() const {
  std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>> result;
  for (const auto &slot : presentation_->slots)
    if (slot->drawn) result.push_back(slot->panel.snapshot());
  return result;
}
void SatelloidOverlay::describe(gleditor::a11y::Builder &into) {
  for (const auto &slot : presentation_->slots)
    if (slot->drawn) {
      auto description = slot->text;
      for (const auto &node : slot->neighbors) description += " " + node.text;
      world_cards::describe(
          into, slot->panel, slot->matrix, presentation_->viewport, slot->id,
          "Cell #" + std::to_string(slot->cell) + " · " + slot->dimension,
          description, true);
    }
}
bool SatelloidOverlay::performAction(std::uint64_t id,
                                     gleditor::a11y::Action action,
                                     std::string_view) {
  if (action != gleditor::a11y::Action::Click) return false;
  id = gleditor::a11y::Ids::localOf(id);
  if (std::ranges::none_of(presentation_->slots, [&](const auto &slot) {
        return slot->drawn && slot->id == id;
      }))
    return false;
  presentation_->pending.push_back(static_cast<std::uint32_t>(id));
  return true;
}
} // namespace xanadu
