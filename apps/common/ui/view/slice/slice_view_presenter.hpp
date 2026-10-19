/**
 * @file slice_view_presenter.hpp
 * @brief Hardware bridging presenter connecting SliceView layout sinks to
 * gleditor Canvas and GPU pipelines.
 *
 * Implements Milestone M3 Package V4:
 * - FrameContributor rendering layout items/edges to the screen.
 * - PickObserver mapping mouse picks back to SubjectId and CellRef.
 * - AccessKit Source building accessible tree descriptions from the view.
 */
#ifndef XANADU_UI_VIEW_SLICE_VIEW_PRESENTER_HPP
#define XANADU_UI_VIEW_SLICE_VIEW_PRESENTER_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>

#include "common/xanadu/view/slice_view.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace xanadu::ui {

/**
 * @class SliceViewPresenter
 * @brief Hardware bridging presenter mapping View LayoutSink to libgleditor
 * Canvas and PickObserver.
 *
 * Implements Milestone M3 Package V4:
 * - Implements FrameContributor, PickObserver, and AccessKit Source.
 * - Bridges LayoutSink quads, frames, and edges to GPU draw commands.
 * - Maps picking clusters back to View SubjectIds.
 */
class SliceViewPresenter : public gleditor::FrameContributor,
                           public gleditor::PickObserver,
                           public gleditor::a11y::Source {
public:
  explicit SliceViewPresenter(std::shared_ptr<view::SliceView> view,
                              std::string fontName = "Sans 12");
  ~SliceViewPresenter() override = default;

  void setView(std::shared_ptr<view::SliceView> view) noexcept {
    view_ = std::move(view);
    ++revision_;
  }
  [[nodiscard]] const view::SliceView *view() const noexcept {
    return view_.get();
  }

  void updateLayout(const view::SliceLayoutInput &input);

  // gleditor::FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override { return false; }

  // gleditor::PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // gleditor::a11y::Source
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const noexcept override {
    return revision_;
  }
  bool performAction(std::uint64_t nodeId, gleditor::a11y::Action action,
                     std::string_view value) override;

  [[nodiscard]] const view::LayoutSink &sink() const noexcept { return sink_; }
  [[nodiscard]] std::optional<view::SubjectId> selected() const noexcept {
    return selected_;
  }
  void setSelected(std::optional<view::SubjectId> sel) noexcept {
    selected_ = sel;
  }

  [[nodiscard]] bool isReady() const noexcept { return canvas_ != nullptr; }

protected:
  std::shared_ptr<view::SliceView> view_;
  std::string fontName_;
  view::LayoutSink sink_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  std::optional<view::SubjectId> selected_{std::nullopt};
  std::uint64_t revision_{1};
};

} // namespace xanadu::ui

#endif // XANADU_UI_VIEW_SLICE_VIEW_PRESENTER_HPP
