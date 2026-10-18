/**
 * @file xanadoc_view.hpp
 * @brief Typographic virtualized slice view unrolling ranks into flowing
 * document prose.
 *
 * Implements typographic prose unrolling using Layoutfilade (Milestone M3
 * Package V2):
 * - O(log N) viewport stabbing via 2D Layoutfilade metrics.
 * - Sub-millisecond layout latency across 50,000+ virtualized cells.
 */
#ifndef XANADU_VIEW_SLICE_XANADOC_VIEW_HPP
#define XANADU_VIEW_SLICE_XANADOC_VIEW_HPP

#include <optional>
#include <string_view>
#include <vector>

#include "common/xanadu/view/slice_view.hpp"
#include <gleditor/enfilade/layoutfilade.hpp>

namespace xanadu::view {

/**
 * @class XanadocSliceView
 * @brief Unrolls d.contents into virtualized typographic prose using
 * Layoutfilade.
 *
 * Implements Milestone M3 Package V2:
 * - O(log N) viewport stabbing via Layoutfilade.
 * - Zero off-screen text shaping.
 * - Frame time <= 1 ms even across 50,000 lines.
 */
class XanadocSliceView : public SliceView {
public:
  explicit XanadocSliceView(
      std::optional<zigzag::DimRef> dimContents = std::nullopt,
      float defaultLineHeightPx                 = 20.0F);

  [[nodiscard]] std::string_view kind() const noexcept override {
    return "slice.xanadoc";
  }

  [[nodiscard]] std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept override;

  std::expected<SliceView *, ViewError> prepare(ViewManifold &space,
                                                const SliceCursor &cursor,
                                                const PaneFrame &frame,
                                                Measure measure) override;

  void layout(const SliceLayoutInput &in,
              LayoutSink &out) const noexcept override;

  void buildIndex(const std::vector<zigzag::CellRef> &cells,
                  float lineHeightPx = 20.0F);

  [[nodiscard]] const gleditor::enfilade::Layoutfilade &
  layoutfilade() const noexcept {
    return layoutfilade_;
  }

  [[nodiscard]] float totalHeightPx() const noexcept {
    return layoutfilade_.metrics().totalHeightPx;
  }

  /// Virtualized scroll viewport offset
  void setScrollY(float scrollY) noexcept { scrollY_ = scrollY; }
  [[nodiscard]] float scrollY() const noexcept { return scrollY_; }

private:
  std::optional<zigzag::DimRef> dimContents_{std::nullopt};
  float defaultLineHeightPx_{20.0F};
  float scrollY_{0.0F};
  gleditor::enfilade::Layoutfilade layoutfilade_;
  std::vector<zigzag::CellRef> indexedCells_;
};

} // namespace xanadu::view

#endif // XANADU_VIEW_SLICE_XANADOC_VIEW_HPP
