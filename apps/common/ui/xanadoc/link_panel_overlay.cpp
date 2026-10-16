/**
 * @file link_panel_overlay.cpp
 * @brief Drawing the selected-link panel and its preview highlight.
 */
#include "link_panel_overlay.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/caret.hpp>
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

  const auto selected      = context.selection();
  std::uint32_t linkColour = 0;
  if (selected && selected->occurrences && !context.selectedPackageDisabled()) {
    linkColour =
        session.selectedLinkColour(selected->key, selected->occurrences->link);
    // Uniform RGB scaling keeps the link hue while giving its edge contrast
    // against the existing, brighter semantic fill.
    const auto edgeColour = ((linkColour >> 24U) * 3U / 5U << 24U) |
                            (((linkColour >> 16U) & 255U) * 3U / 5U << 16U) |
                            (((linkColour >> 8U) & 255U) * 3U / 5U << 8U);
    for (const auto side : {xanadu::LinkSide::Left, xanadu::LinkSide::Right}) {
      const auto &cursor = selected->cursor(side);
      if (!cursor.member) {
        continue;
      }
      const auto &member = selected->occurrences->members(side)[*cursor.member];
      for (std::uint32_t i = 0; i < member.occurrences.size(); ++i) {
        const bool chosen = cursor.occurrence == i;
        const bool active = chosen && selected->active == side;
        std::visit(
            [&]<typename Site>(const Site &site) {
              if constexpr (std::is_same_v<Site, xanadu::CellSite>) {
                if (!context.cellInView(site)) return;
                cells.push_back({.cell   = site.cell,
                                 .start  = site.range.start,
                                 .end    = site.range.end,
                                 .chosen = chosen});
              } else if (const auto view = context.viewIndexOf(site)) {
                highlights.emplace_back(
                    *view,
                    gleditor::SpanStyle{
                        .start       = site.range.start,
                        .end         = site.range.end,
                        .colour      = edgeColour | (active   ? 255U
                                                     : chosen ? 160U
                                                              : 80U),
                        .emphasis    = gleditor::SpanEmphasis::Contour,
                        .edgeWidthPx = active ? config.chosenEdgeWidthPx
                                              : config.memberEdgeWidthPx,
                        .layer = session.linkHighlightLayer(selected->key)});
              }
            },
            member.occurrences[i].site);
      }
    }
  }
  if (cells != cellHighlights || linkColour != cellHighlightColour) {
    cellHighlights      = cells;
    cellHighlightColour = linkColour;
    GLEDITOR_LOG_DEBUG("xudu.links", "marking {} cell ranges", cells.size());
    if (cellHighlighter) {
      cellHighlighter(std::move(cells), linkColour | 0xFFU);
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

LinkPanelOverlay *
LinkPanelOverlay::noteNavigationSelection(const Caret &caret,
                                          std::optional<LinkKey> key) {
  ownedSelection.reset();
  const auto selected = context.selection();
  if (key && selected && selected->key == *key &&
      caret.documentIndex() < session.views().size()) {
    const auto &view = session.views()[caret.documentIndex()];
    ownedSelection   = OwnedSelection{
        *key,
        caret.selectionRevision(),
        selected->generation,
        session.generation(),
        caret.documentIndex(),
        DocumentSite{.store   = session.store(view.storeIndex).documentId(),
                       .version = view.version,
                       .range = {caret.selectionStart(), caret.selectionEnd()}}};
  }
  return this;
}
std::optional<gleditor::SpanStyle>
LinkPanelOverlay::selectionStyle(const Doc &doc, const Caret &caret) {
  const auto selected = context.selection();
  if (context.selectedPackageDisabled() || !ownedSelection ||
      !caret.hasSelection() || caret.documentIndex() != doc.documentIndex() ||
      caret.selectionRevision() != ownedSelection->revision || !selected ||
      selected->key != ownedSelection->key ||
      selected->generation != ownedSelection->generation ||
      session.generation() != ownedSelection->viewsGeneration ||
      caret.documentIndex() != ownedSelection->view ||
      caret.documentIndex() >= session.views().size() || !selected->occurrences)
    return std::nullopt;
  const auto &view = session.views()[caret.documentIndex()];
  if (view.version != ownedSelection->site.version ||
      session.store(view.storeIndex).documentId() !=
          ownedSelection->site.store ||
      caret.selectionStart() != ownedSelection->site.range.start ||
      caret.selectionEnd() != ownedSelection->site.range.end)
    return std::nullopt;
  return gleditor::SpanStyle{
      .start  = caret.selectionStart(),
      .end    = caret.selectionEnd(),
      .colour = session.selectedLinkColour(selected->key,
                                           selected->occurrences->link),
      .layer  = session.linkHighlightLayer(selected->key)};
}

void LinkPanelOverlay::queueAction(gleditor::ui::WidgetId id) {
  const std::scoped_lock lock(actionGuard);
  if (!currentPresentation) return;
  if (currentPresentation->investigationToggle == id) {
    pendingInvestigations.push_back(presentationSelection);
    return;
  }
  if (currentPresentation->toggleControls == id) {
    pendingToggles.push_back(presentationSelection);
    return;
  }
  if (const auto &pages = currentPresentation->pages; pages) {
    if (pages->request.previousId == id || pages->request.nextId == id) {
      pendingPages.emplace_back(presentationSelection,
                                pages->request.previousId == id ? -1 : 1);
      return;
    }
  }
  if (const auto *action = currentPresentation->find(id))
    pending.push_back({presentationSelection, action->command});
}

void LinkPanelOverlay::toggleControls() {
  const std::scoped_lock lock(actionGuard);
  if (currentPresentation && currentPresentation->toggleControls)
    pendingToggles.push_back(presentationSelection);
}

void LinkPanelOverlay::toggleInvestigation() {
  const std::scoped_lock lock(actionGuard);
  if (currentPresentation)
    pendingInvestigations.push_back(presentationSelection);
}

void LinkPanelOverlay::pageControls(int direction) {
  const std::scoped_lock lock(actionGuard);
  if (currentPresentation && currentPresentation->pages)
    pendingPages.emplace_back(presentationSelection, direction);
}

void LinkPanelOverlay::executePending() {
  std::vector<PendingAction> actions;
  std::vector<std::uint64_t> toggles, investigations;
  std::vector<std::pair<std::uint64_t, int>> pages;
  std::optional<common_ui::LinkPanelPages> paging;
  {
    const std::scoped_lock lock(actionGuard);
    actions.swap(pending);
    toggles.swap(pendingToggles);
    investigations.swap(pendingInvestigations);
    pages.swap(pendingPages);
    if (currentPresentation) paging = currentPresentation->pages;
  }
  for (const auto selection : toggles)
    if (selection == context.revision()) {
      controlsCollapsed = !controlsCollapsed;
      panelBuiltFor.reset();
    }
  for (const auto selection : investigations)
    if (selection == context.revision()) {
      investigationExpanded = !investigationExpanded;
      controlsPage          = 0;
      panelBuiltFor.reset();
    }
  for (const auto &[selection, direction] : pages)
    if (selection == context.revision() && paging) {
      if (direction < 0 && controlsPage > 0) --controlsPage;
      if (direction > 0 && controlsPage + 1 < paging->count) ++controlsPage;
      panelBuiltFor.reset();
    }
  for (const auto &action : actions) {
    // The command belongs to the selected state the reader actually pressed.
    // Accessibility callbacks enqueue here because navigation owns render
    // state.
    if (action.selection == context.revision())
      std::ignore = context.execute(action.command);
  }
}

void LinkPanelOverlay::appendInvestigation(std::vector<PanelLine> &lines) {
  const auto selected = context.selection();
  if (!selected) return;
  const auto append = [&](std::string text) {
    const auto required = text.size();
    lines.push_back({.text = std::move(text), .requiredBytes = required});
  };
  lines.front().text = "Master Link Tablet · " + lines.front().text;
  if (const auto visit = context.currentVisit())
    lines.front().text += std::format(" · visit {}", visit->value);
  lines.front().compactIdentity = lines.front().text;
  append("Link authority: " + selected->key.authority.str());
  if (const auto carrier = context.selectedPackageCarrier())
    append("Package carrier: " + carrier->hex());
  const auto place = [&](std::string_view label, const OccurrenceSite &site) {
    std::visit(
        [&](const auto &at) {
          append(std::format("{} store: {}\nState: {} · bytes {}–{}", label,
                             at.store.str(), at.version.str(), at.range.start,
                             at.range.end));
        },
        site);
  };
  if (const auto here = context.reading().here) place("Current reading", *here);
  if (const auto origin = context.originSite())
    place("Return checkpoint", *origin);
  append(context.currentVisit()
             ? std::format(
                   "Current reader visit {} · inspection keeps reading place",
                   context.currentVisit()->value)
             : "Current reader visit unavailable · inspection keeps reading "
               "place");
  for (const auto side : {LinkSide::Left, LinkSide::Right}) {
    if (selected->restoring) {
      const auto &saved = side == LinkSide::Left
                              ? selected->restoring->leftPin
                              : selected->restoring->rightPin;
      if (saved)
        place(side == LinkSide::Left ? "Saved Left occurrence"
                                     : "Saved Right occurrence",
              saved->occurrence.site);
    }
    if (!selected->occurrences) continue;
    const auto &cursor  = selected->cursor(side);
    const auto &members = selected->occurrences->members(side);
    if (!cursor.member || *cursor.member >= members.size()) continue;
    const auto label   = side == LinkSide::Left ? "Left" : "Right";
    const auto &member = members[*cursor.member];
    if (const auto *package =
            session.readerLinkPackages().retainedPackage(selected->key)) {
      const auto &spans = side == LinkSide::Left
                              ? package->links[selected->key.id].left
                              : package->links[selected->key.id].right;
      const auto &span  = spans[*cursor.member];
      append(std::format("{} signed scroll: {}\nSource bytes {}–{}", label,
                         span.scroll, span.start, span.end()));
      if (cursor.occurrence && *cursor.occurrence < member.occurrences.size()) {
        const auto &site = member.occurrences[*cursor.occurrence].site;
        place(std::string(label) + " occurrence", site);
        if (const auto *cell = std::get_if<CellSite>(&site))
          append(std::format("{} occurrence cell: {}", label, cell->cell));
      } else {
        append(std::format(
            "{} occurrence not chosen · choose a place for exact identity",
            label));
      }
      continue;
    }
    if (!cursor.occurrence) {
      append(std::format(
          "{} member {} · {} · choose a place for exact identity", label,
          *cursor.member + 1,
          member.inView() ? "occurrence not chosen" : "not in open views"));
      continue;
    }
    const auto exact =
        context.reference(selected->key, member, *cursor.occurrence);
    if (!exact) {
      // A closed frame cannot supply a source reference, but the chosen
      // destination's saved identity is still available for recovery.
      if (*cursor.occurrence < member.occurrences.size()) {
        const auto &site = member.occurrences[*cursor.occurrence].site;
        place(std::string(label) + " occurrence", site);
        if (const auto *cell = std::get_if<CellSite>(&site))
          append(std::format("{} occurrence cell: {}", label, cell->cell));
      }
      append(std::format("{} exact source reference unavailable: {}", label,
                         name(exact.error())));
      continue;
    }
    append(std::format("{} occurrence store: {}\nPinned state: {} · {}", label,
                       exact->store.str(), exact->version.str(),
                       std::visit(
                           [](const auto &frame) {
                             using Frame = std::decay_t<decltype(frame)>;
                             if constexpr (std::is_same_v<Frame, CellFrame>)
                               return std::format("cell {}", frame.cell);
                             else
                               return frame.birth
                                          ? std::format("document birth {}",
                                                        *frame.birth)
                                          : std::string{"top-level document"};
                           },
                           exact->frame)));
    for (const auto &range : exact->ranges)
      append(std::format(
          "{} source: {}\nScroll {} · bytes {}–{}\nOccurrence bytes {}–{}",
          label, exact->authority.str(), range.source.scroll,
          range.source.start, range.source.start + range.source.length,
          range.range.start, range.range.end));
  }
}

void LinkPanelOverlay::rebuildPanel(gleditor::FrameContext &ctx) {
  const auto selected = context.selection();
  if (!selected && !context.entryPending() && context.entryStatus().empty()) {
    panel.setVisible(false);
    dockLink.reset();
    controlsCollapsed     = false;
    investigationExpanded = false;
    buttons.clear();
    const std::scoped_lock lock(actionGuard);
    currentPresentation.reset();
    return;
  }
  const auto origin = context.originSite();
  std::vector<PanelLine> lines;
  buttons.clear();
  if (selected) {
    lines = xanadu::linkPanelLines(
        *selected, origin, context.reading(),
        [this](const auto &site) { return context.describe(site); });
    buttons = xanadu::linkPanelButtons(*selected, origin.has_value());
    if (investigationExpanded) {
      appendInvestigation(lines);
      auto members = linkMemberButtons(*selected);
      buttons.insert(buttons.end(), members.begin(), members.end());
    }
  } else {
    dockLink.reset();
    controlsCollapsed     = false;
    investigationExpanded = false;
    lines.push_back({.text = "Saved visit restoration"});
    if (const auto target = context.pendingTarget()) {
      lines.push_back({.text = "Store " + target->store.str()});
      lines.push_back(
          {.text = std::format("State {} · bytes {}–{}", target->version.str(),
                               target->range.start, target->range.end)});
    }
  }
  if (context.selectedPackageDisabled()) {
    lines.push_back({.text = "Package drawing disabled · re-enable to enter; "
                             "pinned Link retained",
                     .tone = PanelLine::Tone::Muted});
    buttons.push_back({.label   = "Re-enable package",
                       .command = nav::EnableSelectedPackage{}});
    for (auto &button : buttons)
      if (std::holds_alternative<nav::Enter>(button.command) ||
          std::holds_alternative<nav::EnterAt>(button.command)) {
        button.label   = "Enter unavailable";
        button.enabled = false;
      }
  }
  if (!context.entryStatus().empty()) {
    lines.push_back(
        {.text = context.entryStatus(), .tone = PanelLine::Tone::Muted});
    buttons.push_back({.label = "Cancel", .command = nav::CancelEntry{}});
  }
  if (context.entryPending()) {
    lines.push_back({.text = "Entry pending: building exact target page",
                     .tone = PanelLine::Tone::Muted});
    buttons.push_back({.label = "Cancel", .command = nav::CancelEntry{}});
  }
  // Retain identities independently of which controls page is currently drawn.
  const bool sameCommands =
      identifiedSelection == context.revision() &&
      std::ranges::equal(
          identifiedButtons, buttons, {},
          [](const auto &button) { return button.command; },
          [](const auto &button) { return button.command; });
  if (!sameCommands) {
    buttonIds.clear();
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      if (nextActionId == std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Link panel action identities exhausted");
      buttonIds.push_back(++nextActionId);
    }
    identifiedButtons   = buttons;
    identifiedSelection = context.revision();
  }
  if (!toggleId) {
    if (nextActionId > std::numeric_limits<std::uint32_t>::max() - 3)
      throw std::length_error("Link panel action identities exhausted");
    toggleId            = ++nextActionId;
    const auto previous = ++nextActionId;
    const auto next     = ++nextActionId;
    pageIds             = std::pair{previous, next};
  }
  auto ids = buttonIds;
  if (selected) ids.push_back(*toggleId);
  std::optional<gleditor::ui::Rect> anchor;
  if (panelAnchors) {
    const auto &points = *panelAnchors;
    anchor             = {std::min(points[0].x, points[1].x),
                          std::min(points[0].y, points[1].y),
                          std::abs(points[0].x - points[1].x),
                          std::abs(points[0].y - points[1].y)};
  }
  auto next = std::make_shared<common_ui::LinkPanelPresentation>(
      common_ui::linkPanelPresentation(
          lines, buttons, ids, builtMetrics, ctx.theme, config, anchor,
          measurements, std::nullopt,
          std::span(protectedContent).first(protectedContentCount),
          selected ? std::optional<std::string_view>{investigationExpanded
                                                         ? "Fold investigation"
                                                         : "Inspect members"}
                   : std::nullopt));
  next->investigationToggle = next->toggleControls;
  next->toggleControls.reset();
  if (investigationExpanded) {
    auto inspection = common_ui::linkPanelInvestigationPresentation(
        lines, buttons, buttonIds, *toggleId,
        {pageIds->first, pageIds->second, controlsPage}, builtMetrics,
        ctx.theme, config, measurements,
        std::span(protectedContent).first(protectedContentCount));
    if (inspection.completeContent) {
      inspection.investigationToggle = inspection.toggleControls;
      inspection.toggleControls.reset();
      next = std::make_shared<common_ui::LinkPanelPresentation>(
          std::move(inspection));
      dockLink.reset();
    } else {
      // Refuse occlusion rather than replacing the source with a full-screen
      // tool. Keyboard navigation remains available through the same commands.
      lines = xanadu::linkPanelLines(
          *selected, origin, context.reading(),
          [this](const auto &site) { return context.describe(site); });
      lines.push_back({.text = "Investigation needs more free canvas · enlarge "
                               "window or reduce UI text",
                       .tone = PanelLine::Tone::Muted});
      next = std::make_shared<common_ui::LinkPanelPresentation>(
          common_ui::linkPanelPresentation(
              lines, buttons, ids, builtMetrics, ctx.theme, config, anchor,
              measurements, {},
              std::span(protectedContent).first(protectedContentCount),
              "Fold investigation"));
      next->investigationToggle = next->toggleControls;
      next->toggleControls.reset();
      controlsCollapsed = true;
    }
  }
  const auto viewport = std::pair{ctx.screenWidth, ctx.screenHeight};
  if (dockViewport != viewport) dockLink.reset();
  // Scrolling must not remove the reader's explicit Read/Controls toggle.
  if (selected && ((!investigationExpanded && dockLink == selected->key) ||
                   !next->safePlacement || !next->completeContent)) {
    dockViewport = viewport;
    if (dockLink != selected->key) {
      controlsCollapsed = true;
      dockLink          = selected->key;
      if (nextActionId == std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("Link panel action identities exhausted");
      toggleId = ++nextActionId;
      if (nextActionId > std::numeric_limits<std::uint32_t>::max() - 2)
        throw std::length_error("Link panel action identities exhausted");
      const auto previousId = ++nextActionId;
      const auto nextId     = ++nextActionId;
      pageIds               = std::pair{previousId, nextId};
      controlsPage          = 0;
    }
    next = std::make_shared<common_ui::LinkPanelPresentation>(
        common_ui::linkPanelDockPresentation(
            lines, buttons, buttonIds, *toggleId, controlsCollapsed,
            builtMetrics, ctx.theme, config, measurements,
            common_ui::LinkPanelPageRequest{pageIds->first, pageIds->second,
                                            controlsPage}));
  } else {
    dockLink.reset();
    controlsCollapsed = false;
  }
  if (next->pages) controlsPage = next->pages->request.page;
  panel.setModel(next->model);
  panel.setBounds(next->bounds);
  panel.setVisible(true);
  auto shownMetrics = builtMetrics;
  if (next->reservedBottom) shownMetrics.marginShare = 0;
  std::ignore = panel.prepare(shownMetrics, next->theme);
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
  context.advanceFrame(ctx.state);
  executePending();
  // Documents may be closed with nothing asking decorate(), so the cell
  // marks are kept current from here too.
  rebuildHighlights();
  panelAnchors.reset();
  if (context.selection() && anchorResolver) {
    if (const auto world = anchorResolver(ctx.state)) {
      if (framingHandler && context.reading().entered &&
          framedVisit != context.readingStamp().visit) {
        framingHandler(*world, ctx.timeline);
        framedSelection = context.revision();
        framedVisit     = context.readingStamp().visit;
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
  protectedContentCount = 0;
  visibleCompanionCount = 0;
  visibleCompanionOwner.reset();
  std::array<std::optional<OccurrenceSite>, 3> protectedSites{
      context.reading().here, std::nullopt, std::nullopt};
  if (const auto selected = context.selection();
      selected && selected->occurrences) {
    visibleCompanionOwner = selected->key;
    for (const auto side : {LinkSide::Left, LinkSide::Right}) {
      const auto &cursor  = selected->cursor(side);
      const auto &members = selected->occurrences->members(side);
      if (cursor.member && cursor.occurrence &&
          *cursor.member < members.size() &&
          *cursor.occurrence < members[*cursor.member].occurrences.size())
        protectedSites[side == LinkSide::Left ? 1 : 2] =
            members[*cursor.member].occurrences[*cursor.occurrence].site;
      else {
        // Layout must leave a visible companion readable even before the
        // reader disambiguates its occurrence. This chooses no navigation
        // cursor and records no visit.
        auto &protectedSite = protectedSites[side == LinkSide::Left ? 1 : 2];
        for (const auto &member : members) {
          for (const auto &occurrence : member.occurrences) {
            const auto *site = std::get_if<DocumentSite>(&occurrence.site);
            if (!site) continue;
            const auto view = context.viewIndexOf(*site);
            if (!view || *view >= ctx.state.docs.size() ||
                !ctx.state.docs[*view])
              continue;
            const auto &doc      = *ctx.state.docs[*view];
            const auto pageIndex = doc.pageIndexForOffset(site->range.start);
            if (!pageIndex) continue;
            const auto page = doc.page(*pageIndex);
            if (!page) continue;
            const auto band =
                page->textBandFor(site->range.start, site->range.end);
            if (!band) continue;
            const auto world = doc.worldPoint(
                *pageIndex,
                (page->textLeftPixels() + page->textRightPixels()) * .5F,
                (band->top + band->bottom) * .5F);
            if (!world ||
                (ctx.viewProjection * glm::vec4(*world, 1)).w <= .0001F)
              continue;
            const auto point = gleditor::spatial::projectToScreen(
                ctx.viewProjection, *world, static_cast<float>(ctx.screenWidth),
                static_cast<float>(ctx.screenHeight));
            if (point.x < 0 || point.x > ctx.screenWidth || point.y < 0 ||
                point.y > ctx.screenHeight)
              continue;
            protectedSite = occurrence.site;
            break;
          }
          if (protectedSite) break;
        }
      }
    }
  }
  std::array<std::optional<std::array<float, 4>>, 3> protectedBounds;
  std::array<std::optional<std::size_t>, 3> protectedViews;
  for (const auto &protectedSite : protectedSites) {
    const auto *site =
        protectedSite ? std::get_if<DocumentSite>(&*protectedSite) : nullptr;
    if (!site) continue;
    const auto view = context.viewIndexOf(*site);
    if (!view || *view >= ctx.state.docs.size() || !ctx.state.docs[*view] ||
        std::ranges::find(protectedViews, view) != protectedViews.end())
      continue;
    const auto &doc  = *ctx.state.docs[*view];
    const auto first = doc.anchorFor(site->range.start);
    if (!first) continue;
    const auto frame = doc.pageFrame(first->pageIndex);
    if (!frame) continue;
    // A visible reading column in a long document includes neighbouring
    // pages. A short companion protects its prose, leaving blank canvas free.
    auto top = doc.anchorFor(0);
    auto bottom =
        doc.anchorFor(static_cast<std::uint32_t>(doc.contents().size()));
    if (doc.numPages() != 1) {
      top.reset();
      bottom.reset();
    }
    glm::vec2 low(std::numeric_limits<float>::max()),
        high(std::numeric_limits<float>::lowest());
    for (const auto x : {frame->leftPx, frame->rightPx})
      for (const auto y :
           {top ? top->y + top->height : first->y + first->height,
            bottom ? bottom->y - bottom->height : first->y - first->height}) {
        const auto world = doc.worldPoint(first->pageIndex, x, y);
        if (!world || (ctx.viewProjection * glm::vec4(*world, 1)).w <= .0001F)
          continue;
        const auto point = gleditor::spatial::projectToScreen(
            ctx.viewProjection, *world, static_cast<float>(ctx.screenWidth),
            static_cast<float>(ctx.screenHeight));
        low  = glm::min(low, point);
        high = glm::max(high, point);
      }
    if (high.x < low.x || high.y < low.y) continue;
    if (doc.numPages() > 1) {
      low.y  = 0;
      high.y = ctx.screenHeight;
    }
    const auto rect =
        gleditor::ui::Rect{low.x, low.y, high.x - low.x, high.y - low.y};
    if (high.x >= 0 && low.x <= ctx.screenWidth && high.y >= 0 &&
        low.y <= ctx.screenHeight)
      visibleCompanionSites[visibleCompanionCount++] = *site;
    protectedContent[protectedContentCount] = rect;
    protectedBounds[protectedContentCount]  = {rect.left, rect.bottom,
                                               rect.width, rect.height};
    protectedViews[protectedContentCount++] = view;
  }
  const Stamp stamp{.selection        = context.revision(),
                    .views            = session.generation(),
                    .config           = configRevision,
                    .width            = ctx.screenWidth,
                    .height           = ctx.screenHeight,
                    .reading          = context.readingStamp(),
                    .anchors          = panelAnchors,
                    .protectedContent = protectedBounds};
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
  if (!shown) {
    const std::scoped_lock lock(actionGuard);
    dockClipPending = false;
    return;
  }
  const bool waiting = shown->reservedBottom &&
                       *shown->reservedBottom > ctx.settledChrome.bottom;
  {
    const std::scoped_lock lock(actionGuard);
    dockClipPending = waiting;
  }
  if (shown->reservedBottom)
    ctx.chrome.bottom = std::max(ctx.chrome.bottom, *shown->reservedBottom);
  if (waiting) return;
  auto shownMetrics = builtMetrics;
  if (shown->reservedBottom) shownMetrics.marginShare = 0;
  gleditor::FrameContext child{.state          = ctx.state,
                               .viewProjection = ctx.viewProjection,
                               .screenWidth    = ctx.screenWidth,
                               .screenHeight   = ctx.screenHeight,
                               .timeline       = ctx.timeline,
                               .chrome         = ctx.chrome,
                               .settledChrome  = ctx.settledChrome,
                               .metrics        = shownMetrics,
                               .theme          = shown->theme};
  panel.drawFrame(child);
}

} // namespace xanadu
