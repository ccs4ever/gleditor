/**
 * @file link_panel_overlay.hpp
 * @brief The compact selected-link panel, and the highlight on what it
 *        previews.
 *
 * Draws xanadu::linkPanelLines() above the selected pair when both places
 * have anchors, or in the window's top right corner while they do not, while a
 * link is selected, and colours the chosen member's occurrences in the open
 * documents -- the chosen occurrence more strongly than the rest -- so a
 * preview shows exactly where Enter would go without moving the caret there.
 * Both are rebuilt only when the selection, the open views, the window or the
 * configuration change; an unchanged frame redraws what was committed.
 */
#ifndef XUDU_LINK_PANEL_OVERLAY_HPP
#define XUDU_LINK_PANEL_OVERLAY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/span_decorator.hpp>
#include <gleditor/ui/overlay.hpp>

#include "common/ui/link_panel_presentation.hpp"
#include "common/ui/xanadoc/link_context.hpp"
#include "common/ui/xanadoc/session.hpp"
#include "common/xanadu/link_panel.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"

namespace xanadu {

class LinkPanelOverlay : public gleditor::FrameContributor,
                         public gleditor::PickObserver,
                         public gleditor::SpanDecorator,
                         public gleditor::a11y::Source {
public:
  /// Picking tags from here up are the panel's: its background, then one per
  /// button.
  static constexpr std::uint32_t kTagPanelBase = 18000U;

  /// Where the chosen member's cell occurrences are sent to be marked; unset
  /// outside xuzz. Called only when they change.
  using CellHighlighter = std::function<void(
      std::vector<xanadu::CellHighlight> highlights, std::uint32_t border)>;
  using AnchorPair      = std::array<glm::vec3, 2>;
  using AnchorResolver =
      std::function<std::optional<AnchorPair>(const RenderState &)>;
  using FramingHandler =
      std::function<void(const AnchorPair &, ch::Timeline &)>;

  LinkPanelOverlay(LinkContext &context, Session &session);

  void setCellHighlighter(CellHighlighter highlighter) {
    cellHighlighter = std::move(highlighter);
  }
  void setAnchorResolver(AnchorResolver resolver) {
    anchorResolver = std::move(resolver);
  }
  void setFramingHandler(FramingHandler handler) {
    framingHandler = std::move(handler);
  }

  /// From system://ui; applied at launch and whenever that store changes.
  void setConfig(const xanadu::LinkPanelConfig &next);

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override {
    const std::scoped_lock lock(actionGuard);
    return !pending.empty();
  }
  void decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;
  void describe(gleditor::a11y::Builder &into) override {
    panel.describe(into);
  }
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return panel.accessibilityRevision();
  }
  bool performAction(std::uint64_t id, gleditor::a11y::Action action,
                     std::string_view value) override {
    return panel.performAction(id, action, value);
  }
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  presentation() const {
    return panel.snapshot();
  }

private:
  /// What the committed geometry and highlights were built from.
  struct Stamp {
    std::uint64_t selection{};
    std::uint64_t views{};
    std::uint64_t config{};
    int width{};
    int height{};
    LinkContext::ReadingStamp reading;
    std::optional<std::array<glm::vec2, 2>> anchors;
    std::optional<std::array<float, 4>> protectedContent;
    bool operator==(const Stamp &) const = default;
  };

  void rebuildHighlights();
  void rebuildPanel(gleditor::FrameContext &ctx);
  void queueAction(gleditor::ui::WidgetId);
  void executePending();

  LinkContext &context;
  Session &session;
  xanadu::LinkPanelConfig config;
  std::uint64_t configRevision{1};

  gleditor::ui::ScreenOverlay panel;
  gleditor::text::ShapingCache measurements;
  gleditor::ui::UiMetrics builtMetrics;
  gleditor::ui::Theme builtTheme;
  struct PendingAction {
    std::uint64_t selection{};
    xanadu::NavigationCommand command;
  };
  mutable std::mutex actionGuard;
  std::shared_ptr<const common_ui::LinkPanelPresentation> currentPresentation;
  std::uint64_t presentationSelection{};
  std::vector<PendingAction> pending;
  std::uint32_t nextActionId{1024};
  std::optional<Stamp> panelBuiltFor;
  std::optional<Stamp> highlightsBuiltFor;
  /// Open view index and the range to colour in it.
  std::vector<std::pair<std::size_t, gleditor::SpanStyle>> highlights;
  std::vector<xanadu::CellHighlight> cellHighlights;
  CellHighlighter cellHighlighter;
  AnchorResolver anchorResolver;
  FramingHandler framingHandler;
  std::optional<std::uint64_t> framedSelection;
  std::optional<AnchorPair> framedAnchors;
  std::optional<std::array<glm::vec2, 2>> panelAnchors;
  std::optional<gleditor::ui::Rect> protectedContent;
  /// The buttons drawn, in tag order, so a pick can be mapped back.
  std::vector<xanadu::PanelButton> buttons;
};

} // namespace xanadu

#endif // XUDU_LINK_PANEL_OVERLAY_HPP
