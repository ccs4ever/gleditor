#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <set>

#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

#include "../lib/mocks/device.hpp"
#include "common/ui/store_object_manager.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
namespace ui = gleditor::ui;
void contains(ui::Rect child, ui::Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}
TEST(StoreObjectManagerOverlayTest, FittedRowsShareSafeGeometryAndFullLabels) {
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  auto head =
      store.sliceGenesis({}, "A very long slice identity 名称 with detail");
  std::ignore =
      store.makeXanadoc(head, "Document with a very long 名称 and suffix");
  xanadu::StoreObjectManager manager(store, {}, {}, {}, {},
                                     [](std::uint32_t) { return true; });
  manager.setVisible(true);
  for (auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{2560, 1440}})
    for (auto scale : {.8F, 1.F, 1.5F, 2.F})
      for (auto family : {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(testing::Message()
                     << size.width << ' ' << scale << ' ' << family);
        ui::UiMetrics metrics{.fontScale    = scale,
                              .screenWidth  = static_cast<int>(size.width),
                              .screenHeight = static_cast<int>(size.height),
                              .chrome       = {.top = 44, .bottom = 18}};
        ui::Theme theme;
        theme.fonts[static_cast<std::size_t>(ui::FontRole::Label)].family =
            family;
        const auto scene = manager.prepare(metrics, theme);
        ASSERT_TRUE(scene);
        contains(scene->layout.bounds, metrics.pixelSafeArea());
        gleditor::a11y::Tree tree;
        gleditor::a11y::Builder builder(tree, 32);
        manager.describe(builder);
        bool foundName   = false;
        bool visibleName = false;
        std::set<std::string> visibleControls;
        for (const auto &box : scene->layout.boxes) {
          contains(box.rect, scene->layout.bounds);
          contains(box.contentRect, box.rect);
          if (box.parentId) {
            const auto *parent = scene->layout.find(box.parentId);
            ASSERT_NE(parent, nullptr);
            contains(box.rect, parent->rect);
          }
          const auto *visual = scene->find(box.id);
          if (!visual) continue;
          EXPECT_LE(visual->fitted.widthPx, box.contentRect.width + .01F);
          EXPECT_LE(visual->fitted.heightPx, box.contentRect.height + .01F);
          const auto node = tree.find(builder.id(box.id));
          ASSERT_TRUE(node);
          EXPECT_EQ(node->label, visual->accessibleLabel);
          ASSERT_TRUE(node->bounds);
          EXPECT_DOUBLE_EQ(node->bounds->left, box.rect.left);
          EXPECT_DOUBLE_EQ(node->bounds->top,
                           size.height - box.rect.bottom - box.rect.height);
          if (visual->action == "create-slice" ||
              visual->action == "create-xanadoc" ||
              visual->action == "close-drawer") {
            EXPECT_FALSE(visual->fitted.shaping.glyphs.empty());
            EXPECT_GT(visual->fitted.lines, 0U);
            EXPECT_GT(visual->fitted.visibleBytes, 0U);
            visibleControls.insert(visual->action);
          }
          if (visual->action == "toggle") {
            visibleName |= !visual->fitted.shaping.glyphs.empty();
            foundName = true;
            EXPECT_NE(visual->accessibleLabel.find("名称"), std::string::npos);
            if (box.contentRect.height >= visual->font->metrics().lineHeight)
              EXPECT_FALSE(visual->fitted.shaping.glyphs.empty());
          }
        }
        EXPECT_TRUE(foundName);
        EXPECT_TRUE(visibleName);
        EXPECT_EQ(visibleControls,
                  (std::set<std::string>{"create-slice", "create-xanadoc",
                                         "close-drawer"}));
      }
}

class StoreDrawerDevice : public testing::NiceMock<MockRenderDevice> {
public:
  StoreDrawerDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault(testing::Return(render::BufferHandle{1}));
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
  }
};
TEST(StoreObjectManagerOverlayTest, RetainsDrawingAndQueuesStableBirthActions) {
  StoreDrawerDevice device;
  RenderState state(&device);
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  auto head   = store.sliceGenesis({}, "Original slice");
  std::ignore = store.makeXanadoc(head, "Original document");
  std::set<std::uint32_t> opens;
  int callbacks = 0;
  xanadu::StoreObjectManager manager(
      store, {},
      [&](std::uint32_t birth, xanadu::StructureKind, bool open) {
        ++callbacks;
        if (open)
          opens.insert(birth);
        else
          opens.erase(birth);
      },
      {}, {}, [&](std::uint32_t birth) { return opens.contains(birth); });
  manager.setVisible(true);
  manager.deviceReady(device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 1280, 800, timeline};
  state.beginPickScene();
  manager.drawFrame(context);
  const auto initial = manager.prepare(
      {.screenWidth = 1280, .screenHeight = 800}, ui::defaultTheme());
  ASSERT_TRUE(initial);
  const auto target = std::ranges::find(initial->visuals, std::string{"toggle"},
                                        &ui::WidgetVisual::action);
  ASSERT_NE(target, initial->visuals.end());
  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, target->pickingId,
      0);
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  ASSERT_EQ(pick.overlayWidgetId, target->id);
  EXPECT_TRUE(manager.picked(pick, state));
  EXPECT_EQ(callbacks, 0);
  EXPECT_TRUE(manager.busy());
  std::ignore = store.renameStructure(
      store.latest(), manager.items()[0].birthOp, "Renamed slice");
  manager.refresh();
  manager.drawFrame(context);
  EXPECT_EQ(callbacks, 1);
  EXPECT_TRUE(opens.contains(manager.items()[0].birthOp));
  const auto warm = manager.shapingStats();
  gleditor::text::ShapingStatsScope counters;
  EXPECT_CALL(device, updateBuffer).Times(0);
  for (int i = 0; i < 50; ++i) manager.drawFrame(context);
  EXPECT_EQ(manager.shapingStats().misses, warm.misses);
  EXPECT_EQ(counters.stats().harfbuzzCalls, 0U);
  testing::Mock::VerifyAndClearExpectations(&device);
  EXPECT_TRUE(manager.picked(pick, state));
  manager.drawFrame(context);
  EXPECT_EQ(callbacks, 1);
  manager.setVisible(false)->setVisible(true);
  manager.drawFrame(context);
  EXPECT_TRUE(manager.picked(pick, state));
  manager.drawFrame(context);
  EXPECT_EQ(callbacks, 1);
}
TEST(StoreObjectManagerOverlayTest, ScrollAndModalFocusReachRemainingObjects) {
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  auto head = store.sliceGenesis({}, "Slice");
  for (int i = 0; i < 30; ++i)
    head = store.makeXanadoc(head, "Document " + std::to_string(i));
  xanadu::StoreObjectManager manager(store);
  manager.setVisible(true);
  const ui::UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const auto first = manager.prepare(metrics, ui::defaultTheme());
  ASSERT_TRUE(first);
  const auto firstOrder = first->layout.focusOrder;
  ui::FocusManager focus;
  manager.syncFocus(focus);
  EXPECT_TRUE(focus.modalActive());
  EXPECT_EQ(focus.focusedScope(), &manager);
  EXPECT_FALSE(focus.permitsCommand("document-edit"));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Tab, {}}));
  EXPECT_TRUE(manager.keyPressed(gleditor::Key::PageDown, {}));
  const auto second = manager.prepare(metrics, ui::defaultTheme());
  ASSERT_TRUE(second);
  EXPECT_NE(second->layout.focusOrder, firstOrder);
  const auto area = manager.pointerArea();
  ASSERT_TRUE(area);
  EXPECT_TRUE(manager.pointerEvent({.phase  = ui::PointerPhase::Wheel,
                                    .x      = static_cast<float>(area->x + 1),
                                    .y      = static_cast<float>(area->y + 1),
                                    .deltaY = -100}));
  const auto last = manager.prepare(metrics, ui::defaultTheme());
  ASSERT_TRUE(last);
  EXPECT_TRUE(std::ranges::any_of(last->visuals, [](const auto &visual) {
    return visual.accessibleLabel.find("Document 29") != std::string::npos;
  }));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Escape, {}}));
  EXPECT_FALSE(manager.isVisible());
  EXPECT_FALSE(focus.modalActive());
}
TEST(StoreObjectManagerOverlayTest,
     AccessibilityActionsRejectHiddenReopenedDrawer) {
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  int creates = 0;
  xanadu::StoreObjectManager manager(store, {}, {},
                                     [&](xanadu::StructureKind) { ++creates; });
  manager.setVisible(true);
  const ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  const auto scene = manager.prepare(metrics, ui::defaultTheme());
  ASSERT_TRUE(scene);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 72);
  manager.describe(builder);
  const auto create = std::ranges::find(
      scene->visuals, std::string{"create-slice"}, &ui::WidgetVisual::action);
  ASSERT_NE(create, scene->visuals.end());
  const auto id = builder.id(create->id);
  EXPECT_TRUE(manager.performAction(id, gleditor::a11y::Action::Click, {}));
  EXPECT_EQ(creates, 0);
  manager.setVisible(false)->setVisible(true);
  std::ignore = manager.prepare(metrics, ui::defaultTheme());
  EXPECT_FALSE(manager.performAction(id, gleditor::a11y::Action::Click, {}));
  EXPECT_FALSE(manager.busy());
}
} // namespace
