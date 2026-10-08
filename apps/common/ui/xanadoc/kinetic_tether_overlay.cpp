/**
 * @file apps/xudu/kinetic_tether_overlay.cpp
 * @brief Visual overlay implementation for Hookean spring tether and floating
 * blueprint quad.
 */
#include "kinetic_tether_overlay.hpp"
#include "world_card_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>

#include <gleditor/paths.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/spatial.hpp>

namespace xanadu {
struct KineticTetherOverlay::Presentation {
  world_cards::Card card;
  WorldCardConfig config{190, 88};
  QuotationTetherConfig tether;
  float scale{};
  glm::mat4 matrix{1};
  gleditor::ui::Size viewport;
  glm::vec2 origin{}, position{};
  bool geometryReady{}, detached{}, visible{};
  std::uint64_t revision{1};
  const std::string attachedStatus =
      "Snap-back zone (< " +
      std::to_string(
          static_cast<int>(KineticTetherEngine::kMinDetachmentDistance)) +
      " px)";
};

KineticTetherOverlay::KineticTetherOverlay(KineticTetherEngine &engine,
                                           std::string fontName)
    : presentation_(std::make_unique<Presentation>()), engine_(engine),
      fontName_(std::move(fontName)) {}

KineticTetherOverlay::~KineticTetherOverlay() = default;

void KineticTetherOverlay::deviceReady(render::RenderDevice &device,
                                       const render::PipelineDesc &pipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(
      &device, gleditor::ui::scaledFontDescription(
                   fontName_, gleditor::ui::FontRole::Body, {},
                   gleditor::ui::defaultTheme()));
  canvas_->createPipeline(pipeline, false);
  ribbons_ = std::make_unique<gleditor::CurveRibbons>(&device);
  ribbons_->createPipeline(gleditor::assetPath("shaders"),
                           gleditor::assetPath("shaders/vulkan"));
  presentation_->card.panel.deviceReady(device, pipeline, false);
}

bool KineticTetherOverlay::busy() const {
  return engine_.state() == TetherState::SnappingBack;
}

void KineticTetherOverlay::drawTether(const glm::vec2 &from,
                                      const glm::vec2 &pointer,
                                      const gleditor::ui::UiMetrics &metrics) {
  const auto &cfg       = presentation_->tether;
  const auto run        = pointer - from;
  const float distance  = glm::length(run);
  const float radius    = metrics.px(cfg.pointerRadiusPx);
  const float gap       = metrics.px(cfg.pointerGapPx);
  const float clearance = radius + gap;
  const auto colour     = cfg.colour;
  ribbons_->clear();
  if (distance > clearance) {
    const auto to = pointer - run / distance * clearance;
    const float sag =
        std::min(metrics.px(cfg.sagPx), glm::length(to - from) * cfg.sagShare);
    const auto reach = (to - from) / 3.F;
    const std::array poles{
        glm::vec3(from, 0), glm::vec3(from + reach + glm::vec2(0, -sag), 0),
        glm::vec3(to - reach + glm::vec2(0, -sag), 0), glm::vec3(to, 0)};
    const std::array weights{1.F, 1.F, 1.F, 1.F};
    const std::array knots{0.F, 0.F, 0.F, 0.F, 1.F, 1.F, 1.F, 1.F};
    ribbons_->addNurbs(gleditor::NurbsPath(poles, weights, knots, 3),
                       {.startWidth      = metrics.px(cfg.rootWidthPx),
                        .endWidth        = metrics.px(cfg.tipWidthPx),
                        .edgeSoftness    = 1.F,
                        .texturePeriod   = metrics.px(cfg.texturePeriodPx),
                        .textureStrength = cfg.textureStrength},
                       colour, 0, pointer, radius);
  }
  ribbons_->commit();
  canvas_->clear();
  canvas_->setTag(render::tagKindOverlay, 0);
  const float root = metrics.px(cfg.rootWidthPx) * .5F;
  // At short distances the anchor can coincide with the pointer; no feedback
  // may cover its centre even when no stem fits between the endpoints.
  if (distance > radius + root)
    canvas_->addRect(from.x - root, from.y - root, root * 2, root * 2, colour);
  const float stroke = std::min(radius * .5F, metrics.px(1.F));
  canvas_->addRect(pointer.x - radius, pointer.y + radius - stroke, radius * 2,
                   stroke, colour);
  canvas_->addRect(pointer.x - radius, pointer.y - radius, radius * 2, stroke,
                   colour);
  canvas_->addRect(pointer.x - radius, pointer.y - radius, stroke, radius * 2,
                   colour);
  canvas_->addRect(pointer.x + radius - stroke, pointer.y - radius, stroke,
                   radius * 2, colour);
  canvas_->commit();
}

void KineticTetherOverlay::setTetherConfig(
    const QuotationTetherConfig &config) {
  if (presentation_->tether != config) {
    presentation_->tether        = config;
    presentation_->geometryReady = false;
    engine_.setReducedMotion(config.reducedMotion);
  }
}

void KineticTetherOverlay::setConfig(const WorldCardConfig &config) {
  presentation_->config = config;
}
std::uint64_t KineticTetherOverlay::accessibilityRevision() const {
  return presentation_->revision + presentation_->card.revision;
}
bool KineticTetherOverlay::performAction(std::uint64_t, gleditor::a11y::Action,
                                         std::string_view) {
  return false;
}
std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>>
KineticTetherOverlay::snapshots() const {
  return presentation_->visible
             ? std::vector{presentation_->card.panel.snapshot()}
             : std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>>{};
}
void KineticTetherOverlay::describe(gleditor::a11y::Builder &into) {
  if (!presentation_->visible) return;
  world_cards::describe(into, presentation_->card.panel, presentation_->matrix,
                        presentation_->viewport, 1, "Blueprint card",
                        engine_.payload().previewText);
}
void KineticTetherOverlay::drawFrame(gleditor::FrameContext &ctx) {
  auto &p = *presentation_;
  if (!canvas_ || !engine_.busy()) {
    if (p.visible) {
      p.visible = false;
      ++p.revision;
    }
    return;
  }
  engine_.stepPhysics();
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  p.card.configure(metrics, ctx.theme, p.config, fontName_);
  const bool detached = engine_.isDetached();
  p.card.content(engine_.collecting()
                     ? "Collect quotation"
                     : (detached ? "Spawn xanadoc" : "Blueprint card"),
                 engine_.payload().previewText,
                 engine_.collecting() ? "Release into the pouch"
                 : detached           ? "Release to materialize"
                                      : std::string_view(p.attachedStatus));
  p.card.prepare();
  const auto pos =
      engine_.isDragging() ? engine_.targetPos() : engine_.currentPos();
  const auto bounds = world_cards::awayFromPointer(
      p.card.presentation.size, pos, metrics,
      std::max(p.card.presentation.gap, metrics.px(1)));
  p.card.panel.setBounds({0, 0, bounds.width, bounds.height});
  (void)p.card.panel.prepare(metrics, p.card.presentation.theme);
  const auto matrix =
      world_cards::screenMatrix(bounds, {static_cast<float>(ctx.screenWidth),
                                         static_cast<float>(ctx.screenHeight)});
  if (!p.visible || p.matrix != matrix || p.detached != detached ||
      p.card.dirty)
    ++p.revision;
  p.visible  = true;
  p.matrix   = matrix;
  p.viewport = {static_cast<float>(ctx.screenWidth),
                static_cast<float>(ctx.screenHeight)};
  if (!p.geometryReady || p.origin != engine_.originPos() ||
      p.position != pos || p.detached != detached ||
      p.scale != metrics.px(1.F)) {
    drawTether(engine_.originPos(), pos, metrics);
    p.geometryReady = true;
    p.origin        = engine_.originPos();
    p.position      = pos;
    p.detached      = detached;
    p.scale         = metrics.px(1.F);
  }
  const auto projection =
      glm::ortho(0.F, p.viewport.width, 0.F, p.viewport.height, -1.F, 1.F);
  ribbons_->draw(ctx.state, projection);
  canvas_->draw(ctx.state, projection);
  p.card.panel.draw(ctx.state, p.matrix, p.viewport);
}
} // namespace xanadu
