#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/floating_toolbar_3d.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text_source.hpp>

namespace {
using Toolbar = gleditor::FloatingToolbar3D;

TEST(FloatingToolbarLayoutTest,
     FlowsInsideScaledSafeAreaAndKeepsAccessibleBounds) {
  Toolbar toolbar;
  for (const auto width : {640, 1280, 2560}) {
    for (const auto fontScale : {.8F, 1.0F, 1.5F, 2.0F}) {
      gleditor::ui::UiMetrics metrics{.contentScale = 1.25F,
                                      .fontScale    = fontScale,
                                      .screenWidth  = width,
                                      .screenHeight = width * 3 / 4,
                                      .chrome       = {.top = 40}};
      const auto layout =
          toolbar.prepareLayout(metrics, gleditor::ui::defaultTheme());
      ASSERT_NE(layout, nullptr);
      const auto safe = metrics.pixelSafeArea();
      ASSERT_EQ(layout->focusOrder.size(), 19U);
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
      gleditor::text::ShapingStatsScope retained;
      EXPECT_EQ(toolbar.prepareLayout(metrics, gleditor::ui::defaultTheme()),
                layout);
      EXPECT_EQ(retained.stats(), gleditor::text::ShapingStats{});
      gleditor::a11y::Tree tree;
      gleditor::a11y::Builder builder(tree, 1);
      toolbar.describe(builder);
      const auto group = tree.find(builder.id(0x5000U));
      ASSERT_TRUE(group);
      EXPECT_EQ(group->children.size(), layout->focusOrder.size());
      for (const auto id : layout->focusOrder) {
        const auto node = tree.find(builder.id(0x5000U + id));
        ASSERT_TRUE(node);
        const auto *box = layout->find(id);
        ASSERT_NE(box, nullptr);
        ASSERT_TRUE(node->bounds);
        EXPECT_DOUBLE_EQ(node->bounds->left, box->rect.left);
        EXPECT_DOUBLE_EQ(node->bounds->top, metrics.screenHeight -
                                                box->rect.bottom -
                                                box->rect.height);
        EXPECT_FALSE(node->label.empty());
      }
    }
  }
}

TEST(FloatingToolbarLayoutTest, IgnoresUnrelatedPickingAndHiddenActions) {
  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  Toolbar toolbar;
  int dispatches = 0;
  toolbar.setActionHandler(
      [&](Toolbar::ButtonId, std::uint32_t) { ++dispatches; });
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 0x8000U;
  EXPECT_FALSE(toolbar.picked(pick, state));
  EXPECT_EQ(dispatches, 0);
  pick.tag.clusterIndex =
      static_cast<std::uint32_t>(Toolbar::ButtonId::SaveDoc);
  pick.requestId = 1;
  EXPECT_FALSE(toolbar.picked(pick, state));
  EXPECT_EQ(dispatches, 0);
  pick.requestId = 0;
  toolbar.setVisible(false);
  pick.tag.clusterIndex = static_cast<std::uint32_t>(Toolbar::ButtonId::Bold);
  EXPECT_FALSE(toolbar.picked(pick, state));
  EXPECT_FALSE(toolbar.performAction(0x5000U + pick.tag.clusterIndex,
                                     gleditor::a11y::Action::Click, {}));
  EXPECT_EQ(dispatches, 0);
}

TEST(FloatingToolbarLayoutTest,
     UnchangedFormattingNotificationsPreserveLayout) {
  Toolbar toolbar;
  const gleditor::ui::UiMetrics metrics{.screenWidth  = 1280,
                                        .screenHeight = 800};
  toolbar.setFormattingState(true, false, true, false);
  toolbar.setHeadingLevel(1);
  const auto layout =
      toolbar.prepareLayout(metrics, gleditor::ui::defaultTheme());
  const auto revision = toolbar.accessibilityRevision();
  toolbar.setFormattingState(true, false, true, false);
  toolbar.setHeadingLevel(1);
  toolbar.setActiveDocIndex(0);
  toolbar.setVisible(true);
  EXPECT_EQ(toolbar.accessibilityRevision(), revision);
  EXPECT_EQ(toolbar.prepareLayout(metrics, gleditor::ui::defaultTheme()),
            layout);
}

class ToolbarDevice : public testing::NiceMock<MockRenderDevice> {
public:
  ToolbarDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
  }
};
class FloatingToolbarRenderingTest : public testing::Test {
protected:
  ToolbarDevice device;
  AppStateRef app      = std::make_shared<AppState>();
  RendererRef renderer = Renderer::create(app, render::Backend::OpenGL);
  RenderState state{&device};
  Toolbar toolbar;
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 1280, 800, timeline};

  std::shared_ptr<Doc> document(std::string name) {
    return Doc::create(renderer, &device, glm::mat4{1},
                       gleditor::MemoryTextSource("toolbar", std::move(name)));
  }
  void draw() {
    state.beginPickScene();
    toolbar.drawFrame(context);
  }
  render::PickingResult capturedButton(Toolbar::ButtonId id) {
    render::PickingResult pick;
    pick.requestId = 1;
    if (state.overlayPickScene.widgetOverlays.empty()) return pick;
    pick.tag = render::unpackPickingTag(
        state.overlayPickScene.widgetOverlays.back().identity,
        static_cast<std::uint32_t>(id), 0);
    pick.overlayWidgetId =
        render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
    return pick;
  }
  void SetUp() override {
    ON_CALL(device, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(device, createBuffer(testing::_, testing::_))
        .WillByDefault(testing::Return(render::BufferHandle{1}));
    ON_CALL(device, createPipeline(testing::_))
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
    state.docs.push_back(document("toolbar-test"));
    toolbar.deviceReady(device, render::PipelineDesc{});
  }
};

TEST_F(FloatingToolbarRenderingTest, EveryButtonDrawsVisibleTextAtNormalSizes) {
  std::vector<std::byte> uploaded;
  ON_CALL(device, updateBuffer)
      .WillByDefault([&](render::BufferHandle, std::size_t offset,
                         std::span<const std::byte> bytes) {
        uploaded.resize(std::max(uploaded.size(), offset + bytes.size()));
        std::memcpy(uploaded.data() + offset, bytes.data(), bytes.size());
      });
  std::set<std::uint32_t> ink;
  ON_CALL(device, drawGlyphs)
      .WillByDefault([&](const render::DrawUniforms &, render::BufferHandle,
                         std::size_t offset, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
          Doc::VBORow row{};
          std::memcpy(&row, uploaded.data() + offset + i * sizeof(row),
                      sizeof(row));
          if ((row.foreground & Doc::VBORow::solidFlag) == 0)
            ink.insert(row.paper & 65535U);
        }
      });
  for (const auto width : {640, 800, 1280}) {
    context.screenWidth  = width;
    context.screenHeight = width * 3 / 4;
    ink.clear();
    draw();
    EXPECT_EQ(ink.size(), 19U) << "window width " << width;
  }
}
TEST_F(FloatingToolbarRenderingTest, UnchangedDrawFramesDoNoShaping) {
  draw();
  {
    gleditor::text::ShapingStatsScope retainedFrame;
    draw();
    EXPECT_EQ(retainedFrame.stats(), gleditor::text::ShapingStats{});
  }
  context.metrics.fontScale = 1.5F;
  {
    gleditor::text::ShapingStatsScope changedFrame;
    draw();
    EXPECT_GT(changedFrame.stats().layoutCalls, 0U);
  }
}

TEST_F(FloatingToolbarRenderingTest, RequiresCapturedTargetsFromItsOwnScope) {
  draw();
  auto pick = capturedButton(Toolbar::ButtonId::SaveDoc);
  ASSERT_TRUE(pick.overlayWidgetId);
  const auto target = pick.overlayWidgetId;
  pick.overlayWidgetId.reset();
  int dispatches = 0;
  toolbar.setActionHandler([&](Toolbar::ButtonId id, std::uint32_t doc) {
    EXPECT_EQ(id, Toolbar::ButtonId::SaveDoc);
    EXPECT_EQ(doc, 0U);
    ++dispatches;
  });
  EXPECT_TRUE(toolbar.picked(pick, state));
  EXPECT_EQ(dispatches, 0);
  pick.overlayWidgetId = target;
  EXPECT_TRUE(toolbar.picked(pick, state));
  EXPECT_EQ(dispatches, 1);
  pick.tag.docIndex = state.allocateOverlayPickScope();
  EXPECT_FALSE(toolbar.picked(pick, state));
  EXPECT_EQ(dispatches, 1);
}

TEST_F(FloatingToolbarRenderingTest, OwnerQualifiedActionsCanReenterToolbar) {
  draw();
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 27);
  toolbar.describe(builder);
  const auto group = tree.find(builder.id(0x5000U));
  ASSERT_TRUE(group);
  EXPECT_EQ(group->children.size(), 19U);
  const auto save = builder.id(
      0x5000U + static_cast<std::uint32_t>(Toolbar::ButtonId::SaveDoc));
  ASSERT_TRUE(tree.find(save));
  int dispatches = 0;
  toolbar.setActionHandler([&](Toolbar::ButtonId id, std::uint32_t doc) {
    EXPECT_EQ(id, Toolbar::ButtonId::SaveDoc);
    EXPECT_EQ(doc, 0U);
    ++dispatches;
    toolbar.setVisible(false);
    toolbar.setActionHandler({});
  });
  EXPECT_FALSE(toolbar.performAction(save, gleditor::a11y::Action::Focus, {}));
  EXPECT_TRUE(toolbar.performAction(save, gleditor::a11y::Action::Click, {}));
  EXPECT_EQ(dispatches, 1);
  EXPECT_FALSE(toolbar.isVisible());
}

TEST_F(FloatingToolbarRenderingTest, MissingDocumentsClearActionability) {
  int dispatches = 0;
  toolbar.setActionHandler(
      [&](Toolbar::ButtonId, std::uint32_t) { ++dispatches; });
  const auto saveId = gleditor::a11y::Ids::of(
      27, 0x5000U + static_cast<std::uint32_t>(Toolbar::ButtonId::SaveDoc));
  const auto unavailable = [&] {
    draw();
    EXPECT_TRUE(state.overlayPickScene.widgetOverlays.empty());
    gleditor::a11y::Tree tree;
    gleditor::a11y::Builder builder(tree, 27);
    toolbar.describe(builder);
    EXPECT_TRUE(tree.empty());
    EXPECT_FALSE(
        toolbar.performAction(saveId, gleditor::a11y::Action::Click, {}));
    render::PickingResult manual;
    manual.tag.kind = render::tagKindOverlay;
    manual.tag.clusterIndex =
        static_cast<std::uint32_t>(Toolbar::ButtonId::SaveDoc);
    EXPECT_FALSE(toolbar.picked(manual, state));
    EXPECT_EQ(dispatches, 0);
  };
  draw();
  state.docs.clear();
  unavailable();
  state.docs.push_back(nullptr);
  unavailable();
  state.docs[0] = document("closing");
  state.docs[0]->animateDeparture(timeline);
  unavailable();
  state.docs[0] = document("returned");
  draw();
  EXPECT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  EXPECT_TRUE(toolbar.performAction(saveId, gleditor::a11y::Action::Click, {}));
  EXPECT_EQ(dispatches, 1);
}

TEST_F(FloatingToolbarRenderingTest, CapturedActionsKeepTheirDocumentContext) {
  state.docs.push_back(document("second"));
  draw();
  const auto save  = capturedButton(Toolbar::ButtonId::SaveDoc);
  const auto close = capturedButton(Toolbar::ButtonId::CloseDoc);
  ASSERT_TRUE(save.overlayWidgetId);
  ASSERT_TRUE(close.overlayWidgetId);
  int dispatches = 0;
  toolbar.setActionHandler([&](Toolbar::ButtonId id, std::uint32_t doc) {
    EXPECT_EQ(id, Toolbar::ButtonId::SaveDoc);
    EXPECT_EQ(doc, 1U);
    ++dispatches;
  });
  toolbar.setActiveDocIndex(1);
  EXPECT_TRUE(toolbar.picked(save, state));
  EXPECT_TRUE(toolbar.picked(close, state));
  EXPECT_EQ(dispatches, 0);
  draw();
  EXPECT_TRUE(toolbar.picked(save, state));
  EXPECT_TRUE(toolbar.picked(close, state));
  EXPECT_EQ(dispatches, 0);
  const auto current = capturedButton(Toolbar::ButtonId::SaveDoc);
  ASSERT_TRUE(current.overlayWidgetId);
  EXPECT_TRUE(toolbar.picked(current, state));
  EXPECT_EQ(dispatches, 1);
  std::swap(state.docs[0], state.docs[1]);
  EXPECT_TRUE(toolbar.picked(current, state));
  EXPECT_EQ(dispatches, 1);
}
} // namespace
