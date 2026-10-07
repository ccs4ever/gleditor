#include "link_panel_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <gleditor/text/font.hpp>

namespace common_ui {
namespace ui = gleditor::ui;

const LinkPanelAction *LinkPanelPresentation::find(ui::WidgetId id) const {
  const auto found = std::ranges::find(actions, id, &LinkPanelAction::id);
  return found == actions.end() ? nullptr : &*found;
}

LinkPanelPresentation linkPanelPresentation(
    std::span<const xanadu::PanelLine> lines,
    std::span<const xanadu::PanelButton> buttons,
    std::span<const ui::WidgetId> actionIds, const ui::UiMetrics &metrics,
    const ui::Theme &sourceTheme, const xanadu::LinkPanelConfig &config,
    std::optional<ui::Rect> anchor, gleditor::text::ShapingCache &cache) {
  if (buttons.size() != actionIds.size())
    throw std::invalid_argument("Link panel action identity count mismatch");
  LinkPanelPresentation result;
  result.theme =
      ui::withFontOverride(sourceTheme, ui::FontRole::Label, config.font);
  const auto description =
      metrics.fontDescription(ui::FontRole::Label, result.theme);
  const auto font =
      gleditor::text::FontManager::instance().getFont(description);
  const auto line = font->metrics().lineHeight;
  auto placement  = metrics;
  placement.chrome.top =
      std::max(placement.chrome.top, metrics.px(config.topPx));
  const auto safe  = placement.pixelSafeArea();
  const auto share = [](float value) {
    return std::isfinite(value) ? std::clamp(value, .1F, 1.0F) : .9F;
  };
  const auto maxWidth  = std::floor(safe.width * share(config.maxWidthShare));
  const auto maxHeight = std::floor(safe.height * share(config.maxHeightShare));
  const auto rows      = (buttons.size() + 2) / 3;
  const auto labelCount = static_cast<float>(lines.size());
  const auto rowCount   = static_cast<float>(rows);
  // Reserve visible text before decorative spacing or minimum touch heights.
  // Integer slots leave room for edge rounding at both nested flow boundaries.
  const auto lineSlot = std::ceil(line) + 2;
  const auto spare =
      std::max(0.0F, maxHeight - (labelCount + rowCount) * lineSlot);
  const auto gapCount =
      labelCount + static_cast<float>(rows > 0 ? rows - 1 : 0);
  const auto gap =
      std::floor(std::min(std::max(0.0F, metrics.px(config.lineGapPx)),
                          spare * .25F / std::max(1.0F, gapCount)));
  const auto pad = std::floor(
      std::min({std::max(0.0F, metrics.px(config.paddingPx)), maxWidth * .05F,
                (spare - gap * gapCount) / (4 + rowCount * 2)}));
  result.theme.paddingEm = line > 0 ? pad / line : 0;
  result.theme.gapEm     = line > 0 ? gap / line : 0;
  const auto colour      = [](std::uint32_t rgba) {
    return glm::vec4(static_cast<float>((rgba >> 24U) & 255U) / 255,
                          static_cast<float>((rgba >> 16U) & 255U) / 255,
                          static_cast<float>((rgba >> 8U) & 255U) / 255,
                          static_cast<float>(rgba & 255U) / 255);
  };
  result.theme.colours.surface       = colour(config.backgroundColour);
  result.theme.colours.text          = colour(config.textColour);
  result.theme.colours.muted         = colour(config.mutedColour);
  result.theme.colours.disabled      = colour(config.mutedColour);
  result.theme.colours.buttonSurface = colour(config.buttonColour);

  float desiredWidth = metrics.px(sourceTheme.type.minTouchPx) * 3 + pad * 4;
  for (const auto &entry : lines)
    desiredWidth = std::max(
        desiredWidth, cache.fitted(entry.text, font, {}).widthPx + pad * 2);
  for (const auto &button : buttons)
    desiredWidth =
        std::max(desiredWidth,
                 (cache.fitted(button.label, font, {}).widthPx + pad * 2) * 3 +
                     pad * 4 + gap * 2);
  const auto width         = std::floor(std::min(maxWidth, desiredWidth));
  const auto interiorWidth = std::max(0.0F, width - pad * 2);
  const auto buttonWidth =
      std::floor(std::max(0.0F, (interiorWidth - pad * 2 - gap * 2) / 3));
  const auto availableRows = std::max(0.0F, maxHeight - labelCount * lineSlot -
                                                pad * 4 - gap * gapCount);
  const auto rowHeight =
      rows == 0 ? 0.0F
                : std::floor(
                      std::min(std::max(metrics.px(sourceTheme.type.minTouchPx),
                                        lineSlot + pad * 2),
                               availableRows / rowCount));
  const auto actionHeight =
      rows == 0
          ? 0.0F
          : rowCount * rowHeight + static_cast<float>(rows - 1) * gap + pad * 2;
  const auto maxLines    = std::max<std::uint16_t>(1, config.maxLines);
  const auto labelHeight = lineSlot * maxLines;
  const auto height      = std::min(
      maxHeight, pad * 2 + labelCount * (labelHeight + gap) + actionHeight);
  const auto labelBudget =
      std::max(0.0F, height - pad * 2 - actionHeight - gap * labelCount);
  const auto eachLabel =
      lines.empty()
          ? 0.0F
          : std::floor(std::min(labelHeight, labelBudget / labelCount));
  const auto buttonHeight =
      rows == 0 ? 0.0F
                : std::max(0.0F, (actionHeight - pad * 2 -
                                  gap * static_cast<float>(rows - 1)) /
                                     static_cast<float>(rows));

  result.model = {
      .id        = 1,
      .model     = ui::Panel{},
      .preferred = {metrics.logical(width), metrics.logical(height)}};
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto &entry = lines[index];
    result.model.children.push_back(
        {.id    = static_cast<ui::WidgetId>(index + 3),
         .model = ui::Label{entry.active ? "\u25B8 " + entry.text : entry.text,
                            ui::TextPurpose::Description},
         .preferred = {0, metrics.logical(eachLabel)},
         .fontRole  = ui::FontRole::Label,
         .tone = entry.tone == xanadu::PanelLine::Tone::Muted ? ui::Tone::Muted
                 : entry.tone == xanadu::PanelLine::Tone::Active
                     ? ui::Tone::Accent
                     : ui::Tone::Normal,
         .maxLines = maxLines});
  }
  ui::Widget flow{.id        = 2,
                  .model     = ui::ButtonFlow{},
                  .preferred = {metrics.logical(interiorWidth),
                                metrics.logical(actionHeight)}};
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const auto &button = buttons[index];
    result.actions.push_back(
        {actionIds[index], button.command, button.enabled});
    flow.children.push_back(
        {.id        = actionIds[index],
         .model     = ui::Button{button.label, "navigate", button.enabled},
         .preferred = {metrics.logical(buttonWidth),
                       metrics.logical(buttonHeight)}});
  }
  result.model.children.push_back(std::move(flow));
  const auto margin = std::max(0.0F, metrics.px(config.marginPx));
  result.bounds = anchor ? ui::placeNear(*anchor, width, height, safe, margin)
                         : ui::Rect{safe.left + safe.width - width - margin,
                                    safe.bottom + safe.height - height - margin,
                                    width, height};
  result.bounds = ui::clampToSafeArea(metrics.rounded(result.bounds), safe);
  return result;
}
} // namespace common_ui
