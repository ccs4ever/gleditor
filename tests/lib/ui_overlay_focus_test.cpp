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
  EXPECT_EQ(drawn, 9U); // Surface, four control edges and four focus edges.

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

TEST(ScreenOverlayFocusTest, multilineReturnPrecedesDefaultDialogAction) {
  auto model                                             = focusPanel();
  std::get<TextField>(model.children[1].model).multiline = true;
  ScreenOverlay overlay(std::move(model));
  const UiMetrics metrics{.screenWidth = 960, .screenHeight = 720};
  static_cast<void>(overlay.prepare(metrics, Theme{}));
  FocusManager manager;
  auto handle = manager.registerScope(
      overlay, {.initial = FocusTarget::ExplicitNode, .initialNode = 20});
  std::vector<WidgetAction> actions;
  overlay.setActionHandler(
      [&](const auto &action) { actions.push_back(action); });
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Return}));
  ASSERT_EQ(actions.size(), 1U);
  EXPECT_EQ(actions.back().action, "edit");
  EXPECT_EQ(actions.back().value, "initial\n");
  ASSERT_TRUE(manager.dispatchText("second"));
  EXPECT_EQ(actions.back().value, "initial\nsecond");
  EXPECT_EQ(overlay.prepare(metrics, Theme{})->find(20)->value,
            "initial\nsecond");
  ASSERT_TRUE(manager.focusNode(12));
  ASSERT_TRUE(manager.dispatchKey({.key = Key::Return}));
  EXPECT_EQ(actions.back().action, "accept");
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

TEST(ScreenOverlayFocusTest,
     sensitiveFieldAccessibilityExposesOnlyMaskedValue) {
  const std::string secret = "token-\xC3\xA9";
  ScreenOverlay overlay({.id    = 1,
                         .model = TextField{.value       = secret,
                                            .placeholder = "Password",
                                            .caret       = secret.size(),
                                            .sensitive   = true}});
  static_cast<void>(
      overlay.prepare({.screenWidth = 960, .screenHeight = 720}, Theme{}));
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  overlay.describe(builder);
  ASSERT_FALSE(tree.nodes.empty());
  const auto node =
      std::ranges::find(tree.nodes, builder.id(1), &gleditor::a11y::Node::id);
  ASSERT_NE(node, tree.nodes.end());
  EXPECT_EQ(node->role, gleditor::a11y::Role::PasswordInput);
  EXPECT_EQ(node->label, "Password");
  EXPECT_EQ(node->value, "•••••••");
  for (const auto &entry : tree.nodes) {
    EXPECT_EQ(entry.value.find(secret), std::string::npos);
    EXPECT_EQ(entry.description.find(secret), std::string::npos);
  }
}

TEST(ScreenOverlayFocusTest,
     protectedModalInputRejectsCrLfPasteWithoutChangingTheSecret) {
  const std::string secret = "token-abc";
  ScreenOverlay overlay(
      {.id       = 1,
       .model    = Modal{"Connect"},
       .children = {{.id    = 2,
                     .model = TextField{.value       = secret,
                                        .placeholder = "Password",
                                        .action      = "secret",
                                        .caret       = secret.size(),
                                        .sensitive   = true}}}});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  static_cast<void>(overlay.prepare(metrics, Theme{}));
  std::size_t notifications = 0;
  overlay.setActionHandler([&](const auto &) { ++notifications; });

  bool accepted = true;
  EXPECT_NO_THROW(accepted = overlay.typeInto(2, "\nadditional"));
  EXPECT_FALSE(accepted);
  accepted = true;
  EXPECT_NO_THROW(accepted = overlay.typeInto(2, "\radditional"));
  EXPECT_FALSE(accepted);
  const auto scene = overlay.prepare(metrics, Theme{});
  ASSERT_NE(scene->find(2), nullptr);
  EXPECT_EQ(scene->find(2)->value, "•••••••••");
  EXPECT_EQ(notifications, 0U);
}

TEST(ScreenOverlayFocusTest,
     multilineFieldAccessibilityExposesItsCompleteEditableValue) {
  const std::string value = "recipient@example.test\nsecond@example.test";
  ScreenOverlay overlay({.id        = 1,
                         .model     = TextField{.value       = value,
                                                .placeholder = "Recipients",
                                                .caret       = value.size(),
                                                .multiline   = true},
                         .preferred = {300, 100}});
  static_cast<void>(
      overlay.prepare({.screenWidth = 960, .screenHeight = 720}, Theme{}));
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 16);
  overlay.describe(builder);
  const auto node =
      std::ranges::find(tree.nodes, builder.id(1), &gleditor::a11y::Node::id);
  ASSERT_NE(node, tree.nodes.end());
  EXPECT_EQ(node->role, gleditor::a11y::Role::TextInput);
  EXPECT_EQ(node->label, "Recipients");
  EXPECT_EQ(node->value, value);
}

TEST(ScreenOverlayFocusTest,
     multilineWheelScrollIsReportedAndSurvivesPanelModelReplacement) {
  const std::string value =
      "one two three four five six seven eight nine ten eleven twelve "
      "thirteen fourteen fifteen sixteen seventeen eighteen";
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  ScreenOverlay overlay({.id        = 1,
                         .model     = TextField{.value       = value,
                                                .placeholder = "Body",
                                                .caret       = value.size(),
                                                .multiline   = true},
                         .preferred = {180, 72}});
  const auto initial = overlay.prepare(metrics, Theme{});
  ASSERT_NE(initial->find(1), nullptr);
  ASSERT_NE(initial->layout.find(1), nullptr);
  ASSERT_GT(initial->find(1)->verticalTextOffsetPx, 0.0F);
  const auto originalScroll = initial->find(1)->verticalTextOffsetPx;
  const auto *box           = initial->layout.find(1);
  const auto x              = box->rect.left + box->rect.width * 0.5F;
  const auto y = static_cast<float>(metrics.screenHeight) - box->rect.bottom -
                 box->rect.height * 0.5F;
  std::optional<WidgetAction> action;
  overlay.setActionHandler([&](const auto &update) { action = update; });
  ASSERT_TRUE(overlay.pointerEvent(
      {.phase = PointerPhase::Wheel, .x = x, .y = y, .deltaY = 1.0F}));
  ASSERT_TRUE(action.has_value());
  ASSERT_TRUE(action->verticalScrollPx.has_value());
  ASSERT_TRUE(action->followCaret.has_value());
  EXPECT_FALSE(*action->followCaret);
  EXPECT_LT(*action->verticalScrollPx, originalScroll);

  overlay.setModel(
      {.id        = 1,
       .model     = TextField{.value            = action->value,
                              .placeholder      = "Body",
                              .caret            = *action->caret,
                              .multiline        = true,
                              .followCaret      = *action->followCaret,
                              .verticalScrollPx = *action->verticalScrollPx},
       .preferred = {180, 72}});
  const auto replaced = overlay.prepare(metrics, Theme{});
  ASSERT_NE(replaced->find(1), nullptr);
  EXPECT_FLOAT_EQ(replaced->find(1)->verticalTextOffsetPx,
                  *action->verticalScrollPx);
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

TEST(ScreenOverlayFocusTest, cpuPressReleaseNeverAlsoRequestsGpuActivation) {
  ScreenOverlay overlay(focusPanel());
  const UiMetrics metrics{.screenWidth = 960, .screenHeight = 720};
  const auto scene = overlay.prepare(metrics, Theme{});
  FocusManager manager;
  auto registration = manager.registerScope(overlay);
  int actions       = 0;
  overlay.setActionHandler([&](const auto &) { ++actions; });
  const auto *box = scene->layout.find(10);
  ASSERT_NE(box, nullptr);
  const float x = box->rect.left + box->rect.width * .5F;
  const float y =
      metrics.screenHeight - box->rect.bottom - box->rect.height * .5F;
  const auto down = manager.dispatchPointerWithPick(
      {.phase = PointerPhase::Press, .button = 1, .x = x, .y = y});
  EXPECT_TRUE(down.consumed);
  EXPECT_FALSE(down.gpuPick);
  EXPECT_EQ(actions, 0);
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Release, .button = 1, .x = x, .y = y}));
  EXPECT_EQ(actions, 1);
  // Releasing away cancels a button press instead of activating it on GPU
  // press.
  const auto again = manager.dispatchPointerWithPick(
      {.phase = PointerPhase::Press, .button = 1, .x = x, .y = y});
  EXPECT_FALSE(again.gpuPick);
  EXPECT_TRUE(manager.dispatchPointer(
      {.phase = PointerPhase::Release, .button = 1, .x = 0, .y = 0}));
  EXPECT_EQ(actions, 1);
}
