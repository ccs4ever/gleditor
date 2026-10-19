#include <chrono>
#include <gtest/gtest.h>
#include <vector>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/view/slice/xanadoc_view.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::view {
namespace {

TEST(XanadocViewTest,
     VirtualizedScrollOverFiftyThousandLinesUnderOneMillisecond) {
  constexpr std::size_t kTotalLines = 50000;
  constexpr float kLineHeight       = 20.0F;

  zigzag::ArenaManifold base;
  std::vector<zigzag::CellRef> cells;
  cells.reserve(kTotalLines);
  for (std::size_t i = 0; i < kTotalLines; ++i) {
    cells.push_back(static_cast<zigzag::CellRef>(1000 + i));
  }

  ViewManifold viewManifold(base);
  XanadocSliceView view;
  view.buildIndex(cells, kLineHeight);

  // Total height must be exactly 50,000 * 20 = 1,000,000 px
  EXPECT_FLOAT_EQ(view.totalHeightPx(), 1000000.0F);

  // Viewport of 800x600 px scrolled down to y = 500,000 px (middle of 1M px
  // doc)
  view.setScrollY(500000.0F);

  SliceCursor cursor{.origin = cells.front()};
  auto measure = [](SubjectId, float) -> ContentExtent {
    return ContentExtent{.width = 800.0F, .height = 20.0F};
  };

  SliceLayoutInput input{
      .space   = viewManifold,
      .cursor  = cursor,
      .frame   = PaneFrame{.widthPx = 800.0F, .heightPx = 600.0F},
      .measure = measure,
  };

  LayoutSink sink;

  // Measure frame layout time
  const auto t0 = std::chrono::steady_clock::now();
  view.layout(input, sink);
  const auto t1 = std::chrono::steady_clock::now();

  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // Frame time must be <= 1 ms (1000 us)
  EXPECT_LT(elapsedUs, 1000);

  // In a 600 px high viewport with 20 px lines, at most ~31-35 items are
  // visible and placed!
  EXPECT_GE(sink.items().size(), 30U);
  EXPECT_LE(sink.items().size(), 40U);

  // Verify that placed items correspond to the scrolled region (~ line 25,000)
  const auto firstPlaced = sink.items().front();
  EXPECT_NEAR(firstPlaced.id.value, 1000 + 25000, 5);
}

} // namespace
} // namespace xanadu::view
