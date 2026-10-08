#ifndef COMMON_UI_LINK_PANEL_PRESENTATION_HPP
#define COMMON_UI_LINK_PANEL_PRESENTATION_HPP

#include <gleditor/ui/widgets.hpp>
#include <span>

#include "common/xanadu/link_panel.hpp"
#include "common/xanadu/system_docs.hpp"

namespace common_ui {
struct LinkPanelAction {
  gleditor::ui::WidgetId id{};
  xanadu::NavigationCommand command;
  bool enabled{};
};
/// Plain presentation values: navigation and store access remain in the host.
struct LinkPanelPresentation {
  gleditor::ui::Widget model;
  gleditor::ui::Rect bounds;
  gleditor::ui::Theme theme;
  std::vector<LinkPanelAction> actions;
  [[nodiscard]] const LinkPanelAction *find(gleditor::ui::WidgetId) const;
};
[[nodiscard]] LinkPanelPresentation linkPanelPresentation(
    std::span<const xanadu::PanelLine>, std::span<const xanadu::PanelButton>,
    std::span<const gleditor::ui::WidgetId> actionIds,
    const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &,
    const xanadu::LinkPanelConfig &, std::optional<gleditor::ui::Rect> anchor,
    gleditor::text::ShapingCache &,
    std::optional<gleditor::ui::Rect> protectedContent = {});
} // namespace common_ui
#endif
