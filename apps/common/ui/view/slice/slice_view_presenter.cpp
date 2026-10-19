#include "common/ui/view/slice/slice_view_presenter.hpp"
#include <gleditor/color.hpp>
#include <gleditor/render_state.hpp>

namespace xanadu::ui {

SliceViewPresenter::SliceViewPresenter(std::shared_ptr<view::SliceView> view,
                                       std::string fontName)
    : view_(std::move(view)), fontName_(std::move(fontName)) {}

void SliceViewPresenter::updateLayout(const view::SliceLayoutInput &input) {
  sink_.clear();
  if (view_) {
    view_->layout(input, sink_);
  }
  ++revision_;
}

void SliceViewPresenter::deviceReady(
    render::RenderDevice &device,
    const render::PipelineDesc &documentPipeline) {
  canvas_ = std::make_unique<gleditor::Canvas>(&device, fontName_);
  canvas_->createPipeline(documentPipeline, false);
}

void SliceViewPresenter::drawFrame(gleditor::FrameContext &ctx) {
  if (!canvas_) {
    return;
  }

  canvas_->clear();

  // 1. Draw frames (pane and tag containers)
  for (const auto &frame : sink_.frames()) {
    const float left   = frame.centre.x - (frame.width * 0.5F);
    const float bottom = frame.centre.y - (frame.height * 0.5F);
    // Dark translucent background for pane frames
    const std::uint32_t bgCol = 0x1A1A24EEU;
    canvas_->addRect(left, bottom, frame.width, frame.height, bgCol);

    // Frame border
    const std::uint32_t borderCol = 0x444455FFU;
    canvas_->addLine(left, bottom, left + frame.width, bottom, 1.0F, borderCol);
    canvas_->addLine(left, bottom + frame.height, left + frame.width,
                     bottom + frame.height, 1.0F, borderCol);
    canvas_->addLine(left, bottom, left, bottom + frame.height, 1.0F,
                     borderCol);
    canvas_->addLine(left + frame.width, bottom, left + frame.width,
                     bottom + frame.height, 1.0F, borderCol);
  }

  // 2. Draw items (cells, badges, tags)
  for (const auto &item : sink_.items()) {
    const float left   = item.centre.x - (item.width * 0.5F);
    const float bottom = item.centre.y - (item.height * 0.5F);

    std::uint32_t cellCol = 0x2A2A38FFU;
    if ((item.flags & view::ItemFlags::itemTinted) != 0U) {
      cellCol = 0x3A3A4EFFU;
    }
    if ((item.flags & view::ItemFlags::itemFocus) != 0U ||
        (selected_.has_value() && selected_->value == item.id.value)) {
      cellCol = 0x6A5ACDFFU; // SlateBlue focus
    }

    canvas_->setTag(render::tagKindOverlay,
                    static_cast<std::uint32_t>(item.id.value));
    canvas_->addRect(left, bottom, item.width, item.height, cellCol);
  }

  // 3. Draw edges (dimensions, strands, links)
  const std::uint32_t edgeCol = 0x8888AA88U;
  for (const auto &edge : sink_.edges()) {
    canvas_->addLine(edge.a.x, edge.a.y, edge.b.x, edge.b.y, 2.0F, edgeCol);
  }

  canvas_->commit();
  canvas_->draw(ctx.state, ctx.viewProjection);
}

bool SliceViewPresenter::picked(const render::PickingResult &pick,
                                RenderState &) {
  if (pick.tag.kind != render::tagKindOverlay) {
    return false;
  }

  for (const auto &item : sink_.items()) {
    if (static_cast<std::uint32_t>(item.id.value) == pick.tag.clusterIndex) {
      selected_ = item.id;
      ++revision_;
      return true;
    }
  }

  return false;
}

void SliceViewPresenter::describe(gleditor::a11y::Builder &into) {
  for (const auto &item : sink_.items()) {
    auto &node = into.add(item.id.value, gleditor::a11y::Role::Group);
    node.label = "Cell " + std::to_string(item.id.value);
    into.contribute(node.id);
    if (selected_.has_value() && selected_->value == item.id.value) {
      into.takeFocus(node.id);
    }
  }
}

bool SliceViewPresenter::performAction(std::uint64_t nodeId,
                                       gleditor::a11y::Action action,
                                       std::string_view) {
  if (action == gleditor::a11y::Action::Focus ||
      action == gleditor::a11y::Action::Click) {
    for (const auto &item : sink_.items()) {
      if (item.id.value == nodeId) {
        selected_ = item.id;
        ++revision_;
        return true;
      }
    }
  }
  return false;
}

} // namespace xanadu::ui
