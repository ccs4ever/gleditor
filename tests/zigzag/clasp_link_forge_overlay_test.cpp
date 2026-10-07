#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../lib/mocks/device.hpp"
#include "common/ui/xanadoc/clasp_link_forge.hpp"
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

namespace {
using namespace gleditor::ui;

void contained(Rect child, Rect parent) {
  EXPECT_GE(child.left, parent.left);
  EXPECT_GE(child.bottom, parent.bottom);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}

xanadu::PouchItem cellCard() {
  xanadu::PouchItem card;
  card.originKind = xanadu::PouchOriginKind::ZigzagCell;
  card.originCell = 234;
  card.originRankCoord =
      "d.sequence: an unusually long coordinate and rank name";
  card.span.length = 17;
  return card;
}

TEST(ClaspLinkForgeTest,
     constrainedLabelsDropTargetsAndAccessibilityShareLayout) {
  xanadu::LinkForgeWidget forge;
  forge.dropLeft(cellCard());
  forge.dropRight({.span = {.length = 12345678}});
  forge.setLinkType(xanadu::LinkType::Disagreement);
  for (const auto size : {Size{640, 480}, Size{1280, 800}, Size{2560, 1440}}) {
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(std::to_string(size.width) +
                     " scale=" + std::to_string(scale) + " family=" + family);
        UiMetrics metrics{.fontScale    = scale,
                          .screenWidth  = static_cast<int>(size.width),
                          .screenHeight = static_cast<int>(size.height),
                          .chrome       = {.top = 30, .bottom = 20}};
        Theme theme;
        for (auto &font : theme.fonts) font.family = family;
        const auto safe  = metrics.pixelSafeArea();
        const auto scene = forge.prepareBench(
            metrics, theme,
            {safe.left, safe.bottom, std::min(400.F, safe.width),
             std::min(forge.preferredHeight(metrics, theme), safe.height)});
        ASSERT_TRUE(scene);
        contained(scene->layout.bounds, safe);
        for (const auto &box : scene->layout.boxes) {
          contained(box.rect, scene->layout.bounds);
          if (box.parentId) {
            const auto *parent = scene->layout.find(box.parentId);
            ASSERT_NE(parent, nullptr);
            contained(box.rect, parent->contentRect);
          }
          contained(box.contentRect, box.rect);
        }
        gleditor::a11y::Tree tree;
        gleditor::a11y::Builder builder(tree, 31);
        forge.describe(builder);
        bool sourceLabel = false;
        for (const auto &visual : scene->visuals) {
          const auto *box = scene->layout.find(visual.id);
          ASSERT_NE(box, nullptr);
          EXPECT_LE(visual.fitted.widthPx, box->contentRect.width + .01F);
          EXPECT_LE(visual.fitted.heightPx, box->contentRect.height + .01F);
          const auto node = tree.find(builder.id(visual.id));
          ASSERT_TRUE(node);
          EXPECT_EQ(node->label, visual.accessibleLabel);
          ASSERT_TRUE(node->bounds);
          EXPECT_DOUBLE_EQ(node->bounds->left, box->rect.left);
          EXPECT_DOUBLE_EQ(node->bounds->right,
                           box->rect.left + box->rect.width);
          EXPECT_DOUBLE_EQ(node->bounds->top, metrics.screenHeight -
                                                  box->rect.bottom -
                                                  box->rect.height);
          EXPECT_DOUBLE_EQ(node->bounds->bottom,
                           metrics.screenHeight - box->rect.bottom);
          if (!visual.text.empty()) {
            SCOPED_TRACE(
                visual.text + " rect=" + std::to_string(box->rect.width) + "x" +
                std::to_string(box->rect.height) +
                " content=" + std::to_string(box->contentRect.width) + "x" +
                std::to_string(box->contentRect.height) + " fontline=" +
                std::to_string(visual.font->metrics().lineHeight));
            EXPECT_FALSE(visual.fitted.shaping.lines.empty()) << visual.text;
            EXPECT_FALSE(visual.fitted.shaping.glyphs.empty()) << visual.text;
          }
          if (node->label == cellCard().originRankCoord + " (1 spans)")
            sourceLabel = true;
          if (visual.interactive && box->rect.width > 0 &&
              box->rect.height > 0) {
            const auto *hit =
                scene->layout.hitTest(box->rect.left + box->rect.width / 2,
                                      box->rect.bottom + box->rect.height / 2);
            ASSERT_NE(hit, nullptr);
            EXPECT_EQ(hit->id, box->id);
          }
        }
        EXPECT_TRUE(sourceLabel);
      }
    }
  }
}

TEST(ClaspLinkForgeTest, compactBenchKeepsFullSourceMetadataAndVisibleGlyphs) {
  xanadu::LinkForgeWidget forge;
  forge.dropLeft(cellCard());
  forge.dropRight({.span = {.length = 12345678}});
  const UiMetrics metrics{
      .fontScale = 2, .screenWidth = 640, .screenHeight = 480};
  Theme theme;
  for (auto &font : theme.fonts) font.family = "Noto Sans CJK JP";
  theme.paddingEm   = 0;
  theme.gapEm       = 0;
  const auto height = forge.preferredHeight(metrics, theme, true);
  EXPECT_LE(height, 200);
  const auto scene =
      forge.prepareBench(metrics, theme, {50, 50, 300, height}, true);
  bool fullSource = false;
  for (const auto &visual : scene->visuals) {
    if (visual.text.empty()) continue;
    EXPECT_FALSE(visual.fitted.shaping.glyphs.empty()) << visual.text;
    fullSource |= visual.accessibleLabel == "Homestead · Cell #234";
  }
  EXPECT_TRUE(fullSource);
  EXPECT_TRUE(std::ranges::any_of(scene->visuals, [](const auto &visual) {
    return visual.accessibleLabel == cellCard().originRankCoord + " (1 spans)";
  }));
}

TEST(ClaspLinkForgeTest, unchangedBenchRetainsGeometryAndRejectsOldActions) {
  xanadu::LinkForgeWidget forge;
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  const Theme theme;
  const Rect bounds{100, 100, 700, 500};
  const auto scene    = forge.prepareBench(metrics, theme, bounds);
  const auto revision = forge.presentation().layoutRevision();
  gleditor::text::ShapingStatsScope shaping;
  for (int i = 0; i < 100; ++i)
    EXPECT_EQ(forge.prepareBench(metrics, theme, bounds), scene);
  EXPECT_EQ(forge.presentation().layoutRevision(), revision);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  const auto type = std::ranges::find(scene->visuals, std::string("type"),
                                      &WidgetVisual::action);
  ASSERT_NE(type, scene->visuals.end());
  const auto target = std::ranges::find(scene->visuals, std::string("forge"),
                                        &WidgetVisual::action);
  ASSERT_NE(target, scene->visuals.end());
  int calls = 0;
  forge.setActionHandler([&](std::uint32_t tag) {
    EXPECT_EQ(tag, forge.kTagClaspTypeSelector);
    ++calls;
  });
  EXPECT_TRUE(forge.presentation().activate(type->id));
  EXPECT_EQ(calls, 1);
  forge.dropLeft(cellCard());
  std::ignore = forge.prepareBench(metrics, theme, bounds);
  EXPECT_FALSE(forge.presentation().activate(target->id));
  EXPECT_EQ(calls, 1);
  forge.setVisible(false);
  EXPECT_FALSE(forge.presentation().activate(type->id));
}

TEST(ClaspLinkForgeTest, repeatedKeyboardCyclingKeepsSelectorFocus) {
  xanadu::LinkForgeWidget forge;
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  const Theme theme;
  const Rect bounds{100, 100, 700, 500};
  const auto scene = forge.prepareBench(metrics, theme, bounds);
  forge.setActionHandler([&](std::uint32_t tag) {
    if (tag == forge.kTagClaspTypeSelector) forge.cycleType();
    if (tag == forge.kTagClaspTierSelector) forge.cycleTier();
  });
  FocusManager manager;
  auto registration = manager.registerScope(forge.presentation());
  forge.presentation().activate();
  for (const auto *action : {"type", "tier"}) {
    const auto control = std::ranges::find(scene->visuals, std::string(action),
                                           &WidgetVisual::action);
    ASSERT_NE(control, scene->visuals.end());
    ASSERT_TRUE(manager.focusNode(control->id));
    for (int cycle = 0; cycle < 3; ++cycle) {
      EXPECT_TRUE(manager.dispatchKey({.key = gleditor::Key::Space}));
      std::ignore = forge.prepareBench(metrics, theme, bounds);
      EXPECT_EQ(manager.focusedNode(), control->id);
    }
  }
  const auto type = std::ranges::find(scene->visuals, std::string("type"),
                                      &WidgetVisual::action);
  forge.setVisible(false);
  forge.setVisible(true);
  std::ignore = forge.prepareBench(metrics, theme, bounds);
  EXPECT_FALSE(forge.presentation().activate(type->id));
}

TEST(ClaspLinkForgeTest, movementAndFontChangesPreserveActionIdentity) {
  xanadu::LinkForgeWidget forge;
  UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  Theme theme;
  const auto initial = forge.prepareBench(metrics, theme, {100, 100, 700, 500});
  const auto type    = std::ranges::find(initial->visuals, std::string("type"),
                                         &WidgetVisual::action);
  ASSERT_NE(type, initial->visuals.end());
  metrics.fontScale = 1.5F;
  const auto moved  = forge.prepareBench(metrics, theme, {110, 110, 700, 500});
  EXPECT_NE(moved, initial);
  EXPECT_NE(moved->find(type->id), nullptr);
}

TEST(ClaspLinkForgeTest, forgePreservesEveryEndsetSpanTypeAndTier) {
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  auto &store        = session.store();
  const auto version = store.insert({}, 0, "Left middle right");
  session.views().push_back(
      {.version = version, .pieces = store.rebuild(version)});
  const auto text = store.rebuild(version);
  xanadu::LinkForgeWidget forge;
  EXPECT_FALSE(forge.forge(session, 0));
  for (const auto &span : text.spansFor(0, 4)) forge.dropLeft({.span = span});
  for (const auto &span : text.spansFor(5, 6)) forge.dropLeft({.span = span});
  for (const auto &span : text.spansFor(12, 5)) forge.dropRight({.span = span});
  forge.setLinkType(xanadu::LinkType::Disagreement);
  forge.setProminenceTier(xanadu::ProminenceTier::Curated);
  const auto left  = forge.leftSpans().size();
  const auto right = forge.rightSpans().size();
  ASSERT_TRUE(forge.forge(session, 0));
  ASSERT_EQ(store.linkView().size(), 1U);
  const auto &link = *store.linkView().begin();
  EXPECT_EQ(link.type, xanadu::LinkType::Disagreement);
  EXPECT_EQ(link.tier, xanadu::ProminenceTier::Curated);
  EXPECT_EQ(link.left.size(), left);
  EXPECT_EQ(link.right.size(), right);
  EXPECT_TRUE(forge.leftSpans().empty());
  EXPECT_TRUE(forge.rightSpans().empty());
  EXPECT_FALSE(forge.canForge());
}

TEST(ClaspLinkForgeTest, stableDrawDoesNotUploadOrShape) {
  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device, createTextureArray)
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer)
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  ON_CALL(device, createPipeline)
      .WillByDefault(testing::Return(render::PipelineHandle{1}));
  RenderState state{&device};
  xanadu::LinkForgeWidget forge;
  forge.dropLeft(cellCard());
  forge.dropRight(cellCard());
  ch::Timeline timeline;
  glm::mat4 projection{1};
  UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  Theme theme;
  gleditor::FrameContext context{.state          = state,
                                 .viewProjection = projection,
                                 .screenWidth    = 1280,
                                 .screenHeight   = 800,
                                 .timeline       = timeline,
                                 .metrics        = metrics,
                                 .theme          = theme};
  forge.deviceReady(device, {});
  std::ignore = forge.prepareBench(metrics, theme, {100, 100, 700, 500});
  state.beginPickScene();
  forge.drawPrepared(context);
  EXPECT_CALL(device, updateBuffer).Times(0);
  EXPECT_CALL(device, updateTextureLayer).Times(0);
  EXPECT_CALL(device, createPipeline).Times(0);
  gleditor::text::ShapingStatsScope shaping;
  for (int frame = 0; frame < 50; ++frame) {
    state.beginPickScene();
    std::ignore = forge.prepareBench(metrics, theme, {100, 100, 700, 500});
    forge.drawPrepared(context);
  }
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  testing::Mock::VerifyAndClearExpectations(&device);
  const auto scene  = forge.snapshot();
  const auto button = std::ranges::find(scene->visuals, std::string("forge"),
                                        &WidgetVisual::action);
  ASSERT_NE(button, scene->visuals.end());
  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  render::PickingResult captured;
  captured.requestId = 7;
  captured.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, button->pickingId,
      0);
  captured.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, captured.tag);
  ASSERT_TRUE(captured.overlayWidgetId);
  int actions = 0;
  forge.setActionHandler([&](std::uint32_t) { ++actions; });
  forge.dropLeft(cellCard());
  EXPECT_TRUE(forge.picked(captured, state));
  EXPECT_TRUE(forge.presentation().activate(button->id));
  EXPECT_FALSE(
      forge.performAction(button->id, gleditor::a11y::Action::Click, {}));
  EXPECT_EQ(actions, 0);
  forge.setVisible(false);
  EXPECT_TRUE(forge.picked(captured, state));
  forge.setVisible(true);
  EXPECT_TRUE(forge.picked(captured, state));
  EXPECT_TRUE(forge.presentation().activate(button->id));
  EXPECT_EQ(actions, 0);
  forge.setVisible(false);
  forge.setVisible(true);
  std::ignore = forge.prepareBench(metrics, theme, {100, 100, 700, 500});
  state.beginPickScene();
  forge.drawPrepared(context);
  EXPECT_TRUE(forge.picked(captured, state));
  EXPECT_EQ(actions, 0);
}
} // namespace
