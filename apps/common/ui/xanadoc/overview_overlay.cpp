/**
 * @file overview_overlay.cpp
 * @brief Drawing the overview panel and moving the camera from it.
 */
#include "overview_overlay.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

namespace xanadu {

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
  const std::scoped_lock lock(guard);
  if (next != config) {
    config = next;
    ++configRevision;
    ++revision;
  }
}

void OverviewOverlay::deviceReady(render::RenderDevice &device,
                                  const render::PipelineDesc &pipeline) {
  const std::scoped_lock lock(guard);
  // The panel draws no text; the canvas needs a font all the same.
  canvas = std::make_unique<gleditor::Canvas>(&device, state->defaultFontName);
  canvas->createPipeline(pipeline, false);
  builtFor.reset();
}

void OverviewOverlay::drawFrame(gleditor::FrameContext &ctx) {
  const std::scoped_lock lock(guard);
  if (!canvas) {
    return;
  }
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  Stamp stamp{.width   = ctx.screenWidth,
              .height  = ctx.screenHeight,
              .marks   = markRevision ? markRevision() : 0,
              .config  = configRevision,
              .visible = isVisible(),
              .metrics = metrics};
  {
    std::scoped_lock locker(state->view);
    stamp.camera = state->view.pos;
    stamp.fov    = state->view.fov;
  }
  for (const auto &doc : ctx.state.docs) {
    if (doc) {
      ++stamp.documents;
      stamp.pages += doc->numPages();
      const auto hash = [&](float value) {
        stamp.geometry =
            (stamp.geometry ^ std::bit_cast<std::uint32_t>(value)) *
            1099511628211ULL;
      };
      const auto model = doc->getModel();
      for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) hash(model[column][row]);
      for (std::size_t page = 0; page < doc->numPages(); ++page) {
        if (const auto frame = doc->pageFrame(page)) {
          hash(frame->leftPx);
          hash(frame->bottomPx);
          hash(frame->rightPx);
          hash(frame->topPx);
          for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
              hash(frame->localToWorld[column][row]);
        }
      }
    }
  }
  if (builtFor != stamp) {
    builtFor = stamp;
    ++revision;
    rebuild(ctx, stamp);
  }
  if (canvas->empty()) {
    return;
  }
  if (!pickScope) pickScope = ctx.state.allocatePersistentOverlayPickScope();
  canvas->setIdentity(pickScope, 0);
  ctx.state.bindOverlayWidgets(
      render::packTagIdentity(render::tagKindOverlay, pickScope, 0),
      pickTargets);
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
    // Every page the layout has, built or not: a page not yet built is placed
    // where the stack puts it -- its gap below the one before, as tall as
    // the first -- so the panel shows the arrangement rather than how far
    // building has got, which varies with the machine.
    std::optional<std::pair<glm::vec2, glm::vec2>> previous;
    float firstHeight = 0.0F;
    for (std::size_t p = 0; p < doc->numPages(); ++p) {
      std::pair<glm::vec2, glm::vec2> rect;
      if (const auto frame = doc->pageFrame(p)) {
        const glm::vec2 a(frame->localToWorld * glm::vec4(frame->leftPx,
                                                          frame->bottomPx, 0.0F,
                                                          1.0F));
        const glm::vec2 b(frame->localToWorld *
                          glm::vec4(frame->rightPx, frame->topPx, 0.0F, 1.0F));
        rect = {glm::min(a, b), glm::max(a, b)};
      } else if (previous) {
        const float top =
            previous->first.y - (Doc::pageGapPx * Doc::pixelsToWorld);
        rect = {{previous->first.x, top - firstHeight},
                {previous->second.x, top}};
      } else {
        continue;
      }
      if (0.0F == firstHeight) {
        firstHeight = rect.second.y - rect.first.y;
      }
      previous = rect;
      pages.push_back(rect);
      grow(rect.first);
      grow(rect.second);
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

  const auto bounds = panelBounds(config, stamp.metrics);
  panelMin          = {bounds.left, bounds.bottom};
  panelSize         = {bounds.width, bounds.height};
  if (bounds.width <= 0 || bounds.height <= 0) {
    canvas->commit();
    return;
  }
  if (pickToken == std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("Overview pick identities exhausted");
  ++pickToken;
  pickTargets = std::make_shared<const std::vector<std::uint32_t>>(
      std::initializer_list<std::uint32_t>{pickToken});
  // Inset so pages and marks never touch the panel's edge.
  const float inset = std::min(panelSize.x, panelSize.y) * kInsetShare;
  const auto fit    = xanadu::OverviewFit::fit(
      lo, hi, panelMin + glm::vec2(inset), panelSize - glm::vec2(2.0F * inset));
  lastFit = fit;

  canvas->setTag(render::tagKindOverlay, 1);
  canvas->pushClip(bounds);
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
  canvas->popClip();
  canvas->setTag(render::tagKindOverlay, 0);
  canvas->commit();
}

bool OverviewOverlay::picked(const render::PickingResult &pick,
                             RenderState & /*state*/) {
  const std::scoped_lock lock(guard);
  if (render::tagKindOverlay != pick.tag.kind || !isVisible() || !lastFit)
    return false;
  if (pick.requestId != 0) {
    if (pick.tag.docIndex != pickScope || pick.tag.pageIndex != 0) return false;
    if (pick.overlayWidgetId != pickToken) return true;
  } else if (!(pick.tag.docIndex == 0 && pick.tag.pageIndex == 0 &&
               pick.tag.clusterIndex == kTagOverview) &&
             !(pick.tag.docIndex == pickScope && pick.tag.pageIndex == 0 &&
               pick.tag.clusterIndex == 1)) {
    return false;
  }
  // Picks count rows from the top; the panel is laid out from the bottom.
  const glm::vec2 at(static_cast<float>(pick.x),
                     static_cast<float>(screenHeight - pick.y));
  if (at.x < panelMin.x || at.y < panelMin.y ||
      at.x > panelMin.x + panelSize.x || at.y > panelMin.y + panelSize.y)
    return true;
  const auto world = lastFit->toWorld(at);
  std::scoped_lock locker(state->view);
  state->view.pos.x = world.x;
  state->view.pos.y = world.y;
  return true;
}

void OverviewOverlay::toggle() {
  const std::scoped_lock lock(guard);
  visibleOverride = !isVisible();
  ++revision;
}

gleditor::ui::Rect
OverviewOverlay::panelBounds(const xanadu::OverviewConfig &config,
                             const gleditor::ui::UiMetrics &metrics) {
  return gleditor::ui::clampToSafeArea(
      metrics.rounded({metrics.px(config.leftPx), metrics.px(config.bottomPx),
                       metrics.px(config.widthPx),
                       metrics.px(config.heightPx)}),
      metrics.pixelSafeArea());
}

void OverviewOverlay::describe(gleditor::a11y::Builder &builder) {
  const std::scoped_lock lock(guard);
  if (!isVisible() || !lastFit) return;
  auto &node = builder.add(kTagOverview, gleditor::a11y::Role::Button);
  node.label = "Overview: move camera to scene centre";
  node.bounds =
      gleditor::a11y::Rect{panelMin.x, screenHeight - panelMin.y - panelSize.y,
                           panelMin.x + panelSize.x, screenHeight - panelMin.y};
  node.focusable = true;
  node.actions   = gleditor::a11y::bit(gleditor::a11y::Action::Click);
  builder.contribute(builder.id(kTagOverview));
}

std::uint64_t OverviewOverlay::accessibilityRevision() const {
  const std::scoped_lock lock(guard);
  return revision;
}

bool OverviewOverlay::performAction(std::uint64_t id,
                                    gleditor::a11y::Action action,
                                    std::string_view) {
  const std::scoped_lock lock(guard);
  if (gleditor::a11y::Ids::localOf(id) != kTagOverview ||
      action != gleditor::a11y::Action::Click || !isVisible() || !lastFit)
    return false;
  const auto world = lastFit->toWorld(panelMin + panelSize * 0.5F);
  const std::scoped_lock viewLock(state->view);
  state->view.pos.x = world.x;
  state->view.pos.y = world.y;
  return true;
}

} // namespace xanadu
