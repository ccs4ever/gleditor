#include "../lib/mocks/device.hpp"
#include "common/ui/quotation_builder_overlay.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include <algorithm>
#include <cstring>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <set>

namespace {
namespace ui = gleditor::ui;
class QuotationInkDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::uint32_t next{1};
  std::size_t uploads{}, draws{};
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  struct Drawn {
    Doc::VBORow row;
    std::uint32_t identity;
  };
  std::vector<Drawn> drawn;
  QuotationInkDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t size) {
          const auto id = next++;
          buffers[id].resize(size);
          return render::BufferHandle{id};
        });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault([this](render::BufferHandle handle, std::size_t size) {
          buffers[handle.id].resize(size);
          return handle;
        });
    ON_CALL(*this, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t offset,
                              std::span<const std::byte> data) {
          ++uploads;
          auto &storage = buffers[buffer.id];
          storage.resize(std::max(storage.size(), offset + data.size()));
          std::memcpy(storage.data() + offset, data.data(), data.size());
        });
    ON_CALL(*this, drawGlyphs)
        .WillByDefault([this](const render::DrawUniforms &uniforms,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          if (count) ++draws;
          const auto &storage = buffers.at(buffer.id);
          for (std::uint32_t index = 0; index < count; ++index) {
            Doc::VBORow row{};
            std::memcpy(&row, storage.data() + offset + index * sizeof(row),
                        sizeof(row));
            drawn.push_back({row, uniforms.identity});
          }
        });
  }
};

struct QuotationFixture {
  std::shared_ptr<xanadu::UserPermascroll> scroll{
      std::make_shared<xanadu::UserPermascroll>()};
  xanadu::Store store{scroll};
  xanadu::MicroversionId head;
  zigzag::DimRef dim;
  QuotationFixture() {
    head                 = store.sliceGenesis({}, "Quotation test slice");
    const auto dimension = store.makeDimension(head, "d.items.long.名称");
    head                 = dimension.version;
    dim                  = dimension.dim;
    for (int i = 0; i < 36; ++i) {
      head = store.makeCell(
          head,
          "A full Unicode cell 日本語 👩‍👩‍👧‍👦 café " +
              std::to_string(i));
      const auto cell = store.cellRefOf(head);
      const auto branch =
          store.makeDimension(head, "d.branch." + std::to_string(i));
      head = store.setLink(branch.version, store.homeCell(), branch.dim,
                           zigzag::DimVector::POS, cell);
    }
    for (int i = 0; i < 15; ++i) {
      const auto result =
          store.makeDimension(head, "d.extra.名称." + std::to_string(i));
      head = result.version;
    }
  }
};
const ui::WidgetVisual *findAction(const ui::WidgetScene &scene,
                                   std::string_view action) {
  const auto found =
      std::ranges::find(scene.visuals, action, &ui::WidgetVisual::action);
  return found == scene.visuals.end() ? nullptr : &*found;
}
void checkContains(ui::Rect child, ui::Rect parent) {
  EXPECT_GE(child.left, parent.left - .01F);
  EXPECT_GE(child.bottom, parent.bottom - .01F);
  EXPECT_LE(child.left + child.width, parent.left + parent.width + .01F);
  EXPECT_LE(child.bottom + child.height, parent.bottom + parent.height + .01F);
}
TEST(QuotationBuilderOverlayTest, PagesDrawFittedInkAndExposeFullSharedBounds) {
  QuotationFixture fixture;
  QuotationInkDevice device;
  RenderState state{&device};
  xanadu::QuotationBuilderOverlay overlay(fixture.store, fixture.head, {});
  overlay.setVisible(true)->setMode(xanadu::Selector::Kind::Closure);
  overlay.deviceReady(device, {});
  ch::Timeline timeline;
  glm::mat4 projection{1};
  for (const auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{2560, 1440}})
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F})
      for (const auto family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        ui::UiMetrics metrics{.fontScale    = scale,
                              .screenWidth  = static_cast<int>(size.width),
                              .screenHeight = static_cast<int>(size.height),
                              .chrome       = {.top = 44, .bottom = 18}};
        ui::Theme theme;
        theme.fonts[static_cast<std::size_t>(ui::FontRole::Label)].family =
            family;
        gleditor::FrameContext ctx{state,
                                   projection,
                                   metrics.screenWidth,
                                   metrics.screenHeight,
                                   timeline,
                                   metrics.chrome,
                                   {},
                                   metrics,
                                   theme};
        for (std::size_t step = 0; step < 6; ++step) {
          const std::array<std::string, 6> pages{"Source",   "Selector",
                                                 "Preview",  "Commit",
                                                 "Selector", "Selector"};
          const auto &page = pages[step];
          overlay.setMode(step == 4   ? xanadu::Selector::Kind::Rank
                          : step == 5 ? xanadu::Selector::Kind::Query
                                      : xanadu::Selector::Kind::Closure);
          SCOPED_TRACE(testing::Message() << size.width << ' ' << scale << ' '
                                          << family << ' ' << page);
          auto scene = overlay.prepare(metrics, theme);
          ASSERT_TRUE(scene);
          const auto tab =
              std::ranges::find(scene->visuals, std::string{page},
                                &ui::WidgetVisual::accessibleLabel);
          ASSERT_NE(tab, scene->visuals.end());
          ASSERT_TRUE(overlay.activateNode(tab->id));
          device.drawn.clear();
          state.beginPickScene();
          overlay.drawFrame(ctx);
          scene = overlay.prepare(metrics, theme);
          ASSERT_TRUE(scene);
          checkContains(scene->layout.bounds, metrics.pixelSafeArea());
          std::set<std::uint32_t> ids;
          for (const auto &box : scene->layout.boxes) {
            EXPECT_TRUE(ids.insert(box.id).second);
            checkContains(box.rect, scene->layout.bounds);
            checkContains(box.contentRect, box.rect);
            if (box.parentId) {
              const auto *parent = scene->layout.find(box.parentId);
              ASSERT_NE(parent, nullptr);
              checkContains(box.rect, parent->contentRect);
            }
          }
          std::map<std::uint32_t, std::size_t> glyphs;
          for (const auto &drawn : device.drawn) {
            const auto &row = drawn.row;
            if (row.foreground & Doc::VBORow::solidFlag) continue;
            auto tag = render::unpackPickingTag(drawn.identity,
                                                row.paper & 0xFFFFU, 0);
            tag.kind = row.quad & 3U;
            const auto identity =
                render::resolveOverlayWidget(state.overlayPickScene, tag);
            ASSERT_TRUE(identity);
            const auto *box = scene->layout.find(*identity);
            ASSERT_NE(box, nullptr);
            ++glyphs[*identity];
            const float width  = (row.quad >> 20U) & 4095U,
                        height = (row.quad >> 8U) & 4095U;
            checkContains({row.pos[0] - width * .5F, row.pos[1] - height * .5F,
                           width, height},
                          box->contentRect);
          }
          gleditor::a11y::Tree tree;
          gleditor::a11y::Builder builder(tree, 37);
          overlay.describe(builder);
          bool meaningful = false;
          for (const auto &node : tree.nodes) {
            const auto id = static_cast<std::uint32_t>(
                gleditor::a11y::Ids::localOf(node.id));
            const auto *box = scene->layout.find(id);
            ASSERT_NE(box, nullptr);
            ASSERT_TRUE(node.bounds);
            EXPECT_DOUBLE_EQ(node.bounds->left, box->rect.left);
            EXPECT_DOUBLE_EQ(node.bounds->right,
                             box->rect.left + box->rect.width);
            EXPECT_DOUBLE_EQ(node.bounds->top, metrics.screenHeight -
                                                   box->rect.bottom -
                                                   box->rect.height);
            if (node.label.empty() ||
                node.role == gleditor::a11y::Role::Group ||
                node.role == gleditor::a11y::Role::List)
              continue;
            EXPECT_GT(glyphs[id], 0U) << node.label;
            if (node.label.find("Selector:") != std::string::npos ||
                node.label.find("日本語") != std::string::npos ||
                node.label == "Quotation label" ||
                node.label.find("Root cell:") != std::string::npos ||
                node.label.find("d.items.long.名称") != std::string::npos)
              meaningful = true;
          }
          EXPECT_TRUE(meaningful);
        }
      }
}
TEST(QuotationBuilderOverlayTest,
     UnicodeFieldsKeepCaretAndRegisteredModalFocus) {
  QuotationFixture fixture;
  QuotationInkDevice device;
  RenderState state{&device};
  xanadu::QuotationBuilderOverlay overlay(fixture.store, fixture.head, {});
  overlay.setVisible(true)->setMode(xanadu::Selector::Kind::Rank);
  overlay.deviceReady(device, {});
  ui::FocusManager focus;
  overlay.syncFocus(focus);
  ch::Timeline timeline;
  glm::mat4 projection{1};
  gleditor::FrameContext ctx{state, projection, 1280, 800, timeline};
  overlay.drawFrame(ctx);
  auto scene = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                               ui::defaultTheme());
  const auto selectorTab =
      std::ranges::find(scene->visuals, std::string{"Selector"},
                        &ui::WidgetVisual::accessibleLabel);
  ASSERT_NE(selectorTab, scene->visuals.end());
  ASSERT_TRUE(overlay.activateNode(selectorTab->id));
  overlay.drawFrame(ctx);
  scene            = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                                     ui::defaultTheme());
  const auto *mode = findAction(*scene, "mode");
  ASSERT_NE(mode, nullptr);
  ASSERT_TRUE(overlay.performAction(gleditor::a11y::Ids::of(47, mode->id),
                                    gleditor::a11y::Action::Focus, {}));
  EXPECT_EQ(focus.focusedNode(), mode->id);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Return, {}}));
  EXPECT_EQ(overlay.builder().mode(), xanadu::Selector::Kind::Rank);
  overlay.drawFrame(ctx);
  EXPECT_EQ(overlay.builder().mode(), xanadu::Selector::Kind::Closure);
  scene          = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                                   ui::defaultTheme());
  const auto tab = std::ranges::find(scene->visuals, std::string{"Commit"},
                                     &ui::WidgetVisual::accessibleLabel);
  ASSERT_NE(tab, scene->visuals.end());
  ASSERT_TRUE(overlay.activateNode(tab->id));
  overlay.drawFrame(ctx);
  scene = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                          ui::defaultTheme());
  const auto *field = findAction(*scene, "label");
  ASSERT_NE(field, nullptr);
  const auto fieldId = field->id;
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 47);
  overlay.describe(builder);
  ASSERT_TRUE(overlay.performAction(
      builder.id(fieldId), gleditor::a11y::Action::SetValue, "日本語"));
  overlay.drawFrame(ctx);
  EXPECT_EQ(focus.focusedNode(), fieldId);
  EXPECT_TRUE(focus.modalActive());
  EXPECT_FALSE(focus.permitsCommand("document-edit"));
  ASSERT_TRUE(overlay.textArea());
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Left, {}}));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Backspace, {}}));
  overlay.drawFrame(ctx);
  focus.dispatchText("本");
  overlay.drawFrame(ctx);
  scene = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                          ui::defaultTheme());
  field = findAction(*scene, "label");
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->id, fieldId);
  EXPECT_EQ(field->value, "日本語");
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Home, {}}));
  overlay.setConfig({900, 560});
  overlay.drawFrame(ctx);
  EXPECT_TRUE(focus.dispatchText("前"));
  overlay.drawFrame(ctx);
  scene = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                          ui::defaultTheme());
  field = findAction(*scene, "label");
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->value, "前日本語");
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Escape, {}}));
  EXPECT_FALSE(overlay.isVisible());
  EXPECT_FALSE(focus.modalActive());
}
TEST(QuotationBuilderOverlayTest, RetainsWarmFramesAndRejectsOldCommitPicks) {
  QuotationFixture fixture;
  QuotationInkDevice device;
  RenderState state{&device};
  int commits = 0;
  xanadu::QuotationBuilderOverlay overlay(fixture.store, fixture.head, {},
                                          nullptr, {}, nullptr,
                                          [&](auto, auto) { ++commits; });
  overlay.setVisible(true)->setMode(xanadu::Selector::Kind::Closure);
  overlay.deviceReady(device, {});
  ch::Timeline timeline;
  glm::mat4 projection{1};
  gleditor::FrameContext ctx{state, projection, 1280, 800, timeline};
  overlay.drawFrame(ctx);
  auto scene     = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                                   ui::defaultTheme());
  const auto tab = std::ranges::find(scene->visuals, std::string{"Commit"},
                                     &ui::WidgetVisual::accessibleLabel);
  ASSERT_NE(tab, scene->visuals.end());
  ASSERT_TRUE(overlay.activateNode(tab->id));
  state.beginPickScene();
  overlay.drawFrame(ctx);
  scene = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                          ui::defaultTheme());
  const auto *button = findAction(*scene, "commit");
  ASSERT_NE(button, nullptr);
  ASSERT_FALSE(state.overlayPickScene.widgetOverlays.empty());
  render::PickingResult pick;
  pick.requestId = 1;
  pick.tag       = render::unpackPickingTag(
      state.overlayPickScene.widgetOverlays.back().identity, button->pickingId,
      0);
  pick.overlayWidgetId =
      render::resolveOverlayWidget(state.overlayPickScene, pick.tag);
  ASSERT_EQ(pick.overlayWidgetId, button->id);
  const auto uploads = device.uploads;
  const auto shaping = overlay.shapingStats();
  gleditor::text::ShapingStatsScope counters;
  for (int i = 0; i < 100; ++i) overlay.drawFrame(ctx);
  EXPECT_EQ(device.uploads, uploads);
  EXPECT_EQ(overlay.shapingStats().misses, shaping.misses);
  EXPECT_EQ(counters.stats().harfbuzzCalls, 0U);
  ASSERT_TRUE(overlay.picked(pick, state));
  EXPECT_EQ(commits, 0);
  const auto count = fixture.store.opCount();
  overlay.setVisible(false)->setVisible(true);
  overlay.drawFrame(ctx);
  EXPECT_TRUE(overlay.picked(pick, state));
  overlay.drawFrame(ctx);
  EXPECT_EQ(commits, 0);
  EXPECT_EQ(fixture.store.opCount(), count);
  scene  = overlay.prepare({.screenWidth = 1280, .screenHeight = 800},
                           ui::defaultTheme());
  button = findAction(*scene, "commit");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(overlay.activateNode(button->id));
  std::ignore =
      fixture.store.makeCell(fixture.store.latest(), "Concurrent new cell");
  overlay.drawFrame(ctx);
  EXPECT_EQ(commits, 0);
  EXPECT_TRUE(overlay.isVisible());
}
TEST(QuotationBuilderOverlayTest,
     VirtualListsReachOverflowAndCommitKeepsClosureMeaning) {
  QuotationFixture fixture;
  QuotationInkDevice device;
  RenderState state{&device};
  int commits = 0;
  xanadu::QuotationBuilderOverlay overlay(fixture.store, fixture.head, {},
                                          nullptr, {}, nullptr,
                                          [&](auto, auto) { ++commits; });
  overlay.setVisible(true)->setMode(xanadu::Selector::Kind::Closure);
  overlay.deviceReady(device, {});
  ui::FocusManager focus;
  overlay.syncFocus(focus);
  ch::Timeline timeline;
  glm::mat4 projection{1};
  gleditor::FrameContext ctx{state, projection, 640, 480, timeline};
  overlay.drawFrame(ctx);
  const auto metrics = ui::UiMetrics{.screenWidth = 640, .screenHeight = 480};
  const auto go      = [&](std::string page) {
    const auto scene = overlay.prepare(metrics, ui::defaultTheme());
    const auto tab   = std::ranges::find(scene->visuals, page,
                                              &ui::WidgetVisual::accessibleLabel);
    EXPECT_NE(tab, scene->visuals.end());
    if (tab != scene->visuals.end()) EXPECT_TRUE(overlay.activateNode(tab->id));
    overlay.drawFrame(ctx);
  };
  go("Selector");
  const auto first =
      overlay.prepare(metrics, ui::defaultTheme())->layout.focusOrder;
  EXPECT_TRUE(overlay.keyPressed(gleditor::Key::PageDown, {}));
  overlay.drawFrame(ctx);
  const auto second =
      overlay.prepare(metrics, ui::defaultTheme())->layout.focusOrder;
  EXPECT_NE(first, second);
  go("Preview");
  EXPECT_TRUE(overlay.keyPressed(gleditor::Key::PageDown, {}));
  overlay.drawFrame(ctx);
  const auto preview = overlay.prepare(metrics, ui::defaultTheme());
  EXPECT_TRUE(std::ranges::any_of(preview->visuals, [](const auto &v) {
    return v.action == "preview-cell" && v.itemIndex > 0;
  }));
  const auto *row = findAction(*preview, "preview-cell");
  ASSERT_NE(row, nullptr);
  const auto rowId = row->id;
  EXPECT_TRUE(overlay.performAction(gleditor::a11y::Ids::of(53, rowId),
                                    gleditor::a11y::Action::Click, {}));
  overlay.drawFrame(ctx);
  EXPECT_EQ(focus.focusedNode(), rowId);
  auto alternate = ui::defaultTheme();
  alternate.fonts[static_cast<std::size_t>(ui::FontRole::Label)].family =
      "Serif";
  std::ignore = overlay.prepare(metrics, alternate);
  EXPECT_EQ(focus.focusedNode(), rowId);
  overlay.drawFrame(ctx);
  EXPECT_EQ(focus.focusedNode(), rowId);
  const auto cells = overlay.builder().preview().cells.size();
  EXPECT_GT(cells, 30U);
  go("Commit");
  const auto scene   = overlay.prepare(metrics, ui::defaultTheme());
  const auto *button = findAction(*scene, "commit");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(overlay.activateNode(button->id));
  EXPECT_EQ(commits, 0);
  overlay.drawFrame(ctx);
  EXPECT_EQ(commits, 1);
  EXPECT_FALSE(overlay.isVisible());
  EXPECT_EQ(overlay.builder().preview().cells.size(), cells);
}
} // namespace
