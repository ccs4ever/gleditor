#include <gtest/gtest.h>

#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/ui/button_metrics.hpp>
#include <gleditor/ui/overlay.hpp>
#include <gleditor/ui/recessed_well.hpp>
#include <gleditor/ui/segmented_pill.hpp>
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
    list.rows.push_back({.id     = index + 10,
                         .text   = "Publication " + std::to_string(index) +
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

TEST(UiWidgetTest, clippedListEdgesDoNotExposeUndrawnActions) {
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  Theme theme;
  theme.paddingEm = 0;
  const auto font = gleditor::text::FontManager::instance().getFont(
      metrics.fontDescription(FontRole::Label, theme));
  const auto line = font->metrics().lineHeight;
  List list{.rows        = {{10, "First row", "open", true},
                            {11, "Second row", "open", true},
                            {12, "Third row", "open", true}},
            .rowHeightPx = line * 2};
  Widget widget{.id = 1, .model = list};
  gleditor::text::ShapingCache cache;
  const Rect bounds{20, 20, 240, line * 2.5F};
  auto scene = layoutWidgets(widget, bounds, metrics, theme, cache);
  ASSERT_NE(scene.find(10), nullptr);
  EXPECT_EQ(scene.find(11), nullptr);
  EXPECT_EQ(scene.layout.find(11), nullptr);
  list.scrollPx = line * 1.5F;
  widget.model  = list;
  scene         = layoutWidgets(widget, bounds, metrics, theme, cache);
  EXPECT_EQ(scene.find(10), nullptr);
  const auto *row = scene.find(11);
  ASSERT_NE(row, nullptr);
  EXPECT_FALSE(row->fitted.shaping.glyphs.empty());
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
TEST(UiWidgetTest, positionedDiagramClipsAndCullsNodesAcrossDisplayScales) {
  for (const float scale : {1.F, 1.25F, 2.F}) {
    const UiMetrics metrics{
        .contentScale = scale, .screenWidth = 640, .screenHeight = 480};
    Theme theme;
    theme.paddingEm = 0;
    Widget root{
        .id       = 1,
        .model    = PositionedPanel{.childBounds = {{10, 10, 80, 44},
                                                    {-20, 60, 80, 44},
                                                    {210, 0, 80, 44}}},
        .children = {
            {.id    = 2,
             .model = Button{"I", "visit", true,
                             "Insert operation with complete identity"}},
            {.id = 3, .model = Button{"Partly visible", "visit"}},
            {.id = 4, .model = Button{"Outside", "visit"}}}};
    ScreenOverlay overlay(root);
    overlay.setBounds(Rect{40, 40, 200 * scale, 140 * scale});
    const auto scene    = overlay.prepare(metrics, theme);
    const auto *first   = scene->layout.find(2);
    const auto *partial = scene->layout.find(3);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(partial, nullptr);
    EXPECT_NEAR(first->rect.left, 40 + 10 * scale, .5F);
    EXPECT_NEAR(first->rect.bottom, 40 + 10 * scale, .5F);
    EXPECT_FLOAT_EQ(partial->rect.left, 40);
    EXPECT_FLOAT_EQ(partial->rect.width, 60 * scale);
    EXPECT_EQ(scene->layout.find(4), nullptr);
    EXPECT_EQ(scene->find(4), nullptr);
    EXPECT_EQ(scene->layout.focusOrder, (std::vector<std::uint32_t>{2, 3}));
    EXPECT_EQ(scene->find(2)->accessibleLabel,
              "Insert operation with complete identity");
    EXPECT_EQ(scene->find(2)->text, "I");
    EXPECT_EQ(scene->resolvePickingId(scene->find(2)->pickingId), 2U);
    EXPECT_FALSE(scene->find(1)->background);
  }
}

TEST(UiWidgetTest, positionedDiagramRejectsInvalidGeometry) {
  gleditor::text::ShapingCache cache;
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  Theme theme;
  Widget root{.id       = 1,
              .model    = PositionedPanel{},
              .children = {{.id = 2, .model = Button{"Node", "visit"}}}};
  EXPECT_THROW(
      (void)layoutWidgets(root, {0, 0, 200, 140}, metrics, theme, cache),
      std::invalid_argument);
  std::get<PositionedPanel>(root.model)
      .childBounds.push_back(
          {std::numeric_limits<float>::quiet_NaN(), 0, 80, 44});
  EXPECT_THROW(
      (void)layoutWidgets(root, {0, 0, 200, 140}, metrics, theme, cache),
      std::invalid_argument);
}

TEST(UiWidgetTest, textFieldCallbacksRetainCaretAcrossOwnerRebuilds) {
  TextField input{.value = "á界Z", .placeholder = "Query", .action = "query"};
  input.caret = input.value.size();
  ScreenOverlay overlay({.id = 1, .model = input});
  overlay.setActionHandler([&](const WidgetAction &action) {
    ASSERT_TRUE(action.caret.has_value());
    input.value = action.value;
    input.caret = *action.caret;
    overlay.setModel({.id = 1, .model = input});
  });
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::ignore = overlay.prepare(metrics, theme);
  ASSERT_TRUE(overlay.keyInto(1, Key::Home));
  std::ignore = overlay.prepare(metrics, theme);
  ASSERT_TRUE(overlay.typeInto(1, "前"));
  EXPECT_EQ(input.value, "前á界Z");
  EXPECT_EQ(input.caret, std::string("前").size());
  std::ignore = overlay.prepare(metrics, theme);
  ASSERT_TRUE(overlay.keyInto(1, Key::Right));
  std::ignore = overlay.prepare(metrics, theme);
  ASSERT_TRUE(overlay.keyInto(1, Key::Backspace));
  EXPECT_EQ(input.value, "前界Z");
  EXPECT_EQ(input.caret, std::string("前").size());
}

TEST(UiWidgetTest,
     virtualRowsUseIdentifierAndDescriptionFittingWithoutLosingLabels) {
  const std::string identifier =
      "Key: abcdefghijklmnopqrstuvwxyz0123456789TAIL";
  const std::string description =
      "A long description contains several words and Unicode 名称, preserving "
      "the complete accessible text while wrapping.";
  List list{
      .rows = {{2, identifier, "inspect", true, TextPurpose::Identifier},
               {3, description, "inspect", true, TextPurpose::Description}},
      .rowHeightPx = 110};
  ScreenOverlay overlay({.id = 1, .model = std::move(list), .maxLines = 3});
  overlay.setBounds(Rect{40, 40, 180, 220});
  const auto scene =
      overlay.prepare({.screenWidth = 640, .screenHeight = 480}, Theme{});
  ASSERT_NE(scene->find(2), nullptr);
  ASSERT_NE(scene->find(3), nullptr);
  EXPECT_EQ(scene->find(2)->accessibleLabel, identifier);
  EXPECT_EQ(scene->find(3)->accessibleLabel, description);
  EXPECT_TRUE(scene->find(2)->fitted.truncated);
  EXPECT_GT(scene->find(3)->fitted.shaping.lines.size(), 1U);
  EXPECT_LE(scene->find(3)->fitted.shaping.lines.size(), 3U);
}

TEST(UiWidgetTest, buttonMetricsValuesAndModalityAwareEvaluation) {
  ButtonMetrics metrics;
  EXPECT_FLOAT_EQ(metrics.heightCompactPx, 28.0F);
  EXPECT_FLOAT_EQ(metrics.heightStandardPx, 32.0F);
  EXPECT_FLOAT_EQ(metrics.heightTouchPx, 44.0F);
  EXPECT_FLOAT_EQ(metrics.radiusMicroPx, 2.5F);
  EXPECT_FLOAT_EQ(metrics.radiusPillShare, 0.5F);
  EXPECT_FLOAT_EQ(metrics.radiusChamferPx, 2.0F);
  EXPECT_FLOAT_EQ(metrics.radiusSoftPx, 4.0F);
  EXPECT_FLOAT_EQ(metrics.virtualHitTargetMinPx, 36.0F);
  EXPECT_FLOAT_EQ(metrics.seamWidthPx, 1.0F);

  EXPECT_FLOAT_EQ(metrics.height(InputModality::Pointer, true), 28.0F);
  EXPECT_FLOAT_EQ(metrics.height(InputModality::Pointer, false), 32.0F);
  EXPECT_FLOAT_EQ(metrics.height(InputModality::Touch, true), 44.0F);
  EXPECT_FLOAT_EQ(metrics.height(InputModality::Touch, false), 44.0F);

  EXPECT_FLOAT_EQ(metrics.cornerRadius(ButtonShapeKind::Pill, 32.0F), 16.0F);
  EXPECT_FLOAT_EQ(metrics.cornerRadius(ButtonShapeKind::Micro, 32.0F), 2.5F);
  EXPECT_FLOAT_EQ(metrics.cornerRadius(ButtonShapeKind::Chamfered, 32.0F),
                  2.0F);
  EXPECT_FLOAT_EQ(metrics.cornerRadius(ButtonShapeKind::SoftRect, 32.0F), 4.0F);
  EXPECT_FLOAT_EQ(metrics.cornerRadius(ButtonShapeKind::Ghost, 32.0F), 2.5F);

  // Modality-aware button sizing in WidgetScene
  gleditor::text::ShapingCache cache;
  Widget root{.id = 1, .model = Button{"Action", "commit"}};

  UiMetrics pointerMetrics{.screenWidth = 640, .screenHeight = 480};
  pointerMetrics.touchModality = false;
  Theme theme;
  const auto pointerScene =
      layoutWidgets(root, {0, 0, 200, 100}, pointerMetrics, theme, cache);
  const auto *pointerBox = pointerScene.layout.find(1);
  ASSERT_NE(pointerBox, nullptr);
  EXPECT_FLOAT_EQ(pointerBox->rect.height, 32.0F);

  UiMetrics touchMetrics{.screenWidth = 640, .screenHeight = 480};
  touchMetrics.touchModality = true;
  const auto touchScene =
      layoutWidgets(root, {0, 0, 200, 100}, touchMetrics, theme, cache);
  const auto *touchBox = touchScene.layout.find(1);
  ASSERT_NE(touchBox, nullptr);
  EXPECT_FLOAT_EQ(touchBox->rect.height, 44.0F);

  // Virtual hit target expansion:
  // pointerBox has height 32px (from bottom to top).
  // The virtual hit target expands by (36 - 32) * 0.5 = 2px on top and bottom.
  // Click at pointerBox->rect.bottom - 1.0 is within the 36px virtual target
  // and should hit widget 1.
  const float centerX = pointerBox->rect.left + pointerBox->rect.width * 0.5F;
  EXPECT_EQ(
      pointerScene.layout.hitTest(centerX, pointerBox->rect.bottom - 1.0F),
      nullptr); // raw layout misses
  const auto *hit = pointerScene.hitTest(
      centerX, pointerBox->rect.bottom - 1.0F); // virtual hit target hits
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->id, 1U);

  // A point far outside the virtual hit target (bottom - 10.0F) returns nullptr
  EXPECT_EQ(pointerScene.hitTest(centerX, pointerBox->rect.bottom - 10.0F),
            nullptr);
}

TEST(UiWidgetTest, opticalCenteringCalculations) {
  const Rect box{10.0F, 20.0F, 120.0F, 32.0F};
  const auto font = gleditor::text::FontManager::instance().getFont("Sans 12");
  ASSERT_NE(font, nullptr);
  const auto &fm = font->metrics();
  const float capHeight =
      fm.capHeight > 0.0F ? fm.capHeight : fm.ascent * 0.72F;
  const float xHeight = fm.xHeight > 0.0F ? fm.xHeight : fm.ascent * 0.52F;
  EXPECT_GT(capHeight, 0.0F);
  EXPECT_GT(xHeight, 0.0F);

  // Text without descenders: centers against capHeight
  const std::string upperText = "OK 100";
  bool upperHasDescender      = false;
  for (char c : upperText) {
    if (c == 'g' || c == 'j' || c == 'p' || c == 'q' || c == 'y') {
      upperHasDescender = true;
    }
  }
  EXPECT_FALSE(upperHasDescender);

  const float upperBaseline =
      box.bottom + std::round((box.height - capHeight) * 0.5F);
  const float upperDrawTop = upperBaseline + fm.ascent;
  EXPECT_GT(upperDrawTop, box.bottom + fm.ascent);

  // Text with descenders: centers against full extent (ascent + descent)
  const std::string descenderText = "apply";
  bool descHasDescender           = false;
  for (char c : descenderText) {
    if (c == 'g' || c == 'j' || c == 'p' || c == 'q' || c == 'y') {
      descHasDescender = true;
    }
  }
  EXPECT_TRUE(descHasDescender);

  const float descBaseline =
      box.bottom +
      std::round((box.height - (fm.ascent + std::abs(fm.descent))) * 0.5F +
                 std::abs(fm.descent));
  const float descDrawTop = descBaseline + fm.ascent;
  EXPECT_GT(descDrawTop, box.bottom + fm.ascent);

  // Horizontal optical centering against fitted width
  const float fittedWidth = 48.0F;
  const float drawLeft =
      box.left + std::round((box.width - fittedWidth) * 0.5F);
  EXPECT_FLOAT_EQ(drawLeft, box.left + std::round((120.0F - 48.0F) * 0.5F));
  EXPECT_FLOAT_EQ(drawLeft, 46.0F);

  // Synthetic metrics with pronounced descent differences
  const float testHeight     = 32.0F;
  const float synthAscent    = 14.0F;
  const float synthDescent   = 2.0F;
  const float synthCap       = 9.0F;
  const float synthUpperBase = std::round((testHeight - synthCap) * 0.5F);
  const float synthDescBase  = std::round(
      (testHeight - (synthAscent + synthDescent)) * 0.5F + synthDescent);
  EXPECT_NE(synthUpperBase, synthDescBase);
}

TEST(UiWidgetTest, segmentedPillLayoutAndHairlineSeams) {
  SegmentedPill pill;
  pill.setContainerId(10)
      .setOuterShape(ButtonShapeKind::Pill)
      .setCompact(true)
      .addSegment(SegmentItem{}
                      .setId(11)
                      .setLabel("◀")
                      .setAction("prev")
                      .setCustomWidthPx(28.0F))
      .addSegment(SegmentItem{}
                      .setId(12)
                      .setLabel("Link 42 of 108")
                      .setAction("inspect")
                      .setIsMonospace(true))
      .addSegment(SegmentItem{}
                      .setId(13)
                      .setLabel("▶")
                      .setAction("next")
                      .setCustomWidthPx(28.0F));

  EXPECT_EQ(pill.containerId, 10U);
  EXPECT_EQ(pill.segments.size(), 3U);
  EXPECT_TRUE(pill.compact);
  EXPECT_EQ(pill.outerShape, ButtonShapeKind::Pill);

  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  Theme theme;
  gleditor::text::ShapingCache cache;

  const Rect bounds{50.0F, 50.0F, 240.0F, 0.0F};
  const auto result = layoutSegmentedPill(pill, bounds, metrics, theme, &cache);

  EXPECT_FLOAT_EQ(result.bounds.height, 28.0F); // compact height
  EXPECT_EQ(result.boxes.size(), 4U);           // 1 container + 3 segments

  const auto *containerBox = result.find(10);
  const auto *prevBox      = result.find(11);
  const auto *centerBox    = result.find(12);
  const auto *nextBox      = result.find(13);

  ASSERT_NE(containerBox, nullptr);
  ASSERT_NE(prevBox, nullptr);
  ASSERT_NE(centerBox, nullptr);
  ASSERT_NE(nextBox, nullptr);

  EXPECT_FLOAT_EQ(prevBox->rect.width, 28.0F);
  EXPECT_FLOAT_EQ(nextBox->rect.width, 28.0F);
  EXPECT_GT(centerBox->rect.width, 50.0F); // expanded center

  // 1px inner hairline seams
  EXPECT_FLOAT_EQ(centerBox->rect.left,
                  prevBox->rect.left + prevBox->rect.width + 1.0F);
  EXPECT_FLOAT_EQ(nextBox->rect.left,
                  centerBox->rect.left + centerBox->rect.width + 1.0F);

  // Focus order contains segment IDs
  EXPECT_EQ(result.focusOrder, (std::vector<std::uint32_t>{11, 12, 13}));
}

TEST(UiWidgetTest, recessedWellStylingTokens) {
  RecessedWellStyle well;
  EXPECT_FLOAT_EQ(well.cornerRadiusPx, 3.0F);
  EXPECT_FLOAT_EQ(well.creaseThicknessPx, 1.5F);

  well.setCornerRadiusPx(4.0F).setCreaseThicknessPx(2.0F);
  EXPECT_FLOAT_EQ(well.cornerRadiusPx, 4.0F);
  EXPECT_FLOAT_EQ(well.creaseThicknessPx, 2.0F);

  const glm::vec4 fill{0.1F, 0.12F, 0.15F, 1.0F};
  well.setBackgroundFill(fill);
  EXPECT_EQ(well.backgroundFill, fill);
}

} // namespace

TEST(UiWidgetTest, CompactLabelRetainsCompleteAccessibleDescription) {
  using namespace gleditor::ui;
  Widget root{
      .id    = 1,
      .model = Label{"bytes 87–153", TextPurpose::Description,
                     "Left member 1/3, occurrence 1/2, bytes 87–153, Genesis"}};
  ScreenOverlay overlay(root);
  overlay.setBounds(Rect{0, 0, 320, 60});
  const auto scene = overlay.prepare(UiMetrics{}, Theme{});
  ASSERT_NE(scene, nullptr);
  const auto *visual = scene->find(1);
  ASSERT_NE(visual, nullptr);
  EXPECT_EQ(visual->text, "bytes 87–153");
  EXPECT_EQ(visual->accessibleLabel,
            "Left member 1/3, occurrence 1/2, bytes 87–153, Genesis");
}
