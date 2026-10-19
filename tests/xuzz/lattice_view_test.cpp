#include <gtest/gtest.h>
#include <map>
#include <vector>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/view/slice/lattice_view.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::view {
namespace {

TEST(LatticeViewTest, DeterministicCoordinatesThreeByThreeByThreeCluster) {
  zigzag::ArenaManifold base;
  const auto dimX = base.ensureDimension("d.1");
  const auto dimY = base.ensureDimension("d.2");
  const auto dimZ = base.ensureDimension("d.3");

  // Create 3x3x3 = 27 cells in base
  // Mapping (x, y, z) in [-1, 1]^3 to cell
  std::map<std::tuple<int, int, int>, zigzag::CellRef> grid;
  for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        grid[{x, y, z}] = base.makeCell();
      }
    }
  }

  // Link along dimX, dimY, dimZ
  for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const auto c = grid[{x, y, z}];
        if (x < 1) {
          zigzag::expectWritten(
              base.link(c, dimX, zigzag::DimVector::POS, grid[{x + 1, y, z}]));
        }
        if (y < 1) {
          zigzag::expectWritten(
              base.link(c, dimY, zigzag::DimVector::POS, grid[{x, y + 1, z}]));
        }
        if (z < 1) {
          zigzag::expectWritten(
              base.link(c, dimZ, zigzag::DimVector::POS, grid[{x, y, z + 1}]));
        }
      }
    }
  }

  ViewManifold viewManifold(base);
  LatticeSliceView view(dimX, dimY, dimZ, 100.0F, 50.0F, 20.0F, 10.0F, 50.0F);
  view.setRadius(1);

  EXPECT_EQ(view.kind(), "slice.lattice");
  EXPECT_EQ(view.axisDirection(0), glm::vec3(1.0F, 0.0F, 0.0F));
  EXPECT_EQ(view.axisDirection(1), glm::vec3(0.0F, 1.0F, 0.0F));
  EXPECT_EQ(view.axisDirection(2), glm::vec3(0.0F, 0.0F, 1.0F));

  const auto originCell = grid[{0, 0, 0}];
  SliceCursor cursor{.origin = originCell};
  auto measure = [](SubjectId, float) -> ContentExtent {
    return ContentExtent{.width = 100.0F, .height = 50.0F};
  };

  SliceLayoutInput input{
      .space   = viewManifold,
      .cursor  = cursor,
      .frame   = PaneFrame{},
      .measure = measure,
  };

  LayoutSink sink;
  view.layout(input, sink);

  // Exactly 27 items placed!
  EXPECT_EQ(sink.items().size(), 27U);

  // Check origin cell position and focus flag
  bool originFound = false;
  for (const auto &item : sink.items()) {
    if (item.id.value == originCell) {
      originFound = true;
      EXPECT_EQ(item.centre, glm::vec3(0.0F, 0.0F, 0.0F));
      EXPECT_TRUE((item.flags & itemFocus) != 0);
    }
  }
  EXPECT_TRUE(originFound);

  // Verify corner cell coordinates: (1, 1, 1) -> x = 1 * (100 + 20) = 120, y =
  // 1 * (50 + 10) = 60, z = 1 * (100 + 50) = 150
  const auto cornerCell = grid[{1, 1, 1}];
  bool cornerFound      = false;
  for (const auto &item : sink.items()) {
    if (item.id.value == cornerCell) {
      cornerFound = true;
      EXPECT_FLOAT_EQ(item.centre.x, 120.0F);
      EXPECT_FLOAT_EQ(item.centre.y, 60.0F);
      EXPECT_FLOAT_EQ(item.centre.z, 150.0F);
    }
  }
  EXPECT_TRUE(cornerFound);

  // Check edges placed: Each 1D line of 3 cells has 2 edges. In 3x3x3 grid:
  // Along X: 3 * 3 * 2 = 18 edges
  // Along Y: 3 * 3 * 2 = 18 edges
  // Along Z: 3 * 3 * 2 = 18 edges
  // Total edges = 54
  EXPECT_EQ(sink.edges().size(), 54U);
}

} // namespace
} // namespace xanadu::view
