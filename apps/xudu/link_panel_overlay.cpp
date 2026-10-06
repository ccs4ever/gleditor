/**
 * @file link_panel_overlay.cpp
 * @brief Drawing the selected-link panel and its preview highlight.
 */
#include "link_panel_overlay.hpp"

#include <algorithm>
#include <tuple>
#include <type_traits>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/spatial.hpp>

namespace xudu {

void LinkPanelOverlay::setConfig(const xanadu::LinkPanelConfig &next) {
  if (next == config) {
    return;
  }
  const bool fontChanged = next.font != config.font;
  config                 = next;
  ++configRevision;
  if (fontChanged && nullptr != device) {
    canvas = std::make_unique<gleditor::Canvas>(device, config.font);
    canvas->createPipeline(pipeline, false);
  }
}

void LinkPanelOverlay::deviceReady(render::RenderDevice &aDevice,
                                   const render::PipelineDesc &aPipeline) {
  device   = &aDevice;
  pipeline = aPipeline;
  canvas   = std::make_unique<gleditor::Canvas>(device, config.font);
  canvas->createPipeline(pipeline, false);
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

void LinkPanelOverlay::rebuildPanel(gleditor::FrameContext &ctx) {
  canvas->clear();
  buttons.clear();
  const auto selected = context.selection();
  if (!selected) {
    canvas->commit();
    return;
  }
  const auto origin = context.originSite();
  const auto lines =
      xanadu::linkPanelLines(*selected, origin, context.reading(),
                             [this](const xanadu::OccurrenceSite &site) {
                               return context.describe(site);
                             });
  buttons = xanadu::linkPanelButtons(*selected, origin.has_value());

  // The active side's marker sits in a gutter as wide as itself, so both
  // side names start at the same x whichever is active.
  constexpr std::string_view marker = "\u25b8 ";
  const auto gutter                 = canvas->measureText(marker).width;
  const auto gap                    = config.lineGapPx;

  std::vector<gleditor::TextMetrics> lineSizes;
  lineSizes.reserve(lines.size());
  float inner = 0.0F;
  float tall  = 0.0F;
  for (const auto &line : lines) {
    lineSizes.push_back(canvas->measureText(line.text));
    inner = std::max(inner, gutter + lineSizes.back().width);
    tall += lineSizes.back().height + gap;
  }

  // Buttons flow left to right and wrap at the width the text needs, so the
  // panel is as wide as its widest line or its widest button.
  std::vector<gleditor::TextMetrics> buttonSizes;
  buttonSizes.reserve(buttons.size());
  for (const auto &button : buttons) {
    buttonSizes.push_back(canvas->measureText(button.label));
    inner = std::max(inner, buttonSizes.back().width + (2.0F * gap));
  }
  struct Placed {
    float x;
    float row;
  };
  std::vector<Placed> placed;
  placed.reserve(buttons.size());
  float x       = 0.0F;
  float row     = 0.0F;
  float rowTall = 0.0F;
  float allRows = 0.0F;
  for (const auto &size : buttonSizes) {
    const auto width = size.width + (2.0F * gap);
    if (x > 0.0F && x + width > inner) {
      row += rowTall + gap;
      x       = 0.0F;
      rowTall = 0.0F;
    }
    placed.push_back({.x = x, .row = row});
    x += width + gap;
    rowTall = std::max(rowTall, size.height + (2.0F * gap));
    allRows = row + rowTall;
  }

  const auto screenW = static_cast<float>(ctx.screenWidth);
  const auto screenH = static_cast<float>(ctx.screenHeight);
  const auto panelW  = inner + (2.0F * config.paddingPx);
  const auto panelH  = tall + allRows + (2.0F * config.paddingPx);
  auto left          = screenW - config.marginPx - panelW;
  auto top           = screenH - config.topPx;
  if (panelAnchors) {
    const auto &a      = *panelAnchors;
    const auto centreX = 0.5F * (a[0].x + a[1].x);
    const auto highest = std::max(a[0].y, a[1].y);
    const auto minLeft = ctx.chrome.left + config.marginPx;
    const auto maxLeft = screenW - ctx.chrome.right - config.marginPx - panelW;
    const auto minTop  = ctx.chrome.bottom + config.marginPx + panelH;
    const auto maxTop  = screenH - std::max(ctx.chrome.top, config.topPx);
    if (maxLeft >= minLeft && maxTop >= minTop) {
      left = std::clamp(centreX - panelW * 0.5F, minLeft, maxLeft);
      top  = std::clamp(highest + config.marginPx + panelH, minTop, maxTop);
    }
  }
  const auto x0 = left + config.paddingPx;

  // The whole panel answers picks, so a click on it never falls through to
  // the page behind; the buttons are tagged after it, one each.
  canvas->setTag(render::tagKindOverlay, kTagPanelBase);
  canvas->addRect(left, top - panelH, panelW, panelH, config.backgroundColour);
  canvas->setTextBounds(gleditor::ui::TextBounds{
      .left = left, .bottom = top - panelH, .width = panelW, .height = panelH});

  // The canvas is y-up and a block of text hangs down from its top.
  auto y = top - config.paddingPx;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto colour = xanadu::PanelLine::Tone::Muted == lines[i].tone
                            ? config.mutedColour
                            : config.textColour;
    if (lines[i].active) {
      std::ignore = canvas->addText(ctx.state, x0, y, marker, colour,
                                    config.backgroundColour);
    }
    std::ignore = canvas->addText(ctx.state, x0 + gutter, y, lines[i].text,
                                  colour, config.backgroundColour);
    y -= lineSizes[i].height + gap;
  }

  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const auto &size  = buttonSizes[i];
    const auto width  = size.width + (2.0F * gap);
    const auto height = size.height + (2.0F * gap);
    const auto bLeft  = x0 + placed[i].x;
    const auto bTop   = y - placed[i].row;
    const auto colour =
        buttons[i].enabled ? config.textColour : config.mutedColour;
    canvas->setTag(render::tagKindOverlay,
                   kTagPanelBase + 1U + static_cast<std::uint32_t>(i));
    canvas->addRect(bLeft, bTop - height, width, height, config.buttonColour);
    canvas->setTextBounds(gleditor::ui::TextBounds{.left   = bLeft,
                                                   .bottom = bTop - height,
                                                   .width  = width,
                                                   .height = height});
    std::ignore =
        canvas->addText(ctx.state, bLeft + gap, bTop - gap, buttons[i].label,
                        colour, config.buttonColour);
  }
  canvas->setTextBounds(std::nullopt);
  canvas->setTag(render::tagKindOverlay, 0);
  canvas->commit();
}

bool LinkPanelOverlay::picked(const render::PickingResult &pick,
                              RenderState & /*state*/) {
  const auto tag = pick.tag.clusterIndex;
  if (render::tagKindOverlay != pick.tag.kind || tag < kTagPanelBase ||
      tag > kTagPanelBase + buttons.size()) {
    return false;
  }
  if (tag > kTagPanelBase) {
    // Picks arrive on the render thread, where execute() belongs. A refusal
    // is logged there and changes nothing, which is all a button can do
    // about it.
    [[maybe_unused]] const auto result =
        context.execute(buttons[tag - kTagPanelBase - 1U].command);
  }
  return true;
}

void LinkPanelOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas) {
    return;
  }
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
  if (panelBuiltFor != stamp) {
    panelBuiltFor = stamp;
    rebuildPanel(ctx);
  }
  if (canvas->empty()) {
    return;
  }
  const auto ortho = glm::ortho( // NOLINT(readability-suspicious-call-argument)
      0.0F, static_cast<float>(ctx.screenWidth), 0.0F,
      static_cast<float>(ctx.screenHeight), -1.0F, 1.0F);
  canvas->draw(ctx.state, ortho);
}

} // namespace xudu
