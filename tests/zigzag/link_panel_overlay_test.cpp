#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <set>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text_source.hpp>

#include "../lib/mocks/device.hpp"
#include "common/ui/link_panel_presentation.hpp"
#include "common/ui/xanadoc/link_panel_overlay.hpp"
#include "common/xanadu/link_views.hpp"

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

std::array<ui::WidgetId, 11> actionIds() {
  return {1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034};
}

TEST(LinkPanelPresentationTest, BothChosenLineBandsStayClearOfContextChrome) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  const ui::UiMetrics metrics{.screenWidth  = 1800,
                              .screenHeight = 1050,
                              .chrome       = {.top = 44, .bottom = 18}};
  const ui::Rect band{0, 485, 1800, 65};
  gleditor::text::ShapingCache measurements;
  const auto leaf = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, ui::Theme{}, xanadu::LinkPanelConfig{},
      std::nullopt, measurements, band);
  contained(leaf.bounds, metrics.pixelSafeArea());
  EXPECT_TRUE(leaf.bounds.bottom + leaf.bounds.height <= band.bottom ||
              leaf.bounds.bottom >= band.bottom + band.height);
}

TEST(LinkPanelPresentationTest,
     InvestigationPagesRetainCommandsOutsideReadingText) {
  auto lines = contextLines();
  lines.push_back(
      {.text = "Pinned exact authority and state", .requiredBytes = 32});
  auto buttons = xanadu::linkPanelButtons({}, false);
  for (std::uint32_t i = 0; i < 60; ++i)
    buttons.push_back(
        {.label   = "Preview Left member " + std::to_string(i + 1),
         .command = nav::SelectMember{{}, xanadu::LinkSide::Left, i}});
  std::vector<ui::WidgetId> ids;
  for (std::size_t i = 0; i < buttons.size(); ++i) ids.push_back(1024 + i);
  const ui::UiMetrics metrics{.screenWidth = 1600, .screenHeight = 1000};
  const std::array reading{ui::Rect{0, 0, 850, 1000}};
  gleditor::text::ShapingCache cache;
  std::set<ui::WidgetId> reached;
  std::size_t pages = 1;
  for (std::size_t page = 0; page < pages; ++page) {
    const auto shown = common_ui::linkPanelInvestigationPresentation(
        lines, buttons, ids, 2000, {2001, 2002, page}, metrics, {}, {}, cache,
        reading);
    ASSERT_TRUE(shown.completeContent);
    ASSERT_TRUE(shown.pages);
    EXPECT_FALSE(shown.reservedBottom);
    EXPECT_GE(shown.bounds.left, reading[0].left + reading[0].width);
    pages = shown.pages->count;
    ui::ScreenOverlay panel(shown.model);
    panel.setBounds(shown.bounds);
    const auto scene = panel.prepare(metrics, shown.theme);
    for (const auto &action : shown.actions) {
      reached.insert(action.id);
      const auto *box = scene->layout.find(action.id);
      ASSERT_NE(box, nullptr);
      ASSERT_NE(scene->find(action.id), nullptr);
      ASSERT_NE(scene->layout.find(box->parentId), nullptr);
      contained(box->rect, scene->layout.find(box->parentId)->rect);
      EXPECT_FALSE(scene->find(action.id)->fitted.truncated);
    }
  }
  EXPECT_GT(pages, 1U);
  EXPECT_EQ(reached.size(), buttons.size());
}

TEST(LinkPanelPresentationTest,
     InvestigationRefusesToCoverTheOnlyReadingSurface) {
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  const ui::UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const std::array reading{metrics.pixelSafeArea()};
  gleditor::text::ShapingCache cache;
  const auto shown = common_ui::linkPanelInvestigationPresentation(
      contextLines(), buttons, ids, 2000, {2001, 2002, 0}, metrics, {}, {},
      cache, reading);
  EXPECT_FALSE(shown.completeContent);
  EXPECT_FALSE(shown.safePlacement);
  EXPECT_FALSE(shown.reservedBottom);
}

TEST(LinkPanelPresentationTest,
     SeparateTextExclusionsLeaveBlankCanvasAvailable) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  const ui::UiMetrics metrics{.screenWidth  = 1800,
                              .screenHeight = 1050,
                              .chrome       = {.top = 44, .bottom = 18}};
  const std::array text{ui::Rect{0, 0, 890, 1050},
                        ui::Rect{909, 350, 890, 210}};
  gleditor::text::ShapingCache measurements;
  const auto leaf = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, ui::Theme{}, xanadu::LinkPanelConfig{},
      std::nullopt, measurements, std::nullopt, text);
  contained(leaf.bounds, metrics.pixelSafeArea());
  EXPECT_GE(leaf.bounds.left, 890);
  EXPECT_GE(leaf.bounds.bottom, 560);
}

TEST(LinkPanelPresentationTest, NarrowFreeAreaKeepsFullActionCaptions) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  const ui::UiMetrics metrics{.screenWidth  = 1440,
                              .screenHeight = 1000,
                              .chrome       = {.top = 44, .bottom = 18}};
  const ui::Rect reading{0, 0, 1040, 1000};
  gleditor::text::ShapingCache cache;
  const auto leaf = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, ui::Theme{}, {}, {}, cache, reading);
  ASSERT_TRUE(leaf.safePlacement);
  ASSERT_TRUE(leaf.completeContent);
  EXPECT_GE(leaf.bounds.left, reading.left + reading.width);
  ui::ScreenOverlay panel(leaf.model);
  panel.setBounds(leaf.bounds);
  const auto scene = panel.prepare(metrics, leaf.theme);
  ASSERT_NE(scene, nullptr);
  for (const auto id : ids) {
    ASSERT_NE(scene->find(id), nullptr);
    EXPECT_FALSE(scene->find(id)->fitted.truncated) << scene->find(id)->text;
    contained(scene->layout.find(id)->rect, leaf.bounds);
  }
}

TEST(LinkPanelPresentationTest, CaptionPaddingFitsConstrainedActionColumns) {
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  gleditor::text::ShapingCache cache;
  for (const auto width : {320, 360, 400, 480}) {
    for (const auto height : {270, 320, 400}) {
      SCOPED_TRACE(testing::Message() << width << "x" << height);
      const ui::UiMetrics metrics{.screenWidth = width, .screenHeight = height};
      xanadu::LinkPanelConfig config;
      config.font           = "Sans 10";
      config.maxWidthShare  = 1;
      config.maxHeightShare = 1;
      const auto leaf       = common_ui::linkPanelPresentation(
          contextLines(), buttons, ids, metrics, ui::Theme{}, config, {},
          cache);
      if (!leaf.completeContent) continue;
      ui::ScreenOverlay panel(leaf.model);
      panel.setBounds(leaf.bounds);
      const auto scene = panel.prepare(metrics, leaf.theme);
      ASSERT_NE(scene, nullptr);
      for (const auto id : ids) {
        ASSERT_NE(scene->find(id), nullptr);
        EXPECT_FALSE(scene->find(id)->fitted.truncated)
            << scene->find(id)->text;
      }
    }
  }
}

TEST(LinkPanelPresentationTest, MoreBlankHeightDoesNotInflateLabelSpacing) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  ui::UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  gleditor::text::ShapingCache measurements;
  const auto normal = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, ui::Theme{}, xanadu::LinkPanelConfig{},
      std::nullopt, measurements);
  metrics.screenHeight = 1440;
  const auto taller    = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, ui::Theme{}, xanadu::LinkPanelConfig{},
      std::nullopt, measurements);
  EXPECT_FLOAT_EQ(normal.bounds.height, taller.bounds.height);
  for (std::size_t index = 0; index < lines.size(); ++index)
    EXPECT_FLOAT_EQ(normal.model.children[index].preferred.height,
                    taller.model.children[index].preferred.height);
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
     ExactRangePrefixesSurviveLongContextAndSmallLineLimit) {
  auto lines             = contextLines();
  const std::string left = "Left 2/7 · place 1/4 · bytes\n2302771–2303273";
  const std::string right =
      "Right 3/9 · place 5/5 · bytes\n4294967200–4294967295";
  lines[1].text           = left + "\n" + std::string(250, 'L');
  lines[1].requiredBytes  = left.size();
  lines[2].text           = right + "\n" + std::string(250, 'R');
  lines[2].requiredBytes  = right.size();
  lines[3].compactContext = "outside linked range";
  lines[4].compactContext = "origin saved";
  const auto buttons      = xanadu::linkPanelButtons({}, false);
  const auto ids          = actionIds();
  gleditor::text::ShapingCache measurements;
  xanadu::LinkPanelConfig config;
  config.maxLines = 1;
  for (const auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{1800, 1050}}) {
    for (const auto scale : {1.F, 1.5F, 2.F}) {
      SCOPED_TRACE(testing::Message() << size.width << " scale " << scale);
      const ui::UiMetrics metrics{.fontScale    = scale,
                                  .screenWidth  = static_cast<int>(size.width),
                                  .screenHeight = static_cast<int>(size.height),
                                  .chrome       = {.top = 44, .bottom = 18}};
      const std::optional<ui::Rect> reading =
          size.width == 1280 && scale == 2
              ? std::optional<ui::Rect>{{0, 450, 1280, 350}}
              : std::nullopt;
      const auto leaf = common_ui::linkPanelPresentation(
          lines, buttons, ids, metrics, ui::Theme{}, config, std::nullopt,
          measurements, reading);
      if (!leaf.completeContent) {
        // The overlay reserves a dock before drawing an incomplete floating
        // layout. Every range and command must remain readable through paging.
        auto dockMetrics        = metrics;
        dockMetrics.marginShare = 0;
        std::set<ui::WidgetId> reached;
        std::set<std::string> ranges;
        std::size_t count = 1;
        for (std::size_t page = 0; page < count; ++page) {
          const auto dock = common_ui::linkPanelDockPresentation(
              lines, buttons, ids, 2000, false, dockMetrics, ui::Theme{},
              config, measurements,
              common_ui::LinkPanelPageRequest{2001, 2002, page});
          ASSERT_TRUE(dock.completeContent);
          count = dock.pages ? dock.pages->count : 1;
          ASSERT_TRUE(dock.reservedBottom);
          EXPECT_EQ(*dock.reservedBottom,
                    dock.bounds.bottom + dock.bounds.height);
          ui::ScreenOverlay panel(dock.model);
          panel.setBounds(dock.bounds);
          const auto scene = panel.prepare(dockMetrics, dock.theme);
          ASSERT_NE(scene, nullptr);
          contained(dock.bounds, dockMetrics.pixelSafeArea());
          if (!dock.pages) {
            // A complete compact dock needs no paging. Verify the same exact
            // prefixes and commands rather than requiring the fallback mode.
            for (const auto index : {1U, 2U}) {
              const auto *visual = scene->find(index + 3);
              ASSERT_NE(visual, nullptr);
              EXPECT_GE(visual->fitted.visibleBytes,
                        lines[index].requiredBytes +
                            (lines[index].active ? std::string("\u25B8 ").size()
                                                 : 0));
              ranges.insert(lines[index].text);
            }
            for (const auto &action : dock.actions) {
              reached.insert(action.id);
              const auto *visual = scene->find(action.id);
              ASSERT_NE(visual, nullptr);
              EXPECT_FALSE(visual->fitted.truncated);
            }
            for (const auto &box : scene->layout.boxes)
              contained(box.rect, dock.bounds);
            continue;
          }
          for (const auto id : {4U, 5U, 2000U, 2001U, 2002U}) {
            ASSERT_NE(scene->find(id), nullptr);
            EXPECT_FALSE(scene->find(id)->fitted.truncated);
          }
          EXPECT_NE(scene->find(4)->text.find("2/7"), std::string::npos);
          EXPECT_NE(scene->find(5)->text.find("3/9"), std::string::npos);
          for (const auto &action : dock.actions) {
            reached.insert(action.id);
            const auto *visual = scene->find(action.id);
            ASSERT_NE(visual, nullptr);
            EXPECT_FALSE(visual->fitted.truncated) << visual->text;
            EXPECT_EQ(action.command,
                      buttons[std::ranges::find(ids, action.id) - ids.begin()]
                          .command);
          }
          for (const auto index : {1U, 2U})
            if (const auto *visual = scene->find(index + 20)) {
              EXPECT_FALSE(visual->fitted.truncated);
              EXPECT_EQ(visual->accessibleLabel, lines[index].text);
              const auto required = lines[index].text.substr(
                  lines[index].text.find('\n') + 1,
                  lines[index].requiredBytes - lines[index].text.find('\n') -
                      1);
              EXPECT_NE(visual->text.find(required), std::string::npos);
              ranges.insert(visual->accessibleLabel);
            }
          for (const auto &box : scene->layout.boxes)
            contained(box.rect, dock.bounds);
        }
        EXPECT_EQ(reached, std::set<ui::WidgetId>(ids.begin(), ids.end()));
        EXPECT_EQ(ranges,
                  (std::set<std::string>{lines[1].text, lines[2].text}));
        continue;
      }
      ASSERT_TRUE(leaf.safePlacement);
      ui::ScreenOverlay panel(leaf.model);
      panel.setBounds(leaf.bounds);
      const auto scene = panel.prepare(metrics, leaf.theme);
      ASSERT_NE(scene, nullptr);
      contained(leaf.bounds, metrics.pixelSafeArea());
      if (reading)
        EXPECT_LE(leaf.bounds.bottom + leaf.bounds.height, reading->bottom);
      for (const auto index : {1U, 2U}) {
        const auto *visual = scene->find(index + 3);
        ASSERT_NE(visual, nullptr);
        const auto required =
            lines[index].requiredBytes +
            (lines[index].active ? std::string("\u25B8 ").size() : 0);
        EXPECT_GE(visual->fitted.visibleBytes, required);
        EXPECT_EQ(visual->accessibleLabel, lines[index].active
                                               ? "\u25B8 " + lines[index].text
                                               : lines[index].text);
      }
      for (const auto id : ids) {
        const auto *visual = scene->find(id);
        ASSERT_NE(visual, nullptr);
        const auto *box = scene->layout.find(id);
        ASSERT_NE(box, nullptr);
        EXPECT_FALSE(visual->fitted.truncated)
            << visual->text << " fitted " << visual->fitted.widthPx << "x"
            << visual->fitted.heightPx << " font " << visual->fontDescription
            << " content " << box->contentRect.width << "x"
            << box->contentRect.height << " panel " << leaf.bounds.height
            << " padding " << leaf.theme.paddingEm << " flow "
            << leaf.model.children.back().preferred.height << " row "
            << leaf.model.children.back().children.front().preferred.height
            << " labels " << leaf.model.children.size();
        contained(box->rect, scene->layout.bounds);
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

  gleditor::ScreenInsets draw(gleditor::ScreenInsets settled = {}) {
    state.beginPickScene();
    gleditor::FrameContext frame{.state          = state,
                                 .viewProjection = projection,
                                 .screenWidth    = metrics.screenWidth,
                                 .screenHeight   = metrics.screenHeight,
                                 .timeline       = timeline,
                                 .chrome         = metrics.chrome,
                                 .settledChrome  = settled,
                                 .metrics        = metrics,
                                 .theme          = theme};
    overlay.drawFrame(frame);
    return frame.chrome;
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
       ClosedOccurrenceKeepsExactIdentityUntilItsVersionIsReopened) {
  metrics.screenWidth   = 1800;
  metrics.screenHeight  = 1400;
  const auto version    = session.views().front().version;
  const auto authority  = session.store().documentId();
  const auto wrong      = session.store().insert(version, 0, "Another state ");
  const auto selected   = *navigation.selection();
  const auto operations = session.store().opCount();
  const auto visits     = navigation.walkEntries().size();
  std::optional<xanadu::Extent> focused;
  navigation.setFocusDocument([&](std::size_t view, xanadu::Extent range) {
    EXPECT_EQ(view, 0U);
    focused = range;
  });
  session.viewClosed(0);
  overlay.toggleInvestigation();
  const auto labels = [&] {
    draw();
    gleditor::a11y::Tree tree;
    gleditor::a11y::Builder builder(tree, 27);
    overlay.describe(builder);
    std::string result;
    for (const auto &node : tree.nodes) result += node.label + "\n";
    return result;
  };
  const auto identity = "Left occurrence store: " + authority.str() +
                        "\nState: " + version.str() + " · bytes 0–4";
  const auto unavailable =
      "Left exact source reference unavailable: " +
      std::string(xanadu::name(xanadu::ContentReferenceError::WrongFrame));
  const auto closed = labels();
  EXPECT_NE(closed.find(identity), std::string::npos);
  EXPECT_NE(closed.find(unavailable), std::string::npos);
  EXPECT_FALSE(navigation.execute(nav::Enter{}));
  EXPECT_FALSE(focused);
  EXPECT_EQ(navigation.walkEntries().size(), visits);

  session.viewOpened(wrong);
  const auto other = labels();
  EXPECT_NE(other.find(identity), std::string::npos);
  EXPECT_NE(other.find(unavailable), std::string::npos);
  EXPECT_FALSE(navigation.execute(nav::Enter{}));
  EXPECT_FALSE(focused);
  EXPECT_EQ(navigation.walkEntries().size(), visits);
  session.viewClosed(0);

  session.viewOpened(version);
  const auto reopened = labels();
  EXPECT_NE(reopened.find("Pinned state: " + version.str()), std::string::npos);
  EXPECT_EQ(reopened.find(unavailable), std::string::npos);
  ASSERT_TRUE(navigation.selection());
  EXPECT_EQ(navigation.selection()->key, selected.key);
  EXPECT_EQ(navigation.selection()->left, selected.left);
  EXPECT_EQ(navigation.selection()->right, selected.right);
  EXPECT_EQ(navigation.selection()->origin, selected.origin);
  EXPECT_EQ(session.store().opCount(), operations);
  EXPECT_EQ(navigation.walkEntries().size(), visits);
  ASSERT_TRUE(navigation.execute(nav::Enter{}));
  EXPECT_EQ(focused, (xanadu::Extent{0, 4}));
  EXPECT_EQ(navigation.walkEntries().size(), visits + 1);
  EXPECT_EQ(session.store().opCount(), operations);
}

TEST_F(LinkPanelOverlayTest, PreviewEdgesKeepLinkHueExactRangesAndActiveSide) {
  const auto key = navigation.keyOf(links[0]);
  ASSERT_TRUE(navigation.execute(
      nav::SelectOccurrence{key, xanadu::LinkSide::Left, 0, 0}));
  ASSERT_TRUE(navigation.execute(
      nav::SelectOccurrence{key, xanadu::LinkSide::Right, 0, 0}));
  auto app           = std::make_shared<AppState>();
  auto renderer      = Renderer::create(app, render::Backend::OpenGL);
  auto doc           = Doc::create(renderer, &device, glm::mat4(1.F),
                                   gleditor::MemoryTextSource("Left right", "edge-test"));
  const auto collect = [&] {
    std::vector<gleditor::SpanStyle> styles;
    overlay.decorate(*doc, styles);
    return styles;
  };
  const auto styles = collect();
  ASSERT_EQ(styles.size(), 2U);
  EXPECT_EQ(styles[0].start, 0U);
  EXPECT_EQ(styles[0].end, 4U);
  EXPECT_EQ(styles[1].start, 5U);
  EXPECT_EQ(styles[1].end, 10U);
  const auto selectedState = navigation.selection();
  const auto &selected     = *selectedState;
  const auto hue =
      session.selectedLinkColour(key, selected.occurrences->link) & 0xFFFFFF00U;
  for (const auto &style : styles) {
    EXPECT_EQ(style.emphasis, gleditor::SpanEmphasis::Contour);
    for (const auto shift : {8U, 16U, 24U}) {
      EXPECT_EQ((style.colour >> shift) & 255U,
                ((hue >> shift) & 255U) * 3U / 5U);
    }
  }
  EXPECT_GT(styles[1].edgeWidthPx, styles[0].edgeWidthPx);
  ASSERT_TRUE(navigation.execute(nav::Cross{}));
  const auto crossed = collect();
  ASSERT_EQ(crossed.size(), 2U);
  EXPECT_GT(crossed[0].edgeWidthPx, crossed[1].edgeWidthPx);
  ASSERT_TRUE(navigation.execute(nav::Dismiss{}));
  EXPECT_TRUE(collect().empty());
}

TEST_F(LinkPanelOverlayTest,
       NavigationSelectionOwnershipSurvivesCrossButNotEditsOrOtherLinks) {
  auto app      = std::make_shared<AppState>();
  auto renderer = Renderer::create(app, render::Backend::OpenGL);
  auto doc =
      Doc::create(renderer, &device, glm::mat4(1.F),
                  gleditor::MemoryTextSource("Left right", "ownership-test"));
  Caret caret(&device);
  caret.placeAt(0, 0);
  caret.extendTo(4);
  const auto first = navigation.keyOf(links[0]);
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  overlay.noteNavigationSelection(caret, first);
  const auto owned = overlay.selectionStyle(*doc, caret);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->start, 0U);
  EXPECT_EQ(owned->end, 4U);
  EXPECT_EQ(owned->layer, session.linkHighlightLayer(first));
  ASSERT_TRUE(navigation.execute(nav::Cross{}));
  ASSERT_TRUE(overlay.selectionStyle(*doc, caret));
  EXPECT_EQ(overlay.selectionStyle(*doc, caret)->layer, owned->layer);
  caret.extendTo(3);
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  caret.extendTo(4);
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  overlay.noteNavigationSelection(caret, first);
  ASSERT_TRUE(navigation.execute(nav::SelectLink{navigation.keyOf(links[1])}));
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  EXPECT_TRUE(caret.hasSelection());
  ASSERT_TRUE(navigation.execute(nav::SelectLink{first}));
  overlay.noteNavigationSelection(caret, first);
  ASSERT_TRUE(navigation.execute(nav::Dismiss{}));
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  EXPECT_EQ(caret.selectionStart(), 0U);
  EXPECT_EQ(caret.selectionEnd(), 4U);
  ASSERT_TRUE(navigation.execute(nav::SelectLink{first}));
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  overlay.noteNavigationSelection(caret, std::nullopt);
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
  overlay.noteNavigationSelection(caret, first);
  ASSERT_TRUE(overlay.selectionStyle(*doc, caret));
  session.views()[0].version = {};
  EXPECT_FALSE(overlay.selectionStyle(*doc, caret));
}

TEST_F(LinkPanelOverlayTest, PresentationLayersKeepAuthorityAndLinkIdentity) {
  const auto first  = navigation.keyOf(links[0]);
  auto foreign      = first;
  foreign.authority = xanadu::DocumentId{};
  const auto layer  = session.linkHighlightLayer(first);
  EXPECT_EQ(session.linkHighlightLayer(first), layer);
  EXPECT_NE(session.linkHighlightLayer(navigation.keyOf(links[1])), layer);
  EXPECT_NE(session.linkHighlightLayer(foreign), layer);
  EXPECT_EQ(session.linkHighlightLayer(first), layer);
}

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

TEST(LinkPanelPresentationTest, ReadingContentRemainsClearWhenThereIsRoom) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  gleditor::text::ShapingCache measurements;
  const ui::Theme theme;
  const xanadu::LinkPanelConfig config;
  for (const float scale : {1.F, 1.5F}) {
    const ui::UiMetrics metrics{
        .userScale = scale, .screenWidth = 800, .screenHeight = 600};
    for (const auto reading :
         {ui::Rect{400, 260, 220, 20}, ui::Rect{110, 260, 220, 20},
          ui::Rect{950, 260, 220, 20}}) {
      const auto leaf =
          common_ui::linkPanelPresentation(lines, buttons, ids, metrics, theme,
                                           config, {}, measurements, reading);
      contained(leaf.bounds, metrics.pixelSafeArea());
      const auto &panel = leaf.bounds;
      EXPECT_TRUE(panel.left + panel.width <= reading.left ||
                  panel.left >= reading.left + reading.width ||
                  panel.bottom + panel.height <= reading.bottom ||
                  panel.bottom >= reading.bottom + reading.height);
      ui::ScreenOverlay overlay(leaf.model);
      overlay.setBounds(panel);
      const auto scene = overlay.prepare(metrics, leaf.theme);
      ASSERT_NE(scene, nullptr);
      for (const auto id : ids) {
        const auto *box = scene->layout.find(id);
        ASSERT_NE(box, nullptr);
        EXPECT_GT(box->rect.height, 0);
        contained(box->rect, panel);
      }
    }
  }
}

TEST(LinkPanelPresentationTest, NoFreeAreaIsExplicitAndDockPreservesCommands) {
  auto lines    = contextLines();
  lines[1].text = "Left 2/7 · place 1/4 · bytes\n2302771–2303273 (part)\nKJV";
  lines[2].text = "Right 3/9 · place 5/5 · bytes\n0–34\nNotes";
  lines[1].requiredBytes  = lines[1].text.find("\nKJV");
  lines[2].requiredBytes  = lines[2].text.find("\nNotes");
  lines[3].compactContext = "reading: left member 2";
  lines[4].compactContext = "origin saved";
  const auto buttons      = xanadu::linkPanelButtons({}, false);
  const auto ids          = actionIds();
  const ui::UiMetrics metrics{
      .screenWidth = 640, .screenHeight = 480, .chrome = {.top = 76}};
  gleditor::text::ShapingCache cache;
  const auto floating = common_ui::linkPanelPresentation(
      lines, buttons, ids, metrics, ui::Theme{}, {}, {}, cache,
      metrics.pixelSafeArea());
  EXPECT_FALSE(floating.safePlacement);
  for (const auto collapsed : {false, true}) {
    const auto dock = common_ui::linkPanelDockPresentation(
        lines, buttons, ids, 2000, collapsed, metrics, ui::Theme{}, {}, cache);
    EXPECT_TRUE(dock.completeContent);
    EXPECT_EQ(dock.toggleControls, 2000);
    EXPECT_EQ(dock.bounds.bottom, metrics.chrome.bottom);
    EXPECT_EQ(dock.bounds.width, 640);
    ASSERT_TRUE(dock.reservedBottom);
    EXPECT_LE(*dock.reservedBottom, metrics.screenHeight * .65F);
    EXPECT_EQ(dock.actions.size(), collapsed ? 1 : buttons.size());
    ui::ScreenOverlay panel(dock.model);
    panel.setBounds(dock.bounds);
    auto dockMetrics        = metrics;
    dockMetrics.marginShare = 0;
    const auto scene        = panel.prepare(dockMetrics, dock.theme);
    ASSERT_NE(scene, nullptr);
    for (const auto &action : dock.actions) {
      const auto *visual = scene->find(action.id);
      ASSERT_NE(visual, nullptr);
      EXPECT_FALSE(visual->fitted.truncated) << visual->text;
      contained(scene->layout.find(action.id)->rect, dock.bounds);
    }
    EXPECT_FALSE(scene->find(2000)->fitted.truncated);
    EXPECT_NE(scene->find(4)->accessibleLabel.find("2303273"),
              std::string::npos);
    if (collapsed)
      EXPECT_NE(scene->find(4)->text.find("(part)"), std::string::npos);
    if (!collapsed)
      EXPECT_GE(scene->find(4)->fitted.visibleBytes, lines[1].requiredBytes);
  }
  auto enlarged       = metrics;
  enlarged.fontScale  = 3;
  const auto tooSmall = common_ui::linkPanelDockPresentation(
      lines, buttons, ids, 2000, false, enlarged, ui::Theme{}, {}, cache);
  EXPECT_FALSE(tooSmall.completeContent);
  EXPECT_EQ(tooSmall.actions.size(), 1);
}

TEST_F(LinkPanelOverlayTest, DockWaitsForClipAndReadControlsPreserveSelection) {
  xanadu::LinkPanelConfig config;
  config.maxHeightShare = .2F;
  overlay.setConfig(config);
  const auto revision = navigation.revision();
  auto claim          = draw();
  EXPECT_GT(claim.bottom, 0);
  EXPECT_TRUE(overlay.busy());
  EXPECT_TRUE(state.overlayPickScene.widgetOverlays.empty());
  claim = draw(claim);
  EXPECT_FALSE(overlay.busy());
  const auto controls = captured("Controls");
  ASSERT_TRUE(controls.overlayWidgetId);
  EXPECT_TRUE(overlay.performAction(
      gleditor::a11y::Ids::of(27, *controls.overlayWidgetId),
      gleditor::a11y::Action::Click, {}));
  EXPECT_TRUE(overlay.busy());
  const auto expanded = draw(claim);
  EXPECT_GT(expanded.bottom, claim.bottom);
  EXPECT_TRUE(overlay.busy());
  EXPECT_TRUE(state.overlayPickScene.widgetOverlays.empty());
  claim = draw(expanded);
  EXPECT_FALSE(overlay.busy());
  EXPECT_EQ(navigation.revision(), revision);
  EXPECT_NE(captured("Enter").overlayWidgetId, std::nullopt);
  // A newly available floating position must not remove the active toggle.
  overlay.setConfig(xanadu::LinkPanelConfig{});
  claim = draw(claim);
  EXPECT_NE(captured("Read").overlayWidgetId, std::nullopt);
  overlay.toggleControls();
  const auto collapsed = draw(claim);
  EXPECT_LT(collapsed.bottom, claim.bottom);
  draw(collapsed);
  EXPECT_FALSE(overlay.busy());
  EXPECT_EQ(navigation.revision(), revision);
  EXPECT_NE(captured("Controls").overlayWidgetId, std::nullopt);
}

TEST(LinkPanelPresentationTest,
     EnlargedInspectorPagesKeepRangesAndEveryCommandReachable) {
  auto lines               = contextLines();
  lines[0].compactIdentity = "link 4294967295";
  lines[1].text = "Left 2/7 · place 1/4 · bytes\n2302771–2303273 (part)\nKJV";
  lines[2].text = "Right 3/9 · place 5/5 · bytes\n0–34\nNotes";
  lines[1].requiredBytes  = lines[1].text.find("\nKJV");
  lines[2].requiredBytes  = lines[2].text.find("\nNotes");
  lines[3].compactContext = "reading outside the range";
  lines[4].compactContext = "origin saved";
  const auto buttons      = xanadu::linkPanelButtons({}, false);
  const auto ids          = actionIds();
  for (const auto &[scale, height] :
       std::array{std::pair{2.F, 480}, std::pair{3.F, 800}}) {
    ui::UiMetrics metrics{.fontScale    = scale,
                          .screenWidth  = 640,
                          .screenHeight = height,
                          .chrome       = {.top = 76, .bottom = 18}};
    gleditor::text::ShapingCache cache;
    std::set<ui::WidgetId> reached;
    std::set<std::string> ranges;
    std::size_t count = 1;
    for (std::size_t page = 0; page < count; ++page) {
      const auto leaf = common_ui::linkPanelDockPresentation(
          lines, buttons, ids, 2000, false, metrics, ui::Theme{}, {}, cache,
          common_ui::LinkPanelPageRequest{2001, 2002, page});
      ASSERT_TRUE(leaf.completeContent);
      ASSERT_TRUE(leaf.pages);
      count = leaf.pages->count;
      EXPECT_GT(count, 1);
      EXPECT_EQ(leaf.pages->request.page, page);
      EXPECT_EQ(leaf.bounds.bottom, 18);
      EXPECT_LE(leaf.bounds.bottom + leaf.bounds.height, height - 76);
      metrics.marginShare = 0;
      ui::ScreenOverlay panel(leaf.model);
      panel.setBounds(leaf.bounds);
      const auto scene = panel.prepare(metrics, leaf.theme);
      for (const auto id : {3U, 4U, 5U, 2000U, 2001U, 2002U}) {
        ASSERT_NE(scene->find(id), nullptr);
        EXPECT_FALSE(scene->find(id)->fitted.truncated)
            << scene->find(id)->text;
      }
      EXPECT_NE(scene->find(3)->text.find("Controls"), std::string::npos);
      EXPECT_NE(scene->find(4)->text.find("2/7"), std::string::npos);
      EXPECT_NE(scene->find(5)->text.find("3/9"), std::string::npos);
      for (const auto &action : leaf.actions) {
        reached.insert(action.id);
        ASSERT_NE(scene->find(action.id), nullptr);
        EXPECT_FALSE(scene->find(action.id)->fitted.truncated);
      }
      for (const auto id : {21U, 22U})
        if (const auto *visual = scene->find(id)) {
          EXPECT_FALSE(visual->fitted.truncated) << visual->text;
          ranges.insert(visual->accessibleLabel);
        }
      for (const auto &box : scene->layout.boxes)
        contained(box.rect, leaf.bounds);
    }
    EXPECT_EQ(reached, std::set<ui::WidgetId>(ids.begin(), ids.end()));
    EXPECT_EQ(ranges, (std::set<std::string>{lines[1].text, lines[2].text}));
    metrics.screenHeight  = 200;
    const auto impossible = common_ui::linkPanelDockPresentation(
        lines, buttons, ids, 2000, false, metrics, ui::Theme{}, {}, cache,
        common_ui::LinkPanelPageRequest{2001, 2002, 0});
    EXPECT_FALSE(impossible.completeContent);
    EXPECT_FALSE(impossible.pages);
  }
}

TEST(LinkPanelPresentationTest, ConstrainedDockKeepsMeasuredActionRows) {
  auto lines    = contextLines();
  lines[1].text = "Left 2/7 · place 1/4 · bytes\n2302771–2303273\nContext";
  lines[2].text =
      "Right 3/9 · place 5/5 · bytes\n4294967200–4294967295\nContext";
  for (const auto index : {1U, 2U})
    lines[index].requiredBytes = lines[index].text.rfind('\n');
  lines[3].compactContext = "outside linked range";
  lines[4].compactContext = "origin saved";
  const auto buttons      = xanadu::linkPanelButtons({}, false);
  const auto ids          = actionIds();
  const ui::UiMetrics metrics{.fontScale    = 2,
                              .screenWidth  = 1280,
                              .screenHeight = 800,
                              .chrome       = {.top = 44, .bottom = 18},
                              .marginShare  = 0};
  xanadu::LinkPanelConfig config;
  config.dockMaxHeightShare = .4F;
  config.maxLines           = 1;
  gleditor::text::ShapingCache cache;
  const auto leaf = common_ui::linkPanelDockPresentation(
      lines, buttons, ids, 2000, false, metrics, ui::Theme{}, config, cache);
  ASSERT_TRUE(leaf.completeContent);
  ASSERT_FALSE(leaf.pages);
  ASSERT_TRUE(leaf.reservedBottom);
  ui::ScreenOverlay panel(leaf.model);
  panel.setBounds(leaf.bounds);
  const auto scene = panel.prepare(metrics, leaf.theme);
  ASSERT_NE(scene, nullptr);
  for (const auto id : ids) {
    ASSERT_NE(scene->find(id), nullptr);
    EXPECT_FALSE(scene->find(id)->fitted.truncated);
    contained(scene->layout.find(id)->rect, leaf.bounds);
  }
  EXPECT_FALSE(scene->find(2000)->fitted.truncated);
  for (const auto index : {1U, 2U}) {
    ASSERT_NE(scene->find(index + 3), nullptr);
    EXPECT_GE(scene->find(index + 3)->fitted.visibleBytes,
              lines[index].requiredBytes);
  }
}

TEST(LinkPanelPresentationTest, InspectorPinsWrappedCursorsAndPendingReason) {
  auto lines               = contextLines();
  lines[0].compactIdentity = "link 42";
  lines[1].text = "Left 12345/67890 · place 12345/67890 · bytes\n10–20\nKJV";
  lines[1].requiredBytes  = lines[1].text.find("\nKJV");
  lines[3].compactContext = "reading outside the range";
  lines[4].compactContext = "origin saved";
  lines.push_back({.text = "Entry pending: building exact target page"});
  auto buttons = xanadu::linkPanelButtons({}, false);
  buttons.push_back({.label = "Cancel", .command = xanadu::nav::CancelEntry{}});
  const auto baseIds = actionIds();
  std::vector<ui::WidgetId> ids(baseIds.begin(), baseIds.end());
  ids.push_back(1999);
  ui::UiMetrics metrics{
      .fontScale = 2, .screenWidth = 640, .screenHeight = 900};
  gleditor::text::ShapingCache cache;
  auto config               = xanadu::LinkPanelConfig{};
  config.dockMaxHeightShare = .1F;
  const auto leaf           = common_ui::linkPanelDockPresentation(
      lines, buttons, ids, 2000, false, metrics, ui::Theme{}, config, cache,
      common_ui::LinkPanelPageRequest{2001, 2002, 0});
  ASSERT_TRUE(leaf.completeContent);
  ASSERT_TRUE(leaf.pages);
  ui::ScreenOverlay panel(leaf.model);
  panel.setBounds(leaf.bounds);
  const auto scene = panel.prepare(metrics, leaf.theme);
  ASSERT_NE(scene->find(4), nullptr);
  EXPECT_FALSE(scene->find(4)->fitted.truncated);
  EXPECT_NE(scene->find(4)->text.find("12345/67890"), std::string::npos);
  ASSERT_NE(scene->find(65), nullptr);
  EXPECT_FALSE(scene->find(65)->fitted.truncated);
  EXPECT_NE(scene->find(65)->text.find("Entry pending"), std::string::npos);
  ASSERT_NE(scene->find(1999), nullptr);
}

TEST(LinkPanelPresentationTest, MinimumWarningAndEscapesFitAtEnlargedType) {
  const auto lines   = contextLines();
  const auto buttons = xanadu::linkPanelButtons({}, false);
  const auto ids     = actionIds();
  ui::UiMetrics metrics{.fontScale    = 3,
                        .screenWidth  = 640,
                        .screenHeight = 480,
                        .chrome       = {.top = 114, .bottom = 18}};
  gleditor::text::ShapingCache cache;
  const auto leaf = common_ui::linkPanelDockPresentation(
      lines, buttons, ids, 2000, true, metrics, ui::Theme{}, {}, cache,
      common_ui::LinkPanelPageRequest{2001, 2002, 0});
  EXPECT_FALSE(leaf.completeContent);
  ui::ScreenOverlay panel(leaf.model);
  panel.setBounds(leaf.bounds);
  const auto scene = panel.prepare(metrics, leaf.theme);
  ASSERT_NE(scene->find(3), nullptr);
  EXPECT_EQ(scene->find(3)->text, "Enlarge window");
  EXPECT_FALSE(scene->find(3)->fitted.truncated);
  EXPECT_NE(scene->find(3)->accessibleLabel.find("reduce UI text size"),
            std::string::npos);
  for (const auto id : {2000U, ids.back()}) {
    ASSERT_NE(scene->find(id), nullptr);
    EXPECT_FALSE(scene->find(id)->fitted.truncated);
  }
  EXPECT_EQ(scene->find(2000)->text, "Controls");
}

TEST_F(LinkPanelOverlayTest,
       OverflowPagingRetainsCommandIdentityWithoutNavigation) {
  metrics.fontScale = 2;
  xanadu::LinkPanelConfig config;
  config.maxHeightShare = .2F;
  overlay.setConfig(config);
  auto claim            = draw();
  claim                 = draw(claim);
  const auto revision   = navigation.revision();
  const auto operations = session.store().opCount();
  overlay.toggleControls();
  claim = draw(claim);
  claim = draw(claim);
  EXPECT_NE(captured("Next").overlayWidgetId, std::nullopt);
  const auto firstHeading = overlay.presentation()->find(3)->text;
  const auto next         = captured("Next");
  EXPECT_TRUE(
      overlay.performAction(gleditor::a11y::Ids::of(27, *next.overlayWidgetId),
                            gleditor::a11y::Action::Click, {}));
  claim = draw(claim);
  EXPECT_NE(overlay.presentation()->find(3)->text, firstHeading);
  EXPECT_EQ(navigation.revision(), revision);
  EXPECT_EQ(session.store().opCount(), operations);
  std::map<std::string, ui::WidgetId> commands;
  for (int step = 0; step < 8; ++step) {
    const auto scene = overlay.presentation();
    for (const auto &visual : scene->visuals)
      if (visual.action == "link_controls" && visual.id > 1024 &&
          visual.text != "Previous" && visual.text != "Next" &&
          visual.text != "Read") {
        const auto [at, inserted] = commands.emplace(visual.text, visual.id);
        if (!inserted) EXPECT_EQ(at->second, visual.id);
      }
    overlay.pageControls(1);
    claim = draw(claim);
  }
  for (int step = 0; step < 8; ++step) {
    overlay.pageControls(-1);
    claim = draw(claim);
    for (const auto &visual : overlay.presentation()->visuals)
      if (const auto at = commands.find(visual.text); at != commands.end())
        EXPECT_EQ(at->second, visual.id);
  }
  EXPECT_EQ(navigation.revision(), revision);
  EXPECT_EQ(session.store().opCount(), operations);
  overlay.toggleControls();
  claim = draw(claim);
  draw(claim);
  EXPECT_NE(captured("Controls").overlayWidgetId, std::nullopt);
}
