/**
 * @file link_panel_overlay.hpp
 * @brief The compact selected-link panel, and the highlight on what it
 *        previews.
 *
 * Draws xanadu::linkPanelLines() in the window's top right corner while a
 * link is selected, and colours the chosen member's occurrences in the open
 * documents -- the chosen occurrence more strongly than the rest -- so a
 * preview shows exactly where Enter would go without moving the caret there.
 * Both are rebuilt only when the selection, the open views, the window or the
 * configuration change; an unchanged frame redraws what was committed.
 */
#ifndef XUDU_LINK_PANEL_OVERLAY_HPP
#define XUDU_LINK_PANEL_OVERLAY_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/span_decorator.hpp>

#include "common/xanadu/system_docs.hpp"
#include "xudu/link_context.hpp"
#include "xudu/session.hpp"

namespace xudu {

class LinkPanelOverlay : public gleditor::FrameContributor,
                         public gleditor::SpanDecorator {
public:
  LinkPanelOverlay(LinkContext &context, Session &session) noexcept
      : context(context), session(session) {}

  /// From system://ui; applied at launch and whenever that store changes.
  void setConfig(const xanadu::LinkPanelConfig &next);

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  void decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) override;

private:
  /// What the committed geometry and highlights were built from.
  struct Stamp {
    std::uint64_t selection{};
    std::uint64_t views{};
    std::uint64_t config{};
    int width{};
    int height{};
    bool operator==(const Stamp &) const = default;
  };

  void rebuildHighlights();
  void rebuildPanel(gleditor::FrameContext &ctx);

  LinkContext &context;
  Session &session;
  xanadu::LinkPanelConfig config;
  std::uint64_t configRevision{1};

  render::RenderDevice *device{};
  render::PipelineDesc pipeline;
  std::unique_ptr<gleditor::Canvas> canvas;
  std::optional<Stamp> panelBuiltFor;
  std::optional<Stamp> highlightsBuiltFor;
  /// Open view index and the range to colour in it.
  std::vector<std::pair<std::size_t, gleditor::SpanStyle>> highlights;
};

} // namespace xudu

#endif // XUDU_LINK_PANEL_OVERLAY_HPP
