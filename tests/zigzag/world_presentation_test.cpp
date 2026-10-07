#include "../lib/mocks/world_device.hpp"
#include "xudu/kinetic_tether_overlay.hpp"
#include "xudu/satelloid.hpp"
#include "xudu/wireframe_hull.hpp"
#include "xudu/world_card_presentation.hpp"
#include "zigzag/zigzag_commands.hpp"
#include "zigzag/zigzag_visualizer.hpp"
#include <gleditor/app.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <gtest/gtest.h>

namespace {
using namespace gleditor::ui;
const std::string full = "A long multilingual label 世界 مرحبا "
                         "👨‍👩‍👧‍👦 with an identifying suffix";
void contained(Rect child, Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}
void fitted(const WidgetScene &scene) {
  for (const auto &box : scene.layout.boxes) {
    contained(box.contentRect, box.rect);
    contained(box.rect, scene.layout.bounds);
    if (box.parentId)
      contained(box.rect, scene.layout.find(box.parentId)->rect);
    if (const auto *visual = scene.find(box.id)) {
      EXPECT_LE(visual->fitted.widthPx, box.contentRect.width + .01F);
      EXPECT_LE(visual->fitted.heightPx, box.contentRect.height + .01F);
    }
  }
}
TEST(WorldPresentationTest,
     CardsFitTheViewportFontMatrixAndDoNotCoverDropPixels) {
  for (const auto size : {Size{640, 480}, Size{1280, 800}, Size{2560, 1440}})
    for (float scale : {.8F, 1.F, 1.5F, 2.F})
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        UiMetrics metrics{.fontScale    = scale,
                          .screenWidth  = static_cast<int>(size.width),
                          .screenHeight = static_cast<int>(size.height)};
        Theme theme;
        for (auto &font : theme.fonts) font.family = family;
        for (const xanadu::WorldCardConfig config :
             {xanadu::WorldCardConfig{}, xanadu::WorldCardConfig{190, 88},
              xanadu::WorldCardConfig{260, 46}}) {
          xudu::world_cards::Card card;
          card.configure(metrics, theme, config, {});
          card.content(full, full, full);
          card.prepare();
          fitted(*card.panel.snapshot());
          const auto safe = metrics.pixelSafeArea();
          for (const auto pointer :
               {glm::vec2(1, 1), glm::vec2(size.width - 1, size.height - 1),
                glm::vec2(size.width * .5F, size.height * .5F)}) {
            const auto box = xudu::world_cards::awayFromPointer(
                card.presentation.size, pointer, metrics, 8);
            contained(box, safe);
            EXPECT_FALSE((pointer.x >= box.left &&
                          pointer.x <= box.left + box.width &&
                          pointer.y >= box.bottom &&
                          pointer.y <= box.bottom + box.height));
          }
        }
      }
}
TEST(WorldPresentationTest, TelemetryAndBlueprintRetainGeometryAndFullNames) {
  WorldRecordingDevice device;
  RenderState state(&device);
  glm::mat4 projection =
      glm::ortho(-320.F, 320.F, -240.F, 240.F, -100.F, 100.F);
  ch::Timeline timeline;
  gleditor::FrameContext ctx{
      .state          = state,
      .viewProjection = projection,
      .screenWidth    = 640,
      .screenHeight   = 480,
      .timeline       = timeline,
      .metrics        = {.screenWidth = 640, .screenHeight = 480}};
  xudu::WireframeHullOverlay hull({});
  hull.deviceReady(device, {});
  hull.setTelemetry("download", full, "Downloading (3/12 dependencies)", .25F);
  hull.drawFrame(ctx);
  ASSERT_EQ(hull.snapshots().size(), 1U);
  fitted(*hull.snapshots().front());
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  hull.describe(builder);
  auto node = tree.find(builder.id(1));
  ASSERT_TRUE(node);
  EXPECT_EQ(node->label, full);
  EXPECT_EQ(node->description, "Downloading (3/12 dependencies)");
  device.uploads = 0;
  gleditor::text::ShapingStatsScope capture;
  for (unsigned i = 0; i < 5; ++i) hull.drawFrame(ctx);
  EXPECT_EQ(device.uploads, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  hull.clearTelemetry("download");
  hull.drawFrame(ctx);
  EXPECT_TRUE(hull.snapshots().empty());
  xanadu::KineticTetherEngine engine;
  xudu::KineticTetherOverlay tether(engine);
  tether.deviceReady(device, {});
  engine.startDrag({.previewText = full}, 320, 240);
  tether.drawFrame(ctx);
  ASSERT_EQ(tether.snapshots().size(), 1U);
  fitted(*tether.snapshots().front());
  gleditor::a11y::Tree blueprint;
  gleditor::a11y::Builder blueprintBuilder(blueprint, 17);
  tether.describe(blueprintBuilder);
  auto preview = blueprint.find(blueprintBuilder.id(1));
  ASSERT_TRUE(preview);
  EXPECT_EQ(preview->description, full);
  ASSERT_TRUE(preview->bounds);
  EXPECT_FALSE(preview->bounds->left <= 320 && preview->bounds->right >= 320 &&
               preview->bounds->top <= 240 && preview->bounds->bottom >= 240);
  device.uploads = 0;
  gleditor::text::ShapingStatsScope warm;
  tether.drawFrame(ctx);
  EXPECT_EQ(device.uploads, 0U);
  EXPECT_EQ(warm.stats().harfbuzzCalls, 0U);
}
TEST(WorldPresentationTest, ZigzagLabelsHaveLodWithoutChangingCellContent) {
  WorldRecordingDevice device;
  RenderState state(&device);
  zigzag::ZigzagVisualizer viz("");
  viz.setHasKeyboard(false);
  viz.setPresentationTransformResolver(
      [] { return std::optional{glm::mat4(1)}; });
  auto config              = xanadu::ZigzagPresentationConfig{};
  config.minReadableTextPx = 0;
  viz.setPresentationConfig(config);
  viz.deviceReady(device, {});
  glm::mat4 projection(1);
  ch::Timeline timeline;
  gleditor::FrameContext ctx{
      .state          = state,
      .viewProjection = projection,
      .screenWidth    = 640,
      .screenHeight   = 480,
      .timeline       = timeline,
      .metrics        = {.screenWidth = 640, .screenHeight = 480}};
  const auto operations = viz.operationCount();
  for (auto mode : {zigzag::ZigzagVisualizer::ViewMode::Topology,
                    zigzag::ZigzagVisualizer::ViewMode::CellContent}) {
    viz.setViewMode(mode);
    for (float zoom : {.25F, 1.F, 2.F}) {
      projection = glm::ortho(-320.F / zoom, 320.F / zoom, -240.F / zoom,
                              240.F / zoom, -1000.F, 1000.F);
      for (unsigned i = 0; i < 300; ++i) {
        state.beginPickScene();
        ctx.chrome = {};
        viz.drawFrame(ctx);
      }
      ASSERT_TRUE(viz.projectedCell(viz.focusCellId()));
      EXPECT_EQ(viz.visibleWorldLabelCount() > 0, zoom >= 1);
      device.uploads = 0;
      gleditor::text::ShapingStatsScope warm;
      for (unsigned i = 0; i < 5; ++i) {
        state.beginPickScene();
        ctx.chrome = {};
        viz.drawFrame(ctx);
      }
      EXPECT_EQ(warm.stats().harfbuzzCalls, 0U);
      EXPECT_EQ(warm.stats().layoutCalls, 0U);
      EXPECT_EQ(device.uploads, 0U);
      EXPECT_EQ(viz.operationCount(), operations);
      EXPECT_FALSE(state.overlayPickScene.overlays.empty());
    }
  }
  const auto beforeTypography = viz.accessibilityRevision();
  ctx.metrics.fontScale       = 1.5F;
  ctx.chrome                  = {};
  state.beginPickScene();
  viz.drawFrame(ctx);
  EXPECT_GT(viz.accessibilityRevision(), beforeTypography);
  zigzag::RenderStateCell rich{.id = 42, .text = full + "\n" + full};
  rich.decorated_ranges = {
      {0, static_cast<std::uint32_t>(full.size()),
       gleditor::decorationBit(gleditor::Decoration::Underline)}};
  const auto layout = viz.measureCellLayout(rich, true);
  EXPECT_EQ(layout.value.visibleBytes, rich.text.size());
  EXPECT_FALSE(layout.value.truncated);
  EXPECT_GT(layout.value.lines, 1);
  EXPECT_TRUE(
      std::ranges::any_of(layout.value.shaping.glyphs, [](const auto &glyph) {
        return gleditor::hasDecoration(glyph.decorations,
                                       gleditor::Decoration::Underline);
      }));
  const auto previousFocus = viz.focusCellId();
  viz.setCellRadius(1);
  viz.navigateFocus("d.2", zigzag::DimVector::POS);
  viz.navigateFocus("d.3", zigzag::DimVector::POS);
  ASSERT_NE(viz.focusCellId(), previousFocus);
  for (unsigned i = 0; i < 300; ++i) {
    ctx.chrome = {};
    state.beginPickScene();
    viz.drawFrame(ctx);
  }
  EXPECT_FALSE(viz.projectedCell(previousFocus));
  EXPECT_EQ(viz.operationCount(), operations);
}
TEST(WorldPresentationTest,
     SatelloidPicksAndAccessibilityUseStableFullCardIdentity) {
  WorldRecordingDevice device;
  RenderState state(&device);
  glm::mat4 projection =
      glm::ortho(-320.F, 320.F, -240.F, 240.F, -1000.F, 1000.F);
  ch::Timeline timeline;
  gleditor::FrameContext ctx{
      .state          = state,
      .viewProjection = projection,
      .screenWidth    = 640,
      .screenHeight   = 480,
      .timeline       = timeline,
      .metrics        = {.screenWidth = 640, .screenHeight = 480}};
  xudu::SatelloidOverlay sat({});
  sat.deviceReady(device, {});
  xudu::CellSatelloid card;
  card.cellRef      = 42;
  card.text         = full;
  card.dimName      = "d.long_dimension_with_identifying_suffix";
  card.alpha        = 1;
  card.active       = true;
  card.neighborhood = {
      {42, full, {0, 0}, 0}, {43, full, {1, 0}, 1}, {44, full, {0, 1}, 1}};
  sat.setSatelloid(card);
  unsigned navigations = 0;
  sat.setNavigationCallback([&](zigzag::CellRef cell, bool) {
    EXPECT_EQ(cell, 42U);
    ++navigations;
  });
  state.beginPickScene();
  sat.drawFrame(ctx);
  ASSERT_EQ(sat.snapshots().size(), 1U);
  fitted(*sat.snapshots().front());
  ASSERT_NE(sat.snapshots().front()->find(100), nullptr);
  EXPECT_FALSE(
      sat.snapshots().front()->find(100)->fitted.shaping.glyphs.empty());
  ASSERT_FALSE(device.drawn.empty());
  auto tag    = render::unpackPickingTag(device.identities.front(),
                                         device.drawn.front().paper & 65535U, 0);
  tag.kind    = render::tagKindOverlay;
  auto widget = render::resolveOverlayWidget(state.overlayPickScene, tag);
  ASSERT_TRUE(widget);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  sat.describe(builder);
  auto node = tree.find(builder.id(*widget));
  ASSERT_TRUE(node);
  EXPECT_NE(node->description.find(full), std::string::npos);
  ASSERT_TRUE(node->bounds);
  EXPECT_TRUE(sat.picked({.tag = tag, .overlayWidgetId = widget}, state));
  EXPECT_EQ(navigations, 1U);
  EXPECT_TRUE(sat.performAction(builder.id(*widget),
                                gleditor::a11y::Action::Click, {}));
  state.beginPickScene();
  sat.drawFrame(ctx);
  EXPECT_EQ(navigations, 2U);
  sat.clear();
  EXPECT_TRUE(sat.snapshots().empty());
  EXPECT_TRUE(sat.picked({.tag = tag, .overlayWidgetId = widget}, state));
  EXPECT_EQ(navigations, 2U);
}

TEST(WorldPresentationTest,
     CommandsRunOnTheConfiguredOwnerBeforeMutatingTheSlice) {
  auto viz = std::make_shared<zigzag::ZigzagVisualizer>("");
  gleditor::CommandTable commands;
  std::vector<std::function<void()>> queued;
  zigzag::registerZigzagCommands(
      commands, viz, {.dispatch = [&](std::function<void()> action) {
        queued.push_back(std::move(action));
      }});
  const auto before = viz->operationCount();
  EXPECT_TRUE(commands.run(xanadu::settings::kKeymapInsertCellXPos));
  EXPECT_EQ(viz->operationCount(), before);
  ASSERT_EQ(queued.size(), 1U);
  queued.front()();
  EXPECT_GT(viz->operationCount(), before);
}

} // namespace
