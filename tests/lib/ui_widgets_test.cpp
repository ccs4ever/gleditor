#include <gtest/gtest.h>

#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/ui/overlay.hpp>
#include <gleditor/ui/widgets.hpp>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gleditor::ui;
using gleditor::Key;

TEST(UiWidgetTest, textEditingPreservesUtf8AndPlacesInsertionAtTheCaret) {
  TextField field{.value = "A\xC3\xA9\xE7\x95\x8C"
                           "Z",
                  .caret = 7};
  EXPECT_TRUE(field.key(Key::Left));
  EXPECT_EQ(field.caret, 6U);
  EXPECT_TRUE(field.key(Key::Backspace));
  EXPECT_EQ(field.value, "A\xC3\xA9"
                         "Z");
  EXPECT_EQ(field.caret, 3U);
  field.insert("\xE6\xBC\xA2");
  EXPECT_EQ(field.value, "A\xC3\xA9\xE6\xBC\xA2"
                         "Z");
  EXPECT_TRUE(field.key(Key::Home));
  EXPECT_TRUE(field.key(Key::Delete));
  EXPECT_EQ(field.value, "\xC3\xA9\xE6\xBC\xA2"
                         "Z");
  const auto unchanged = field.value;
  EXPECT_THROW(field.insert("\xC0\xAF"), std::invalid_argument);
  EXPECT_THROW(field.insert("\xED\xA0\x80"), std::invalid_argument);
  EXPECT_EQ(field.value, unchanged);
}

TEST(UiWidgetTest, backspaceAndNavigationKeepCombiningAndZwjGraphemesWhole) {
  const std::string combining = "a\xCC\x81";
  const std::string family    = "👨‍👩‍👧‍👦";
  TextField field{.value = combining + family + "Z"};
  field.caret = field.value.size();
  EXPECT_TRUE(field.key(Key::Left));
  EXPECT_EQ(field.caret, combining.size() + family.size());
  EXPECT_TRUE(field.key(Key::Left));
  EXPECT_EQ(field.caret, combining.size());
  EXPECT_TRUE(field.key(Key::Right));
  EXPECT_EQ(field.caret, combining.size() + family.size());
  EXPECT_TRUE(field.key(Key::Backspace));
  EXPECT_EQ(field.value, combining + "Z");
  EXPECT_TRUE(field.key(Key::Backspace));
  EXPECT_EQ(field.value, "Z");
  EXPECT_EQ(field.caret, 0U);
}

TEST(UiWidgetTest, longTextFieldsScrollToTheCaretAndKeepTheirCompleteText) {
  const std::string value =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  ScreenOverlay overlay({.id        = 1,
                         .model     = TextField{.value       = value,
                                                .placeholder = "Identifier",
                                                .caret       = value.size()},
                         .preferred = {100, 44}});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  const auto first   = overlay.prepare(metrics, theme);
  const auto *visual = first->find(1);
  const auto *box    = first->layout.find(1);
  ASSERT_NE(visual, nullptr);
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(visual->value, value);
  EXPECT_FALSE(visual->fitted.truncated);
  EXPECT_GT(visual->fitted.widthPx, box->contentRect.width);
  EXPECT_GT(visual->textOffsetPx, 0.0F);
  EXPECT_NEAR(visual->fitted.widthPx - visual->textOffsetPx,
              box->contentRect.width, 0.01F);
  for (const auto &glyph : visual->fitted.shaping.glyphs)
    EXPECT_NE(glyph.chr, "\xE2\x80\xA6");
  EXPECT_TRUE(overlay.keyInto(1, Key::Home));
  const auto home = overlay.prepare(metrics, theme);
  EXPECT_FLOAT_EQ(home->find(1)->textOffsetPx, 0.0F);
  EXPECT_TRUE(overlay.typeInto(1, "PREFIX"));
  EXPECT_EQ(overlay.prepare(metrics, theme)->find(1)->value, "PREFIX" + value);
}

TEST(UiWidgetTest, rtlTextFieldsRevealTheCorrectVisualEdgeForHomeAndEnd) {
  const std::string value = "مرحبا بالعالم مرحبا بالعالم مرحبا بالعالم";
  ScreenOverlay overlay({.id        = 1,
                         .model     = TextField{.value       = value,
                                                .placeholder = "Arabic title",
                                                .caret       = value.size()},
                         .preferred = {100, 60}});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  const auto end = overlay.prepare(metrics, theme);
  ASSERT_TRUE(end->find(1)->caretOffsetPx.has_value());
  EXPECT_NEAR(*end->find(1)->caretOffsetPx, 0.0F, 0.01F);
  EXPECT_FLOAT_EQ(end->find(1)->textOffsetPx, 0.0F);
  EXPECT_TRUE(overlay.keyInto(1, Key::Home));
  const auto home    = overlay.prepare(metrics, theme);
  const auto *visual = home->find(1);
  const auto *box    = home->layout.find(1);
  ASSERT_NE(visual, nullptr);
  ASSERT_NE(box, nullptr);
  ASSERT_TRUE(visual->caretOffsetPx.has_value());
  EXPECT_NEAR(*visual->caretOffsetPx + visual->textOffsetPx,
              visual->fitted.widthPx, 0.01F);
  EXPECT_GT(visual->textOffsetPx, 0.0F);
  EXPECT_LE(*visual->caretOffsetPx, box->contentRect.width + 0.01F);
}

TEST(UiWidgetTest, virtualListsNeverShapeThousandsOfOffscreenRows) {
  List list;
  list.rowHeightPx = 40;
  for (unsigned index = 0; index < 10000; ++index) {
    list.rows.push_back({.id   = index + 10,
                         .text = "Publication " + std::to_string(index) +
                                 " with a long descriptive title",
                         .action = "open"});
  }
  Widget widget{.id = 1, .model = list, .preferred = {300, 200}};
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  gleditor::text::ShapingCache cache;
  gleditor::text::ShapingStatsScope capture;
  const auto scene =
      layoutWidgets(widget, metrics.pixelSafeArea(), metrics, theme, cache);
  EXPECT_LE(scene.visuals.size(), 8U);
  EXPECT_LT(capture.stats().harfbuzzCalls, 32U);
  EXPECT_LE(cache.stats().entries, 16U);
  list.scrollPx = 20000;
  widget.model  = list;
  const auto scrolled =
      layoutWidgets(widget, metrics.pixelSafeArea(), metrics, theme, cache);
  EXPECT_LE(scrolled.visuals.size(), 8U);
  ASSERT_GT(scrolled.visuals.size(), 1U);
  EXPECT_GE(scrolled.visuals[1].itemIndex, 499U);
  EXPECT_LT(capture.stats().harfbuzzCalls, 64U);
}

TEST(UiWidgetTest,
     disabledTabsCannotActivateAndSelectionReportsTheOwningWidget) {
  Tabs tabs{.tabs = {{10, "First tab", "first", true},
                     {11, "Disabled tab", "disabled", false},
                     {12, "Third tab", "third", true}}};
  ScreenOverlay overlay({.id = 1, .model = tabs});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  std::ignore = overlay.prepare(metrics, theme);
  EXPECT_FALSE(overlay.activate(11));
  EXPECT_TRUE(actions.empty());
  EXPECT_TRUE(overlay.activate(12));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions[0].id, 1U);
  EXPECT_EQ(actions[0].action, "third");
  EXPECT_EQ(actions[0].itemIndex, 2U);
  const auto selected = overlay.prepare(metrics, theme);
  EXPECT_TRUE(selected->find(12)->selected);
  EXPECT_FALSE(selected->find(10)->selected);
}

TEST(UiWidgetTest, steppersClampAndOnlyExposeEnabledMovesAtTheirLimits) {
  ScreenOverlay overlay(
      {.id = 1, .model = Stepper{"Copies", "copies", 1, 0, 2, 1}});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  const auto scene = overlay.prepare(metrics, theme);
  WidgetId plus = 0, minus = 0;
  for (const auto &visual : scene->visuals) {
    if (visual.stepDirection > 0) plus = visual.id;
    if (visual.stepDirection < 0) minus = visual.id;
  }
  ASSERT_NE(plus, 0U);
  ASSERT_NE(minus, 0U);
  EXPECT_TRUE(overlay.activate(plus));
  const auto maximum = overlay.prepare(metrics, theme);
  EXPECT_FALSE(maximum->layout.find(plus)->enabled);
  EXPECT_FALSE(overlay.activate(plus));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().action, "copies");
  EXPECT_EQ(std::stod(actions.front().value), 2);
  EXPECT_TRUE(overlay.activate(minus));
  std::ignore = overlay.prepare(metrics, theme);
  EXPECT_TRUE(overlay.activate(minus));
  const auto minimum = overlay.prepare(metrics, theme);
  EXPECT_FALSE(minimum->layout.find(minus)->enabled);
  EXPECT_FALSE(overlay.activate(minus));
  Stepper invalid;
  invalid.step = std::numeric_limits<double>::infinity();
  EXPECT_THROW(invalid.change(1), std::invalid_argument);
}

TEST(UiWidgetTest,
     scrubbersClampFractionsAndRejectInvalidNumbersWithoutMutation) {
  Scrubber scrubber{"Time", "seek", 5, 0, 10};
  EXPECT_TRUE(scrubber.setFraction(2));
  EXPECT_DOUBLE_EQ(scrubber.value, 10);
  EXPECT_DOUBLE_EQ(scrubber.fraction(), 1);
  EXPECT_TRUE(scrubber.setFraction(-1));
  EXPECT_DOUBLE_EQ(scrubber.value, 0);
  EXPECT_THROW(scrubber.setFraction(std::numeric_limits<double>::quiet_NaN()),
               std::invalid_argument);
  EXPECT_DOUBLE_EQ(scrubber.value, 0);
  scrubber.minimum = 20;
  EXPECT_THROW(scrubber.setFraction(0.5), std::invalid_argument);
}
} // namespace
