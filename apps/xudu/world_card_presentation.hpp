#ifndef XUDU_WORLD_CARD_PRESENTATION_HPP
#define XUDU_WORLD_CARD_PRESENTATION_HPP

#include <algorithm>
#include <cmath>
#include <gleditor/text/font.hpp>
#include <gleditor/ui/world_panel.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include "common/xanadu/system_docs.hpp"

namespace xanadu::world_cards {
namespace ui = gleditor::ui;
struct Style {
  ui::UiMetrics metrics;
  ui::Theme theme;
  ui::Size size;
  float padding{}, gap{}, line{};
};
inline Style style(const ui::UiMetrics &metrics, const ui::Theme &source,
                   const xanadu::WorldCardConfig &config, ui::FontRole role,
                   std::string_view override, float rows) {
  Style out{metrics, ui::withFontOverride(source, role, override)};
  out.theme.paddingEm = std::min(out.theme.paddingEm, .25F);
  out.theme.gapEm     = std::min(out.theme.gapEm, .25F);
  const auto font     = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(role, out.theme));
  out.line =
      std::ceil(font->metrics().lineHeight * (1 + out.theme.type.lineGapEm)) +
      2;
  out.padding      = font->metrics().lineHeight * out.theme.paddingEm;
  out.gap          = font->metrics().lineHeight * out.theme.gapEm;
  const auto safe  = metrics.pixelSafeArea();
  const auto share = [](float v) {
    return std::isfinite(v) ? std::clamp(v, .1F, 1.F) : .9F;
  };
  const auto length = [&](float v, float fallback) {
    return metrics.px(std::isfinite(v) && v > 0 ? v : fallback);
  };
  out.size = {std::min(safe.width * share(config.maxWidthShare),
                       std::max(length(config.widthPx, 200), out.line * 8)),
              std::min(safe.height * share(config.maxHeightShare),
                       std::max(length(config.heightPx, 180),
                                rows * out.line + (rows + 1) * out.gap +
                                    4 * out.padding + 4))};
  return out;
}
inline ui::Rect floating(ui::Size size, glm::vec2 anchor,
                         const ui::UiMetrics &metrics, float gap) {
  return ui::placeNear({anchor.x, anchor.y, 0, 0}, size.width, size.height,
                       metrics.pixelSafeArea(), gap);
}
inline glm::mat4 screenMatrix(ui::Rect bounds, ui::Size viewport) {
  return glm::ortho(0.F, viewport.width, 0.F, viewport.height) *
         glm::translate(glm::mat4(1), glm::vec3(bounds.left, bounds.bottom, 0));
}
inline void describe(gleditor::a11y::Builder &into, const ui::WorldPanel &panel,
                     const glm::mat4 &matrix, ui::Size viewport,
                     std::uint32_t id, std::string_view title,
                     std::string_view description, bool actionable = false) {
  const auto projected = panel.projected(1, matrix, viewport);
  if (!projected) return;
  const auto &rect = projected->bounds;
  auto &node       = into.add(id, actionable ? gleditor::a11y::Role::Button
                                             : gleditor::a11y::Role::Group);
  node.label       = title;
  node.description = description;
  node.bounds      = gleditor::a11y::Rect{
      rect.left, viewport.height - rect.bottom - rect.height,
      rect.left + rect.width, viewport.height - rect.bottom};
  node.focusable = actionable;
  if (actionable)
    node.actions = gleditor::a11y::bit(gleditor::a11y::Action::Click);
  into.contribute(into.id(id));
}

struct Card {
  ui::WorldPanel panel;
  Style presentation;
  xanadu::WorldCardConfig config;
  ui::UiMetrics metrics;
  ui::Theme theme;
  std::string title, body, footer;
  bool ready{}, dirty{true};
  std::uint64_t revision{1};
  void configure(const ui::UiMetrics &nextMetrics, const ui::Theme &nextTheme,
                 const xanadu::WorldCardConfig &nextConfig,
                 std::string_view font) {
    if (ready && metrics == nextMetrics && theme == nextTheme &&
        config == nextConfig)
      return;
    metrics      = nextMetrics;
    theme        = nextTheme;
    config       = nextConfig;
    presentation = style(metrics, theme, config, ui::FontRole::Body, font,
                         static_cast<float>(config.maxLines) + 4);
    panel.setBounds({0, 0, presentation.size.width, presentation.size.height});
    panel.setLabelLod({0, theme.type.minFontPx});
    ready = true;
    dirty = true;
  }
  void content(std::string_view nextTitle, std::string_view nextBody,
               std::string_view nextFooter = {}) {
    if (title == nextTitle && body == nextBody && footer == nextFooter) return;
    title  = nextTitle;
    body   = nextBody;
    footer = nextFooter;
    dirty  = true;
  }
  void prepare() {
    const auto previous = panel.snapshot();
    if (dirty) {
      const auto line = presentation.line;
      ui::Widget model{
          .id = 1, .model = ui::Panel{}, .maxLines = config.maxLines};
      model.children.push_back(
          {.id        = 2,
           .model     = ui::Label{title, ui::TextPurpose::Title},
           .preferred = {0, metrics.logical(line * 2)},
           .fontRole  = ui::FontRole::Caption,
           .maxLines  = 2});
      if (!body.empty())
        model.children.push_back(
            {.id        = 3,
             .model     = ui::Label{body, ui::TextPurpose::Description},
             .preferred = {0, metrics.logical(line * config.maxLines)},
             .fontRole  = ui::FontRole::Body,
             .maxLines  = config.maxLines});
      if (!footer.empty())
        model.children.push_back(
            {.id        = 4,
             .model     = ui::Label{footer, ui::TextPurpose::Description},
             .preferred = {0, metrics.logical(line * 2)},
             .fontRole  = ui::FontRole::Caption,
             .maxLines  = 2});
      panel.setModel(std::move(model));
      dirty = false;
    }
    (void)panel.prepare(metrics, presentation.theme);
    if (previous != panel.snapshot()) ++revision;
  }
};
inline ui::Rect awayFromPointer(ui::Size size, glm::vec2 pointer,
                                const ui::UiMetrics &metrics, float gap) {
  const auto safe  = metrics.pixelSafeArea();
  const auto above = std::max(0.F, safe.bottom + safe.height - pointer.y - gap);
  const auto below = std::max(0.F, pointer.y - safe.bottom - gap);
  size.height      = std::min(size.height, std::max(above, below));
  return ui::clampToSafeArea(
      {pointer.x - size.width * .5F,
       above >= below ? pointer.y + gap : pointer.y - gap - size.height,
       size.width, size.height},
      safe);
}
} // namespace xanadu::world_cards
#endif
