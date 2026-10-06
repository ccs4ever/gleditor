#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gleditor/render_state.hpp>

#include "mocks/device.hpp"

#include <gleditor/text/diagnostics.hpp>
#include <gleditor/ui/overlay.hpp>

#include <array>
#include <vector>

namespace {
using namespace gleditor::ui;
using gleditor::Key;
using gleditor::KeyMods;

Widget focusPanel() {
  Widget panel{.id = 1, .model = Modal{"Managed dialog"}};
  Widget buttons{.id = 2, .model = ButtonFlow{}};
  buttons.children = {
      {.id = 10, .model = Button{"First", "first"}},
      {.id = 11, .model = Button{"Disabled", "disabled", false}},
      {.id = 12, .model = Button{"Accept", "accept"}, .defaultAction = true},
  };
  panel.children = {
      buttons,
      {.id    = 20,
       .model = TextField{.value       = "initial",
                          .placeholder = "Name",
                          .action      = "edit",
                          .caret       = 7}},
      {.id = 30,
       .model =
           Scrubber{.label = "Position", .action = "position", .value = 0.5}},
  };
  return panel;
}

TEST(ScreenOverlayFocusTest, replacingFocusedControlWithLabelClearsFocusDraw) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device, createTextureArray)
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  ON_CALL(device, resizeBuffer)
      .WillByDefault(
          [](render::BufferHandle buffer, std::size_t) { return buffer; });
  ON_CALL(device, createPipeline)
      .WillByDefault(testing::Return(render::PipelineHandle{1}));
  std::uint32_t drawn = 0;
  ON_CALL(device, drawGlyphs)
      .WillByDefault([&](const render::DrawUniforms &, render::BufferHandle,
                         std::size_t, std::uint32_t count) { drawn += count; });
  RenderState state{&device};
  glm::mat4 projection{1.0F};
  ch::Timeline timeline;
  gleditor::FrameContext context{.state          = state,
                                 .viewProjection = projection,
                                 .screenWidth    = 640,
                                 .screenHeight   = 480,
                                 .timeline       = timeline};
  ScreenOverlay overlay({.id = 1, .model = Button{"", "activate"}});
  overlay.deviceReady(device, {});
  static_cast<void>(
      overlay.prepare({.screenWidth = 640, .screenHeight = 480}, Theme{}));
  FocusManager manager;
  auto registration = manager.registerScope(overlay);
  ASSERT_EQ(manager.focusedNode(), 1U);
  overlay.drawFrame(context);
  EXPECT_EQ(drawn, 5U); // One control surface and four focus edges.

  overlay.setModel({.id = 1, .model = Label{""}});
  static_cast<void>(
      overlay.prepare({.screenWidth = 640, .screenHeight = 480}, Theme{}));
  EXPECT_FALSE(manager.focusedNode());
  drawn = 0;
  overlay.drawFrame(context);
  EXPECT_EQ(drawn, 0U);
}

TEST(ScreenOverlayFocusTest, tabOrderEqualsRetainedLayoutAcrossSizesAndScales) {
  for (const auto size :
       std::array<Size, 3>{{{640, 360}, {1280, 720}, {2560, 1440}}}) {
    for (const auto content : {1.0F, 1.25F, 2.0F}) {
      for (const auto font : {0.8F, 1.0F, 1.5F, 2.0F}) {
        SCOPED_TRACE(std::to_string(size.width) + "/" +
                     std::to_string(content) + "/" + std::to_string(font));
        ScreenOverlay overlay(focusPanel());
        const UiMetrics metrics{.contentScale = content,
                                .fontScale    = font,
                                .screenWidth  = static_cast<int>(size.width),
                                .screenHeight = static_cast<int>(size.height)};
        const auto scene = overlay.prepare(metrics, Theme{});
        FocusManager manager;
        auto handle = manager.registerScope(overlay);
        ASSERT_FALSE(scene->layout.focusOrder.empty());
        EXPECT_EQ(manager.focusedNode(), scene->layout.focusOrder.front());
        for (const auto id : scene->layout.focusOrder) {
          EXPECT_NE(id, 11U);
          const auto *box = scene->layout.find(id);
          ASSERT_NE(box, nullptr);
          EXPECT_GT(box->rect.width, 0);
          EXPECT_GT(box->rect.height, 0);
        }
        for (std::size_t i = 1; i <= scene->layout.focusOrder.size(); ++i) {
          ASSERT_TRUE(manager.dispatchKey({.key = Key::Tab}));
          EXPECT_EQ(
              manager.focusedNode(),
              scene->layout.focusOrder[i % scene->layout.focusOrder.size()]);
        }
        ASSERT_TRUE(
            manager.dispatchKey({.key = Key::Tab, .mods = KeyMods::Shift}));
        EXPECT_EQ(manager.focusedNode(), scene->layout.focusOrder.back());
      }
    }
  }
}

TEST(ScreenOverlayFocusTest, defaultAndExplicitInitialFocusUseWidgetMetadata) {
  ScreenOverlay overlay(focusPanel());
  static_cast<void>(
      overlay.prepare({.screenWidth = 960, .screenHeight = 720}, Theme{}));
  FocusManager manager;
  auto handle =
      manager.registerScope(overlay, {.initial = FocusTarget::DefaultAction});
  EXPECT_EQ(manager.focusedNode(), 12U);
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Return}));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().action, "accept");
  handle.reset();
  auto explicitHandle = manager.registerScope(
      overlay, {.initial = FocusTarget::ExplicitNode, .initialNode = 20});
  EXPECT_EQ(manager.focusedNode(), 20U);
}

TEST(ScreenOverlayFocusTest,
     onlyFocusedTextFieldsReceiveComposedTextAndImeArea) {
  ScreenOverlay overlay(focusPanel());
  const UiMetrics metrics{.screenWidth = 960, .screenHeight = 720};
  const auto scene = overlay.prepare(metrics, Theme{});
  FocusManager manager;
  auto handle = manager.registerScope(overlay);
  EXPECT_FALSE(manager.textArea());
  ASSERT_TRUE(manager.dispatchText("not a button label"));
  EXPECT_EQ(overlay.snapshot(), scene);
  ASSERT_TRUE(manager.focusNode(20));
  const auto *box = scene->layout.find(20);
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(manager.textArea(),
            toInputArea(box->contentRect, metrics.screenHeight));
  ASSERT_TRUE(manager.dispatchText(" + é"));
  const auto typed = overlay.prepare(metrics, Theme{});
  EXPECT_EQ(typed->find(20)->value, "initial + é");
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Left}));
  EXPECT_EQ(manager.focusedNode(), 20U);
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Backspace}));
  EXPECT_EQ(overlay.prepare(metrics, Theme{})->find(20)->value, "initial +é");
  ASSERT_TRUE(manager.focusNode(10));
  EXPECT_FALSE(manager.textArea());
}

TEST(ScreenOverlayFocusTest, arrowsMoveWithinButtonGroupsAndSpaceActivates) {
  ScreenOverlay overlay(focusPanel());
  static_cast<void>(
      overlay.prepare({.screenWidth = 960, .screenHeight = 720}, Theme{}));
  FocusManager manager;
  auto handle = manager.registerScope(overlay);
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Right}));
  EXPECT_EQ(manager.focusedNode(), 12U);
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Space}));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.front().id, 12U);
  ASSERT_TRUE(manager.focusNode(30));
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Right}));
  EXPECT_EQ(manager.focusedNode(), 30U);
  EXPECT_GT(std::stod(actions.back().value), 0.5);
}

TEST(ScreenOverlayFocusTest,
     pointerFocusAndCaptureUseThePublishedPixelGeometry) {
  ScreenOverlay overlay(focusPanel());
  const UiMetrics metrics{.screenWidth = 960, .screenHeight = 720};
  const auto scene = overlay.prepare(metrics, Theme{});
  FocusManager manager;
  auto handle           = manager.registerScope(overlay);
  std::size_t activated = 0;
  overlay.setActionHandler([&](const auto &) { ++activated; });
  const auto *box = scene->layout.find(12);
  ASSERT_NE(box, nullptr);
  const auto x = box->rect.left + box->rect.width * 0.5F;
  const auto y = static_cast<float>(metrics.screenHeight) - box->rect.bottom -
                 box->rect.height * 0.5F;
  ASSERT_TRUE(manager.dispatchPointer({.phase     = PointerPhase::Press,
                                       .button    = 1,
                                       .x         = x,
                                       .y         = y,
                                       .pointerId = 7}));
  EXPECT_EQ(manager.focusedNode(), 12U);
  EXPECT_EQ(activated, 0U);
  ASSERT_TRUE(manager.dispatchPointer({.phase     = PointerPhase::Release,
                                       .button    = 1,
                                       .x         = x,
                                       .y         = y,
                                       .pointerId = 7}));
  EXPECT_EQ(activated, 1U);
  ASSERT_TRUE(manager.dispatchPointer({.phase     = PointerPhase::Press,
                                       .button    = 1,
                                       .x         = x,
                                       .y         = y,
                                       .pointerId = 7}));
  manager.focusLost();
  static_cast<void>(manager.dispatchPointer({.phase     = PointerPhase::Release,
                                             .button    = 1,
                                             .x         = x,
                                             .y         = y,
                                             .pointerId = 7}));
  EXPECT_EQ(activated, 1U);
}

TEST(ScreenOverlayFocusTest,
     accessibilityFocusRequestsAreValidatedByTheManager) {
  ScreenOverlay overlay(focusPanel());
  static_cast<void>(
      overlay.prepare({.screenWidth = 960, .screenHeight = 720}, Theme{}));
  FocusManager manager;
  auto handle = manager.registerScope(overlay);
  ASSERT_TRUE(overlay.performAction(20, gleditor::a11y::Action::Focus, {}));
  EXPECT_EQ(manager.focusedNode(), 20U);
  EXPECT_FALSE(overlay.performAction(11, gleditor::a11y::Action::Focus, {}));
  EXPECT_EQ(manager.focusedNode(), 20U);
  overlay.requestFocus(9999);
  EXPECT_EQ(manager.focusedNode(), 20U);
}

TEST(ScreenOverlayFocusTest, focusChangesKeepTheLayoutAndShapingRetained) {
  ScreenOverlay overlay(focusPanel());
  const UiMetrics metrics{.screenWidth = 960, .screenHeight = 720};
  const Theme theme;
  const auto scene    = overlay.prepare(metrics, theme);
  const auto revision = overlay.layoutRevision();
  FocusManager manager;
  auto handle = manager.registerScope(overlay);
  gleditor::text::ShapingStatsScope capture;
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(manager.dispatchKey({.key = Key::Tab}));
    EXPECT_EQ(overlay.prepare(metrics, theme), scene);
  }
  EXPECT_EQ(overlay.layoutRevision(), revision);
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
}

TEST(ScreenOverlayFocusTest,
     escapeHidesTopOverlayAndRestoresThePriorScopeAndNode) {
  ScreenOverlay underlying(focusPanel()), dialog(focusPanel());
  const UiMetrics metrics{.screenWidth = 960, .screenHeight = 720};
  static_cast<void>(underlying.prepare(metrics, Theme{}));
  static_cast<void>(dialog.prepare(metrics, Theme{}));
  FocusManager manager;
  auto pane = manager.registerScope(underlying, {.modal = false});
  ASSERT_TRUE(manager.focusNode(20));
  auto modal =
      manager.registerScope(dialog, {.initial = FocusTarget::DefaultAction});
  EXPECT_EQ(manager.focusedScope(), &dialog);
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Escape}));
  EXPECT_FALSE(dialog.visible());
  EXPECT_EQ(manager.focusedScope(), &underlying);
  EXPECT_EQ(manager.focusedNode(), 20U);
}
} // namespace
