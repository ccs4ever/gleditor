/**
 * @file link_panel_overlay.cpp
 * @brief Drawing the selected-link panel and its preview highlight.
 */
#include "link_panel_overlay.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

namespace xanadu {

LinkPanelOverlay::LinkPanelOverlay(LinkContext &aContext, Session &aSession)
    : context(aContext), session(aSession),
      panel({.id = 1, .model = gleditor::ui::Panel{}}) {
  panel.setVisible(false);
  panel.setActionHandler([this](const gleditor::ui::WidgetAction &action) {
    queueAction(action.id);
  });
}

void LinkPanelOverlay::setConfig(const xanadu::LinkPanelConfig &next) {
  if (next == config) {
    return;
  }
  config = next;
  ++configRevision;
}

void LinkPanelOverlay::deviceReady(render::RenderDevice &aDevice,
                                   const render::PipelineDesc &aPipeline) {
  panel.deviceReady(aDevice, aPipeline);
  panelBuiltFor.reset();
}

void LinkPanelOverlay::rebuildHighlights() {
  const Stamp stamp{.selection = context.revision(),
                    .views     = session.generation(),
                    .config    = configRevision};
  if (highlightsBuiltFor == stamp) {
    return;
  }
  highlightsBuiltFor = stamp;
  highlights.clear();
  std::vector<xanadu::CellHighlight> cells;

  const auto selected = context.selection();
  if (selected && selected->occurrences) {
    for (const auto side : {xanadu::LinkSide::Left, xanadu::LinkSide::Right}) {
      const auto &cursor = selected->cursor(side);
      if (!cursor.member) {
        continue;
      }
      const auto &member = selected->occurrences->members(side)[*cursor.member];
      for (std::uint32_t i = 0; i < member.occurrences.size(); ++i) {
        const bool chosen = cursor.occurrence == i;
        std::visit(
            [&]<typename Site>(const Site &site) {
              if constexpr (std::is_same_v<Site, xanadu::CellSite>) {
                cells.push_back({.cell   = site.cell,
                                 .start  = site.range.start,
                                 .end    = site.range.end,
                                 .chosen = chosen});
              } else if (const auto view = context.viewIndexOf(site)) {
                highlights.emplace_back(
                    *view,
                    gleditor::SpanStyle{
                        .start  = site.range.start,
                        .end    = site.range.end,
                        .colour = chosen ? config.chosenHighlightColour
                                         : config.memberHighlightColour});
              }
            },
            member.occurrences[i].site);
      }
    }
  }
  if (cells != cellHighlights) {
    cellHighlights = cells;
    GLEDITOR_LOG_DEBUG("xudu.links", "marking {} cell ranges", cells.size());
    if (cellHighlighter) {
      // The border is the chosen colour made opaque: it outlines a card, and
      // a translucent outline over the card's own border would be lost.
      cellHighlighter(std::move(cells), config.chosenHighlightColour | 0xFFU);
    }
  }
}

void LinkPanelOverlay::decorate(const Doc &doc,
                                std::vector<gleditor::SpanStyle> &out) {
  rebuildHighlights();
  for (const auto &[view, style] : highlights) {
    if (view == doc.documentIndex()) {
      out.push_back(style);
    }
  }
}

void LinkPanelOverlay::queueAction(gleditor::ui::WidgetId id) {
  const std::scoped_lock lock(actionGuard);
  if (!currentPresentation) return;
  if (const auto *action = currentPresentation->find(id))
    pending.push_back({presentationSelection, action->command});
}

void LinkPanelOverlay::executePending() {
  std::vector<PendingAction> actions;
  {
    const std::scoped_lock lock(actionGuard);
    actions.swap(pending);
  }
  for (const auto &action : actions) {
    // The command belongs to the selected state the reader actually pressed.
    // Accessibility callbacks enqueue here because navigation owns render
    // state.
    if (action.selection == context.revision())
      std::ignore = context.execute(action.command);
  }
}

void LinkPanelOverlay::rebuildPanel(gleditor::FrameContext &ctx) {
  const auto selected = context.selection();
  if (!selected) {
    panel.setVisible(false);
    buttons.clear();
    const std::scoped_lock lock(actionGuard);
    currentPresentation.reset();
    return;
  }
  const auto origin = context.originSite();
  const auto lines =
      xanadu::linkPanelLines(*selected, origin, context.reading(),
                             [this](const xanadu::OccurrenceSite &site) {
                               return context.describe(site);
                             });
  buttons = xanadu::linkPanelButtons(*selected, origin.has_value());
  std::vector<gleditor::ui::WidgetId> ids;
  ids.reserve(buttons.size());
  {
    const std::scoped_lock lock(actionGuard);
    const bool sameActions =
        currentPresentation && presentationSelection == context.revision() &&
        currentPresentation->actions.size() == buttons.size() &&
        std::ranges::equal(
            currentPresentation->actions, buttons, {},
            [](const auto &action) {
              return std::pair{action.command, action.enabled};
            },
            [](const auto &button) {
              return std::pair{button.command, button.enabled};
            });
    if (sameActions)
      for (const auto &action : currentPresentation->actions)
        ids.push_back(action.id);
  }
  if (ids.empty()) {
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      if (nextActionId == std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Link panel action identities exhausted");
      ids.push_back(++nextActionId);
    }
  }
  std::optional<gleditor::ui::Rect> anchor;
  if (panelAnchors) {
    const auto &points = *panelAnchors;
    anchor             = {std::min(points[0].x, points[1].x),
                          std::min(points[0].y, points[1].y),
                          std::abs(points[0].x - points[1].x),
                          std::abs(points[0].y - points[1].y)};
  }
  auto next = std::make_shared<common_ui::LinkPanelPresentation>(
      common_ui::linkPanelPresentation(lines, buttons, ids, builtMetrics,
                                       ctx.theme, config, anchor,
                                       measurements));
  panel.setModel(next->model);
  panel.setBounds(next->bounds);
  panel.setVisible(true);
  std::ignore = panel.prepare(builtMetrics, next->theme);
  {
    const std::scoped_lock lock(actionGuard);
    currentPresentation   = std::move(next);
    presentationSelection = context.revision();
  }
}

bool LinkPanelOverlay::picked(const render::PickingResult &pick,
                              RenderState &state) {
  if (pick.requestId == 0 && pick.tag.kind == render::tagKindOverlay &&
      pick.tag.clusterIndex >= kTagPanelBase &&
      pick.tag.clusterIndex <= kTagPanelBase + buttons.size()) {
    if (!panel.visible()) return false;
    const auto index = pick.tag.clusterIndex - kTagPanelBase;
    if (index > 0) {
      std::uint32_t id{};
      {
        const std::scoped_lock lock(actionGuard);
        if (!currentPresentation || index > currentPresentation->actions.size())
          return true;
        id = currentPresentation->actions[index - 1].id;
      }
      queueAction(id);
    }
    return true;
  }
  if (!panel.picked(pick, state)) return false;
  // A disabled pointer control still sends the exact command so the navigator
  // owns its refusal. Accessibility accurately exposes no action for it.
  const auto scene = panel.snapshot();
  const auto id    = pick.overlayWidgetId ? pick.overlayWidgetId
                     : pick.requestId == 0 && scene
                         ? scene->resolvePickingId(pick.tag.clusterIndex)
                         : std::nullopt;
  if (id) {
    bool disabled{};
    {
      const std::scoped_lock lock(actionGuard);
      const auto *action =
          currentPresentation ? currentPresentation->find(*id) : nullptr;
      disabled = action && !action->enabled;
    }
    if (disabled) queueAction(*id);
  }
  return true;
}

void LinkPanelOverlay::drawFrame(gleditor::FrameContext &ctx) {
  executePending();
  // Documents may be closed with nothing asking decorate(), so the cell
  // marks are kept current from here too.
  rebuildHighlights();
  panelAnchors.reset();
  if (context.selection() && anchorResolver) {
    if (const auto world = anchorResolver(ctx.state)) {
      const bool moved =
          framedAnchors &&
          (glm::distance((*framedAnchors)[0], (*world)[0]) > 0.5F ||
           glm::distance((*framedAnchors)[1], (*world)[1]) > 0.5F);
      if (framingHandler && (framedSelection != context.revision() || moved)) {
        framingHandler(*world, ctx.timeline);
        framedSelection = context.revision();
        framedAnchors   = *world;
      }
      const auto width   = static_cast<float>(ctx.screenWidth);
      const auto height  = static_cast<float>(ctx.screenHeight);
      const auto inFront = [&](const glm::vec3 &point) {
        return (ctx.viewProjection * glm::vec4(point, 1.0F)).w > 0.0001F;
      };
      if (inFront((*world)[0]) && inFront((*world)[1])) {
        panelAnchors =
            std::array{gleditor::spatial::projectToScreen(
                           ctx.viewProjection, (*world)[0], width, height),
                       gleditor::spatial::projectToScreen(
                           ctx.viewProjection, (*world)[1], width, height)};
      }
    }
  }
  const Stamp stamp{.selection = context.revision(),
                    .views     = session.generation(),
                    .config    = configRevision,
                    .width     = ctx.screenWidth,
                    .height    = ctx.screenHeight,
                    .reading   = context.readingStamp(),
                    .anchors   = panelAnchors};
  auto metrics         = ctx.metrics;
  metrics.screenWidth  = ctx.screenWidth;
  metrics.screenHeight = ctx.screenHeight;
  metrics.chrome       = ctx.chrome;
  metrics.chrome.top   = std::max(metrics.chrome.top, metrics.px(config.topPx));
  if (panelBuiltFor != stamp || metrics != builtMetrics ||
      ctx.theme != builtTheme) {
    panelBuiltFor = stamp;
    builtMetrics  = metrics;
    builtTheme    = ctx.theme;
    rebuildPanel(ctx);
  }
  std::shared_ptr<const common_ui::LinkPanelPresentation> shown;
  {
    const std::scoped_lock lock(actionGuard);
    shown = currentPresentation;
  }
  if (!shown) return;
  gleditor::FrameContext child{.state          = ctx.state,
                               .viewProjection = ctx.viewProjection,
                               .screenWidth    = ctx.screenWidth,
                               .screenHeight   = ctx.screenHeight,
                               .timeline       = ctx.timeline,
                               .chrome         = ctx.chrome,
                               .settledChrome  = ctx.settledChrome,
                               .metrics        = builtMetrics,
                               .theme          = shown->theme};
  panel.drawFrame(child);
}

} // namespace xanadu
