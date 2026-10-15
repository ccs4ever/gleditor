/**
 * @file occupied_regions_test.cpp
 * @brief The rectangles a slice publishes and the one rule that places the
 *        overview thumbnail and the link panel clear of them (M2 third gate,
 *        F2 to F4).
 */
#include <gtest/gtest.h>

#include <vector>

#include "common/ui/occupied_regions.hpp"
#include "common/ui/xanadoc/overview_overlay.hpp"

namespace {

using gleditor::ui::Rect;

const auto kNoSoft = [](const Rect &) { return 0.0F; };

TEST(OccupiedRegionsTest, APublisherChangesTheRevisionOnlyWhenItsRectsChange) {
  xanadu::OccupiedRegions regions;
  const int owner{};
  EXPECT_TRUE(regions.empty());
  const auto first = regions.revision();
  const std::vector<Rect> cells{{10, 10, 50, 20}, {70, 10, 50, 20}};
  regions.publish(&owner, cells);
  const auto second = regions.revision();
  EXPECT_GT(second, first);
  regions.publish(&owner, cells);
  EXPECT_EQ(regions.revision(), second) << "the same frame again is no change";

  std::vector<Rect> seen;
  regions.forEach([&](const Rect &rect) { seen.push_back(rect); });
  EXPECT_EQ(seen.size(), 2U);

  regions.publish(&owner, {});
  EXPECT_TRUE(regions.empty());
  EXPECT_GT(regions.revision(), second);
}

TEST(OccupiedRegionsTest, WithNothingToAvoidAPanelStaysWhereItWasPut) {
  const Rect safe{0, 0, 800, 600};
  const Rect wanted{16, 56, 220, 160};
  EXPECT_EQ(xanadu::placeClear(wanted, safe, 16.0F, {}, kNoSoft).left,
            wanted.left);
  const std::vector<Rect> far{{500, 400, 100, 50}};
  const auto kept = xanadu::placeClear(wanted, safe, 16.0F, far, kNoSoft);
  EXPECT_EQ(kept.left, wanted.left);
  EXPECT_EQ(kept.bottom, wanted.bottom);
}

TEST(OccupiedRegionsTest, APanelOverCellsMovesToAPlaceThatCoversNone) {
  const Rect safe{0, 0, 800, 600};
  const Rect wanted{16, 56, 220, 160};
  // Cells where the thumbnail was put, as slice_view_stepped had them, near the
  // thumbnail.
  const std::vector<Rect> cells{{30, 100, 220, 30}, {30, 140, 220, 30}};
  ASSERT_GT(xanadu::overlapArea(wanted, cells[0]) +
                xanadu::overlapArea(wanted, cells[1]),
            0.0F)
      << "the case is one that overlaps";
  const auto moved = xanadu::placeClear(wanted, safe, 16.0F, cells, kNoSoft);
  for (const auto &cell : cells) {
    EXPECT_EQ(xanadu::overlapArea(moved, cell), 0.0F);
  }
  EXPECT_GE(moved.left, safe.left);
  EXPECT_LE(moved.left + moved.width, safe.left + safe.width);
  EXPECT_EQ(moved.width, wanted.width);
}

TEST(OccupiedRegionsTest, PageTextBreaksATieBetweenPlacesThatCoverNoCell) {
  const Rect safe{0, 0, 800, 600};
  const Rect wanted{16, 56, 220, 160};
  const std::vector<Rect> cells{{20, 60, 200, 100}};
  // The left half is page; the right half is empty.
  const Rect page{0, 0, 400, 600};
  const auto onPage = [&](const Rect &at) {
    return xanadu::overlapArea(at, page);
  };
  const auto moved = xanadu::placeClear(wanted, safe, 16.0F, cells, onPage);
  EXPECT_EQ(xanadu::overlapArea(moved, cells[0]), 0.0F);
  EXPECT_EQ(xanadu::overlapArea(moved, page), 0.0F)
      << "somewhere in the empty half, clear of text";
}

TEST(OccupiedRegionsTest, TheOverviewThumbnailStandsClearOfPublishedCells) {
  const xanadu::OverviewConfig config;
  const gleditor::ui::UiMetrics metrics{
      .contentScale = 1.0F, .screenWidth = 800, .screenHeight = 600};
  const auto home = xanadu::OverviewOverlay::panelBounds(config, metrics);
  const std::vector<Rect> cells{
      {home.left + 10, home.bottom + 20, home.width - 20, 30}};
  const auto placed =
      xanadu::OverviewOverlay::panelClear(config, metrics, cells, {});
  EXPECT_EQ(xanadu::overlapArea(placed, cells[0]), 0.0F);
  EXPECT_EQ(xanadu::OverviewOverlay::panelClear(config, metrics, {}, {}).left,
            home.left)
      << "with no slice the thumbnail is where it always was";
}

} // namespace
