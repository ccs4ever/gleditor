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

#include <gleditor/render/types.hpp>
#include <gleditor/spatial.hpp>

namespace xanadu {
struct KineticTetherOverlay::Presentation {
  world_cards::Card card;
  WorldCardConfig config{190, 88};
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
  presentation_->card.panel.deviceReady(device, pipeline, false);
}

bool KineticTetherOverlay::busy() const {
  return engine_.state() == TetherState::SnappingBack;
}

void KineticTetherOverlay::drawTether(gleditor::Canvas &canvas,
                                      RenderState & /*unused*/,
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

  // Subdivided quadratic Bezier, stopping short of the pointer: whatever is
  // drawn under the pointer answers the pick that decides where a drop
  // lands, and the tether is never what it was dropped on.
  constexpr int kSegments = 16;
  glm::vec2 prevPt        = p0;
  for (int i = 1; i < kSegments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSegments);
    const glm::vec2 pt =
        gleditor::spatial::evaluateQuadraticBezier(p0, mid, p1, t);
    canvas.addLine(prevPt.x, prevPt.y, pt.x, pt.y, thickness, col);
    prevPt = pt;
  }

  // Cursor end: a hollow ring, for the same reason -- the pixel under the
  // pointer stays whatever the drag is over.
  constexpr float kRing = 6.0F;
  constexpr float kBar  = 2.0F;
  canvas.addRect(p1.x - kRing, p1.y + kRing - kBar, 2.0F * kRing, kBar, col);
  canvas.addRect(p1.x - kRing, p1.y - kRing, 2.0F * kRing, kBar, col);
  canvas.addRect(p1.x - kRing, p1.y - kRing, kBar, 2.0F * kRing, col);
  canvas.addRect(p1.x + kRing - kBar, p1.y - kRing, kBar, 2.0F * kRing, col);
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
  p.card.content(detached ? "Spawn xanadoc" : "Blueprint card",
                 engine_.payload().previewText,
                 detached ? "Release to materialize"
                          : std::string_view(p.attachedStatus));
  p.card.prepare();
  const auto pos    = engine_.currentPos();
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
      p.position != pos || p.detached != detached) {
    canvas_->clear();
    drawTether(*canvas_, ctx.state, engine_.originPos(), pos, detached);
    canvas_->commit();
    p.geometryReady = true;
    p.origin        = engine_.originPos();
    p.position      = pos;
    p.detached      = detached;
  }
  canvas_->draw(ctx.state, glm::ortho(0.F, p.viewport.width, 0.F,
                                      p.viewport.height, -1.F, 1.F));
  p.card.panel.draw(ctx.state, p.matrix, p.viewport);
}
} // namespace xanadu
