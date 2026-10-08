#include "mocks/device.hpp"
#include <gleditor/curve_ribbons.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>

TEST(CurveRibbonsTest, BoundsUploadWorkAndPreservesPointerExclusion) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  gleditor::CurveRibbons ribbons(&device);
  ribbons.add({0, 0}, {0, 100}, {100, 100}, {}, 0xFFD700CC, 42, {100, 100}, 6);
  const auto &rows = ribbons.pending();
  ASSERT_GT(rows.size(), 1U);
  EXPECT_EQ(rows.front().interval[0], 0);
  EXPECT_EQ(rows.back().interval[1], 1);
  for (std::size_t i = 1; i < rows.size(); ++i)
    EXPECT_EQ(rows[i - 1].interval[1], rows[i].interval[0]);
  EXPECT_EQ(rows.front().widths[0], 4);
  EXPECT_EQ(rows.front().widths[1], 1);
  EXPECT_EQ(rows.back().hole, (std::array<float, 3>{100, 100, 6}));
  ribbons.clear();
  ribbons.add({0, 0}, {1e9F, 0}, {1e9F, 1e9F}, {}, 0, 0);
  EXPECT_LE(ribbons.pending().size(), 256U);
  ribbons.commit();
}
TEST(CurveRibbonsTest, RefusesNonFiniteGeometryAndInvalidStyle) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  gleditor::CurveRibbons ribbons(&device);
  EXPECT_THROW(ribbons.add({0, 0}, {1, 1},
                           {std::numeric_limits<float>::infinity(), 1}, {}, 0,
                           0),
               std::invalid_argument);
  auto style             = gleditor::CurveRibbons::Style{};
  style.maxSegmentLength = 0;
  EXPECT_THROW(ribbons.add({0, 0}, {1, 1}, {2, 2}, style, 0, 0),
               std::invalid_argument);
  EXPECT_TRUE(ribbons.pending().empty());
}

TEST(CurveRibbonsTest, SegmentJoinsShareNormalsAndDistanceBasedTaper) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  gleditor::CurveRibbons ribbons(&device);
  ribbons.add({0, 0}, {10, 200}, {300, 20}, {}, 0, 7);
  const auto &rows = ribbons.pending();
  ASSERT_GT(rows.size(), 2U);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i - 1].to, rows[i].from);
    EXPECT_EQ(rows[i - 1].normals[2], rows[i].normals[0]);
    EXPECT_EQ(rows[i - 1].normals[3], rows[i].normals[1]);
    EXPECT_EQ(rows[i - 1].interval[1], rows[i].interval[0]);
  }
  EXPECT_LT(rows.front().interval[1] - rows.front().interval[0],
            rows.back().interval[1] - rows.back().interval[0]);
}
