/**
 * @file overview_overlay.cpp
 * @brief Drawing the overview panel and moving the camera from it.
 */
#include "overview_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

namespace xudu {

namespace {

/// Share of the panel's shorter side kept clear around the scene, so pages
/// and marks never touch its edge; also the size of a mark, which should read
/// as a point at the panel's scale whatever the panel's size.
constexpr float kInsetShare = 0.05F;

/// The view outline's thickness as a share of the inset: thin enough not to
/// hide the pages it frames, never thinner than a pixel.
constexpr float kOutlineShareOfInset = 0.25F;

} // namespace

void OverviewOverlay::setConfig(const xanadu::OverviewConfig &next) {
  if (next != config) {
    config = next;
    ++configRevision;
  }
}

void OverviewOverlay::deviceReady(render::RenderDevice &device,
                                  const render::PipelineDesc &pipeline) {
  // The panel draws no text; the canvas needs a font all the same.
  canvas = std::make_unique<gleditor::Canvas>(&device, state->defaultFontName);
  canvas->createPipeline(pipeline, false);
  builtFor.reset();
}

void OverviewOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas) {
    return;
  }
  Stamp stamp{.width   = ctx.screenWidth,
              .height  = ctx.screenHeight,
              .marks   = markRevision ? markRevision() : 0,
              .config  = configRevision,
              .visible = isVisible()};
  {
    std::scoped_lock locker(state->view);
    stamp.camera = state->view.pos;
    stamp.fov    = state->view.fov;
  }
  for (const auto &doc : ctx.state.docs) {
    if (doc) {
      ++stamp.documents;
      stamp.pages += doc->builtPageCount();
    }
  }
  if (builtFor != stamp) {
    builtFor = stamp;
    rebuild(ctx, stamp);
  }
  if (canvas->empty()) {
    return;
  }
  const auto ortho = glm::ortho( // NOLINT(readability-suspicious-call-argument)
      0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
      static_cast<float>(ctx.screenHeight), -1.0F, 1.0F);
  canvas->draw(ctx.state, ortho);
}

void OverviewOverlay::rebuild(gleditor::FrameContext &ctx, const Stamp &stamp) {
  canvas->clear();
  lastFit.reset();
  screenHeight = ctx.screenHeight;
  if (!stamp.visible || 0 == stamp.documents || ctx.screenHeight <= 0) {
    canvas->commit();
    return;
  }

  // Every built page, as a world rectangle at the depth documents rest on.
  pages.clear();
  glm::vec2 lo{std::numeric_limits<float>::max()};
  glm::vec2 hi{std::numeric_limits<float>::lowest()};
  const auto grow = [&](const glm::vec2 point) {
    lo = glm::min(lo, point);
    hi = glm::max(hi, point);
  };
  for (const auto &doc : ctx.state.docs) {
    if (!doc) {
      continue;
    }
    for (std::size_t p = 0; p < doc->numPages(); ++p) {
      const auto frame = doc->pageFrame(p);
      if (!frame) {
        continue;
      }
      const glm::vec2 a(frame->localToWorld *
                        glm::vec4(frame->leftPx, frame->bottomPx, 0.0F, 1.0F));
      const glm::vec2 b(frame->localToWorld *
                        glm::vec4(frame->rightPx, frame->topPx, 0.0F, 1.0F));
      pages.emplace_back(glm::min(a, b), glm::max(a, b));
      grow(a);
      grow(b);
    }
  }
  marks.clear();
  if (markSource) {
    markSource(ctx.state, marks);
    for (const auto &mark : marks) {
      grow(glm::vec2(mark));
    }
  }
  if (pages.empty()) {
    canvas->commit();
    return;
  }

  // What the camera shows, on the plane the documents rest on.
  const float halfH =
      std::max(stamp.camera.z, 0.0F) * std::tan(glm::radians(stamp.fov) * 0.5F);
  const float halfW = halfH * static_cast<float>(ctx.screenWidth) /
                      static_cast<float>(ctx.screenHeight);
  const glm::vec2 viewLo(stamp.camera.x - halfW, stamp.camera.y - halfH);
  const glm::vec2 viewHi(stamp.camera.x + halfW, stamp.camera.y + halfH);

  panelMin  = glm::vec2(config.leftPx, config.bottomPx);
  panelSize = glm::vec2(config.widthPx, config.heightPx);
  // Inset so pages and marks never touch the panel's edge.
  const float inset = std::min(panelSize.x, panelSize.y) * kInsetShare;
  const auto fit    = xanadu::OverviewFit::fit(
      lo, hi, panelMin + glm::vec2(inset), panelSize - glm::vec2(2.0F * inset));
  lastFit = fit;

  canvas->setTag(render::tagKindOverlay, kTagOverview);
  canvas->addRect(panelMin.x, panelMin.y, panelSize.x, panelSize.y,
                  config.backgroundColour);
  for (const auto &[pageLo, pageHi] : pages) {
    const auto a = fit.toPanel(pageLo);
    const auto b = fit.toPanel(pageHi);
    canvas->addRect(a.x, a.y, b.x - a.x, b.y - a.y, config.pageColour);
  }
  const float markSize = inset;
  for (const auto &mark : marks) {
    const auto at = fit.toPanel(glm::vec2(mark));
    canvas->addRect(at.x - (markSize * 0.5F), at.y - (markSize * 0.5F),
                    markSize, markSize, config.markColour);
  }

  // The view's outline, clamped to the panel: zoomed out past the whole
  // scene, it runs along the panel's edges rather than off them.
  const auto clampToPanel = [&](const glm::vec2 point) {
    return glm::clamp(point, panelMin, panelMin + panelSize);
  };
  const auto a    = clampToPanel(fit.toPanel(viewLo));
  const auto b    = clampToPanel(fit.toPanel(viewHi));
  const float rim = std::max(1.0F, inset * kOutlineShareOfInset);
  canvas->addLine(a.x, a.y, b.x, a.y, rim, config.viewportColour);
  canvas->addLine(b.x, a.y, b.x, b.y, rim, config.viewportColour);
  canvas->addLine(b.x, b.y, a.x, b.y, rim, config.viewportColour);
  canvas->addLine(a.x, b.y, a.x, a.y, rim, config.viewportColour);
  canvas->setTag(render::tagKindOverlay, 0);
  canvas->commit();
}

bool OverviewOverlay::picked(const render::PickingResult &pick,
                             RenderState & /*state*/) {
  if (render::tagKindOverlay != pick.tag.kind ||
      kTagOverview != pick.tag.clusterIndex || !lastFit) {
    return false;
  }
  // Picks count rows from the top; the panel is laid out from the bottom.
  const glm::vec2 at(static_cast<float>(pick.x),
                     static_cast<float>(screenHeight - pick.y));
  const auto world = lastFit->toWorld(at);
  std::scoped_lock locker(state->view);
  state->view.pos.x = world.x;
  state->view.pos.y = world.y;
  return true;
}

} // namespace xudu
