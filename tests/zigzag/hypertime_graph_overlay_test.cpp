#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <set>

#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

#include "../lib/mocks/device.hpp"
#include "common/ui/hypertime_graph.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
namespace ui = gleditor::ui;
using Graph  = xanadu::ui::HypertimeGraph;
void graphContains(ui::Rect child, ui::Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}
class GraphDevice : public testing::NiceMock<MockRenderDevice> {
public:
  GraphDevice() {
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
TEST(HypertimeGraphOverlayTest,
     FittedNodesAndComparisonShareSafeGeometryAndFullIdentity) {
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto first = store.insert(
      {}, 0, "Universal text with Unicode 名称 and a long description");
  const auto left  = store.insert(first, 0, "Left branch ");
  const auto right = store.insert(first, 0, "Right branch ");
  store.setVersionAnnotation(
      first,
      {.alias = "A very long operation alias 名称 with an identifying suffix"});
  GraphDevice device;
  RenderState state(&device);
  Graph graph(
      {}, [&](std::size_t) -> const xanadu::Store & { return store; },
      [&] { return static_cast<std::uint64_t>(store.opCount()); });
  graph.setVisible(true)->setCurrent(left);
  graph.toggleComparison(first)->toggleComparison(left)->toggleComparison(
      right);
  graph.deviceReady(device, {});
  graph.setAnnotateHandler([](const auto &) {});
  const auto initial = graph.prepare({.screenWidth = 1280, .screenHeight = 800},
                                     ui::defaultTheme());
  ASSERT_TRUE(initial);
  const auto initialNode =
      std::ranges::find_if(initial->visuals, [&](const auto &visual) {
        return visual.action == "node" &&
               visual.accessibleLabel.starts_with("I · Version " + first.str() +
                                                  " ");
      });
  ASSERT_NE(initialNode, initial->visuals.end());
  EXPECT_TRUE(graph.activateNode(initialNode->id));
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext initialFrame{state, projection, 1280, 800, timeline};
  graph.drawFrame(initialFrame);
  for (auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{2560, 1440}})
    for (auto scale : {.8F, 1.F, 1.5F, 2.F})
      for (auto family : {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"})
        for (bool comparison : {false, true}) {
          SCOPED_TRACE(testing::Message() << size.width << ' ' << scale << ' '
                                          << family << ' ' << comparison);
          ui::UiMetrics metrics{.fontScale    = scale,
                                .screenWidth  = static_cast<int>(size.width),
                                .screenHeight = static_cast<int>(size.height),
                                .chrome       = {.top = 44, .bottom = 18}};
          ui::Theme theme;
          theme.fonts[static_cast<std::size_t>(ui::FontRole::Label)].family =
              family;
          auto scene = graph.prepare(metrics, theme);
          ASSERT_TRUE(scene);
          const auto page = std::ranges::find(
              scene->visuals, std::string{comparison ? "comparison" : "graph"},
              &ui::WidgetVisual::action);
          ASSERT_NE(page, scene->visuals.end());
          ASSERT_TRUE(graph.activateNode(page->id));
          gleditor::FrameContext frame{state,
                                       projection,
                                       metrics.screenWidth,
                                       metrics.screenHeight,
                                       timeline,
                                       metrics.chrome,
                                       {},
                                       metrics,
                                       theme};
          graph.drawFrame(frame);
          scene = graph.prepare(metrics, theme);
          ASSERT_TRUE(scene);
          graphContains(scene->layout.bounds, metrics.pixelSafeArea());
          gleditor::a11y::Tree tree;
          gleditor::a11y::Builder builder(tree, 43);
          graph.describe(builder);
          std::set<std::uint32_t> identities;
          bool foundVersion = false;
          for (const auto &box : scene->layout.boxes) {
            EXPECT_TRUE(identities.insert(box.id).second);
            graphContains(box.rect, scene->layout.bounds);
            graphContains(box.contentRect, box.rect);
            if (box.parentId) {
              const auto *parent = scene->layout.find(box.parentId);
              ASSERT_NE(parent, nullptr);
              graphContains(box.rect, parent->contentRect);
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
            if (!visual->accessibleLabel.empty() && visual->interactive)
              EXPECT_FALSE(visual->fitted.shaping.glyphs.empty());
            if (visual->action == "node" &&
                visual->accessibleLabel.starts_with("I · Version ")) {
              EXPECT_EQ(visual->text, "I");
              EXPECT_FALSE(visual->fitted.truncated);
            }
            if (visual->action == "node" &&
                visual->accessibleLabel.starts_with("I · Version " +
                                                    first.str() + " ")) {
              foundVersion = true;
              EXPECT_NE(visual->accessibleLabel.find("identifying suffix"),
                        std::string::npos);
              EXPECT_NE(node->value.find("current view"), std::string::npos);
              EXPECT_NE(node->value.find("selected operation"),
                        std::string::npos);
              EXPECT_NE(node->value.find("in comparison"), std::string::npos);
            }
          }
          if (!comparison) EXPECT_TRUE(foundVersion);
        }
}
TEST(HypertimeGraphOverlayTest,
     WarmFramesRetainBuffersAndShapingAndStalePicksAreInert) {
  GraphDevice device;
  RenderState state(&device);
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto first  = store.insert({}, 0, "first");
  const auto second = store.insert(first, 0, "second");
  Graph graph(
      {}, [&](std::size_t) -> const xanadu::Store & { return store; },
      [&] { return static_cast<std::uint64_t>(store.opCount()); });
  int visits = 0;
  graph.setGoer([&](const auto &) { ++visits; });
  graph.setVisible(true)->setCurrent(second);
  graph.deviceReady(device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext frame{state, projection, 1280, 800, timeline};
  state.beginPickScene();
  graph.drawFrame(frame);
  auto scene = graph.prepare({.screenWidth = 1280, .screenHeight = 800},
                             ui::defaultTheme());
  ASSERT_TRUE(scene);
  const auto target =
      std::ranges::find_if(scene->visuals, [&](const auto &visual) {
        return visual.action == "node" &&
               visual.accessibleLabel.find("Version " + first.str()) !=
                   std::string::npos;
      });
  ASSERT_NE(target, scene->visuals.end());
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, target->pickingId,
      0);
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  ASSERT_EQ(pick.overlayWidgetId, target->id);
  EXPECT_TRUE(graph.picked(pick, state));
  EXPECT_EQ(visits, 0);
  graph.drawFrame(frame);
  EXPECT_EQ(visits, 1);
  EXPECT_EQ(graph.current(), first);
  const auto warm = graph.shapingStats();
  gleditor::text::ShapingStatsScope counters;
  EXPECT_CALL(device, updateBuffer).Times(0);
  for (int i = 0; i < 100; ++i) graph.drawFrame(frame);
  EXPECT_EQ(graph.shapingStats().misses, warm.misses);
  EXPECT_EQ(counters.stats().harfbuzzCalls, 0U);
  testing::Mock::VerifyAndClearExpectations(&device);
  graph.setVisible(false)->setVisible(true);
  graph.drawFrame(frame);
  EXPECT_TRUE(graph.picked(pick, state));
  graph.drawFrame(frame);
  EXPECT_EQ(visits, 1);
  scene = graph.prepare({.screenWidth = 1280, .screenHeight = 800},
                        ui::defaultTheme());
  const auto currentNode =
      std::ranges::find_if(scene->visuals, [&](const auto &visual) {
        return visual.action == "node" &&
               visual.accessibleLabel.find("Version " + first.str()) !=
                   std::string::npos;
      });
  ASSERT_NE(currentNode, scene->visuals.end());
  const auto preservedId = currentNode->id;
  ui::FocusManager focus;
  graph.syncFocus(focus);
  ASSERT_TRUE(focus.focusNode(preservedId));
  graph.setConfig({600, 460, .95F, .95F});
  EXPECT_EQ(focus.focusedNode(), preservedId);
  ui::Theme resizedTheme;
  resizedTheme.fonts[static_cast<std::size_t>(ui::FontRole::Label)].family =
      "Monospace";
  const auto resized = graph.prepare(
      {.fontScale = 1.5F, .screenWidth = 640, .screenHeight = 480},
      resizedTheme);
  ASSERT_TRUE(resized);
  EXPECT_NE(resized->find(preservedId), nullptr);
  EXPECT_EQ(focus.focusedNode(), preservedId);
  graph.scroll(1, 0, false, false, 0, 0);
  EXPECT_EQ(focus.focusedNode(), preservedId);
  const auto panned = graph.prepare(
      {.fontScale = 1.5F, .screenWidth = 640, .screenHeight = 480},
      resizedTheme);
  ASSERT_TRUE(panned);
  EXPECT_NE(panned->find(preservedId), nullptr);
  EXPECT_EQ(focus.focusedNode(), preservedId);
  EXPECT_TRUE(graph.activateNode(preservedId));
  std::ignore = store.insert(second, 0, "a new generation");
  graph.drawFrame(frame);
  EXPECT_EQ(visits, 1);
}
TEST(HypertimeGraphOverlayTest,
     KeyboardFocusScrubbingAndComparisonKeepDomainSemantics) {
  GraphDevice device;
  RenderState state(&device);
  const auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto first  = store.insert({}, 0, "first");
  const auto second = store.insert(first, 0, "second");
  Graph graph(
      {}, [&](std::size_t) -> const xanadu::Store & { return store; },
      [&] { return static_cast<std::uint64_t>(store.opCount()); });
  graph.setVisible(true)->setCurrent(first);
  graph.toggleComparison(first)->toggleComparison(second);
  graph.deviceReady(device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext frame{state, projection, 1280, 800, timeline};
  graph.drawFrame(frame);
  ui::FocusManager focus;
  graph.syncFocus(focus);
  auto scene        = graph.prepare({.screenWidth = 1280, .screenHeight = 800},
                                    ui::defaultTheme());
  const auto slider = std::ranges::find(scene->visuals, std::string{"time"},
                                        &ui::WidgetVisual::action);
  ASSERT_NE(slider, scene->visuals.end());
  const auto sliderId = slider->id;
  EXPECT_TRUE(focus.focusNode(sliderId));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Right}));
  graph.drawFrame(frame);
  EXPECT_EQ(graph.current(), second);
  EXPECT_EQ(focus.focusedNode(), sliderId);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Left}));
  graph.drawFrame(frame);
  EXPECT_EQ(graph.current(), first);
  EXPECT_EQ(focus.focusedNode(), sliderId);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Right}));
  graph.drawFrame(frame);
  EXPECT_EQ(graph.current(), second);
  EXPECT_EQ(focus.focusedNode(), sliderId);
  scene              = graph.prepare({.screenWidth = 1280, .screenHeight = 800},
                                     ui::defaultTheme());
  const auto compare = std::ranges::find(
      scene->visuals, std::string{"comparison"}, &ui::WidgetVisual::action);
  ASSERT_NE(compare, scene->visuals.end());
  EXPECT_TRUE(focus.focusNode(compare->id));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Space}));
  graph.drawFrame(frame);
  std::vector<xanadu::MicroversionId> compared;
  graph.setCompareHandler([&](const auto &versions) { compared = versions; });
  scene           = graph.prepare({.screenWidth = 1280, .screenHeight = 800},
                                  ui::defaultTheme());
  const auto open = std::ranges::find(scene->visuals, std::string{"open3d"},
                                      &ui::WidgetVisual::action);
  ASSERT_NE(open, scene->visuals.end());
  EXPECT_TRUE(graph.activateNode(open->id));
  graph.drawFrame(frame);
  EXPECT_EQ(compared, (std::vector<xanadu::MicroversionId>{first, second}));
  EXPECT_EQ(store.opCount(), 2U);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Escape}));
  EXPECT_FALSE(graph.isVisible());
}
} // namespace
