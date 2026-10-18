#include "common/xanadu/view/slice/xanadoc_view.hpp"
#include "common/xanadu/view/transductive_view.hpp"

namespace xanadu::view {

XanadocSliceView::XanadocSliceView(std::optional<zigzag::DimRef> dimContents,
                                   float defaultLineHeightPx)
    : dimContents_(dimContents), defaultLineHeightPx_(defaultLineHeightPx) {}

std::optional<glm::vec3>
XanadocSliceView::axisDirection(ViewAxisId axis) const noexcept {
  if (axis == 0) {
    return glm::vec3(0.0F, -1.0F, 0.0F); // Downward flow
  }
  return std::nullopt;
}

void XanadocSliceView::buildIndex(const std::vector<zigzag::CellRef> &cells,
                                  float lineHeightPx) {
  indexedCells_ = cells;
  std::vector<gleditor::enfilade::LayoutEntry> entries;
  entries.reserve(cells.size());

  for (std::size_t i = 0; i < cells.size(); ++i) {
    entries.push_back(gleditor::enfilade::LayoutEntry{
        .byteLength        = 80,
        .heightPx          = lineHeightPx,
        .widthPx           = 600.0F,
        .kind              = gleditor::enfilade::LayoutEntryKind::TextLine,
        .effectiveHeightPx = lineHeightPx,
    });
  }

  layoutfilade_ = gleditor::enfilade::Layoutfilade::buildFromEntries(entries);
}

std::expected<SliceView *, ViewError>
XanadocSliceView::prepare(ViewManifold &space, const SliceCursor &cursor,
                          const PaneFrame &frame, Measure measure) {
  std::ignore = frame;
  std::ignore = measure;

  if (!cursor.origin.has_value() || !dimContents_.has_value()) {
    return this;
  }

  // Walk d.contents rank from origin if not yet indexed or origin changed
  std::vector<zigzag::CellRef> cells;
  CycleBoundedRankIterator it(space, cursor.origin, dimContents_);
  while (it.hasNext()) {
    const auto c = it.next();
    if (c.has_value()) {
      cells.push_back(*c);
    }
  }

  if (!cells.empty()) {
    buildIndex(cells, defaultLineHeightPx_);
  }
  return this;
}

void XanadocSliceView::layout(const SliceLayoutInput &in,
                              LayoutSink &out) const noexcept {
  if (layoutfilade_.empty() || indexedCells_.empty()) {
    return;
  }

  const float viewportHeight =
      in.frame.heightPx > 0.0F ? in.frame.heightPx : 768.0F;
  const auto visRange =
      layoutfilade_.visibleRange(scrollY_, scrollY_ + viewportHeight);

  if (visRange.entryCount == 0) {
    return;
  }

  const auto first = visRange.firstEntryIndex;
  const auto last  = visRange.lastEntryIndex;

  for (std::size_t i = first; i <= last && i < indexedCells_.size(); ++i) {
    const auto hit = layoutfilade_.findEntryByIndex(i);
    if (!hit.has_value()) {
      continue;
    }

    const float itemY = -hit->startYPx + scrollY_;
    const glm::vec3 centre(0.0F, itemY, 0.0F);

    const auto cell = indexedCells_[i];
    PlacedItem item{
        .id = SubjectId{.kind = SubjectKind::Cell, .slot = 0, .value = cell},
        .centre  = centre,
        .width   = in.frame.widthPx > 0.0F ? in.frame.widthPx : 800.0F,
        .height  = hit->entry.heightPx,
        .opacity = 1.0F,
        .facing  = Facing::Plane,
        .content = ContentMode::Full,
        .flags =
            (cell == in.cursor.origin ? static_cast<std::uint32_t>(itemFocus)
                                      : 0U),
    };
    out.push(item);
  }
}

} // namespace xanadu::view
