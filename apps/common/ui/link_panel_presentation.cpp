#include "link_panel_presentation.hpp"

#include <algorithm>
#include <array>
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
    std::optional<ui::Rect> anchor, gleditor::text::ShapingCache &cache,
    std::optional<ui::Rect> protectedContent) {
  if (buttons.size() != actionIds.size())
    throw std::invalid_argument("Link panel action identity count mismatch");
  LinkPanelPresentation result;
  result.theme = sourceTheme;
  if (const auto role = ui::fontRoleNamed(config.font)) {
    result.theme.fonts[static_cast<std::size_t>(ui::FontRole::Label)] =
        sourceTheme.font(*role);
  } else {
    result.theme =
        ui::withFontOverride(sourceTheme, ui::FontRole::Label, config.font);
  }
  const auto description =
      metrics.fontDescription(ui::FontRole::Label, result.theme);
  const auto font =
      gleditor::text::FontManager::instance().getFont(description);
  const auto line = font->metrics().lineHeight;
  auto placement  = metrics;
  placement.chrome.top =
      std::max(placement.chrome.top, static_cast<float>(config.topPx));
  const auto safe  = placement.pixelSafeArea();
  const auto share = [](float value) {
    return std::isfinite(value) ? std::clamp(value, .1F, 1.0F) : .9F;
  };
  const auto fullMaxWidth =
      std::floor(safe.width * share(config.maxWidthShare));
  const auto fullMaxHeight =
      std::floor(safe.height * share(config.maxHeightShare));
  auto panelArea = safe;
  if (protectedContent) {
    const auto at    = *protectedContent;
    const float gap  = metrics.px(config.marginPx);
    const auto right = safe.left + safe.width, top = safe.bottom + safe.height;
    const float leftCut = std::clamp(at.left - gap, safe.left, right);
    const float rightCut =
        std::clamp(at.left + at.width + gap, safe.left, right);
    const float bottomCut = std::clamp(at.bottom - gap, safe.bottom, top);
    const float topCut =
        std::clamp(at.bottom + at.height + gap, safe.bottom, top);
    const std::array candidates{
        ui::Rect{safe.left, safe.bottom, leftCut - safe.left, safe.height},
        ui::Rect{rightCut, safe.bottom, right - rightCut, safe.height},
        ui::Rect{safe.left, safe.bottom, safe.width, bottomCut - safe.bottom},
        ui::Rect{safe.left, topCut, safe.width, top - topCut}};
    const float minimumHeight =
        (lines.size() + (buttons.size() + 2) / 3) * (std::ceil(line) + 2);
    float best = 0;
    for (const auto &candidate : candidates) {
      if (candidate.width < metrics.px(sourceTheme.type.minTouchPx) * 3 ||
          candidate.height < minimumHeight)
        continue;
      const float area = std::min(fullMaxWidth, candidate.width) *
                         std::min(fullMaxHeight, candidate.height);
      if (area > best) {
        best      = area;
        panelArea = candidate;
      }
    }
  }
  const auto maxWidth  = std::floor(std::min(fullMaxWidth, panelArea.width));
  const auto maxHeight = std::floor(std::min(fullMaxHeight, panelArea.height));
  const float lineSlot = std::ceil(line) + 2.0F;
  const auto maxLines  = std::max<std::uint16_t>(1, config.maxLines);
  const float wantedPadding = std::max(0.0F, metrics.px(config.paddingPx));
  const float pad           = std::floor(
      std::clamp(wantedPadding, 4.0F, std::max(6.0F, maxWidth * 0.02F)));
  const float gap =
      std::floor(std::clamp(metrics.px(config.lineGapPx), 4.0F, 12.0F));
  result.theme.paddingEm = line > 0 ? pad / line : 0;
  result.theme.gapEm     = line > 0 ? gap / line : 0;

  const auto colour = [](std::uint32_t rgba) {
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

  std::vector<float> btnWidths(buttons.size());
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const float labelW = cache.fitted(buttons[i].label, font, {}).widthPx;
    if (buttons[i].label == "\u00d7") {
      btnWidths[i] = std::max(lineSlot, labelW + pad * 1.5F);
    } else if (buttons[i].label == "Cross" || buttons[i].label == "Follow" ||
               buttons[i].label == "Enter") {
      btnWidths[i] = labelW + pad * 2.5F;
    } else {
      btnWidths[i] = labelW + pad * 1.8F;
    }
  }

  for (std::size_t i = 0; i < buttons.size(); ++i) {
    result.actions.push_back(
        {actionIds[i], buttons[i].command, buttons[i].enabled});
  }

  float leftButtonsW   = 0.0F;
  float centerButtonsW = 0.0F;
  float rightButtonsW  = 0.0F;
  if (buttons.size() == 10) {
    leftButtonsW =
        btnWidths[2] + btnWidths[3] + btnWidths[0] + btnWidths[1] + 3 * gap;
    centerButtonsW = btnWidths[4] + btnWidths[5] + btnWidths[6] + 2 * gap;
    rightButtonsW  = btnWidths[7] + btnWidths[8] + btnWidths[9] + 2 * gap;
  }
  const float totalButtonsW =
      leftButtonsW + centerButtonsW + rightButtonsW + 2 * gap * 2.0F;

  const float minDockH = pad * 2 + 3 * lineSlot + 2 * gap;
  const bool canUseDock =
      (buttons.size() == 10 && lines.size() >= 3 &&
       maxWidth >= totalButtonsW + pad * 2 + 16.0F && maxHeight >= minDockH);

  float width   = 0.0F;
  float height  = 0.0F;
  float rowBtnH = lineSlot;
  std::vector<ui::Rect> childBounds(lines.size() + buttons.size());

  if (canUseDock) {
    width                     = std::floor(std::min(
        maxWidth, std::max(totalButtonsW + pad * 2 + 32.0F, maxWidth * 0.88F)));
    const float interiorWidth = std::max(0.0F, width - pad * 2);

    const float availH     = std::max(0.0F, maxHeight - pad * 2 - 2 * gap);
    const float rowIdentH  = lineSlot;
    const float remainingH = std::max(0.0F, availH - rowIdentH);
    rowBtnH                = std::floor(
        std::clamp(remainingH * 0.45F, lineSlot, lineSlot + pad * 1.5F));
    const float rowEndsetH = std::floor(
        std::clamp(remainingH - rowBtnH, lineSlot, lineSlot * maxLines));
    const float totalH = pad * 2 + rowBtnH + gap + rowEndsetH + gap + rowIdentH;
    height             = std::floor(std::min(maxHeight, totalH));

    const float yButtons = 0.0F;
    const float yEndsets = yButtons + rowBtnH + gap;
    const float yIdent   = yEndsets + rowEndsetH + gap;

    const float wingW = std::floor((interiorWidth - gap) * 0.5F);
    childBounds[1]    = {metrics.logical(0.0F), metrics.logical(yEndsets),
                         metrics.logical(wingW), metrics.logical(rowEndsetH)};
    childBounds[2] = {metrics.logical(wingW + gap), metrics.logical(yEndsets),
                      metrics.logical(wingW), metrics.logical(rowEndsetH)};

    if (lines.size() >= 5) {
      const float originW  = std::floor((interiorWidth - 2 * gap) * 0.28F);
      const float readingW = std::floor((interiorWidth - 2 * gap) * 0.28F);
      const float identW =
          std::max(10.0F, interiorWidth - originW - readingW - 2 * gap);

      childBounds[4] = {metrics.logical(0.0F), metrics.logical(yIdent),
                        metrics.logical(originW), metrics.logical(rowIdentH)};
      childBounds[0] = {metrics.logical(originW + gap), metrics.logical(yIdent),
                        metrics.logical(identW), metrics.logical(rowIdentH)};
      childBounds[3] = {metrics.logical(originW + identW + 2 * gap),
                        metrics.logical(yIdent), metrics.logical(readingW),
                        metrics.logical(rowIdentH)};
      for (std::size_t i = 5; i < lines.size(); ++i) {
        childBounds[i] = {metrics.logical(0.0F), metrics.logical(yIdent),
                          metrics.logical(interiorWidth),
                          metrics.logical(rowIdentH)};
      }
    } else {
      childBounds[0] = {metrics.logical(0.0F), metrics.logical(yIdent),
                        metrics.logical(interiorWidth),
                        metrics.logical(rowIdentH)};
      if (lines.size() == 4) {
        const float halfW = std::floor((interiorWidth - gap) * 0.5F);
        childBounds[0] = {metrics.logical(0.0F), metrics.logical(yIdent),
                          metrics.logical(halfW), metrics.logical(rowIdentH)};
        childBounds[3] = {metrics.logical(halfW + gap), metrics.logical(yIdent),
                          metrics.logical(halfW), metrics.logical(rowIdentH)};
      }
    }

    const std::size_t bOffset = lines.size();
    childBounds[bOffset + 2]  = {
        metrics.logical(0.0F), metrics.logical(yButtons),
        metrics.logical(btnWidths[2]), metrics.logical(rowBtnH)};
    childBounds[bOffset + 3] = {
        metrics.logical(btnWidths[2] + gap), metrics.logical(yButtons),
        metrics.logical(btnWidths[3]), metrics.logical(rowBtnH)};
    childBounds[bOffset + 0] = {
        metrics.logical(btnWidths[2] + btnWidths[3] + 2 * gap),
        metrics.logical(yButtons), metrics.logical(btnWidths[0]),
        metrics.logical(rowBtnH)};
    childBounds[bOffset + 1] = {
        metrics.logical(btnWidths[2] + btnWidths[3] + btnWidths[0] + 3 * gap),
        metrics.logical(yButtons), metrics.logical(btnWidths[1]),
        metrics.logical(rowBtnH)};

    const float remainingSpace = std::max(
        0.0F, interiorWidth - leftButtonsW - centerButtonsW - rightButtonsW);
    const float interGroupSpace = std::floor(remainingSpace * 0.5F);
    const float cX              = leftButtonsW + interGroupSpace;
    childBounds[bOffset + 4] = {metrics.logical(cX), metrics.logical(yButtons),
                                metrics.logical(btnWidths[4]),
                                metrics.logical(rowBtnH)};
    childBounds[bOffset + 5] = {
        metrics.logical(cX + btnWidths[4] + gap), metrics.logical(yButtons),
        metrics.logical(btnWidths[5]), metrics.logical(rowBtnH)};
    childBounds[bOffset + 6] = {
        metrics.logical(cX + btnWidths[4] + btnWidths[5] + 2 * gap),
        metrics.logical(yButtons), metrics.logical(btnWidths[6]),
        metrics.logical(rowBtnH)};

    const float rX           = interiorWidth - rightButtonsW;
    childBounds[bOffset + 7] = {metrics.logical(rX), metrics.logical(yButtons),
                                metrics.logical(btnWidths[7]),
                                metrics.logical(rowBtnH)};
    childBounds[bOffset + 8] = {
        metrics.logical(rX + btnWidths[7] + gap), metrics.logical(yButtons),
        metrics.logical(btnWidths[8]), metrics.logical(rowBtnH)};
    childBounds[bOffset + 9] = {
        metrics.logical(rX + btnWidths[7] + btnWidths[8] + 2 * gap),
        metrics.logical(yButtons), metrics.logical(btnWidths[9]),
        metrics.logical(rowBtnH)};
    result.model = {
        .id = 1,
        .model =
            ui::PositionedPanel{.childBounds = childBounds, .background = true},
        .preferred = {metrics.logical(width), metrics.logical(height)}};

    for (std::size_t index = 0; index < lines.size(); ++index) {
      const auto &entry = lines[index];
      result.model.children.push_back(
          {.id = static_cast<ui::WidgetId>(index + 3),
           .model =
               ui::Label{entry.active ? "\u25B8 " + entry.text : entry.text},
           .preferred = {0, metrics.logical(lineSlot * maxLines)},
           .fontRole  = ui::FontRole::Label,
           .tone      = entry.tone == xanadu::PanelLine::Tone::Muted
                            ? ui::Tone::Muted
                        : entry.tone == xanadu::PanelLine::Tone::Active
                            ? ui::Tone::Accent
                            : ui::Tone::Normal,
           .maxLines  = maxLines});
    }

    for (std::size_t index = 0; index < buttons.size(); ++index) {
      const auto &button = buttons[index];
      result.model.children.push_back(
          {.id        = actionIds[index],
           .model     = ui::Button{button.label, "navigate", button.enabled},
           .preferred = {metrics.logical(btnWidths[index]),
                         metrics.logical(rowBtnH)}});
    }

    const auto margin = std::max(0.0F, config.marginPx);
    if (anchor) {
      const float posX = std::floor(
          panelArea.left + std::max(0.0F, (panelArea.width - width) * 0.5F));
      const float posY = std::floor(panelArea.bottom + margin);
      result.bounds    = {posX, posY, width, height};
    } else {
      result.bounds = ui::Rect{
          panelArea.left + panelArea.width - width - margin,
          panelArea.bottom + panelArea.height - height - margin, width, height};
    }
    result.bounds =
        ui::clampToSafeArea(metrics.rounded(result.bounds), panelArea);
    return result;
  }

  // Fallback mode for constrained/compact viewports:
  float longestButton = 0.0F;
  for (const auto &button : buttons) {
    longestButton =
        std::max(longestButton, cache.fitted(button.label, font, {}).widthPx);
  }
  auto columns = static_cast<unsigned>(
      std::clamp(std::floor(std::max(0.0F, maxWidth - wantedPadding * 2) /
                            std::max(1.0F, longestButton + wantedPadding * 2)),
                 1.0F, 5.0F));
  if ((lines.size() + (buttons.size() + columns - 1) / columns) *
          (std::ceil(line) + 2) >
      maxHeight) {
    columns = static_cast<unsigned>(std::clamp(
        std::floor(std::max(0.0F, maxWidth - wantedPadding * 2) /
                   std::max(1.0F, (longestButton + wantedPadding * 2) * 0.55F)),
        3.0F, 5.0F));
  }
  const auto rows       = (buttons.size() + columns - 1) / columns;
  const auto labelCount = static_cast<float>(lines.size());
  const auto rowCount   = static_cast<float>(rows);
  const auto spare =
      std::max(0.0F, maxHeight - (labelCount + rowCount) * lineSlot);
  const auto gapCount =
      labelCount + static_cast<float>(rows > 0 ? rows - 1 : 0);
  const auto gapF =
      std::floor(std::min(std::max(0.0F, metrics.px(config.lineGapPx)),
                          spare * .25F / std::max(1.0F, gapCount)));
  const auto padF = std::floor(
      std::min({std::max(0.0F, metrics.px(config.paddingPx)), maxWidth * .05F,
                (spare - gapF * gapCount) / (4 + rowCount * 2)}));
  result.theme.paddingEm = line > 0 ? padF / line : 0;
  result.theme.gapEm     = line > 0 ? gapF / line : 0;

  float desiredWidth =
      metrics.px(sourceTheme.type.minTouchPx) * columns + padF * 4;
  for (const auto &entry : lines) {
    desiredWidth = std::max(
        desiredWidth, cache.fitted(entry.text, font, {}).widthPx + padF * 2);
  }
  for (const auto &button : buttons) {
    desiredWidth = std::max(
        desiredWidth,
        (cache.fitted(button.label, font, {}).widthPx + padF * 2) * columns +
            padF * 4 + gapF * 2);
  }
  width                    = std::floor(std::min(maxWidth, desiredWidth));
  const auto interiorWidth = std::max(0.0F, width - padF * 2);
  const auto buttonWidth   = std::floor(std::max(
      0.0F, (interiorWidth - padF * 2 - gapF * (columns - 1)) / columns));
  const auto availableRows = std::max(0.0F, maxHeight - labelCount * lineSlot -
                                                padF * 4 - gapF * gapCount);
  const auto rowHeight =
      rows == 0 ? 0.0F
                : std::floor(
                      std::min(std::max(metrics.px(sourceTheme.type.minTouchPx),
                                        lineSlot + padF * 2),
                               availableRows / rowCount));
  const auto actionHeight =
      rows == 0 ? 0.0F
                : rowCount * rowHeight + static_cast<float>(rows - 1) * gapF +
                      padF * 2;
  const auto labelHeight = lineSlot * maxLines;
  height = std::min(maxHeight, padF * 2 + labelCount * (labelHeight + gapF) +
                                   actionHeight);
  const auto labelBudget =
      std::max(0.0F, height - padF * 2 - actionHeight - gapF * labelCount);
  const auto eachLabel =
      lines.empty()
          ? 0.0F
          : std::floor(std::min(labelHeight, labelBudget / labelCount));
  const auto buttonHeight =
      rows == 0 ? 0.0F
                : std::max(0.0F, (actionHeight - padF * 2 -
                                  gapF * static_cast<float>(rows - 1)) /
                                     static_cast<float>(rows));

  result.model = {
      .id        = 1,
      .model     = ui::Panel{},
      .preferred = {metrics.logical(width), metrics.logical(height)}};
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto &entry = lines[index];
    result.model.children.push_back(
        {.id    = static_cast<ui::WidgetId>(index + 3),
         .model = ui::Label{entry.active ? "\u25B8 " + entry.text : entry.text},
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
    flow.children.push_back(
        {.id        = actionIds[index],
         .model     = ui::Button{button.label, "navigate", button.enabled},
         .preferred = {metrics.logical(buttonWidth),
                       metrics.logical(buttonHeight)}});
  }
  result.model.children.push_back(std::move(flow));

  const auto margin = std::max(0.0F, config.marginPx);
  result.bounds =
      anchor ? ui::placeNear(*anchor, width, height, panelArea, margin)
             : ui::Rect{panelArea.left + panelArea.width - width - margin,
                        panelArea.bottom + panelArea.height - height - margin,
                        width, height};
  result.bounds =
      ui::clampToSafeArea(metrics.rounded(result.bounds), panelArea);
  return result;
}
} // namespace common_ui
