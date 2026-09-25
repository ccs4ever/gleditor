/**
 * @file link_panel_overlay.cpp
 * @brief Drawing the selected-link panel and its preview highlight.
 */
#include "link_panel_overlay.hpp"

#include <algorithm>
#include <variant>

#include <glm/ext/matrix_clip_space.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>

#include "common/xanadu/link_panel.hpp"

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

  const auto selected = context.selection();
  if (!selected || !selected->occurrences) {
    return;
  }
  for (const auto side : {xanadu::LinkSide::Left, xanadu::LinkSide::Right}) {
    const auto &cursor = selected->cursor(side);
    if (!cursor.member) {
      continue;
    }
    const auto &member = selected->occurrences->members(side)[*cursor.member];
    for (std::uint32_t i = 0; i < member.occurrences.size(); ++i) {
      const auto *const site =
          std::get_if<xanadu::DocumentSite>(&member.occurrences[i].site);
      if (nullptr == site) {
        continue;
      }
      const auto view = context.viewIndexOf(*site);
      if (!view) {
        continue;
      }
      highlights.emplace_back(
          *view,
          gleditor::SpanStyle{.start  = site->range.start,
                              .end    = site->range.end,
                              .colour = cursor.occurrence == i
                                            ? config.chosenHighlightColour
                                            : config.memberHighlightColour});
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
  const auto selected = context.selection();
  if (selected) {
    const auto lines =
        xanadu::linkPanelLines(*selected, context.originSite(),
                               [this](const xanadu::OccurrenceSite &site) {
                                 return context.describe(site);
                               });

    // The active side's marker sits in a gutter as wide as itself, so both
    // side names start at the same x whichever is active.
    constexpr std::string_view marker = "▸ ";
    const auto gutter                 = canvas->measureText(marker).width;

    std::vector<gleditor::TextMetrics> sizes;
    sizes.reserve(lines.size());
    float widest = 0.0F;
    float tall   = 0.0F;
    for (const auto &line : lines) {
      sizes.push_back(canvas->measureText(line.text));
      widest = std::max(widest, sizes.back().width);
      tall += sizes.back().height;
    }
    widest += gutter;
    tall += config.lineGapPx * static_cast<float>(lines.size() - 1);

    const auto screenW = static_cast<float>(ctx.screenWidth);
    const auto screenH = static_cast<float>(ctx.screenHeight);
    const auto panelW  = widest + (2.0F * config.paddingPx);
    const auto panelH  = tall + (2.0F * config.paddingPx);
    const auto left    = screenW - config.marginPx - panelW;
    const auto top     = screenH - config.topPx;
    canvas->addRect(left, top - panelH, panelW, panelH,
                    config.backgroundColour);

    // The canvas is y-up and a block of text hangs down from its top.
    auto y = top - config.paddingPx;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      const auto colour = xanadu::PanelLine::Tone::Muted == lines[i].tone
                              ? config.mutedColour
                              : config.textColour;
      const auto x      = left + config.paddingPx;
      if (lines[i].active) {
        static_cast<void>(canvas->addText(ctx.state, x, y, marker, colour,
                                          config.backgroundColour));
      }
      static_cast<void>(canvas->addText(ctx.state, x + gutter, y, lines[i].text,
                                        colour, config.backgroundColour));
      y -= sizes[i].height + config.lineGapPx;
    }
  }
  canvas->commit();
}

void LinkPanelOverlay::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas) {
    return;
  }
  const Stamp stamp{.selection = context.revision(),
                    .views     = session.generation(),
                    .config    = configRevision,
                    .width     = ctx.screenWidth,
                    .height    = ctx.screenHeight};
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
