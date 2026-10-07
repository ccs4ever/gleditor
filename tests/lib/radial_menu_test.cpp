/**
 * @file radial_menu_test.cpp
 * @brief Unit tests for gleditor::RadialMenu.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include "mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/radial_menu.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

using namespace gleditor;

TEST(RadialMenuTest, SectorResolutionDynamicCount) {
  // Test 8-way sector resolution
  // North (0, +1) should resolve to sector 0
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 10.0F, 8), 0U);
  // East (+1, 0) should resolve to sector 2
  EXPECT_EQ(RadialMenu::resolveSector(10.0F, 0.0F, 8), 2U);
  // South (0, -1) should resolve to sector 4
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, -10.0F, 8), 4U);
  // West (-1, 0) should resolve to sector 6
  EXPECT_EQ(RadialMenu::resolveSector(-10.0F, 0.0F, 8), 6U);

  // Test 4-way sector resolution (quadrants)
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 10.0F, 4), 0U);  // North
  EXPECT_EQ(RadialMenu::resolveSector(10.0F, 0.0F, 4), 1U);  // East
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, -10.0F, 4), 2U); // South
  EXPECT_EQ(RadialMenu::resolveSector(-10.0F, 0.0F, 4), 3U); // West

  // Test 6-way sector resolution
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 10.0F, 6), 0U);
  // 60 deg clockwise: cos(30 deg), sin(30 deg)
  EXPECT_EQ(RadialMenu::resolveSector(8.66F, 5.0F, 6), 1U);
}

TEST(RadialMenuTest, OpenCloseToggleState) {
  RadialMenu menu("Sans 10");
  EXPECT_FALSE(menu.isOpen());

  menu.open(400.0F, 300.0F, 1, 10, 5);
  EXPECT_TRUE(menu.isOpen());
  EXPECT_FLOAT_EQ(menu.centerX(), 400.0F);
  EXPECT_FLOAT_EQ(menu.centerY(), 300.0F);
  EXPECT_EQ(menu.targetDocIndex(), 1U);
  EXPECT_EQ(menu.targetCharOffset(), 10U);
  EXPECT_EQ(menu.targetCharLength(), 5U);

  menu.close();
  EXPECT_FALSE(menu.isOpen());

  menu.toggle(200.0F, 150.0F, 0, 0, 0);
  EXPECT_TRUE(menu.isOpen());
  menu.toggle(200.0F, 150.0F, 0, 0, 0);
  EXPECT_FALSE(menu.isOpen());
}

TEST(RadialMenuTest, SubRadialTransition) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F);
  EXPECT_FALSE(menu.inSubRadial());

  // In default config, action 5 is "align" with 4 sub-actions
  menu.enterSubRadial(5);
  EXPECT_TRUE(menu.inSubRadial());

  menu.exitSubRadial();
  EXPECT_FALSE(menu.inSubRadial());
}

TEST(RadialMenuTest, ActionDispatchAndHandlers) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F, 2, 42, 8);

  std::string receivedId;
  std::string receivedAction;
  std::uint32_t receivedDoc    = 0;
  std::uint32_t receivedOffset = 0;
  std::uint32_t receivedLen    = 0;

  menu.setActionHandler([&](const std::string &id, const std::string &action,
                            std::uint32_t doc, std::uint32_t off,
                            std::uint32_t len) {
    receivedId     = id;
    receivedAction = action;
    receivedDoc    = doc;
    receivedOffset = off;
    receivedLen    = len;
  });

  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device,
          createTextureArray(testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer(testing::_, testing::_))
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  RenderState rState(&device);

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = RadialMenu::kRadialTagBase; // Action 0: Bold
  pick.x                = 400;
  pick.y                = 300;

  const bool handled = menu.picked(pick, rState);
  EXPECT_TRUE(handled);
  EXPECT_FALSE(menu.isOpen()); // Closes after action
  EXPECT_EQ(receivedId, "bold");
  EXPECT_EQ(receivedAction, "format:bold");
  EXPECT_EQ(receivedDoc, 2U);
  EXPECT_EQ(receivedOffset, 42U);
  EXPECT_EQ(receivedLen, 8U);
}

TEST(RadialMenuTest, AccessibilityMenuTree) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F);

  a11y::Tree tree;
  a11y::Builder builder(tree, 1);
  menu.describe(builder);

  EXPECT_FALSE(tree.empty());
  const auto group = tree.find(builder.id(0x8000U));
  ASSERT_TRUE(group);
  EXPECT_EQ(group->children.size(), menu.config().actions.size() + 1);
  for (const auto id : group->children) EXPECT_TRUE(tree.find(id));
}

TEST(RadialMenuTest, CircularHubPicking) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F);
  EXPECT_TRUE(menu.isOpen());

  testing::NiceMock<MockRenderDevice> device;
  RenderState rState(&device);

  // Pick on center hub tag closes menu
  render::PickingResult pickHub;
  pickHub.tag.kind         = render::tagKindOverlay;
  pickHub.tag.clusterIndex = RadialMenu::kRadialTagHub;
  pickHub.x                = 400;
  pickHub.y                = 300;

  EXPECT_TRUE(menu.picked(pickHub, rState));
  EXPECT_FALSE(menu.isOpen());

  // Test inside sub-radial: picking hub exits sub-radial
  menu.open(400.0F, 300.0F);
  menu.enterSubRadial(5);
  EXPECT_TRUE(menu.inSubRadial());

  render::PickingResult pickBack;
  pickBack.tag.kind         = render::tagKindOverlay;
  pickBack.tag.clusterIndex = RadialMenu::kRadialTagBack;
  pickBack.x                = 400;
  pickBack.y                = 300;

  EXPECT_TRUE(menu.picked(pickBack, rState));
  EXPECT_FALSE(menu.inSubRadial());
  EXPECT_TRUE(menu.isOpen()); // Root menu remains open
}

TEST(RadialMenuTest, WedgeGeometryAndSectorResolution) {
  // Test 10-way default menu sector resolution
  // 10 sectors, sector width = 36 degrees = 2*pi/10
  // Sector 0 is North: angle = pi/2
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 50.0F, 10), 0U);
  // Sector 1: 36 deg clockwise from North -> (cos(54 deg), sin(54 deg))
  EXPECT_EQ(RadialMenu::resolveSector(
                50.0F * std::cos(0.3F * std::numbers::pi_v<float>),
                50.0F * std::sin(0.3F * std::numbers::pi_v<float>), 10),
            1U);
  // South (0, -50) should be Sector 5 (180 deg from North)
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, -50.0F, 10), 5U);
  // A menu with no sectors has no sector to point into.
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 50.0F, 0), std::nullopt);
}

TEST(RadialMenuTest, RetainsScaledSafeGeometryAndFullAccessibleLabels) {
  RadialMenu menu;
  auto config             = RadialConfig::createDefault();
  config.actions[0].label = std::string(160, 'W');
  config.actions[0].desc  = "Full unabridged description for the radial action";
  menu.setConfig(config);
  menu.open(1, 1, 2, 42, 8);
  for (const auto width : {640, 1280, 2560}) {
    for (const auto fontScale : {.8F, 1.0F, 1.5F, 2.0F}) {
      ui::UiMetrics metrics{.contentScale = 1.25F,
                            .fontScale    = fontScale,
                            .screenWidth  = width,
                            .screenHeight = width * 3 / 4,
                            .chrome       = {.top = 40}};
      const auto layout = menu.prepareLayout(metrics, ui::defaultTheme());
      ASSERT_NE(layout, nullptr);
      const auto safe = metrics.pixelSafeArea();
      for (const auto &box : layout->boxes) {
        EXPECT_GE(box.rect.left, safe.left - .001F);
        EXPECT_GE(box.rect.bottom, safe.bottom - .001F);
        EXPECT_LE(box.rect.left + box.rect.width,
                  safe.left + safe.width + .001F);
        EXPECT_LE(box.rect.bottom + box.rect.height,
                  safe.bottom + safe.height + .001F);
        EXPECT_GE(box.contentRect.left, box.rect.left - .001F);
        EXPECT_GE(box.contentRect.bottom, box.rect.bottom - .001F);
        EXPECT_LE(box.contentRect.left + box.contentRect.width,
                  box.rect.left + box.rect.width + .001F);
        EXPECT_LE(box.contentRect.bottom + box.contentRect.height,
                  box.rect.bottom + box.rect.height + .001F);
      }
      text::ShapingStatsScope retainedFrame;
      EXPECT_EQ(menu.prepareLayout(metrics, ui::defaultTheme()), layout);
      EXPECT_EQ(retainedFrame.stats(), text::ShapingStats{});

      a11y::Tree tree;
      a11y::Builder builder(tree, 1);
      menu.describe(builder);
      constexpr auto firstAction = 0x8000U + 100U;
      const auto node            = tree.find(builder.id(firstAction));
      ASSERT_TRUE(node);
      EXPECT_EQ(node->label, config.actions[0].desc);
      const auto *box = layout->find(firstAction);
      ASSERT_NE(box, nullptr);
      ASSERT_TRUE(node->bounds);
      EXPECT_DOUBLE_EQ(node->bounds->left, box->rect.left);
      EXPECT_DOUBLE_EQ(node->bounds->top,
                       static_cast<double>(metrics.screenHeight) -
                           static_cast<double>(box->rect.bottom) -
                           static_cast<double>(box->rect.height));
    }
  }
}

TEST(RadialMenuTest, KeyboardAndAccessibilityPreserveSelectionProvenance) {
  RadialMenu menu;
  menu.open(300, 200, 2, 42, 8);
  std::string selected;
  menu.setActionHandler([&](const std::string &id, const std::string &,
                            std::uint32_t doc, std::uint32_t offset,
                            std::uint32_t length) {
    selected = id;
    EXPECT_EQ(doc, 2U);
    EXPECT_EQ(offset, 42U);
    EXPECT_EQ(length, 8U);
  });
  constexpr auto firstAction = 0x8000U + 100U;
  menu.focusedNodeChanged(firstAction);
  EXPECT_TRUE(menu.keyPressed(Key::Return, KeyMods::None));
  EXPECT_EQ(selected, "bold");
  EXPECT_FALSE(menu.isOpen());

  menu.open(300, 200, 2, 42, 8);
  a11y::Tree tree;
  a11y::Builder builder(tree, 27);
  menu.describe(builder);
  EXPECT_TRUE(
      menu.performAction(builder.id(firstAction + 5), a11y::Action::Click, {}));
  EXPECT_TRUE(menu.inSubRadial());
  EXPECT_TRUE(
      menu.performAction(builder.id(firstAction + 1), a11y::Action::Click, {}));
  EXPECT_EQ(selected, "align-centre");
  EXPECT_FALSE(menu.isOpen());
}

TEST(RadialMenuTest, OwnerQualifiedActionsInvokeCallbacksOutsideStateLock) {
  RadialMenu menu;
  auto config                = menu.config();
  config.actions[0].onSelect = [&] { menu.open(150, 100, 3, 24, 6); };
  menu.setConfig(std::move(config));
  menu.open(300, 200, 2, 42, 8);
  int dispatched = 0;
  menu.setActionHandler([&](const std::string &id, const std::string &,
                            std::uint32_t doc, std::uint32_t offset,
                            std::uint32_t length) {
    EXPECT_EQ(id, "bold");
    EXPECT_EQ(doc, 2U);
    EXPECT_EQ(offset, 42U);
    EXPECT_EQ(length, 8U);
    EXPECT_TRUE(menu.isOpen());
    menu.setOpenOnRightClick(false);
    ++dispatched;
  });
  const auto action = a11y::Ids::of(27, 0x8000U + 100U);
  EXPECT_TRUE(menu.performAction(action, a11y::Action::Focus, {}));
  EXPECT_TRUE(menu.performAction(action, a11y::Action::Click, {}));
  EXPECT_EQ(dispatched, 1);
  EXPECT_TRUE(menu.isOpen());
  EXPECT_EQ(menu.targetDocIndex(), 3U);
  EXPECT_EQ(menu.targetCharOffset(), 24U);
  EXPECT_EQ(menu.targetCharLength(), 6U);
  EXPECT_FALSE(menu.openOnRightClick());
}

TEST(RadialMenuTest, DisabledActionsDoNotDispatchOrReceiveKeyboardFocus) {
  RadialMenu menu;
  auto config               = RadialConfig::createDefault();
  config.actions[0].enabled = false;
  menu.setConfig(config);
  menu.open(300, 200);
  int dispatched = 0;
  menu.setActionHandler([&](const std::string &, const std::string &,
                            std::uint32_t, std::uint32_t,
                            std::uint32_t) { ++dispatched; });
  constexpr auto firstAction = 0x8000U + 100U;
  const auto layout          = menu.focusLayout();
  ASSERT_NE(layout, nullptr);
  EXPECT_EQ(std::ranges::find(layout->focusOrder, firstAction),
            layout->focusOrder.end());
  EXPECT_FALSE(menu.performAction(firstAction, a11y::Action::Click, {}));
  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = RadialMenu::kRadialTagBase;
  EXPECT_TRUE(menu.picked(pick, state));
  EXPECT_EQ(dispatched, 0);
  EXPECT_TRUE(menu.isOpen());
}

TEST(RadialMenuTest, RightClickUsesTheClosedMenusCurrentViewport) {
  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  FrameContext context{state, projection, 640, 480, timeline};
  RadialMenu menu;
  menu.drawFrame(context);
  render::PickingResult pick;
  pick.button = 3;
  pick.x      = 150;
  pick.y      = 100;
  EXPECT_TRUE(menu.picked(pick, state));
  EXPECT_FLOAT_EQ(menu.centerX(), 150);
  EXPECT_FLOAT_EQ(menu.centerY(), 380);
}

TEST(RadialMenuTest, UnchangedDrawFramesDoNoShaping) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device,
          createTextureArray(testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer(testing::_, testing::_))
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  ON_CALL(device, createPipeline(testing::_))
      .WillByDefault(testing::Return(render::PipelineHandle{1}));
  RenderState state(&device);
  RadialMenu menu;
  menu.deviceReady(device, render::PipelineDesc{});
  menu.open(300, 200, 2, 42, 8);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  FrameContext context{state, projection, 640, 480, timeline};
  menu.drawFrame(context);
  {
    text::ShapingStatsScope retainedFrame;
    menu.drawFrame(context);
    EXPECT_EQ(retainedFrame.stats(), text::ShapingStats{});
  }
  context.metrics.fontScale = 1.5F;
  {
    text::ShapingStatsScope changedFrame;
    menu.drawFrame(context);
    EXPECT_GT(changedFrame.stats().layoutCalls, 0U);
  }

  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  const auto captured = state.overlayPickScene;
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag = render::unpackPickingTag(captured.widgetOverlays.back().identity,
                                      RadialMenu::kRadialTagBase, 0);
  pick.overlayWidgetId = render::resolveOverlayWidget(captured, pick.tag);
  ASSERT_TRUE(pick.overlayWidgetId);
  std::string selected;
  menu.setActionHandler([&](const std::string &id, const std::string &,
                            std::uint32_t doc, std::uint32_t offset,
                            std::uint32_t length) {
    selected = id;
    EXPECT_EQ(doc, 2U);
    EXPECT_EQ(offset, 42U);
    EXPECT_EQ(length, 8U);
  });
  auto reordered = menu.config();
  std::swap(reordered.actions[0], reordered.actions[1]);
  menu.setConfig(std::move(reordered));
  EXPECT_TRUE(menu.picked(pick, state));
  EXPECT_TRUE(selected.empty());
  EXPECT_TRUE(menu.isOpen());

  state.beginPickScene();
  menu.drawFrame(context);
  EXPECT_TRUE(menu.picked(pick, state));
  EXPECT_TRUE(selected.empty());
  EXPECT_TRUE(menu.isOpen());
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  ASSERT_TRUE(pick.overlayWidgetId);
  EXPECT_TRUE(menu.picked(pick, state));
  EXPECT_EQ(selected, "italic");
  EXPECT_FALSE(menu.isOpen());
}
