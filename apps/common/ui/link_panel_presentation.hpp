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
struct LinkPanelPageRequest {
  gleditor::ui::WidgetId previousId, nextId;
  std::size_t page{};
};
struct LinkPanelPages {
  LinkPanelPageRequest request;
  std::size_t count{};
};
/// Plain presentation values: navigation and store access remain in the host.
struct LinkPanelPresentation {
  gleditor::ui::Widget model;
  gleditor::ui::Rect bounds;
  gleditor::ui::Theme theme;
  std::vector<LinkPanelAction> actions;
  bool safePlacement{true};
  bool completeContent{true};
  std::optional<gleditor::ui::WidgetId> toggleControls;
  std::optional<gleditor::ui::WidgetId> investigationToggle;
  std::optional<float> reservedBottom;
  std::optional<LinkPanelPages> pages;
  [[nodiscard]] const LinkPanelAction *find(gleditor::ui::WidgetId) const;
};
[[nodiscard]] LinkPanelPresentation linkPanelPresentation(
    std::span<const xanadu::PanelLine>, std::span<const xanadu::PanelButton>,
    std::span<const gleditor::ui::WidgetId> actionIds,
    const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &,
    const xanadu::LinkPanelConfig &, std::optional<gleditor::ui::Rect> anchor,
    gleditor::text::ShapingCache &,
    std::optional<gleditor::ui::Rect> protectedContent             = {},
    std::span<const gleditor::ui::Rect> additionalProtectedContent = {},
    std::optional<std::string_view> controlsLabel                  = {});
/// Paged investigation fits only unoccupied canvas; it cannot hide readable
/// source content or reframe the camera. An insufficient area is reported.
[[nodiscard]] LinkPanelPresentation linkPanelInvestigationPresentation(
    std::span<const xanadu::PanelLine>, std::span<const xanadu::PanelButton>,
    std::span<const gleditor::ui::WidgetId>, gleditor::ui::WidgetId,
    LinkPanelPageRequest, const gleditor::ui::UiMetrics &,
    const gleditor::ui::Theme &, const xanadu::LinkPanelConfig &,
    gleditor::text::ShapingCache &, std::span<const gleditor::ui::Rect>);
[[nodiscard]] LinkPanelPresentation linkPanelDockPresentation(
    std::span<const xanadu::PanelLine>, std::span<const xanadu::PanelButton>,
    std::span<const gleditor::ui::WidgetId> actionIds,
    gleditor::ui::WidgetId toggleId, bool collapsed,
    const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &,
    const xanadu::LinkPanelConfig &, gleditor::text::ShapingCache &,
    std::optional<LinkPanelPageRequest> = {});
} // namespace common_ui
#endif
