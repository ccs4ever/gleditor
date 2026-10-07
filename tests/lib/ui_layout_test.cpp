#include <gtest/gtest.h>

#include <gleditor/ui/layout.hpp>
#include <gleditor/ui/metrics.hpp>
#include <gleditor/ui/theme.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace gleditor::ui;

void expectContained(Rect child, Rect parent) {
  EXPECT_TRUE(std::isfinite(child.left));
  EXPECT_TRUE(std::isfinite(child.bottom));
  EXPECT_GE(child.width, 0.0F);
  EXPECT_GE(child.height, 0.0F);
  EXPECT_GE(child.left, parent.left);
  EXPECT_GE(child.bottom, parent.bottom);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + 0.001F);
  EXPECT_LE(child.bottom + child.height,
            parent.bottom + parent.height + 0.001F);
}
void expectLayoutContained(const LayoutResult &result) {
  for (const auto &box : result.boxes) {
    expectContained(box.rect, result.bounds);
    expectContained(box.contentRect, box.rect);
  }
}

TEST(UiMetricsTest, ScalesLengthsAndFontsIndependently) {
  UiMetrics metrics{
      .contentScale = 1.25F, .userScale = 2.0F, .fontScale = 1.5F};
  Theme theme;
  EXPECT_FLOAT_EQ(metrics.px(16.0F), 40.0F);
  EXPECT_FLOAT_EQ(metrics.logical(40.0F), 16.0F);
  EXPECT_FLOAT_EQ(metrics.fontPixels(FontRole::Body, theme), 60.0F);
  EXPECT_EQ(metrics.fontDescription(FontRole::Body, theme), "Sans 45");
  theme.fonts[static_cast<std::size_t>(FontRole::Body)].points = 0.25F;
  EXPECT_FLOAT_EQ(metrics.fontPixels(FontRole::Body, theme),
                  theme.type.minFontPx);
}
TEST(UiMetricsTest, SafeAreaCombinesChromeAndSymmetricMargin) {
  UiMetrics metrics{
      .contentScale = 2.0F,
      .screenWidth  = 640,
      .screenHeight = 480,
      .chrome = {.top = 30.0F, .bottom = 10.0F, .left = 20.0F, .right = 40.0F},
      .marginShare = 0.05F};
  const auto pixels = metrics.pixelSafeArea();
  EXPECT_FLOAT_EQ(pixels.left, 44.0F);
  EXPECT_FLOAT_EQ(pixels.bottom, 34.0F);
  EXPECT_FLOAT_EQ(pixels.width, 532.0F);
  EXPECT_FLOAT_EQ(pixels.height, 392.0F);
  const auto logical = metrics.safeArea();
  EXPECT_FLOAT_EQ(logical.left, 22.0F);
  EXPECT_FLOAT_EQ(logical.width, 266.0F);
}
TEST(UiMetricsTest, ImpossibleChromeStillProducesContainedEmptyArea) {
  const UiMetrics metrics{
      .screenWidth  = 100,
      .screenHeight = 50,
      .chrome       = {
                .top = 200.0F, .bottom = 200.0F, .left = 500.0F, .right = 500.0F}};
  const auto area = metrics.pixelSafeArea();
  expectContained(area, {0.0F, 0.0F, 100.0F, 50.0F});
  EXPECT_FLOAT_EQ(area.width, 0.0F);
  EXPECT_FLOAT_EQ(area.height, 0.0F);
}
TEST(UiMetricsTest, InvalidScaleAndInsetsDoNotCreateInvalidGeometry) {
  const auto nan      = std::numeric_limits<float>::quiet_NaN();
  const auto infinity = std::numeric_limits<float>::infinity();
  const UiMetrics metrics{.contentScale = nan,
                          .userScale    = -2.0F,
                          .fontScale    = infinity,
                          .screenWidth  = -10,
                          .screenHeight = 80,
                          .chrome       = {.top = nan, .left = infinity},
                          .marginShare  = nan};
  EXPECT_FLOAT_EQ(metrics.scale(), 1.0F);
  EXPECT_TRUE(std::isfinite(metrics.fontPixels(FontRole::Label, Theme{})));
  expectContained(metrics.pixelSafeArea(), {0.0F, 0.0F, 0.0F, 80.0F});
}
TEST(UiMetricsTest, RoundedSharedEdgesStayAdjacentAtAllScales) {
  for (float scale : {1.0F, 1.25F, 2.0F}) {
    const UiMetrics metrics{.contentScale = scale};
    const auto left  = metrics.rounded({metrics.px(0.0F), metrics.px(2.0F),
                                        metrics.px(10.5F), metrics.px(7.0F)});
    const auto right = metrics.rounded({metrics.px(10.5F), metrics.px(2.0F),
                                        metrics.px(20.5F), metrics.px(7.0F)});
    EXPECT_FLOAT_EQ(left.left + left.width, right.left);
    EXPECT_FLOAT_EQ(right.left + right.width, std::round(metrics.px(31.0F)));
    EXPECT_FLOAT_EQ(left.bottom, right.bottom);
  }
}
TEST(UiMetricsTest, PlacementFlipsAboveAnchorAndClampsLargePanel) {
  const Rect safe{10.0F, 20.0F, 100.0F, 100.0F};
  const auto above =
      placeNear({30.0F, 22.0F, 10.0F, 10.0F}, 50.0F, 40.0F, safe, 5.0F);
  EXPECT_FLOAT_EQ(above.bottom, 37.0F);
  expectContained(above, safe);
  const auto large =
      placeNear({120.0F, -10.0F, 10.0F, 10.0F}, 300.0F, 200.0F, safe);
  EXPECT_FLOAT_EQ(large.left, safe.left);
  EXPECT_FLOAT_EQ(large.width, safe.width);
  expectContained(large, safe);
}
TEST(UiLayoutTest, FlowWrapsWholeItemsInReadingOrder) {
  const LayoutItem items[]{
      {.id = 1, .intrinsic = {60.0F, 20.0F}, .focusable = true},
      {.id = 2, .intrinsic = {60.0F, 30.0F}, .focusable = true},
      {.id = 3, .intrinsic = {20.0F, 10.0F}, .focusable = true}};
  const auto result =
      flow({0.0F, 0.0F, 100.0F, 100.0F}, items, {.gap = 5.0F, .lineGap = 5.0F});
  ASSERT_EQ(result.boxes.size(), 3U);
  EXPECT_FLOAT_EQ(result.find(1)->rect.bottom, 80.0F);
  EXPECT_FLOAT_EQ(result.find(2)->rect.bottom, 45.0F);
  EXPECT_FLOAT_EQ(result.find(3)->rect.left, 65.0F);
  EXPECT_EQ(result.focusOrder, (std::vector<std::uint32_t>{1, 2, 3}));
  expectLayoutContained(result);
}
TEST(UiLayoutTest, WeightedGrowthRedistributesSaturatedShare) {
  const LayoutItem items[]{
      {.id        = 1,
       .intrinsic = {20.0F, 20.0F},
       .maximum   = {30.0F, 30.0F},
       .grow      = 1.0F},
      {.id = 2, .intrinsic = {20.0F, 20.0F}, .grow = 1.0F}};
  const auto result =
      stack({0.0F, 0.0F, 100.0F, 20.0F}, items, {.axis = Axis::Horizontal});
  EXPECT_FLOAT_EQ(result.find(1)->rect.width, 30.0F);
  EXPECT_FLOAT_EQ(result.find(2)->rect.width, 70.0F);
  expectLayoutContained(result);
}
TEST(UiLayoutTest, ShrinkRespectsMinimaWhenTheyFit) {
  const LayoutItem items[]{
      {.id = 1, .intrinsic = {100.0F, 20.0F}, .minimum = {40.0F, 10.0F}},
      {.id = 2, .intrinsic = {100.0F, 20.0F}, .minimum = {20.0F, 10.0F}}};
  const auto result =
      stack({0.0F, 0.0F, 100.0F, 30.0F}, items, {.axis = Axis::Horizontal});
  EXPECT_GE(result.find(1)->rect.width, 40.0F);
  EXPECT_GE(result.find(2)->rect.width, 20.0F);
  EXPECT_FLOAT_EQ(result.find(1)->rect.width + result.find(2)->rect.width,
                  100.0F);
  expectLayoutContained(result);
}
TEST(UiLayoutTest, ImpossibleMinimaAndGapsShrinkInsideFractionalBounds) {
  const LayoutItem items[]{{.id        = 1,
                            .intrinsic = {100.0F, 20.0F},
                            .minimum   = {90.0F, 20.0F},
                            .paddingPx = 30.0F},
                           {.id        = 2,
                            .intrinsic = {100.0F, 20.0F},
                            .minimum   = {90.0F, 20.0F},
                            .paddingPx = 30.0F}};
  const Rect bounds{0.25F, 0.75F, 30.1F, 5.1F};
  expectLayoutContained(
      stack(bounds, items, {.axis = Axis::Horizontal, .gap = 400.0F}));
  expectLayoutContained(
      grid(bounds, items, {.columns = 2, .columnGap = 400.0F}));
  expectLayoutContained(split(bounds, items[0], items[1], {.gap = 400.0F}));
}
TEST(UiLayoutTest, GridRoundsCumulativeBoundariesWithoutGaps) {
  const LayoutItem items[]{{.id = 1, .intrinsic = {10.0F, 20.0F}},
                           {.id = 2, .intrinsic = {10.0F, 20.0F}},
                           {.id = 3, .intrinsic = {10.0F, 20.0F}}};
  const auto result = grid({0.0F, 0.0F, 100.0F, 20.0F}, items, {.columns = 3});
  EXPECT_FLOAT_EQ(result.find(1)->rect.width, 33.0F);
  EXPECT_FLOAT_EQ(result.find(2)->rect.width, 34.0F);
  EXPECT_FLOAT_EQ(result.find(3)->rect.width, 33.0F);
  EXPECT_FLOAT_EQ(result.find(1)->rect.left + result.find(1)->rect.width,
                  result.find(2)->rect.left);
  EXPECT_FLOAT_EQ(result.find(2)->rect.left + result.find(2)->rect.width,
                  result.find(3)->rect.left);
  expectLayoutContained(result);
}
TEST(UiLayoutTest, SplitKeepsMinimaAndReadingOrderOnBothAxes) {
  const LayoutItem first{.id = 1, .minimum = {30.0F, 30.0F}, .focusable = true};
  const LayoutItem second{
      .id = 2, .minimum = {20.0F, 20.0F}, .focusable = true};
  for (auto axis : {Axis::Horizontal, Axis::Vertical}) {
    const auto result = split({0.0F, 0.0F, 100.0F, 100.0F}, first, second,
                              {.axis = axis, .firstShare = 0.01F, .gap = 5.0F});
    EXPECT_EQ(result.focusOrder, (std::vector<std::uint32_t>{1, 2}));
    if (axis == Axis::Horizontal)
      EXPECT_FLOAT_EQ(result.find(1)->rect.width, 30.0F);
    else
      EXPECT_FLOAT_EQ(result.find(1)->rect.height, 30.0F);
    expectLayoutContained(result);
  }
}
TEST(UiLayoutTest, InputAreaAndHitTestUseTheSameResolvedBox) {
  const LayoutItem item{
      .id = 42, .intrinsic = {30.0F, 20.0F}, .focusable = true};
  const auto result = flow({10.0F, 30.0F, 100.0F, 80.0F}, std::span{&item, 1U});
  const auto area   = result.inputArea(42, 200);
  ASSERT_TRUE(area.has_value());
  EXPECT_EQ(*area, (gleditor::InputArea{10, 90, 30, 20}));
  ASSERT_NE(result.hitTest(15.0F, 100.0F), nullptr);
  EXPECT_EQ(result.hitTest(15.0F, 100.0F)->id, 42U);
  EXPECT_EQ(result.hitTest(40.0F, 100.0F), nullptr);
}
TEST(UiLayoutTest, DisabledAndClippedItemsAreAbsentFromFocusOrder) {
  const LayoutItem items[]{
      {.id        = 1,
       .intrinsic = {80.0F, 20.0F},
       .focusable = true,
       .enabled   = false},
      {.id = 2, .intrinsic = {80.0F, 20.0F}, .focusable = true}};
  const auto result = flow({0.0F, 0.0F, 100.0F, 20.0F}, items);
  EXPECT_TRUE(result.focusOrder.empty());
  EXPECT_EQ(result.hitTest(10.0F, 10.0F), nullptr);
  expectLayoutContained(result);
}
TEST(UiLayoutTest, DuplicateIdsCannotCreateAmbiguousHitTargets) {
  const LayoutItem items[]{{.id = 1}, {.id = 1}};
  EXPECT_THROW(static_cast<void>(flow({}, items)), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(grid({}, items)), std::invalid_argument);
  auto first        = flow({}, std::span{items, 1U});
  const auto second = flow({}, std::span{items + 1, 1U});
  EXPECT_THROW(first.append(second), std::invalid_argument);
  EXPECT_EQ(first.boxes.size(), 1U);
}
TEST(UiLayoutTest, EmptyAndInvalidInputsStillHaveFiniteContainedGeometry) {
  const auto nan      = std::numeric_limits<float>::quiet_NaN();
  const auto infinity = std::numeric_limits<float>::infinity();
  const LayoutItem item{.id        = 1,
                        .intrinsic = {nan, infinity},
                        .minimum   = {-10.0F, nan},
                        .grow      = infinity,
                        .paddingPx = nan};
  const Rect bounds{nan, infinity, -10.0F, 20.0F};
  expectLayoutContained(flow(bounds, std::span{&item, 1U}, {.gap = nan}));
  EXPECT_TRUE(grid(bounds, {}, {.columns = 0}).boxes.empty());
}
TEST(UiLayoutTest, ResponsivePropertyMatrixKeepsEveryBoxInsideSafeArea) {
  for (const float scale : {1.0F, 1.25F, 2.0F}) {
    for (const int width : {1, 40, 320, 640, 1280}) {
      for (const int count : {0, 1, 2, 7, 19}) {
        const UiMetrics metrics{.contentScale = scale,
                                .screenWidth  = width,
                                .screenHeight = 480,
                                .chrome = {.top = 31.0F, .bottom = 17.0F}};
        std::vector<LayoutItem> items;
        for (int index = 0; index < count; ++index) {
          items.push_back(
              {.id        = static_cast<std::uint32_t>(index + 1),
               .intrinsic = {metrics.px(50.5F + static_cast<float>(index)),
                             metrics.px(20.5F)},
               .minimum   = {metrics.px(10.0F), metrics.px(5.0F)},
               .grow      = static_cast<float>(index % 3),
               .focusable = true,
               .paddingPx = metrics.px(3.5F)});
        }
        const auto safe = metrics.pixelSafeArea();
        for (const auto axis : {Axis::Horizontal, Axis::Vertical}) {
          const auto wrapping = flow(safe, items,
                                     {.axis    = axis,
                                      .gap     = metrics.px(2.5F),
                                      .lineGap = metrics.px(3.5F)});
          const auto linear =
              stack(safe, items, {.axis = axis, .gap = metrics.px(2.5F)});
          expectLayoutContained(wrapping);
          expectLayoutContained(linear);
          const auto repeat = flow(safe, items,
                                   {.axis    = axis,
                                    .gap     = metrics.px(2.5F),
                                    .lineGap = metrics.px(3.5F)});
          ASSERT_EQ(wrapping.boxes.size(), repeat.boxes.size());
          for (std::size_t index = 0; index < repeat.boxes.size(); ++index) {
            EXPECT_FLOAT_EQ(wrapping.boxes[index].rect.left,
                            repeat.boxes[index].rect.left);
            EXPECT_FLOAT_EQ(wrapping.boxes[index].rect.bottom,
                            repeat.boxes[index].rect.bottom);
            EXPECT_FLOAT_EQ(wrapping.boxes[index].rect.width,
                            repeat.boxes[index].rect.width);
            EXPECT_FLOAT_EQ(wrapping.boxes[index].rect.height,
                            repeat.boxes[index].rect.height);
          }
        }
        expectLayoutContained(grid(safe, items,
                                   {.columns   = 3,
                                    .columnGap = metrics.px(2.5F),
                                    .rowGap    = metrics.px(3.5F)}));
      }
    }
  }
}
} // namespace

TEST(UiMetricsTest, LegacyFontOverridesScaleWithoutLosingFamilyOrStyle) {
  using namespace gleditor::ui;
  const UiMetrics metrics{
      .contentScale = 1.25F, .userScale = 2, .fontScale = 1.5F};
  Theme theme;
  EXPECT_EQ(scaledFontDescription({}, FontRole::Label, metrics, theme),
            "Sans 45");
  EXPECT_EQ(
      scaledFontDescription("Serif Bold 10", FontRole::Label, metrics, theme),
      "Serif Bold 37.5");
  EXPECT_EQ(scaledFontDescription("Serif", FontRole::Label, metrics, theme),
            "Serif 60");
  EXPECT_EQ(
      scaledFontDescription("Sans 0.1", FontRole::Caption, UiMetrics{}, theme),
      "Sans 6.75");
  EXPECT_EQ(theme.font(FontRole::Label).points, 12);
}
