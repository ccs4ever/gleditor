#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

#include "../lib/mocks/device.hpp"
#include "common/ui/link_panel_presentation.hpp"
#include "common/xanadu/link_views.hpp"
#include "xudu/link_panel_overlay.hpp"

namespace {
namespace ui  = gleditor::ui;
namespace nav = xanadu::nav;

void contained(ui::Rect child, ui::Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}

std::vector<xanadu::PanelLine> contextLines() {
  return {{.text = "comment · link 4294967295 · A long author identity 名称 "
                   "with further provenance and a document name"},
          {.text   = "Left 2/7 · occurrence 1/4 · a long document name, bytes "
                     "100 to 200, before and after a transclusion",
           .tone   = xanadu::PanelLine::Tone::Active,
           .active = true},
          {.text = "Right 3/9 · occurrence 5/5 · cell 90000000, bytes 0 to "
                   "500 with further endpoint context"},
          {.text = "reading: outside the linked range",
           .tone = xanadu::PanelLine::Tone::Muted},
          {.text = "origin: document 12345, bytes 100000 to 100500 with a "
                   "long origin document description",
           .tone = xanadu::PanelLine::Tone::Muted}};
}

std::array<ui::WidgetId, 9> actionIds() {
  return {1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032};
}

TEST(LinkPanelPresentationTest,
     ContextAndEveryActionRemainVisibleAndAccessible) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  const xanadu::LinkPanelConfig config;
  gleditor::text::ShapingCache measurements;
  for (const auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{2560, 1440}}) {
    for (const auto scale : {.8F, 1.0F, 1.5F, 2.0F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(testing::Message() << size.width << 'x' << size.height
                                        << " scale " << scale << ' ' << family);
        const ui::UiMetrics metrics{.fontScale   = scale,
                                    .screenWidth = static_cast<int>(size.width),
                                    .screenHeight =
                                        static_cast<int>(size.height),
                                    .chrome = {.top = 44, .bottom = 18}};
        ui::Theme theme;
        theme.fonts[static_cast<std::size_t>(ui::FontRole::Label)].family =
            family;
        const auto leaf = common_ui::linkPanelPresentation(
            lines, buttons, ids, metrics, theme, config,
            ui::Rect{size.width + 100, size.height + 100, 80, 0}, measurements);
        contained(leaf.bounds, metrics.pixelSafeArea());
        ui::ScreenOverlay panel(leaf.model);
        panel.setBounds(leaf.bounds);
        const auto scene = panel.prepare(metrics, leaf.theme);
        ASSERT_NE(scene, nullptr);
        for (const auto &box : scene->layout.boxes) {
          SCOPED_TRACE(box.id);
          contained(box.rect, scene->layout.bounds);
          contained(box.contentRect, box.rect);
          if (box.parentId) {
            const auto *parent = scene->layout.find(box.parentId);
            ASSERT_NE(parent, nullptr);
            contained(box.rect, parent->rect);
          }
          if (const auto *visual = scene->find(box.id)) {
            EXPECT_LE(visual->fitted.widthPx, box.contentRect.width + .01F);
            EXPECT_LE(visual->fitted.heightPx, box.contentRect.height + .01F);
          }
        }
        gleditor::a11y::Tree tree;
        gleditor::a11y::Builder builder(tree, 27);
        panel.describe(builder);
        const auto check = [&](ui::WidgetId id, const std::string &label) {
          const auto *visual = scene->find(id);
          ASSERT_NE(visual, nullptr);
          EXPECT_FALSE(visual->fitted.shaping.glyphs.empty());
          EXPECT_GT(visual->fitted.lines, 0U);
          EXPECT_GT(visual->fitted.visibleBytes, 0U);
          EXPECT_EQ(visual->accessibleLabel, label);
          const auto node = tree.find(builder.id(id));
          ASSERT_TRUE(node);
          EXPECT_EQ(node->label, label);
          const auto *box = scene->layout.find(id);
          ASSERT_NE(box, nullptr);
          ASSERT_TRUE(node->bounds);
          EXPECT_DOUBLE_EQ(node->bounds->left, box->rect.left);
          EXPECT_DOUBLE_EQ(node->bounds->right,
                           box->rect.left + box->rect.width);
          EXPECT_DOUBLE_EQ(node->bounds->top, metrics.screenHeight -
                                                  box->rect.bottom -
                                                  box->rect.height);
          EXPECT_DOUBLE_EQ(node->bounds->bottom,
                           metrics.screenHeight - box->rect.bottom);
        };
        for (std::size_t index = 0; index < lines.size(); ++index)
          check(static_cast<ui::WidgetId>(index + 3),
                lines[index].active ? "\u25B8 " + lines[index].text
                                    : lines[index].text);
        ASSERT_EQ(leaf.actions.size(), buttons.size());
        for (std::size_t index = 0; index < buttons.size(); ++index) {
          check(ids[index], buttons[index].label);
          const auto *action = leaf.find(ids[index]);
          ASSERT_NE(action, nullptr);
          EXPECT_EQ(action->command, buttons[index].command);
          EXPECT_EQ(action->enabled, buttons[index].enabled);
          const auto node = tree.find(builder.id(ids[index]));
          ASSERT_TRUE(node);
          EXPECT_EQ(node->focusable, buttons[index].enabled);
          EXPECT_EQ((node->actions &
                     gleditor::a11y::bit(gleditor::a11y::Action::Click)) != 0,
                    buttons[index].enabled);
        }
      }
    }
  }
}

TEST(LinkPanelPresentationTest,
     LegacyPlacementFontsAndColoursFollowLiveMetrics) {
  const std::array<xanadu::PanelLine, 1> lines{{{.text = "Selected link"}}};
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  gleditor::text::ShapingCache measurements;
  const ui::UiMetrics metrics{.fontScale    = 1.5F,
                              .screenWidth  = 1280,
                              .screenHeight = 800,
                              .marginShare  = .013F};
  ui::Theme theme;
  theme.fonts[static_cast<std::size_t>(ui::FontRole::Label)] = {"Monospace",
                                                                15};
  xanadu::LinkPanelConfig config;
  config.topPx    = 53;
  config.marginPx = 21;
  auto leaf       = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, theme, config, std::nullopt, measurements);
  EXPECT_EQ(leaf.theme.font(ui::FontRole::Label),
            theme.font(ui::FontRole::Label));
  EXPECT_EQ(ui::rgba(leaf.theme.colours.surface), config.backgroundColour);
  ASSERT_TRUE(leaf.theme.colours.buttonSurface);
  EXPECT_EQ(ui::rgba(*leaf.theme.colours.buttonSurface), config.buttonColour);
  auto safeMetrics       = metrics;
  safeMetrics.chrome.top = config.topPx;
  const auto safe        = safeMetrics.pixelSafeArea();
  EXPECT_NEAR(leaf.bounds.left + leaf.bounds.width,
              safe.left + safe.width - config.marginPx, 1);
  EXPECT_NEAR(leaf.bounds.bottom + leaf.bounds.height,
              safe.bottom + safe.height - config.marginPx, 1);
  config.font           = "Serif Bold 10";
  config.maxWidthShare  = std::numeric_limits<float>::quiet_NaN();
  config.maxHeightShare = std::numeric_limits<float>::infinity();
  leaf = common_ui::linkPanelPresentation(lines, buttons, ids, metrics, theme,
                                          config, ui::Rect{-1000, -1000, 0, 0},
                                          measurements);
  EXPECT_EQ(leaf.theme.font(ui::FontRole::Label).family, "Serif Bold");
  EXPECT_EQ(leaf.theme.font(ui::FontRole::Label).points, 10);
  EXPECT_EQ(metrics.fontDescription(ui::FontRole::Label, leaf.theme),
            "Serif Bold 15");
  contained(leaf.bounds, safe);
  EXPECT_THROW(std::ignore = common_ui::linkPanelPresentation(
                   lines, buttons, std::span<const ui::WidgetId>{}, metrics,
                   theme, config, std::nullopt, measurements),
               std::invalid_argument);
}

class LinkPanelDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::uint32_t nextBuffer{1};
  LinkPanelDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t) {
          return render::BufferHandle{nextBuffer++};
        });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault(
            [](render::BufferHandle buffer, std::size_t) { return buffer; });
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
  }
};

class LinkPanelOverlayTest : public testing::Test {
protected:
  LinkPanelDevice device;
  RenderState state{&device};
  xanadu::Session session{"", std::make_shared<xanadu::UserPermascroll>()};
  xanadu::LinkContext navigation{session};
  xanadu::LinkPanelOverlay overlay{navigation, session};
  ch::Timeline timeline;
  glm::mat4 projection{1};
  ui::UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  ui::Theme theme;
  std::array<zigzag::CellRef, 2> links{};
  xanadu::LinkPanelOverlay::AnchorPair anchors{{{-.5F, .3F, 0}, {.2F, .3F, 0}}};

  void SetUp() override {
    auto &store     = session.store();
    auto at         = store.insert({}, 0, "Left right");
    const auto text = store.rebuild(at);
    for (const auto *owner : {"first", "second"}) {
      xanadu::Link link;
      link.owner = owner;
      link.left  = text.spansFor(0, 4);
      link.right = text.spansFor(5, 5);
      at         = store.addLink(at, std::move(link));
    }
    for (const auto &link : store.linkView()) {
      if (link.owner == "first") links[0] = link.id;
      if (link.owner == "second") links[1] = link.id;
    }
    ASSERT_NE(links[0], zigzag::noCell);
    ASSERT_NE(links[1], zigzag::noCell);
    session.views().push_back({.version = at, .pieces = store.rebuild(at)});
    overlay.setAnchorResolver(
        [this](const RenderState &) { return std::optional{anchors}; });
    overlay.deviceReady(device, {});
    ASSERT_TRUE(
        navigation.execute(nav::SelectLink{.key = navigation.keyOf(links[0])}));
    ASSERT_TRUE(navigation.selection());
    draw();
  }

  void draw() {
    state.beginPickScene();
    gleditor::FrameContext frame{.state          = state,
                                 .viewProjection = projection,
                                 .screenWidth    = metrics.screenWidth,
                                 .screenHeight   = metrics.screenHeight,
                                 .timeline       = timeline,
                                 .chrome         = metrics.chrome,
                                 .metrics        = metrics,
                                 .theme          = theme};
    overlay.drawFrame(frame);
  }

  render::PickingResult captured(const std::string &label) {
    const auto scene = overlay.presentation();
    const auto found =
        std::ranges::find(scene->visuals, label, &ui::WidgetVisual::text);
    EXPECT_NE(found, scene->visuals.end());
    render::PickingResult pick;
    pick.requestId = 1;
    if (found == scene->visuals.end()) return pick;
    EXPECT_FALSE(state.overlayPickScene.widgetOverlays.empty());
    pick.tag = render::unpackPickingTag(
        state.overlayPickScene.widgetOverlays.back().identity, found->pickingId,
        0);
    pick.overlayWidgetId =
        render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
    return pick;
  }
};

TEST_F(LinkPanelOverlayTest,
       WarmFramesReuseShapingAndMovingAnchorsKeepActionIdentity) {
  const auto first = overlay.presentation();
  ASSERT_NE(first, nullptr);
  const auto pick = captured("\u00d7");
  ASSERT_TRUE(pick.overlayWidgetId);
  EXPECT_CALL(device, updateBuffer).Times(0);
  {
    gleditor::text::ShapingStatsScope shaping;
    for (int frame = 0; frame < 50; ++frame) draw();
    EXPECT_EQ(overlay.presentation(), first);
    EXPECT_EQ(shaping.stats(), gleditor::text::ShapingStats{});
  }
  testing::Mock::VerifyAndClearExpectations(&device);
  anchors[0].x += .1F;
  anchors[1].x += .1F;
  draw();
  EXPECT_NE(overlay.presentation(), first);
  EXPECT_NE(overlay.presentation()->find(*pick.overlayWidgetId), nullptr);
  metrics.fontScale = 1.5F;
  draw();
  EXPECT_NE(overlay.presentation()->find(*pick.overlayWidgetId), nullptr);
  EXPECT_TRUE(overlay.picked(pick, state));
  EXPECT_TRUE(overlay.busy());
  EXPECT_TRUE(navigation.selection());
  draw();
  EXPECT_FALSE(overlay.busy());
  EXPECT_FALSE(navigation.selection());
}

TEST_F(LinkPanelOverlayTest,
       CapturedAndQueuedActionsCannotApplyToAnotherSelection) {
  const auto old = captured("\u00d7");
  ASSERT_TRUE(old.overlayWidgetId);
  EXPECT_TRUE(
      overlay.performAction(gleditor::a11y::Ids::of(27, *old.overlayWidgetId),
                            gleditor::a11y::Action::Click, {}));
  EXPECT_TRUE(overlay.busy());
  ASSERT_TRUE(
      navigation.execute(nav::SelectLink{.key = navigation.keyOf(links[1])}));
  draw();
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->key.id, links[1]);
  EXPECT_FALSE(overlay.busy());
  EXPECT_TRUE(overlay.picked(old, state));
  EXPECT_FALSE(overlay.busy());
  EXPECT_FALSE(
      overlay.performAction(gleditor::a11y::Ids::of(27, *old.overlayWidgetId),
                            gleditor::a11y::Action::Click, {}));
  auto missing = captured("\u00d7");
  missing.overlayWidgetId.reset();
  EXPECT_TRUE(overlay.picked(missing, state));
  EXPECT_FALSE(overlay.busy());
  auto foreign = old;
  ++foreign.tag.pageIndex;
  EXPECT_FALSE(overlay.picked(foreign, state));
  const auto current = captured("\u00d7");
  ASSERT_TRUE(current.overlayWidgetId);
  EXPECT_TRUE(overlay.performAction(
      gleditor::a11y::Ids::of(27, *current.overlayWidgetId),
      gleditor::a11y::Action::Click, {}));
  draw();
  EXPECT_FALSE(navigation.selection());
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 27);
  overlay.describe(builder);
  EXPECT_TRUE(tree.nodes.empty());
}

TEST_F(LinkPanelOverlayTest,
       DisabledPointerCommandIsQueuedForNavigatorRefusal) {
  const auto origin = captured("Origin");
  ASSERT_TRUE(origin.overlayWidgetId);
  EXPECT_FALSE(overlay.performAction(
      gleditor::a11y::Ids::of(27, *origin.overlayWidgetId),
      gleditor::a11y::Action::Click, {}));
  EXPECT_FALSE(overlay.busy());
  const auto revision = navigation.revision();
  EXPECT_TRUE(overlay.picked(origin, state));
  EXPECT_TRUE(overlay.busy());
  draw();
  EXPECT_FALSE(overlay.busy());
  EXPECT_EQ(navigation.revision(), revision);
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->key.id, links[0]);
}

TEST(LinkPanelPresentationTest,
     NamedFontRolesFollowLiveThemeAndLegacyFontsRemainExplicit) {
  const auto lines = contextLines();
  gleditor::text::ShapingCache cache;
  const ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  ui::Theme theme;
  auto &caption = theme.fonts[static_cast<std::size_t>(ui::FontRole::Caption)];
  caption       = {"Serif", 14};
  xanadu::LinkPanelConfig config;
  config.font      = "caption";
  const auto named = common_ui::linkPanelPresentation(lines, {}, {}, metrics,
                                                      theme, config, {}, cache);
  EXPECT_EQ(named.theme.font(ui::FontRole::Label), caption);
  caption            = {"Monospace", 18};
  const auto changed = common_ui::linkPanelPresentation(
      lines, {}, {}, metrics, theme, config, {}, cache);
  EXPECT_EQ(changed.theme.font(ui::FontRole::Label), caption);
  config.font       = "Serif Bold 13";
  const auto legacy = common_ui::linkPanelPresentation(
      lines, {}, {}, metrics, theme, config, {}, cache);
  EXPECT_EQ(legacy.theme.font(ui::FontRole::Label).family, "Serif Bold");
  EXPECT_FLOAT_EQ(legacy.theme.font(ui::FontRole::Label).points, 13);
  config.font.clear();
  const auto empty = common_ui::linkPanelPresentation(lines, {}, {}, metrics,
                                                      theme, config, {}, cache);
  EXPECT_EQ(empty.theme.font(ui::FontRole::Label),
            theme.font(ui::FontRole::Label));
  for (std::size_t i = 0; i < ui::kFontRoleCount; ++i)
    EXPECT_EQ(ui::fontRoleNamed(ui::kFontRoleNames[i]),
              static_cast<ui::FontRole>(i));
  EXPECT_FALSE(ui::fontRoleNamed("unknown").has_value());
}
} // namespace
