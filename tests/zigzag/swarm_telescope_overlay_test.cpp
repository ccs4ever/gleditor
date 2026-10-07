#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <set>

#include "../lib/mocks/device.hpp"
#include "xudu/swarm_telescope_overlay.hpp"

namespace {
namespace ui = gleditor::ui;
class TelescopeDevice : public testing::NiceMock<MockRenderDevice> {
public:
  TelescopeDevice() {
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
xanadu::PublicationEntry entry(int index) {
  return {.infoHash = std::string(38, 'a') + (index < 10 ? "0" : "") +
                      std::to_string(index),
          .bep46Uri = "magnet:?local=long-publication-identity-" +
                      std::to_string(index),
          .title = "Publication 名称 é العربية with a long title " +
                   std::to_string(index),
          .authorName        = "Author 名称 with a full author name",
          .authorFingerprint = std::string(64, 'b'),
          .topics            = {"hypertext", "multilingual"},
          .abstractText      = "An extended abstract describes publication "
                               "identity, provenance, and reading 中文 العربية.",
          .totalBytes        = 987654321,
          .microversions     = 123};
}
void contains(ui::Rect child, ui::Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}
const ui::WidgetVisual *named(const ui::WidgetScene &scene,
                              std::string_view label) {
  const auto found = std::ranges::find(scene.visuals, label,
                                       &ui::WidgetVisual::accessibleLabel);
  return found == scene.visuals.end() ? nullptr : &*found;
}
TEST(SwarmTelescopeOverlayTest,
     FittedResponsivePagesKeepFullAccessibilityAndInk) {
  xanadu::SwarmCatalog catalog;
  for (int i = 0; i < 20; ++i) catalog.addPublication(entry(i));
  TelescopeDevice device;
  RenderState state(&device);
  xanadu::SwarmTelescopeOverlay telescope(catalog, {});
  telescope.deviceReady(device, {});
  telescope.setVisible(true);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
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
        for (auto &font : theme.fonts) font.family = family;
        const auto inspect = [&](const auto &scene) {
          ASSERT_TRUE(scene);
          contains(scene->layout.bounds, metrics.pixelSafeArea());
          gleditor::a11y::Tree tree;
          gleditor::a11y::Builder builder(tree, 80);
          telescope.describe(builder);
          std::set<ui::WidgetId> ids;
          for (const auto &box : scene->layout.boxes) {
            EXPECT_TRUE(ids.insert(box.id).second);
            contains(box.rect, scene->layout.bounds);
            contains(box.contentRect, box.rect);
            if (box.parentId) {
              ASSERT_TRUE(scene->layout.find(box.parentId));
              contains(box.rect, scene->layout.find(box.parentId)->rect);
            }
            const auto *visual = scene->find(box.id);
            if (!visual) continue;
            const auto node = tree.find(builder.id(box.id));
            ASSERT_TRUE(node);
            ASSERT_TRUE(node->bounds);
            EXPECT_DOUBLE_EQ(node->bounds->left, box.rect.left);
            EXPECT_DOUBLE_EQ(node->bounds->top,
                             size.height - box.rect.bottom - box.rect.height);
            if (visual->id != 1)
              EXPECT_EQ(node->label, visual->accessibleLabel);
            EXPECT_LE(visual->fitted.widthPx, box.contentRect.width + .01F);
            EXPECT_LE(visual->fitted.heightPx, box.contentRect.height + .01F);
            if (!visual->text.empty()) {
              EXPECT_FALSE(visual->fitted.shaping.glyphs.empty())
                  << visual->text;
              EXPECT_GT(visual->fitted.visibleBytes, 0U) << visual->text;
            }
          }
          EXPECT_TRUE(named(*scene, "Open selected publication"));
          EXPECT_TRUE(named(*scene, "Refresh"));
          EXPECT_TRUE(named(*scene, "Close"));
        };
        auto scene = telescope.prepare(metrics, theme);
        inspect(scene);
        const auto *inspector = named(*scene, "Inspector");
        ASSERT_TRUE(inspector);
        EXPECT_TRUE(telescope.activateNode(inspector->id));
        gleditor::FrameContext context{state,
                                       projection,
                                       metrics.screenWidth,
                                       metrics.screenHeight,
                                       timeline,
                                       metrics.chrome,
                                       {},
                                       metrics,
                                       theme};
        telescope.drawFrame(context);
        scene = telescope.prepare(metrics, theme);
        inspect(scene);
        EXPECT_TRUE(std::ranges::any_of(scene->visuals, [](const auto &v) {
          return v.accessibleLabel.starts_with("Publication 名称");
        }));
        const auto *publications = named(*scene, "Publications");
        ASSERT_TRUE(publications);
        EXPECT_TRUE(telescope.activateNode(publications->id));
        telescope.drawFrame(context);
      }
}
TEST(SwarmTelescopeOverlayTest,
     StablePublicationActionsRetireOnSearchAndReopen) {
  xanadu::SwarmCatalog catalog;
  catalog.addPublication(entry(1));
  catalog.addPublication(entry(2));
  TelescopeDevice device;
  RenderState state(&device);
  xanadu::SwarmTelescopeOverlay telescope(catalog, {});
  telescope.deviceReady(device, {});
  std::vector<std::string> opened;
  telescope.setOnSummon(
      [&](const auto &publication) { opened.push_back(publication.infoHash); });
  telescope.setVisible(true);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 1280, 800, timeline};
  state.beginPickScene();
  telescope.drawFrame(context);
  auto scene         = telescope.snapshot();
  const auto *summon = named(*scene, "Open selected publication");
  ASSERT_TRUE(summon);
  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, summon->pickingId,
      0);
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  EXPECT_TRUE(telescope.picked(pick, state));
  EXPECT_TRUE(opened.empty());
  telescope.setSearchQuery("nonexistentunlikelytext");
  telescope.drawFrame(context);
  EXPECT_TRUE(opened.empty());
  EXPECT_TRUE(telescope.picked(pick, state));
  telescope.drawFrame(context);
  EXPECT_TRUE(opened.empty());
  telescope.setSearchQuery("");
  telescope.drawFrame(context);
  scene  = telescope.snapshot();
  summon = named(*scene, "Open selected publication");
  ASSERT_TRUE(summon);
  const auto oldId = summon->id;
  telescope.setVisible(false);
  telescope.setVisible(true);
  telescope.drawFrame(context);
  EXPECT_FALSE(telescope.activateNode(oldId));
  scene  = telescope.snapshot();
  summon = named(*scene, "Open selected publication");
  ASSERT_TRUE(summon);
  EXPECT_TRUE(telescope.activateNode(summon->id));
  telescope.drawFrame(context);
  ASSERT_EQ(opened.size(), 1U);
  EXPECT_TRUE(opened[0] == entry(1).infoHash || opened[0] == entry(2).infoHash);
  EXPECT_FALSE(telescope.isVisible());
}
TEST(SwarmTelescopeOverlayTest,
     SharedFocusEditsUnicodeAndWarmFramesDoNoShapingOrUploads) {
  xanadu::SwarmCatalog catalog;
  for (int i = 0; i < 30; ++i) catalog.addPublication(entry(i));
  TelescopeDevice device;
  RenderState state(&device);
  xanadu::SwarmTelescopeOverlay telescope(catalog, {});
  telescope.deviceReady(device, {});
  telescope.setVisible(true);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 640, 480, timeline};
  telescope.drawFrame(context);
  ui::FocusManager focus;
  auto registration = focus.registerScope(telescope);
  EXPECT_TRUE(focus.modalActive());
  EXPECT_FALSE(focus.permitsCommand("document-edit"));
  auto scene        = telescope.snapshot();
  const auto search = std::ranges::find_if(
      scene->visuals, [](const auto &v) { return v.textInput; });
  ASSERT_NE(search, scene->visuals.end());
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 90);
  telescope.describe(builder);
  EXPECT_TRUE(telescope.performAction(builder.id(search->id),
                                      gleditor::a11y::Action::Focus, {}));
  focus.dispatchText("名称");
  EXPECT_EQ(telescope.searchQuery(), "名称");
  telescope.drawFrame(context);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Backspace, {}}));
  EXPECT_EQ(telescope.searchQuery(), "名");
  telescope.drawFrame(context);
  focus.dispatchText("称");
  EXPECT_EQ(telescope.searchQuery(), "名称");
  telescope.drawFrame(context);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Home, {}}));
  telescope.drawFrame(context);
  focus.dispatchText("X");
  EXPECT_EQ(telescope.searchQuery(), "X名称");
  telescope.drawFrame(context);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Left, {}}));
  telescope.drawFrame(context);
  focus.dispatchText("Y");
  EXPECT_EQ(telescope.searchQuery(), "YX名称");
  telescope.drawFrame(context);
  const auto warm = telescope.shapingStats();
  gleditor::text::ShapingStatsScope counters;
  EXPECT_CALL(device, updateBuffer).Times(0);
  for (int i = 0; i < 100; ++i) telescope.drawFrame(context);
  EXPECT_EQ(counters.stats().harfbuzzCalls, 0U);
  EXPECT_EQ(telescope.shapingStats().misses, warm.misses);
  testing::Mock::VerifyAndClearExpectations(&device);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Escape, {}}));
  EXPECT_FALSE(telescope.isVisible());
  EXPECT_FALSE(focus.modalActive());
}
TEST(SwarmTelescopeOverlayTest,
     RealChannelsAndVirtualScrollingReachRemainingPublications) {
  xanadu::SwarmCatalog catalog;
  for (int i = 0; i < 30; ++i) catalog.addPublication(entry(i));
  TelescopeDevice device;
  RenderState state(&device);
  xanadu::SwarmTelescopeOverlay telescope(catalog, {});
  telescope.deviceReady(device, {});
  telescope.setVisible(true);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 640, 480, timeline};
  telescope.drawFrame(context);
  const auto first = telescope.snapshot()->layout.focusOrder;
  EXPECT_TRUE(telescope.keyPressed(gleditor::Key::PageDown, {}));
  telescope.drawFrame(context);
  EXPECT_NE(telescope.snapshot()->layout.focusOrder, first);
  auto scene           = telescope.snapshot();
  const auto *channels = named(*scene, "Channels");
  ASSERT_TRUE(channels);
  EXPECT_TRUE(telescope.activateNode(channels->id));
  telescope.drawFrame(context);
  scene             = telescope.snapshot();
  const auto *topic = named(*scene, "#hypertext");
  ASSERT_TRUE(topic);
  EXPECT_TRUE(telescope.activateNode(topic->id));
  telescope.drawFrame(context);
  EXPECT_EQ(telescope.searchQuery(), "#hypertext");
  EXPECT_TRUE(
      std::ranges::any_of(telescope.snapshot()->visuals,
                          [](const auto &v) { return v.action == "select"; }));
}
TEST(SwarmTelescopeOverlayTest,
     InspectorWrapsDescriptionsAndRetainsIdentifierSuffixes) {
  xanadu::SwarmCatalog catalog;
  auto publication              = entry(1);
  publication.authorFingerprint = std::string(256, 'b') + "TAIL";
  publication.abstractText =
      std::string(8, 'A') +
      " extended provenance explanation 名称 العربية repeated for a bounded "
      "wrapped abstract description with reader-visible detail";
  catalog.addPublication(publication);
  TelescopeDevice device;
  RenderState state(&device);
  xanadu::SwarmTelescopeOverlay telescope(catalog, {});
  telescope.deviceReady(device, {});
  telescope.setVisible(true);
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 640, 480, timeline};
  telescope.drawFrame(context);
  auto scene            = telescope.snapshot();
  const auto *inspector = named(*scene, "Inspector");
  ASSERT_TRUE(inspector);
  ASSERT_TRUE(telescope.activateNode(inspector->id));
  telescope.drawFrame(context);
  bool wrapped = false, suffix = false, uri = false;
  for (int step = 0; step < 8; ++step) {
    scene = telescope.snapshot();
    for (const auto &visual : scene->visuals) {
      if (visual.accessibleLabel.starts_with("Abstract:")) {
        EXPECT_EQ(visual.accessibleLabel,
                  "Abstract: " + publication.abstractText);
        EXPECT_GE(visual.fitted.lines, 2U);
        EXPECT_LE(visual.fitted.lines, 3U);
        wrapped = true;
      }
      if (visual.accessibleLabel.starts_with("Key:")) {
        EXPECT_EQ(visual.accessibleLabel,
                  "Key: " + publication.authorFingerprint);
        EXPECT_TRUE(visual.fitted.truncated);
        suffix |= std::ranges::any_of(
            visual.fitted.shaping.clusters, [&](const auto &cluster) {
              return cluster.byteLength &&
                     cluster.byteStart >= publication.authorFingerprint.size();
            });
      }
      uri |= visual.accessibleLabel == "URI: " + publication.bep46Uri;
    }
    EXPECT_TRUE(telescope.keyPressed(gleditor::Key::PageDown, {}));
    telescope.drawFrame(context);
  }
  EXPECT_TRUE(wrapped);
  EXPECT_TRUE(suffix);
  EXPECT_TRUE(uri);
}
TEST(SwarmTelescopeOverlayTest,
     RegisteredPointerFocusAndReturnOpenTheFocusedPublication) {
  xanadu::SwarmCatalog catalog;
  catalog.addPublication(entry(1));
  TelescopeDevice device;
  RenderState state(&device);
  xanadu::SwarmTelescopeOverlay telescope(catalog, {});
  telescope.deviceReady(device, {});
  telescope.setVisible(true);
  std::optional<xanadu::PublicationEntry> opened;
  telescope.setOnSummon([&](const auto &publication) { opened = publication; });
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{state, projection, 640, 480, timeline};
  telescope.drawFrame(context);
  ui::FocusManager focus;
  auto registration = focus.registerScope(telescope);
  auto scene        = telescope.snapshot();
  const auto result = std::ranges::find(scene->visuals, std::string{"select"},
                                        &ui::WidgetVisual::action);
  ASSERT_NE(result, scene->visuals.end());
  const auto identity = result->id;
  const auto *box     = scene->layout.find(identity);
  ASSERT_TRUE(box);
  const auto x = box->rect.left + box->rect.width / 2;
  const auto y = 480 - box->rect.bottom - box->rect.height / 2;
  EXPECT_TRUE(focus.dispatchPointer({.phase     = ui::PointerPhase::Press,
                                     .button    = 1,
                                     .x         = x,
                                     .y         = y,
                                     .pointerId = 7}));
  EXPECT_EQ(focus.focusedNode(), identity);
  EXPECT_TRUE(focus.dispatchPointer({.phase     = ui::PointerPhase::Release,
                                     .button    = 1,
                                     .x         = x,
                                     .y         = y,
                                     .pointerId = 7}));
  telescope.drawFrame(context);
  EXPECT_FALSE(opened);
  EXPECT_EQ(focus.focusedNode(), identity);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Space, {}}));
  telescope.drawFrame(context);
  EXPECT_FALSE(opened);
  auto metrics         = context.metrics;
  metrics.screenWidth  = 1280;
  metrics.screenHeight = 800;
  metrics.fontScale    = 1.5F;
  ui::Theme theme;
  for (auto &font : theme.fonts) font.family = "Serif";
  scene = telescope.prepare(metrics, theme);
  EXPECT_TRUE(scene->find(identity));
  EXPECT_EQ(focus.focusedNode(), identity);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Return, {}}));
  EXPECT_FALSE(opened);
  telescope.drawFrame(context);
  ASSERT_TRUE(opened);
  EXPECT_EQ(opened->infoHash, entry(1).infoHash);
  EXPECT_EQ(opened->bep46Uri, entry(1).bep46Uri);
  EXPECT_FALSE(telescope.isVisible());
}
} // namespace
