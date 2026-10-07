#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../lib/mocks/device.hpp"
#include "xudu/overview_overlay.hpp"
#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text_source.hpp>

namespace {
TEST(OverviewOverlayTest, ScalesAndClampsConfiguredPanelToSafeArea) {
  xanadu::OverviewConfig config;
  config.widthPx  = 5000;
  config.heightPx = 4000;
  config.leftPx   = -100;
  config.bottomPx = 2000;
  for (const auto size :
       {std::pair{640, 480}, std::pair{1280, 800}, std::pair{2560, 1440}}) {
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F}) {
      gleditor::ui::UiMetrics metrics{.contentScale = scale,
                                      .screenWidth  = size.first,
                                      .screenHeight = size.second,
                                      .chrome = {.top = 80, .bottom = 60}};
      const auto safe = metrics.pixelSafeArea();
      const auto box  = xudu::OverviewOverlay::panelBounds(config, metrics);
      EXPECT_GE(box.left, safe.left);
      EXPECT_GE(box.bottom, safe.bottom);
      EXPECT_LE(box.left + box.width, safe.left + safe.width);
      EXPECT_LE(box.bottom + box.height, safe.bottom + safe.height);
    }
  }
}

class OverviewDevice : public testing::NiceMock<MockRenderDevice> {
public:
  OverviewDevice() {
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
TEST(OverviewOverlayTest, RetainsGeometryAndRejectsStaleAndHiddenPicks) {
  OverviewDevice device;
  const auto app = std::make_shared<AppState>();
  app->view.pos  = {0, 0, 500};
  auto renderer  = Renderer::create(app, render::Backend::OpenGL);
  RenderState state(&device);
  auto doc = Doc::create(renderer, &device, glm::mat4{1},
                         gleditor::MemoryTextSource("Overview test"));
  doc->makePages(state);
  std::ignore = doc->buildPendingPages(state);
  state.docs.push_back(doc);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 640, 480, timeline};
  xudu::OverviewOverlay overlay(app);
  overlay.deviceReady(device, {});
  state.beginPickScene();
  overlay.drawFrame(context);
  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, 1, 0);
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 3);
  overlay.describe(builder);
  const auto node = tree.find(builder.id(xudu::OverviewOverlay::kTagOverview));
  ASSERT_TRUE(node);
  ASSERT_TRUE(node->bounds);
  auto metrics         = context.metrics;
  metrics.screenWidth  = 640;
  metrics.screenHeight = 480;
  const auto bounds    = xudu::OverviewOverlay::panelBounds({}, metrics);
  EXPECT_DOUBLE_EQ(node->bounds->left, bounds.left);
  EXPECT_DOUBLE_EQ(node->bounds->top, 480 - bounds.bottom - bounds.height);
  EXPECT_CALL(device, updateBuffer).Times(0);
  gleditor::text::ShapingStatsScope warm;
  const auto revision = overlay.accessibilityRevision();
  for (int frame = 0; frame < 50; ++frame) {
    state.beginPickScene();
    overlay.drawFrame(context);
  }
  EXPECT_EQ(warm.stats(), gleditor::text::ShapingStats{});
  EXPECT_EQ(overlay.accessibilityRevision(), revision);
  testing::Mock::VerifyAndClearExpectations(&device);
  auto moved = doc->getModel();
  moved[3].x += 100;
  doc->setModel(moved);
  state.beginPickScene();
  overlay.drawFrame(context);
  EXPECT_GT(overlay.accessibilityRevision(), revision);
  context.chrome.top = 100;
  state.beginPickScene();
  overlay.drawFrame(context);
  const auto before = app->view.pos;
  EXPECT_TRUE(overlay.picked(pick, state));
  EXPECT_EQ(app->view.pos, before);
  overlay.toggle();
  EXPECT_FALSE(overlay.picked(pick, state));
  EXPECT_FALSE(
      overlay.performAction(builder.id(xudu::OverviewOverlay::kTagOverview),
                            gleditor::a11y::Action::Click, {}));
}
} // namespace
