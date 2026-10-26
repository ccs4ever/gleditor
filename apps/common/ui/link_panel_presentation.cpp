#include "link_panel_presentation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

#include <gleditor/logging.hpp>
#include <gleditor/text/font.hpp>

namespace common_ui {
namespace ui = gleditor::ui;

namespace {

std::optional<float> boundedExtent(const float value) {
  return value > 0.0F ? std::optional<float>{value} : std::nullopt;
}

std::uint16_t widgetLineLimit(const std::size_t lines) {
  return static_cast<std::uint16_t>(
      std::min<std::size_t>(lines, std::numeric_limits<std::uint16_t>::max()));
}

} // namespace

const LinkPanelAction *LinkPanelPresentation::find(ui::WidgetId id) const {
  const auto found = std::ranges::find(actions, id, &LinkPanelAction::id);
  return found == actions.end() ? nullptr : &*found;
}

LinkPanelPresentation linkPanelPresentation(
    std::span<const xanadu::PanelLine> sourceLines,
    std::span<const xanadu::PanelButton> sourceButtons,
    std::span<const ui::WidgetId> actionIds, const ui::UiMetrics &metrics,
    const ui::Theme &sourceTheme, const xanadu::LinkPanelConfig &config,
    std::optional<ui::Rect> anchor, gleditor::text::ShapingCache &cache,
    std::optional<ui::Rect> protectedContent,
    std::span<const ui::Rect> additionalProtectedContent,
    std::optional<std::string_view> controlsLabel) {
  struct Button {
    std::string label;
    std::optional<xanadu::NavigationCommand> command;
    bool enabled;
  };
  std::vector<Button> buttons;
  for (const auto &button : sourceButtons)
    buttons.push_back({button.label, button.command, button.enabled});
  if (controlsLabel) buttons.push_back({std::string(*controlsLabel), {}, true});
  if (additionalProtectedContent.size() > 3)
    throw std::invalid_argument(
        "Link panel protects reading focus and two sides");
  if (buttons.size() != actionIds.size())
    throw std::invalid_argument("Link panel action identity count mismatch");
  auto lines = sourceLines;
  std::vector<xanadu::PanelLine> compactLines;
  std::optional<std::size_t> compactContextIndex;
  std::string compactContextCaption;
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
      std::max(placement.chrome.top, metrics.px(config.topPx));
  const auto safe  = placement.pixelSafeArea();
  const auto share = [](float value) {
    return std::isfinite(value) ? std::clamp(value, .1F, 1.0F) : .9F;
  };
  const auto fullMaxWidth =
      std::floor(safe.width * share(config.maxWidthShare));
  const auto fullMaxHeight =
      std::floor(safe.height * share(config.maxHeightShare));
  float textHeight = line;
  for (const auto &button : buttons)
    textHeight =
        std::max(textHeight, cache.fitted(button.label, font, {}).heightPx);
  const auto lineSlot     = std::ceil(textHeight) + 2;
  const auto metadataFont = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(ui::FontRole::Caption, result.theme));
  float captionTextHeight = metadataFont->metrics().lineHeight;
  for (const auto &button : buttons)
    captionTextHeight =
        std::max(captionTextHeight,
                 cache.fitted(button.label, metadataFont, {}).heightPx);
  const auto captionSlot = std::ceil(captionTextHeight) + 2;
  const auto columnsFor  = [&](float width, const auto &face,
                              float padding = 0.F) {
    float longest = metrics.px(sourceTheme.type.minTouchPx);
    for (const auto &button : buttons)
      longest = std::max(
          longest, std::ceil(cache.fitted(button.label, face, {}).widthPx) + 2 +
                       padding * 2);
    return static_cast<std::size_t>(std::clamp(
        std::floor(std::max(0.F, width) / std::max(1.F, longest)), 1.F,
        static_cast<float>(std::max(std::size_t{1}, buttons.size()))));
  };
  const auto mandatoryHeight =
      [&](float width, bool compact, bool captions = false,
          bool expanded                            = false,
          std::optional<std::size_t> chosenColumns = {}) {
        const auto actionSlot = captions ? captionSlot : lineSlot;
        const auto capacity = columnsFor(width, captions ? metadataFont : font);
        const auto columns  = chosenColumns.value_or(
            expanded ? capacity : std::min(std::size_t{3}, capacity));
        float height = ((buttons.size() + columns - 1) / columns) * actionSlot;
        std::size_t contexts = 0;
        for (const auto &entry : sourceLines) {
          if (compact && !entry.compactContext.empty() && contexts++ > 0)
            continue;
          const auto prefix = (entry.active ? "\u25B8 " : "") +
                              entry.text.substr(0, entry.requiredBytes);
          const auto &required = cache.fitted(
              prefix, captions && !entry.requiredBytes ? metadataFont : font,
              {.maxWidthPx = boundedExtent(width),
               .maxLines   = std::nullopt,
               .overflow   = gleditor::text::Overflow::Wrap});
          height += entry.requiredBytes
                        ? std::max(lineSlot, std::ceil(required.heightPx) + 2)
                    : captions ? actionSlot
                               : lineSlot;
        }
        return height;
      };
  const auto caption = [&](std::size_t index) {
    if (compactContextIndex == index) return compactContextCaption;
    return lines[index].active ? "\u25B8 " + lines[index].text
                               : lines[index].text;
  };
  auto panelArea = safe;
  if (protectedContent || !additionalProtectedContent.empty()) {
    result.safePlacement = false;
    std::vector<ui::Rect> candidates{safe};
    const float gap    = metrics.px(config.marginPx);
    const auto exclude = [&](const ui::Rect &at) {
      std::vector<ui::Rect> next;
      for (const auto &area : candidates) {
        const auto right = area.left + area.width;
        const auto top   = area.bottom + area.height;
        if (at.left - gap >= right || at.left + at.width + gap <= area.left ||
            at.bottom - gap >= top ||
            at.bottom + at.height + gap <= area.bottom) {
          next.push_back(area);
          continue;
        }
        const auto leftCut = std::clamp(at.left - gap, area.left, right);
        const auto rightCut =
            std::clamp(at.left + at.width + gap, area.left, right);
        const auto bottomCut = std::clamp(at.bottom - gap, area.bottom, top);
        const auto topCut =
            std::clamp(at.bottom + at.height + gap, area.bottom, top);
        for (const auto &remaining : std::array{
                 ui::Rect{area.left, area.bottom, leftCut - area.left,
                          area.height},
                 ui::Rect{rightCut, area.bottom, right - rightCut, area.height},
                 ui::Rect{area.left, area.bottom, area.width,
                          bottomCut - area.bottom},
                 ui::Rect{area.left, topCut, area.width, top - topCut}})
          if (remaining.width > 0 && remaining.height > 0)
            next.push_back(remaining);
      }
      candidates = std::move(next);
    };
    if (protectedContent) exclude(*protectedContent);
    for (const auto &area : additionalProtectedContent) exclude(area);
    float best = 0;
    for (const auto &candidate : candidates) {
      if (candidate.width < metrics.px(sourceTheme.type.minTouchPx) * 3 ||
          std::min(fullMaxHeight, candidate.height) <
              mandatoryHeight(
                  std::max(0.F, std::min(fullMaxWidth, candidate.width) -
                                    metrics.px(config.paddingPx) * 2),
                  true, true, true))
        continue;
      const float area = std::min(fullMaxWidth, candidate.width) *
                         std::min(fullMaxHeight, candidate.height);
      if (area > best) {
        best                 = area;
        panelArea            = candidate;
        result.safePlacement = true;
      }
    }
  }
  const auto maxWidth  = std::floor(std::min(fullMaxWidth, panelArea.width));
  const auto maxHeight = std::floor(std::min(fullMaxHeight, panelArea.height));
  if (mandatoryHeight(maxWidth, false) > maxHeight &&
      std::ranges::count_if(lines, [](const auto &entry) {
        return !entry.compactContext.empty();
      }) > 1) {
    xanadu::PanelLine context{.tone = xanadu::PanelLine::Tone::Muted};
    for (const auto &entry : lines) {
      if (entry.compactContext.empty())
        compactLines.push_back(entry);
      else {
        if (!context.text.empty()) context.text += "\n";
        context.text += entry.text;
        compactContextCaption =
            entry.compactContext + (compactContextCaption.empty()
                                        ? ""
                                        : " · " + compactContextCaption);
      }
    }
    compactContextIndex = compactLines.size();
    compactLines.push_back(std::move(context));
    lines = compactLines;
  }
  // Metadata takes its existing caption role before actionable range text
  // loses space. At enlarged type this remains larger than ordinary labels.
  const bool captionMetadata = mandatoryHeight(maxWidth, true) > maxHeight;
  const auto rowFont         = [&](const xanadu::PanelLine &entry) {
    return captionMetadata && !entry.requiredBytes ? metadataFont : font;
  };
  const auto actionFont     = captionMetadata ? metadataFont : font;
  const auto actionLineSlot = captionMetadata ? captionSlot : lineSlot;
  const float wantedPadding = std::max(0.F, metrics.px(config.paddingPx));
  auto columns =
      std::min(std::size_t{3}, columnsFor(maxWidth - wantedPadding * 4 -
                                              metrics.px(config.lineGapPx) * 2,
                                          actionFont, wantedPadding));
  // Drop decorative spacing before wrapping controls into more rows. The
  // measured labels, rather than a fixed column count, determine capacity.
  if (mandatoryHeight(maxWidth - wantedPadding * 2, true, captionMetadata,
                      false, columns) > maxHeight)
    columns = columnsFor(maxWidth, actionFont);
  const auto rows       = (buttons.size() + columns - 1) / columns;
  const auto labelCount = static_cast<float>(lines.size());
  const auto rowCount   = static_cast<float>(rows);
  // Reserve visible text before decorative spacing or minimum touch heights.
  // Integer slots leave room for edge rounding at both nested flow boundaries.
  const auto spare =
      std::max(0.0F, maxHeight - (labelCount + rowCount) * lineSlot);
  const auto gapCount =
      labelCount + static_cast<float>(rows > 0 ? rows - 1 : 0);
  auto gap = std::floor(std::min(std::max(0.0F, metrics.px(config.lineGapPx)),
                                 spare * .25F / std::max(1.0F, gapCount)));
  auto pad = std::floor(
      std::min({std::max(0.0F, metrics.px(config.paddingPx)), maxWidth * .05F,
                (spare - gap * gapCount) / (4 + rowCount * 2)}));
  // Widget padding follows each role's line height. Caption controls may
  // therefore need more inner space than the panel's configured label font.
  const auto actionPadRatio =
      line > 0 ? actionFont->metrics().lineHeight / line : 1.F;
  float longestAction = 0;
  for (const auto &button : buttons)
    longestAction = std::max(
        longestAction,
        std::ceil(cache.fitted(button.label, actionFont, {}).widthPx) + 2);
  const auto horizontalDecoration =
      pad * (4 + 2 * actionPadRatio * columns) + gap * (columns - 1);
  const auto horizontalBudget =
      std::max(0.F, maxWidth - longestAction * columns);
  if (horizontalDecoration > horizontalBudget) {
    const auto scale = horizontalBudget / horizontalDecoration;
    pad              = std::floor(pad * scale);
    gap              = std::floor(gap * scale);
  }
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

  float desiredWidth =
      metrics.px(sourceTheme.type.minTouchPx) * columns + pad * 4;
  for (const auto &entry : lines)
    desiredWidth = std::max(
        desiredWidth, cache.fitted(entry.text, font, {}).widthPx + pad * 2);
  desiredWidth             = std::max(desiredWidth,
                                      (longestAction + pad * 2 * actionPadRatio) * columns +
                                          pad * 4 + gap * (columns - 1));
  const auto width         = std::floor(std::min(maxWidth, desiredWidth));
  const auto interiorWidth = std::max(0.0F, width - pad * 2);
  const auto buttonWidth   = std::floor(std::max(
      0.0F, (interiorWidth - pad * 2 - gap * (columns - 1)) / columns));
  std::vector<float> minimumLabels;
  std::vector<std::uint16_t> labelLineLimits;
  float minimumLabelHeight = 0;
  for (const auto &entry : lines) {
    const auto prefix = (entry.active ? "\u25B8 " : "") +
                        entry.text.substr(0, entry.requiredBytes);
    const auto &required =
        cache.fitted(prefix, rowFont(entry),
                     {.maxWidthPx = boundedExtent(interiorWidth),
                      .maxLines   = std::nullopt,
                      .overflow   = gleditor::text::Overflow::Wrap});
    const auto minimum =
        entry.requiredBytes
            ? std::max(lineSlot, std::ceil(required.heightPx) + 2)
            : std::ceil(rowFont(entry)->metrics().lineHeight) + 2;
    minimumLabels.push_back(minimum);
    minimumLabelHeight += minimum;
    labelLineLimits.push_back(widgetLineLimit(
        std::max({std::size_t{1}, static_cast<std::size_t>(config.maxLines),
                  required.lines})));
  }
  // Mandatory wrapped ranges get space before decorative padding. Otherwise
  // a readable prefix can squeeze every action down to an empty hit target.
  const auto decorationBudget =
      std::max(0.F, maxHeight - minimumLabelHeight - rowCount * actionLineSlot);
  const auto decorationHeight = pad * (4 + rowCount * 2) + gap * gapCount;
  if (decorationHeight > decorationBudget) {
    const auto scale       = decorationBudget / decorationHeight;
    pad                    = std::floor(pad * scale);
    gap                    = std::floor(gap * scale);
    result.theme.paddingEm = line > 0 ? pad / line : 0;
    result.theme.gapEm     = line > 0 ? gap / line : 0;
  }
  result.completeContent =
      minimumLabelHeight + rowCount * actionLineSlot <= maxHeight;
  GLEDITOR_LOG_TRACE("ui.layout",
                     "Link panel minimum: width={} height={} labels={} "
                     "actionSlot={} rows={} complete={} safe={}",
                     maxWidth, maxHeight, minimumLabelHeight, actionLineSlot,
                     rows, result.completeContent, result.safePlacement);
  const auto availableRows =
      std::max(0.0F, maxHeight - minimumLabelHeight - pad * 4 - gap * gapCount);
  const auto rowHeight =
      rows == 0 ? 0.0F
                : std::floor(
                      std::min(std::max(metrics.px(sourceTheme.type.minTouchPx),
                                        actionLineSlot + pad * 2),
                               availableRows / rowCount));
  const auto actionHeight =
      rows == 0
          ? 0.0F
          : rowCount * rowHeight + static_cast<float>(rows - 1) * gap + pad * 2;
  const auto maxLines    = std::max<std::uint16_t>(1, config.maxLines);
  const auto labelHeight = lineSlot * maxLines;
  std::vector<float> labelHeights;
  labelHeights.reserve(lines.size());
  float wantedLabels = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto label   = caption(index);
    const auto &fitted = cache.fitted(
        label, rowFont(lines[index]),
        {.maxWidthPx  = boundedExtent(interiorWidth),
         .maxHeightPx = std::max(labelHeight, minimumLabels[index]),
         .maxLines    = labelLineLimits[index],
         .overflow    = gleditor::text::Overflow::Wrap});
    const auto wanted =
        std::max(minimumLabels[index], std::ceil(fitted.heightPx) + 2);
    labelHeights.push_back(wanted);
    wantedLabels += wanted;
  }
  const auto height = std::min(maxHeight, pad * 2 + wantedLabels +
                                              labelCount * gap + actionHeight);
  const auto labelBudget =
      std::max(0.0F, height - pad * 2 - actionHeight - gap * labelCount);
  if (wantedLabels > labelBudget) {
    const auto base  = minimumLabelHeight;
    const auto extra = std::max(0.0F, labelBudget - base);
    for (std::size_t index = 0; index < labelHeights.size(); ++index) {
      auto &label = labelHeights[index];
      label       = minimumLabels[index] +
              std::floor((label - minimumLabels[index]) * extra /
                         std::max(1.0F, wantedLabels - base));
    }
  }
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
    const auto &entry    = lines[index];
    const auto fullLabel = entry.active ? "\u25B8 " + entry.text : entry.text;
    auto label           = caption(index);
    const auto required  = entry.requiredBytes +
                          (entry.active ? std::string("\u25B8 ").size() : 0);
    if (entry.requiredBytes) {
      const auto &fitted =
          cache.fitted(fullLabel, font,
                       {.maxWidthPx  = boundedExtent(width - pad * 2),
                        .maxHeightPx = labelHeights[index],
                        .maxLines    = labelLineLimits[index],
                        .overflow    = gleditor::text::Overflow::Wrap});
      // An ellipsis on the last retained line can displace range digits.
      // Keeping only the required block removes that competition entirely.
      if (fitted.visibleBytes < required ||
          (fitted.truncated && fitted.visibleBytes <= required + 1))
        label = fullLabel.substr(0, required);
    }
    result.model.children.push_back(
        {.id        = static_cast<ui::WidgetId>(index + 3),
         .model     = ui::Label{label, ui::TextPurpose::Description, fullLabel},
         .preferred = {0, metrics.logical(labelHeights[index])},
         .fontRole  = captionMetadata && !entry.requiredBytes
                          ? ui::FontRole::Caption
                          : ui::FontRole::Label,
         .tone = entry.tone == xanadu::PanelLine::Tone::Muted ? ui::Tone::Muted
                 : entry.tone == xanadu::PanelLine::Tone::Active
                     ? ui::Tone::Accent
                     : ui::Tone::Normal,
         .maxLines = labelLineLimits[index]});
  }
  ui::Widget flow{.id        = 2,
                  .model     = ui::ButtonFlow{},
                  .preferred = {metrics.logical(interiorWidth),
                                metrics.logical(actionHeight)}};
  for (std::size_t index = 0; index < buttons.size(); ++index) {
    const auto &button = buttons[index];
    if (button.command)
      result.actions.push_back(
          {actionIds[index], *button.command, button.enabled});
    else
      result.toggleControls = actionIds[index];
    flow.children.push_back(
        {.id        = actionIds[index],
         .model     = ui::Button{button.label, "navigate", button.enabled},
         .preferred = {metrics.logical(buttonWidth),
                       metrics.logical(buttonHeight)},
         .fontRole =
             captionMetadata ? ui::FontRole::Caption : ui::FontRole::Label});
  }
  result.model.children.push_back(std::move(flow));
  const auto margin = std::max(0.0F, metrics.px(config.marginPx));
  result.bounds =
      anchor ? ui::placeNear(*anchor, width, height, panelArea, margin)
             : ui::Rect{panelArea.left + panelArea.width - width - margin,
                        panelArea.bottom + panelArea.height - height - margin,
                        width, height};
  result.bounds =
      ui::clampToSafeArea(metrics.rounded(result.bounds), panelArea);
  return result;
}
namespace {
LinkPanelPresentation
inspectorPresentation(std::span<const xanadu::PanelLine> lines,
                      std::span<const xanadu::PanelButton> buttons,
                      std::span<const ui::WidgetId> ids, ui::WidgetId toggleId,
                      LinkPanelPageRequest request,
                      const ui::UiMetrics &metrics, const ui::Theme &theme,
                      const xanadu::LinkPanelConfig &config,
                      gleditor::text::ShapingCache &cache) {
  auto local            = metrics;
  local.marginShare     = 0;
  auto sizing           = config;
  sizing.maxWidthShare  = 1;
  sizing.maxHeightShare = 1;
  sizing.marginPx       = 0;
  // Reuse the panel's configured typography and colour treatment.
  auto result =
      linkPanelPresentation({}, {}, {}, local, theme, sizing, {}, cache);
  const auto font = gleditor::text::FontManager::instance().getFont(
      local.fontDescription(ui::FontRole::Label, result.theme));
  const auto caption = gleditor::text::FontManager::instance().getFont(
      local.fontDescription(ui::FontRole::Caption, result.theme));
  const auto pad         = std::max(0.F, local.px(config.paddingPx));
  const auto gap         = std::max(0.F, local.px(config.lineGapPx));
  result.theme.paddingEm = 0;
  result.theme.gapEm =
      font->metrics().lineHeight > 0 ? gap / font->metrics().lineHeight : 0;
  result.bounds = {
      0, metrics.chrome.bottom, static_cast<float>(metrics.screenWidth),
      std::max(0.F, metrics.screenHeight -
                        std::max(metrics.chrome.top, local.px(config.topPx)) -
                        metrics.chrome.bottom)};
  const auto width       = std::max(0.F, result.bounds.width - pad * 2);
  const auto captionSlot = std::ceil(caption->metrics().lineHeight) + 2;
  const auto actionSlot  = std::ceil(font->metrics().lineHeight) + pad * 2;
  std::vector<ui::Widget> pinned;
  std::string fullContext;
  for (const auto &line : lines) fullContext += line.text + "\n";
  for (std::size_t i = 0; i < std::min<std::size_t>(3, lines.size()); ++i) {
    auto text = i == 0 && lines[i].compactIdentity
                    ? *lines[i].compactIdentity
                    : lines[i].text.substr(0, lines[i].text.find('\n'));
    if (const auto unit = text.find(" · bytes"); unit != std::string::npos)
      text.resize(unit);
    if (lines[i].text.substr(0, lines[i].requiredBytes).find("(part)") !=
        std::string::npos)
      text += " (part)";
    if (lines[i].active) text = "\u25B8 " + text;
    const auto pageLimit  = std::to_string(lines.size() + buttons.size());
    const auto headingFit = cache.fitted(
        i == 0 ? "Controls " + pageLimit + "/" + pageLimit + " · " + text
               : text,
        caption,
        {.maxWidthPx = boundedExtent(width),
         .maxLines   = std::nullopt,
         .overflow   = gleditor::text::Overflow::Wrap});
    const auto slot = std::max(captionSlot, std::ceil(headingFit.heightPx) + 2);
    pinned.push_back(
        {.id        = static_cast<ui::WidgetId>(i + 3),
         .model     = ui::Label{text,
                            i == 0 ? ui::TextPurpose::Description
                                       : ui::TextPurpose::Label,
                            i == 0 ? fullContext : lines[i].text},
         .preferred = {local.logical(width), local.logical(slot)},
         .fontRole  = ui::FontRole::Caption,
         .tone      = lines[i].active ? ui::Tone::Accent : ui::Tone::Normal,
         .maxLines  = widgetLineLimit(headingFit.lines)});
  }
  // Pending entry remains visible on every page, beside its pinned
  // cancellation.
  for (std::size_t i = 3; i < lines.size(); ++i) {
    const auto &entry = lines[i];
    if (entry.requiredBytes || !entry.compactContext.empty()) continue;
    const auto &fit =
        cache.fitted(entry.text, caption,
                     {.maxWidthPx = boundedExtent(width),
                      .maxLines   = std::nullopt,
                      .overflow   = gleditor::text::Overflow::Wrap});
    pinned.push_back(
        {.id = static_cast<ui::WidgetId>(60 + i),
         .model =
             ui::Label{entry.text, ui::TextPurpose::Description, entry.text},
         .preferred = {local.logical(width),
                       local.logical(std::ceil(fit.heightPx) + 2)},
         .fontRole  = ui::FontRole::Caption,
         .tone      = ui::Tone::Accent,
         .maxLines  = widgetLineLimit(fit.lines)});
  }
  std::vector<ui::Widget> footer;
  const auto addButton = [&](ui::WidgetId id, std::string label, bool enabled,
                             std::string accessible = {}) {
    return ui::Widget{.id        = id,
                      .model     = ui::Button{std::move(label), "link_controls",
                                          enabled, std::move(accessible)},
                      .preferred = {0, local.logical(actionSlot)}};
  };
  footer.push_back(addButton(request.previousId, "Previous", true));
  footer.push_back(addButton(request.nextId, "Next", true));
  footer.push_back(addButton(toggleId, "Read", true));
  for (std::size_t i = 0; i < buttons.size(); ++i)
    if (std::holds_alternative<xanadu::nav::Dismiss>(buttons[i].command) ||
        std::holds_alternative<xanadu::nav::CancelEntry>(buttons[i].command)) {
      footer.push_back(addButton(
          ids[i], buttons[i].label, buttons[i].enabled,
          std::holds_alternative<xanadu::nav::Dismiss>(buttons[i].command)
              ? "Dismiss selected link"
              : "Cancel pending entry"));
      result.actions.push_back(
          {ids[i], buttons[i].command, buttons[i].enabled});
    }
  float longest = 0;
  for (const auto &button : footer)
    longest = std::max(
        longest, cache.fitted(std::get<ui::Button>(button.model).text, font, {})
                         .widthPx +
                     pad * 2);
  const auto columns      = static_cast<std::size_t>(std::clamp(
      std::floor((width + gap) / std::max(1.F, longest + gap)), 1.F, 3.F));
  const auto footerRows   = (footer.size() + columns - 1) / columns;
  const auto footerHeight = footerRows * actionSlot + (footerRows - 1) * gap;
  float headerHeight      = 0;
  for (const auto &widget : pinned)
    headerHeight += local.px(widget.preferred.height) + gap;
  const auto bodyHeight =
      result.bounds.height - pad * 2 - headerHeight - footerHeight - gap;
  struct Block {
    ui::Widget widget;
    float height;
    std::vector<LinkPanelAction> actions;
  };
  std::vector<Block> blocks;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto &entry = lines[i];
    if (!entry.requiredBytes) continue;
    const auto firstBreak = entry.text.find('\n');
    const auto sideEnd    = entry.text.find(' ');
    auto core =
        firstBreak == std::string::npos || firstBreak >= entry.requiredBytes
            ? entry.text.substr(0, entry.requiredBytes)
            : entry.text.substr(0, sideEnd) + ": bytes " +
                  entry.text.substr(firstBreak + 1,
                                    entry.requiredBytes - firstBreak - 1);
    const auto fit =
        [&](const std::string &text) -> const gleditor::text::FittedText & {
      return cache.fitted(text, font,
                          {.maxWidthPx = boundedExtent(width),
                           .maxLines   = std::nullopt,
                           .overflow   = gleditor::text::Overflow::Wrap});
    };
    const auto minimum = std::ceil(fit(core).heightPx) + 2;
    auto text          = core + entry.text.substr(entry.requiredBytes);
    if (std::ceil(fit(text).heightPx) + 2 > bodyHeight) text = core;
    const auto &fitted = fit(text);
    blocks.push_back(
        {{.id    = static_cast<ui::WidgetId>(20 + i),
          .model = ui::Label{text, ui::TextPurpose::Description, entry.text},
          .preferred = {local.logical(width),
                        local.logical(std::ceil(fitted.heightPx) + 2)},
          .tone      = entry.active ? ui::Tone::Accent : ui::Tone::Normal,
          .maxLines  = widgetLineLimit(fitted.lines)},
         std::max(minimum, std::ceil(fitted.heightPx) + 2),
         {}});
  }
  std::vector<std::size_t> ordinary;
  for (std::size_t i = 0; i < buttons.size(); ++i)
    if (!std::holds_alternative<xanadu::nav::Dismiss>(buttons[i].command) &&
        !std::holds_alternative<xanadu::nav::CancelEntry>(buttons[i].command))
      ordinary.push_back(i);
  for (std::size_t start = 0; start < ordinary.size(); start += columns) {
    Block block{
        {.id        = static_cast<ui::WidgetId>(40 + start),
         .model     = ui::ButtonFlow{},
         .preferred = {local.logical(width), local.logical(actionSlot)}},
        actionSlot,
        {}};
    const auto cellWidth = (width - (columns - 1) * gap) / columns;
    for (auto j = start; j < std::min(start + columns, ordinary.size()); ++j) {
      const auto i = ordinary[j];
      auto button  = addButton(ids[i], buttons[i].label, buttons[i].enabled);
      button.preferred.width = local.logical(cellWidth);
      block.widget.children.push_back(std::move(button));
      block.actions.push_back({ids[i], buttons[i].command, buttons[i].enabled});
    }
    blocks.push_back(std::move(block));
  }
  if (pinned.empty() || blocks.empty() ||
      std::ranges::any_of(
          blocks,
          [&](const auto &block) { return block.height > bodyHeight; }) ||
      longest > width) {
    GLEDITOR_LOG_DEBUG(
        "ui.layout",
        "Link inspector minimum: body={} width={} header={} footer={} "
        "largest={}",
        bodyHeight, width, headerHeight, footerHeight,
        blocks.empty() ? 0.F
                       : std::ranges::max(blocks, {}, &Block::height).height);
    result.completeContent = false;
    return result;
  }
  std::vector<std::vector<std::size_t>> pages(1);
  float used = 0;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto cost = blocks[i].height + (pages.back().empty() ? 0 : gap);
    if (!pages.back().empty() && used + cost > bodyHeight) {
      pages.emplace_back();
      used = 0;
    }
    used += blocks[i].height + (pages.back().empty() ? 0 : gap);
    pages.back().push_back(i);
  }
  request.page          = std::min(request.page, pages.size() - 1);
  result.pages          = LinkPanelPages{request, pages.size()};
  result.toggleControls = toggleId;
  std::get<ui::Label>(pinned.front().model).text =
      "Controls " + std::to_string(request.page + 1) + "/" +
      std::to_string(pages.size()) + " · " +
      std::get<ui::Label>(pinned.front().model).text;
  auto &previous           = std::get<ui::Button>(footer[0].model);
  auto &next               = std::get<ui::Button>(footer[1].model);
  previous.enabled         = request.page > 0;
  next.enabled             = request.page + 1 < pages.size();
  previous.accessibleLabel = "Previous controls page";
  next.accessibleLabel     = "Next controls page";
  std::get<ui::Label>(pinned.front().model).accessibleLabel +=
      "Controls page " + std::to_string(request.page + 1) + " of " +
      std::to_string(pages.size());
  ui::PositionedPanel placement;
  result.model      = {.id        = 1,
                       .model     = placement,
                       .preferred = {local.logical(result.bounds.width),
                                     local.logical(result.bounds.height)}};
  const auto append = [&](ui::Widget widget, ui::Rect bounds) {
    std::get<ui::PositionedPanel>(result.model.model)
        .childBounds.push_back(
            {local.logical(bounds.left), local.logical(bounds.bottom),
             local.logical(bounds.width), local.logical(bounds.height)});
    result.model.children.push_back(std::move(widget));
  };
  float top = result.bounds.height - pad;
  for (auto &widget : pinned) {
    const auto slot = local.px(widget.preferred.height);
    top -= slot;
    append(std::move(widget), {pad, top, width, slot});
    top -= gap;
  }
  for (const auto index : pages[request.page]) {
    top -= blocks[index].height;
    append(std::move(blocks[index].widget),
           {pad, top, width, blocks[index].height});
    top -= gap;
    result.actions.insert(result.actions.end(), blocks[index].actions.begin(),
                          blocks[index].actions.end());
  }
  ui::Widget flow{
      .id        = 2,
      .model     = ui::ButtonFlow{},
      .preferred = {local.logical(width), local.logical(footerHeight)}};
  for (auto &button : footer) {
    button.preferred.width =
        local.logical((width - (columns - 1) * gap) / columns);
    flow.children.push_back(std::move(button));
  }
  append(std::move(flow), {pad, pad, width, footerHeight});
  // PositionedPanel is a transparent diagram; the existing Panel supplies the
  // inspector's continuous configured surface without changing diagram widgets.
  auto content          = std::move(result.model);
  const auto preferred  = content.preferred;
  result.model          = {.id        = 70,
                           .model     = ui::Panel{},
                           .children  = {std::move(content)},
                           .preferred = preferred};
  result.reservedBottom = result.bounds.bottom + result.bounds.height;
  return result;
}
} // namespace
LinkPanelPresentation linkPanelDockPresentation(
    std::span<const xanadu::PanelLine> sourceLines,
    std::span<const xanadu::PanelButton> sourceButtons,
    std::span<const ui::WidgetId> sourceIds, ui::WidgetId toggleId,
    bool collapsed, const ui::UiMetrics &metrics, const ui::Theme &theme,
    const xanadu::LinkPanelConfig &config, gleditor::text::ShapingCache &cache,
    std::optional<LinkPanelPageRequest> paging) {
  if (sourceIds.size() != sourceButtons.size())
    throw std::invalid_argument("Link panel action identity count mismatch");
  auto dockMetrics        = metrics;
  dockMetrics.marginShare = 0;
  std::vector<xanadu::PanelLine> lines;
  std::vector<std::string> descriptions;
  for (const auto &source : sourceLines) {
    if (!source.compactContext.empty()) continue;
    auto entry = source;
    if (collapsed && entry.requiredBytes) {
      entry.text = entry.text.substr(0, entry.text.find('\n'));
      if (const auto unit = entry.text.find(" · bytes");
          unit != std::string::npos)
        entry.text.resize(unit);
      if (source.text.substr(0, source.requiredBytes).find("(part)") !=
          std::string::npos)
        entry.text += " (part)";
      entry.requiredBytes = entry.text.size();
    }
    descriptions.push_back((source.active ? "\u25B8 " : "") + source.text);
    lines.push_back(std::move(entry));
  }
  if (!descriptions.empty())
    for (const auto &entry : sourceLines)
      if (!entry.compactContext.empty())
        descriptions.front() += "\n" + entry.text;
  std::vector<xanadu::PanelButton> buttons;
  std::vector<ui::WidgetId> ids;
  for (std::size_t i = 0; i < sourceButtons.size(); ++i)
    if (!collapsed ||
        std::holds_alternative<xanadu::nav::Dismiss>(
            sourceButtons[i].command) ||
        std::holds_alternative<xanadu::nav::CancelEntry>(
            sourceButtons[i].command)) {
      buttons.push_back(sourceButtons[i]);
      ids.push_back(sourceIds[i]);
    }
  ids.push_back(toggleId);
  auto dockConfig           = config;
  dockConfig.maxWidthShare  = 1;
  dockConfig.maxHeightShare = config.dockMaxHeightShare;
  dockConfig.maxLines       = 1;
  dockConfig.marginPx       = 0;
  auto result =
      linkPanelPresentation(lines, buttons, ids, dockMetrics, theme, dockConfig,
                            {}, cache, {}, {}, collapsed ? "Controls" : "Read");
  for (std::size_t i = 0; i < descriptions.size(); ++i)
    std::get<ui::Label>(result.model.children[i].model).accessibleLabel =
        descriptions[i];
  if (!result.completeContent && !collapsed && paging) {
    auto inspector =
        inspectorPresentation(sourceLines, sourceButtons, sourceIds, toggleId,
                              *paging, dockMetrics, theme, config, cache);
    if (inspector.completeContent) return inspector;
  }
  if (!result.completeContent) {
    const std::array warning{xanadu::PanelLine{.text = "Enlarge window"}};
    std::vector<xanadu::PanelButton> essential;
    std::vector<ui::WidgetId> essentialIds;
    for (std::size_t i = 0; i < sourceButtons.size(); ++i)
      if (std::holds_alternative<xanadu::nav::Dismiss>(
              sourceButtons[i].command) ||
          std::holds_alternative<xanadu::nav::CancelEntry>(
              sourceButtons[i].command)) {
        essential.push_back(sourceButtons[i]);
        essentialIds.push_back(sourceIds[i]);
      }
    essentialIds.push_back(toggleId);
    result = linkPanelPresentation(warning, essential, essentialIds,
                                   dockMetrics, theme, dockConfig, {}, cache,
                                   {}, {}, collapsed ? "Controls" : "Read");
    std::string context;
    for (const auto &source : sourceLines) context += source.text + "\n";
    std::get<ui::Label>(result.model.children.front().model).accessibleLabel =
        context + "Link controls need more space. Enlarge the window or "
                  "reduce UI text size.";
    result.completeContent = false;
  }
  result.bounds.bottom         = metrics.chrome.bottom;
  result.bounds.left           = 0;
  result.bounds.width          = metrics.screenWidth;
  result.model.preferred.width = metrics.logical(result.bounds.width);
  const auto font = gleditor::text::FontManager::instance().getFont(
      dockMetrics.fontDescription(ui::FontRole::Label, result.theme));
  const auto padding = font->metrics().lineHeight * result.theme.paddingEm;
  const auto gap     = font->metrics().lineHeight * result.theme.gapEm;
  auto &flow         = result.model.children.back();
  // Dock expansion must retain the columns that its measured height covers.
  // Forcing fewer columns here adds rows without reserving their text height.
  const auto columns =
      flow.children.empty()
          ? std::size_t{1}
          : static_cast<std::size_t>(std::clamp(
                std::floor(
                    (metrics.px(flow.preferred.width) - padding * 2 + gap) /
                    std::max(1.F,
                             metrics.px(flow.children.front().preferred.width) +
                                 gap)),
                1.F, static_cast<float>(flow.children.size())));
  flow.preferred.width =
      metrics.logical(std::max(0.F, result.bounds.width - padding * 2));
  const auto buttonWidth = metrics.logical(
      std::max(0.F, (result.bounds.width - padding * 4 - gap * (columns - 1)) /
                        columns));
  for (auto &button : flow.children) button.preferred.width = buttonWidth;
  result.reservedBottom = result.bounds.bottom + result.bounds.height;
  return result;
}
} // namespace common_ui
