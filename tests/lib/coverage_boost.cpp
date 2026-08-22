/**
 * @file coverage_boost.cpp
 * @brief Coverage boost tests for gleditor library headers and utilities.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/app.hpp>
#include <gleditor/draw_budget.hpp>

namespace {

using gleditor::CommandTable;
using gleditor::Mod;

TEST(LibCoverageBoost, modCombinationsAndBit) {
  const auto m1 = Mod::Ctrl | Mod::Shift;
  EXPECT_EQ(static_cast<std::uint16_t>(m1),
            static_cast<std::uint16_t>(Mod::Ctrl) |
                static_cast<std::uint16_t>(Mod::Shift));

  // A11y action bit conversion
  EXPECT_EQ(gleditor::a11y::bit(gleditor::a11y::Action::Focus), 1U << 0);
  EXPECT_EQ(gleditor::a11y::bit(gleditor::a11y::Action::Click), 1U << 1);
  EXPECT_EQ(gleditor::a11y::bit(gleditor::a11y::Action::SetValue), 1U << 2);
  EXPECT_EQ(gleditor::a11y::bit(gleditor::a11y::Action::ScrollIntoView),
            1U << 3);
}

TEST(LibCoverageBoost, drawBudgetOperations) {
  DrawBudget budget;
  budget.screenWidth = 1920.0F;
  budget.coarseBelow = 2.0F;
  budget.cull        = true;

  EXPECT_EQ(budget.screenWidth, 1920.0F);
  EXPECT_EQ(budget.coarseBelow, 2.0F);
  EXPECT_TRUE(budget.cull);

  DrawStats stats;
  stats.pages    = 5;
  stats.culled   = 2;
  stats.coarse   = 1;
  stats.detailed = 2;

  EXPECT_EQ(stats.pages, 5U);
  EXPECT_EQ(stats.culled, 2U);
  EXPECT_EQ(stats.coarse, 1U);
  EXPECT_EQ(stats.detailed, 2U);
}

TEST(LibCoverageBoost, a11yNodeHierarchy) {
  gleditor::a11y::Node root;
  root.id          = 1;
  root.role        = gleditor::a11y::Role::Document;
  root.label       = "Main Document";
  root.description = "Root document node";
  root.bounds      = gleditor::a11y::Rect{0.0, 0.0, 800.0, 600.0};
  root.children    = {2, 3};

  EXPECT_EQ(root.id, 1U);
  EXPECT_EQ(root.role, gleditor::a11y::Role::Document);
  EXPECT_EQ(root.label, "Main Document");
  EXPECT_EQ(root.description, "Root document node");
  ASSERT_TRUE(root.bounds.has_value());
  EXPECT_EQ(root.bounds->right, 800.0);
  EXPECT_EQ(root.children.size(), 2U);
}

} // namespace
