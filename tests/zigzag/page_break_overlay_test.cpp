#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../lib/mocks/device.hpp"
#include <gleditor/render_state.hpp>

#include "common/ui/page_break_presentation.hpp"
#include "xudu/page_break_overlay.hpp"
#include "xudu/session.hpp"
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text_source.hpp>
#include <tuple>

namespace {
using namespace gleditor::ui;

void contained(Rect child, Rect parent) {
  EXPECT_GE(child.left, parent.left);
  EXPECT_GE(child.bottom, parent.bottom);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + 0.01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + 0.01F);
}

TEST(PageBreakPresentationTest, constrainedLabelsAndAccessibilityShareBounds) {
  xanadu::PageBreakPresentation overlay;
  overlay.select(1);
  for (const auto size : {Size{640, 480}, Size{1280, 800}, Size{2560, 1440}}) {
    for (const auto scale : {0.8F, 1.0F, 1.5F, 2.0F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        UiMetrics metrics{.fontScale    = scale,
                          .screenWidth  = static_cast<int>(size.width),
                          .screenHeight = static_cast<int>(size.height),
                          .chrome       = {.top = 30, .bottom = 20}};
        Theme theme;
        theme.fonts[static_cast<std::size_t>(FontRole::Label)].family = family;
        for (const auto gap :
             {Rect{-100, -50, 200, 0},
              Rect{size.width - 10, size.height + 10, 100, 0}}) {
          const auto scene = overlay.prepareGap(gap, metrics, theme);
          ASSERT_EQ(scene->visuals.size(), 1U);
          const auto &visual = scene->visuals.front();
          const auto *box    = scene->layout.find(visual.id);
          ASSERT_NE(box, nullptr);
          contained(box->rect, metrics.pixelSafeArea());
          contained(box->contentRect, box->rect);
          EXPECT_LE(visual.fitted.widthPx, box->contentRect.width + 0.01F);
          EXPECT_LE(visual.fitted.heightPx, box->contentRect.height + 0.01F);
          gleditor::a11y::Tree tree;
          gleditor::a11y::Builder builder(tree, 12);
          overlay.describe(builder);
          const auto node = tree.find(builder.id(visual.id));
          ASSERT_TRUE(node);
          EXPECT_EQ(node->label, "+ Split to New Page (Ctrl+Ret)");
          ASSERT_TRUE(node->bounds);
          EXPECT_DOUBLE_EQ(node->bounds->left, box->rect.left);
          EXPECT_DOUBLE_EQ(node->bounds->top, metrics.screenHeight -
                                                  box->rect.bottom -
                                                  box->rect.height);
          EXPECT_DOUBLE_EQ(node->bounds->right,
                           box->rect.left + box->rect.width);
          EXPECT_DOUBLE_EQ(node->bounds->bottom,
                           metrics.screenHeight - box->rect.bottom);
        }
      }
    }
  }
}

TEST(PageBreakPresentationTest, intrinsicButtonShowsItsCompleteLabel) {
  xanadu::PageBreakPresentation overlay;
  overlay.select(1);
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  const auto scene = overlay.prepareGap({0, 400, 1280, 0}, metrics, Theme{});
  ASSERT_EQ(scene->visuals.size(), 1U);
  EXPECT_FALSE(scene->visuals.front().fitted.truncated);
  EXPECT_EQ(scene->visuals.front().fitted.visibleBytes,
            scene->visuals.front().text.size());
}

TEST(PageBreakPresentationTest, unchangedPlacementDoesNoShaping) {
  xanadu::PageBreakPresentation overlay;
  overlay.select(1);
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  const Rect gap{100, 200, 400, 0};
  const auto first    = overlay.prepareGap(gap, metrics, theme);
  const auto revision = overlay.layoutRevision();
  gleditor::text::ShapingStatsScope shaping;
  for (int frame = 0; frame < 100; ++frame)
    EXPECT_EQ(overlay.prepareGap(gap, metrics, theme), first);
  EXPECT_EQ(overlay.layoutRevision(), revision);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST(PageBreakPresentationTest, oldTargetIdentityCannotSplitCurrentParagraph) {
  xanadu::PageBreakPresentation overlay;
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  std::vector<WidgetId> activated;
  overlay.setActionHandler(
      [&](const auto &action) { activated.push_back(action.id); });
  overlay.select(1);
  const auto old   = overlay.prepareGap({100, 200, 400, 0}, metrics, theme);
  const auto oldId = old->visuals.front().id;
  overlay.select(2);
  const auto current   = overlay.prepareGap({100, 200, 400, 0}, metrics, theme);
  const auto currentId = current->visuals.front().id;
  ASSERT_NE(currentId, oldId);
  EXPECT_FALSE(overlay.performAction(gleditor::a11y::Ids::of(12, oldId),
                                     gleditor::a11y::Action::Click, {}));
  EXPECT_TRUE(activated.empty());
  EXPECT_TRUE(overlay.performAction(gleditor::a11y::Ids::of(12, currentId),
                                    gleditor::a11y::Action::Click, {}));
  ASSERT_EQ(activated.size(), 1U);
  EXPECT_EQ(activated.front(), currentId);
  overlay.select(std::nullopt);
  EXPECT_FALSE(overlay.performAction(gleditor::a11y::Ids::of(12, currentId),
                                     gleditor::a11y::Action::Click, {}));
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 12);
  overlay.describe(builder);
  EXPECT_TRUE(tree.nodes.empty());
}

TEST(PageBreakPresentationTest, warmDrawingAndCapturedPicksUseRetainedScene) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device, createTextureArray)
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  ON_CALL(device, createPipeline)
      .WillByDefault(testing::Return(render::PipelineHandle{1}));
  RenderState state(&device);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  gleditor::FrameContext ctx{.state          = state,
                             .viewProjection = projection,
                             .screenWidth    = 640,
                             .screenHeight   = 480,
                             .timeline       = timeline,
                             .metrics        = metrics,
                             .theme          = theme};
  xanadu::PageBreakPresentation overlay;
  overlay.select(1);
  overlay.deviceReady(device, {});
  std::ignore = overlay.prepareGap({100, 200, 400, 0}, metrics, theme);
  state.beginPickScene();
  overlay.drawPrepared(ctx);
  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, 1, 0);
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  ASSERT_TRUE(pick.overlayWidgetId);
  std::vector<WidgetId> activated;
  overlay.setActionHandler(
      [&](const auto &action) { activated.push_back(action.id); });
  const auto revision = overlay.layoutRevision();
  EXPECT_CALL(device, updateBuffer).Times(0);
  gleditor::text::ShapingStatsScope shaping;
  for (int frame = 0; frame < 50; ++frame) {
    state.beginPickScene();
    std::ignore = overlay.prepareGap({100, 200, 400, 0}, metrics, theme);
    overlay.drawPrepared(ctx);
  }
  EXPECT_EQ(overlay.layoutRevision(), revision);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  testing::Mock::VerifyAndClearExpectations(&device);
  overlay.select(2);
  std::ignore = overlay.prepareGap({100, 200, 400, 0}, metrics, theme);
  state.beginPickScene();
  overlay.drawPrepared(ctx);
  EXPECT_TRUE(overlay.picked(pick, state));
  EXPECT_TRUE(activated.empty());
}

TEST(PageBreakOverlayTest, scaledButtonKeepsTheOriginalTargetUnderItsPointer) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device, createTextureArray)
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  ON_CALL(device, createPipeline)
      .WillByDefault(testing::Return(render::PipelineHandle{1}));
  RenderState state(&device);
  const auto app = std::make_shared<AppState>();
  auto renderer  = Renderer::create(app, render::Backend::OpenGL);
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  const std::string text = "First paragraph.\nSecond paragraph.";
  auto &store            = session.store();
  const auto head        = store.insert({}, 0, text);
  session.views().push_back({.version = head, .pieces = store.rebuild(head)});
  auto doc = Doc::create(renderer, &device, glm::mat4{1},
                         gleditor::MemoryTextSource(text));
  doc->makePages(state);
  std::ignore = doc->buildPendingPages(state);
  state.docs.push_back(doc);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  const UiMetrics metrics{
      .fontScale = 2, .screenWidth = 640, .screenHeight = 480};
  gleditor::FrameContext frame{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = 640,
                               .screenHeight   = 480,
                               .timeline       = timeline,
                               .metrics        = metrics};
  xanadu::PageBreakOverlay overlay(session, renderer);
  overlay.deviceReady(device, {});
  overlay.setSampleForceVisible(true);
  overlay.drawFrame(frame);
  ASSERT_TRUE(overlay.isHovering());
  const auto offset = overlay.hoverCharOffset();
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 17);
  overlay.describe(builder);
  ASSERT_EQ(tree.nodes.size(), 1U);
  const auto &node = tree.nodes.front();
  ASSERT_TRUE(node.bounds);
  app->mouseX = static_cast<int>((node.bounds->left + node.bounds->right) / 2);
  app->mouseY = static_cast<int>(node.bounds->top + 2);
  int splits  = 0;
  overlay.setOnSplit([&](std::uint32_t document, std::uint32_t at) {
    EXPECT_EQ(document, 0U);
    EXPECT_EQ(at, offset);
    ++splits;
  });
  overlay.setSampleForceVisible(false);
  EXPECT_TRUE(
      overlay.performAction(node.id, gleditor::a11y::Action::Click, {}));
  overlay.drawFrame(frame);
  EXPECT_TRUE(overlay.isHovering());
  EXPECT_EQ(overlay.hoverCharOffset(), offset);
  EXPECT_EQ(splits, 1);
}

TEST(PageBreakPresentationTest, explicitLegacyFontScalesWithMetrics) {
  xanadu::PageBreakPresentation overlay;
  overlay.select(1);
  UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  Theme theme;
  const auto first =
      overlay.prepareGap({100, 400, 900, 0}, metrics, theme, "Serif 10");
  const auto firstHeight = first->visuals.front().font->metrics().lineHeight;
  metrics.fontScale      = 2;
  const auto larger =
      overlay.prepareGap({100, 400, 900, 0}, metrics, theme, "Serif 10");
  EXPECT_GT(larger->visuals.front().font->metrics().lineHeight, firstHeight);
  EXPECT_NE(larger, first);
}
} // namespace
