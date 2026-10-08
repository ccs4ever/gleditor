/**
 * @file ui_pane_tree_test.cpp
 * @brief Tests that a pane tree's rectangles always tile its bounds and that
 *        its focus order survives every change but the ones that add or
 *        remove a pane.
 */
#include <gtest/gtest.h>

#include <gleditor/ui/pane_tree.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace {

using namespace gleditor::ui;

std::map<PaneId, Rect> rectsOf(const PaneTree &tree, const Rect bounds) {
  std::map<PaneId, Rect> rects;
  std::vector<PaneId> visited;
  tree.rects(bounds, [&](const PaneId pane, const Rect rect) {
    rects[pane] = rect;
    visited.push_back(pane);
  });
  // Visited in focus order, each pane once.
  EXPECT_TRUE(std::ranges::equal(visited, tree.order()));
  return rects;
}

std::vector<Divider> dividersOf(const PaneTree &tree, const Rect bounds) {
  std::vector<Divider> dividers;
  tree.dividers(bounds,
                [&](const Divider &divider) { dividers.push_back(divider); });
  return dividers;
}

float area(const Rect rect) { return rect.width * rect.height; }

bool overlap(const Rect a, const Rect b) {
  return a.left < b.left + b.width && b.left < a.left + a.width &&
         a.bottom < b.bottom + b.height && b.bottom < a.bottom + a.height;
}

/// Whole-pixel rectangles inside @p bounds, disjoint, covering all of it.
void expectPartition(const std::map<PaneId, Rect> &rects, const Rect bounds) {
  float total{};
  for (const auto &[pane, rect] : rects) {
    EXPECT_EQ(rect.left, std::round(rect.left)) << pane;
    EXPECT_EQ(rect.bottom, std::round(rect.bottom)) << pane;
    EXPECT_EQ(rect.width, std::round(rect.width)) << pane;
    EXPECT_EQ(rect.height, std::round(rect.height)) << pane;
    EXPECT_GE(rect.left, bounds.left);
    EXPECT_GE(rect.bottom, bounds.bottom);
    EXPECT_LE(rect.left + rect.width, bounds.left + bounds.width);
    EXPECT_LE(rect.bottom + rect.height, bounds.bottom + bounds.height);
    total += area(rect);
    for (const auto &[other, otherRect] : rects) {
      if (other != pane) {
        EXPECT_FALSE(overlap(rect, otherRect)) << pane << " and " << other;
      }
    }
  }
  EXPECT_EQ(total, area(bounds));
}

constexpr Rect kBounds{0.0F, 0.0F, 1001.0F, 777.0F};

TEST(PaneTree, aNewTreeIsOnePaneFillingTheBounds) {
  const PaneTree tree;
  ASSERT_EQ(tree.order().size(), 1U);
  const auto rects = rectsOf(tree, kBounds);
  ASSERT_EQ(rects.size(), 1U);
  EXPECT_EQ(rects.begin()->second.width, kBounds.width);
  EXPECT_EQ(rects.begin()->second.height, kBounds.height);
  EXPECT_TRUE(dividersOf(tree, kBounds).empty());
}

TEST(PaneTree, horizontalPutsTheOldPaneLeftAndVerticalPutsItOnTop) {
  PaneTree tree;
  const PaneId first = tree.order().front();
  const auto right   = tree.split(first, Axis::Horizontal, 0.25F);
  ASSERT_TRUE(right.has_value());
  const auto below = tree.split(*right, Axis::Vertical);
  ASSERT_TRUE(below.has_value());

  const auto rects = rectsOf(tree, kBounds);
  EXPECT_LT(rects.at(first).left, rects.at(*right).left);
  EXPECT_EQ(rects.at(first).width, std::round(kBounds.width * 0.25F));
  EXPECT_GT(rects.at(*right).bottom, rects.at(*below).bottom);
  // Neighbours meet exactly.
  EXPECT_EQ(rects.at(first).left + rects.at(first).width,
            rects.at(*right).left);
  EXPECT_EQ(rects.at(*below).bottom + rects.at(*below).height,
            rects.at(*right).bottom);
}

TEST(PaneTree, splitsPartitionTheBoundsExactly) {
  PaneTree tree;
  const Rect offset{13.0F, 7.0F, 1001.0F, 777.0F};
  std::vector<PaneId> panes{tree.order().front()};
  // Thirds and sevenths, so no edge falls on a whole pixel before rounding.
  for (int round = 0; round < 12; ++round) {
    const auto axis   = round % 2 == 0 ? Axis::Horizontal : Axis::Vertical;
    const float share = round % 3 == 0 ? 1.0F / 3.0F : 4.0F / 7.0F;
    const auto added  = tree.split(
        panes[static_cast<std::size_t>(round) % panes.size()], axis, share);
    ASSERT_TRUE(added.has_value());
    panes.push_back(*added);
    expectPartition(rectsOf(tree, offset), offset);
  }
  EXPECT_EQ(tree.order().size(), panes.size());
}

TEST(PaneTree, closingGivesTheRoomToTheSibling) {
  PaneTree tree;
  const PaneId left = tree.order().front();
  const auto right  = tree.split(left, Axis::Horizontal);
  ASSERT_TRUE(right.has_value());
  const auto lower = tree.split(*right, Axis::Vertical);
  ASSERT_TRUE(lower.has_value());
  const auto before = rectsOf(tree, kBounds);

  // The sibling of the left pane is the whole right column, a subtree.
  ASSERT_TRUE(tree.close(left).has_value());
  const auto after = rectsOf(tree, kBounds);
  ASSERT_EQ(after.size(), 2U);
  EXPECT_EQ(after.at(*right).left, kBounds.left);
  EXPECT_EQ(after.at(*right).width, kBounds.width);
  EXPECT_EQ(after.at(*right).height, before.at(*right).height);
  expectPartition(after, kBounds);

  ASSERT_TRUE(tree.close(*lower).has_value());
  const auto last = rectsOf(tree, kBounds);
  EXPECT_EQ(last.at(*right).height, kBounds.height);
}

TEST(PaneTree, refusesWhatItCannotDo) {
  PaneTree tree;
  const PaneId only = tree.order().front();
  EXPECT_EQ(tree.close(only).error(), PaneError::LastPane);
  EXPECT_EQ(tree.resize(only, 0.3F).error(), PaneError::NoDivider);
  EXPECT_EQ(tree.split(only, Axis::Vertical, std::nanf("")).error(),
            PaneError::InvalidShare);
  EXPECT_EQ(tree.split(only + 99, Axis::Vertical).error(),
            PaneError::UnknownPane);
  EXPECT_EQ(tree.resizeDivider(0, 0.5F).error(), PaneError::UnknownDivider);

  const auto added = tree.split(only, Axis::Vertical);
  ASSERT_TRUE(added.has_value());
  ASSERT_TRUE(tree.close(*added).has_value());
  // A closed pane is gone for good, and its id is not handed out again.
  EXPECT_FALSE(tree.contains(*added));
  EXPECT_EQ(tree.close(*added).error(), PaneError::UnknownPane);
  const auto again = tree.split(only, Axis::Vertical);
  ASSERT_TRUE(again.has_value());
  EXPECT_NE(*again, *added);
  EXPECT_EQ(tree.resizeDivider(0, 0.5F).error(), PaneError::UnknownDivider);
}

TEST(PaneTree, orderIsReadingOrderAndStableUnderResize) {
  PaneTree tree;
  const PaneId a = tree.order().front();
  const auto c   = tree.split(a, Axis::Horizontal);
  const auto b   = tree.split(a, Axis::Vertical);
  const auto d   = tree.split(*c, Axis::Vertical);
  ASSERT_TRUE(b && c && d);
  // A over B on the left, C over D on the right.
  const std::vector<PaneId> expected{a, *b, *c, *d};
  EXPECT_TRUE(std::ranges::equal(tree.order(), expected));

  for (const float share : {0.1F, 0.9F, 0.5F, 0.0F, 1.0F}) {
    ASSERT_TRUE(tree.resize(*d, share).has_value());
    ASSERT_TRUE(tree.resize(a, 1.0F - share).has_value());
    EXPECT_TRUE(std::ranges::equal(tree.order(), expected));
  }

  ASSERT_TRUE(tree.close(*b).has_value());
  const std::vector<PaneId> closed{a, *c, *d};
  EXPECT_TRUE(std::ranges::equal(tree.order(), closed));
}

TEST(PaneTree, resizeGivesThePaneItsShareFromEitherSide) {
  PaneTree tree;
  const PaneId left = tree.order().front();
  const auto right  = tree.split(left, Axis::Horizontal);
  ASSERT_TRUE(right.has_value());

  ASSERT_TRUE(tree.resize(*right, 0.2F).has_value());
  auto rects = rectsOf(tree, kBounds);
  EXPECT_EQ(rects.at(*right).width, std::round(kBounds.width * 0.2F));

  ASSERT_TRUE(tree.resize(left, 0.2F).has_value());
  rects = rectsOf(tree, kBounds);
  EXPECT_EQ(rects.at(left).width, std::round(kBounds.width * 0.2F));

  // Shares are clamped, and setters chain.
  ASSERT_TRUE(tree.resize(left, 7.0F).and_then([&](PaneTree *self) {
    return self->resize(left, 7.0F);
  }));
  rects = rectsOf(tree, kBounds);
  EXPECT_EQ(rects.at(left).width, kBounds.width);
  EXPECT_EQ(rects.at(*right).width, 0.0F);
}

TEST(PaneTree, aDividerBetweenTwoSubtreesIsReachedByItsId) {
  // Two columns, each split again: the middle divider is next to no pane.
  PaneTree tree;
  const PaneId a = tree.order().front();
  const auto c   = tree.split(a, Axis::Horizontal);
  const auto b   = tree.split(a, Axis::Horizontal);
  const auto d   = tree.split(*c, Axis::Horizontal);
  ASSERT_TRUE(b && c && d);

  const auto dividers = dividersOf(tree, kBounds);
  ASSERT_EQ(dividers.size(), 3U);
  const auto &middle = dividers.front(); // outer before inner
  EXPECT_EQ(middle.axis, Axis::Horizontal);
  EXPECT_EQ(middle.span.width, kBounds.width);
  EXPECT_EQ(middle.edge.width, 0.0F);
  EXPECT_EQ(middle.edge.left, rectsOf(tree, kBounds).at(*c).left);

  ASSERT_TRUE(tree.resizeDivider(middle.id, 0.8F).has_value());
  const auto rects = rectsOf(tree, kBounds);
  EXPECT_EQ(rects.at(*c).left, std::round(kBounds.width * 0.8F));
  expectPartition(rects, kBounds);
}

TEST(PaneTree, aVerticalDividersEdgeIsWhereTheTopPaneEnds) {
  PaneTree tree;
  const PaneId top = tree.order().front();
  ASSERT_TRUE(tree.split(top, Axis::Vertical, 0.3F).has_value());
  const auto dividers = dividersOf(tree, kBounds);
  ASSERT_EQ(dividers.size(), 1U);
  EXPECT_EQ(dividers[0].edge.height, 0.0F);
  EXPECT_EQ(dividers[0].edge.bottom, rectsOf(tree, kBounds).at(top).bottom);
}

} // namespace
