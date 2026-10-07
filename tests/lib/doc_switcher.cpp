#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "mocks/device.hpp"
#include <gleditor/doc.hpp>
#include <gleditor/doc_switcher.hpp>
#include <gleditor/floating_toolbar_3d.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text_source.hpp>

using gleditor::DocumentSwitcher;
using testing::NiceMock;
using testing::Return;

class DocumentSwitcherTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<DocumentSwitcher> switcher;
  std::vector<Doc::VBORow> uploaded;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));
    ON_CALL(*device, createPipeline(testing::_))
        .WillByDefault(Return(render::PipelineHandle{1}));
    ON_CALL(*device, updateBuffer(testing::_, testing::_, testing::_))
        .WillByDefault([this](render::BufferHandle, std::size_t,
                              std::span<const std::byte> data) {
          for (std::size_t offset = 0;
               offset + sizeof(Doc::VBORow) <= data.size();
               offset += sizeof(Doc::VBORow)) {
            Doc::VBORow row{};
            std::memcpy(&row, data.data() + offset, sizeof(row));
            uploaded.push_back(row);
          }
        });

    state    = std::make_unique<RenderState>(device.get());
    switcher = std::make_unique<DocumentSwitcher>("Monospace 10");
  }

  void addDoc(const std::string &name) {
    state->docs.push_back(Doc::create({}, device.get(), glm::mat4(1),
                                      gleditor::MemoryTextSource({}, name)));
  }
  void draw(const gleditor::ui::UiMetrics &metrics,
            const gleditor::ui::Theme &theme = gleditor::ui::defaultTheme()) {
    state->beginPickScene();
    ch::Timeline timeline;
    const glm::mat4 projection(1);
    gleditor::FrameContext context{.state          = *state,
                                   .viewProjection = projection,
                                   .screenWidth    = metrics.screenWidth,
                                   .screenHeight   = metrics.screenHeight,
                                   .timeline       = timeline,
                                   .chrome         = metrics.chrome,
                                   .metrics        = metrics,
                                   .theme          = theme};
    switcher->drawFrame(context);
  }

  void TearDown() override {
    switcher.reset();
    state.reset();
    device.reset();
  }
};

TEST_F(DocumentSwitcherTest, defaultVisibilityAndSelection) {
  EXPECT_TRUE(switcher->isVisible());
  EXPECT_EQ(switcher->activeDocIndex(), 0U);

  switcher->setActiveDocIndex(2U);
  EXPECT_EQ(switcher->activeDocIndex(), 2U);

  switcher->setVisible(false);
  EXPECT_FALSE(switcher->isVisible());
}

TEST_F(DocumentSwitcherTest, pickTabSelectionTriggersHandler) {
  std::uint32_t selectedDoc = ~0U;
  switcher->setSelectHandler(
      [&selectedDoc](const std::uint32_t index) { selectedDoc = index; });

  // Mock picking tag for selecting docIndex 1: (1 << 1) | 0 = 2
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 2U; // docIndex 1, select

  // Needs at least 2 docs in state for valid index check
  state->docs.resize(2);

  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_EQ(selectedDoc, 1U);
  EXPECT_EQ(switcher->activeDocIndex(), 1U);
}

TEST_F(DocumentSwitcherTest, pickCloseButtonTriggersCloseHandler) {
  std::uint32_t closedDoc = ~0U;
  switcher->setCloseHandler(
      [&closedDoc](const std::uint32_t index) { closedDoc = index; });

  // Mock picking tag for closing docIndex 0: (0 << 1) | 1 = 1
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 1U; // docIndex 0, close

  state->docs.resize(1);

  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_EQ(closedDoc, 0U);
}

TEST_F(DocumentSwitcherTest, ignoresNonOverlayPicks) {
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindGlyph;
  pick.tag.clusterIndex = 2U;

  state->docs.resize(2);
  EXPECT_FALSE(switcher->picked(pick, *state));
}

TEST_F(DocumentSwitcherTest, pickNewDocButtonTriggersHandler) {
  bool newDocTriggered = false;
  switcher->setNewDocHandler([&newDocTriggered] { newDocTriggered = true; });

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = DocumentSwitcher::kNewDocTag;

  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_TRUE(newDocTriggered);
}

TEST_F(DocumentSwitcherTest,
       responsiveTabsKeepDrawnAndAccessibleBoundsTogether) {
  using namespace gleditor;
  std::ifstream fixtures("tests/samples/ui/long-labels.tsv");
  std::vector<std::string> labels;
  for (std::string line; std::getline(fixtures, line);) {
    if (line.empty() || line.front() == '#') continue;
    auto text = line.substr(line.find('\t') + 1);
    // Titles are filenames; keep the long path fixture as a displayed title.
    std::ranges::replace(text, '/', '_');
    labels.push_back(std::move(text));
    addDoc(labels.back());
  }
  ASSERT_EQ(labels.size(), 9U);
  switcher = std::make_unique<DocumentSwitcher>();
  switcher->deviceReady(*device, {});
  for (const auto size :
       {ui::Size{640, 480}, ui::Size{1280, 800}, ui::Size{2560, 1440}}) {
    for (const auto scale : {0.8F, 1.0F, 1.5F, 2.0F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(testing::Message() << size.width << 'x' << size.height
                                        << ' ' << scale << ' ' << family);
        ui::UiMetrics metrics{.fontScale    = scale,
                              .screenWidth  = static_cast<int>(size.width),
                              .screenHeight = static_cast<int>(size.height),
                              .chrome       = {.top = 18, .bottom = 20}};
        ui::Theme theme;
        theme.fonts[static_cast<std::size_t>(ui::FontRole::Caption)].family =
            family;
        uploaded.clear();
        draw(metrics, theme);
        const auto safe      = metrics.pixelSafeArea();
        const auto contained = [](ui::Rect child, ui::Rect parent) {
          EXPECT_GE(child.left, parent.left - 0.01F);
          EXPECT_GE(child.bottom, parent.bottom - 0.01F);
          EXPECT_LE(child.left + child.width,
                    parent.left + parent.width + 0.01F);
          EXPECT_LE(child.bottom + child.height,
                    parent.bottom + parent.height + 0.01F);
        };
        const auto &layout = switcher->layout();
        contained(layout.bounds, safe);
        for (const auto &box : layout.boxes) {
          contained(box.rect, layout.bounds);
          contained(box.contentRect, box.rect);
          if (box.parentId != 0) {
            const auto *parent = layout.find(box.parentId);
            ASSERT_NE(parent, nullptr);
            contained(box.rect, parent->rect);
          }
        }
        ASSERT_FALSE(uploaded.empty());
        for (const auto &row : uploaded) {
          const auto width  = static_cast<float>((row.quad >> 20U) & 4095U);
          const auto height = static_cast<float>((row.quad >> 8U) & 4095U);
          const ui::Rect rect{row.pos[0] - width * 0.5F,
                              row.pos[1] - height * 0.5F, width, height};
          const auto tag  = row.paper & 65535U;
          const auto id   = tag <= labels.size() * 2 ? tag
                            : tag == labels.size() * 2 + 1
                                ? DocumentSwitcher::kManagerTag + 1U
                                : DocumentSwitcher::kNewDocTag + 1U;
          const auto *box = layout.find(id);
          contained(rect, box ? box->rect : layout.bounds);
          if (box && (row.foreground & Doc::VBORow::solidFlag) == 0)
            contained(rect, box->contentRect);
        }
        a11y::Tree tree;
        a11y::Builder builder(tree, 12);
        switcher->describe(builder);
        for (std::size_t index = 0; index < labels.size(); ++index) {
          const auto id   = static_cast<std::uint32_t>(index * 2 + 1);
          const auto node = tree.find(builder.id(id));
          ASSERT_TRUE(node);
          EXPECT_EQ(node->label, labels[index]);
          ASSERT_TRUE(node->bounds);
          const auto *box = layout.find(id);
          ASSERT_NE(box, nullptr);
          EXPECT_FLOAT_EQ(static_cast<float>(node->bounds->left),
                          box->rect.left);
          EXPECT_FLOAT_EQ(static_cast<float>(node->bounds->top),
                          static_cast<float>(metrics.screenHeight) -
                              box->rect.bottom - box->rect.height);
          EXPECT_TRUE(tree.find(builder.id(id + 1)));
        }
        EXPECT_TRUE(tree.find(builder.id(DocumentSwitcher::kManagerTag + 1U)));
        EXPECT_TRUE(tree.find(builder.id(DocumentSwitcher::kNewDocTag + 1U)));
        const text::ShapingStatsScope capture;
        const auto targets =
            state->overlayPickScene.widgetOverlays.front().targets;
        uploaded.clear();
        draw(metrics, theme);
        EXPECT_TRUE(uploaded.empty());
        ASSERT_EQ(state->overlayPickScene.widgetOverlays.size(), 1U);
        EXPECT_EQ(state->overlayPickScene.widgetOverlays.front().targets,
                  targets);
        EXPECT_EQ(capture.stats().layoutCalls, 0U);
        EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
      }
    }
  }
}

TEST_F(DocumentSwitcherTest,
       accessibilityUsesTheSameSelectCloseAndUtilityActions) {
  using namespace gleditor;
  addDoc(
      "A complete long document title that remains available to accessibility");
  addDoc("Second document");
  switcher->deviceReady(*device, {});
  const ui::UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  draw(metrics);
  std::uint32_t selected = ~0U;
  std::uint32_t closed   = ~0U;
  bool created{}, managed{};
  switcher->setSelectHandler([&](std::uint32_t index) {
    selected = index;
    EXPECT_EQ(switcher->activeDocIndex(), index);
  });
  switcher->setCloseHandler([&](std::uint32_t index) { closed = index; });
  switcher->setNewDocHandler([&] { created = true; });
  switcher->setManagerHandler([&] { managed = true; });
  EXPECT_TRUE(
      switcher->performAction(a11y::Ids::of(12, 3), a11y::Action::Click, {}));
  EXPECT_EQ(selected, 1U);
  EXPECT_TRUE(
      switcher->performAction(a11y::Ids::of(12, 2), a11y::Action::Click, {}));
  EXPECT_EQ(closed, 0U);
  EXPECT_TRUE(switcher->performAction(
      a11y::Ids::of(12, DocumentSwitcher::kNewDocTag + 1U), a11y::Action::Click,
      {}));
  EXPECT_TRUE(created);
  EXPECT_TRUE(switcher->performAction(
      a11y::Ids::of(12, DocumentSwitcher::kManagerTag + 1U),
      a11y::Action::Click, {}));
  EXPECT_TRUE(managed);
  switcher->setVisible(false);
  EXPECT_FALSE(
      switcher->performAction(a11y::Ids::of(12, 1), a11y::Action::Click, {}));
}

TEST_F(DocumentSwitcherTest, documentChangesInvalidateRetainedTabsExactlyOnce) {
  using namespace gleditor;
  addDoc("First document");
  switcher->deviceReady(*device, {});
  const ui::UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  draw(metrics);
  const auto revision = switcher->accessibilityRevision();
  addDoc("A newly opened document");
  draw(metrics);
  EXPECT_GT(switcher->accessibilityRevision(), revision);
  const auto updated = switcher->accessibilityRevision();
  const text::ShapingStatsScope capture;
  draw(metrics);
  EXPECT_EQ(switcher->accessibilityRevision(), updated);
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
}

TEST_F(DocumentSwitcherTest,
       asynchronousPicksKeepTheirDocumentAndWidgetIdentity) {
  using namespace gleditor;
  addDoc("First document");
  addDoc("Second document");
  switcher->deviceReady(*device, {});
  draw({.screenWidth = 640, .screenHeight = 480});
  ASSERT_TRUE(state->overlayPickScene.overlays.empty());
  ASSERT_EQ(state->overlayPickScene.widgetOverlays.size(), 1U);
  const auto saved = state->overlayPickScene;
  ASSERT_EQ(saved.widgetOverlays.front().targets->size(), 6U);
  render::PickingResult pick{.requestId = 1};
  pick.tag =
      render::unpackPickingTag(saved.widgetOverlays.front().identity, 3U, 0);
  pick.overlayWidgetId = render::resolveOverlayWidget(saved, pick.tag);
  ASSERT_TRUE(pick.overlayWidgetId);
  std::uint32_t selected = ~0U, closed = ~0U;
  bool created{}, managed{};
  switcher->setSelectHandler([&](std::uint32_t index) { selected = index; });
  switcher->setCloseHandler([&](std::uint32_t index) { closed = index; });
  switcher->setNewDocHandler([&] { created = true; });
  switcher->setManagerHandler([&] { managed = true; });
  auto utility             = pick;
  utility.tag.clusterIndex = 5U;
  utility.overlayWidgetId  = render::resolveOverlayWidget(saved, utility.tag);
  EXPECT_TRUE(switcher->picked(utility, *state));
  EXPECT_TRUE(managed);
  utility.tag.clusterIndex = 6U;
  utility.overlayWidgetId  = render::resolveOverlayWidget(saved, utility.tag);
  EXPECT_TRUE(switcher->picked(utility, *state));
  EXPECT_TRUE(created);
  auto unrelated = pick;
  --unrelated.tag.docIndex;
  EXPECT_FALSE(switcher->picked(unrelated, *state));
  unrelated               = pick;
  unrelated.tag.pageIndex = 1;
  EXPECT_FALSE(switcher->picked(unrelated, *state));
  unrelated = pick;
  unrelated.overlayWidgetId.reset();
  EXPECT_FALSE(switcher->picked(unrelated, *state));

  state->docs.erase(state->docs.begin());
  state->beginPickScene();
  draw({.screenWidth = 640, .screenHeight = 480});
  EXPECT_EQ(state->overlayPickScene.widgetOverlays.front().targets->front(),
            *pick.overlayWidgetId);
  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_EQ(selected, 0U);
  pick.tag.clusterIndex = 4U;
  pick.overlayWidgetId  = render::resolveOverlayWidget(saved, pick.tag);
  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_EQ(closed, 0U);

  state->docs.clear();
  addDoc("Replacement document");
  state->beginPickScene();
  draw({.screenWidth = 640, .screenHeight = 480});
  EXPECT_NE(state->overlayPickScene.widgetOverlays.front().targets->front(),
            *pick.overlayWidgetId);
  closed = ~0U;
  EXPECT_FALSE(switcher->picked(pick, *state));
  EXPECT_EQ(closed, ~0U);
}

class FloatingToolbar3DTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<gleditor::FloatingToolbar3D> toolbar;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    state   = std::make_unique<RenderState>(device.get());
    toolbar = std::make_unique<gleditor::FloatingToolbar3D>("Monospace 10");
  }

  void TearDown() override {
    toolbar.reset();
    state.reset();
    device.reset();
  }
};

TEST_F(FloatingToolbar3DTest, DefaultStateAndVisibility) {
  EXPECT_TRUE(toolbar->isVisible());
  EXPECT_EQ(toolbar->activeDocIndex(), 0U);

  toolbar->setActiveDocIndex(3U);
  EXPECT_EQ(toolbar->activeDocIndex(), 3U);

  toolbar->setVisible(false);
  EXPECT_FALSE(toolbar->isVisible());
}

TEST_F(FloatingToolbar3DTest, FormattingAndHeadingState) {
  toolbar->setFormattingState(true, false, true, false);
  toolbar->setHeadingLevel(1);
  EXPECT_TRUE(toolbar->accessibilityRevision() > 1);
}

TEST_F(FloatingToolbar3DTest, PickingActionDispatchesHandler) {
  using ButtonId              = gleditor::FloatingToolbar3D::ButtonId;
  ButtonId dispatchedBtn      = ButtonId::NewDoc;
  std::uint32_t dispatchedDoc = ~0U;

  toolbar->setActionHandler([&dispatchedBtn, &dispatchedDoc](
                                const ButtonId btn, const std::uint32_t doc) {
    dispatchedBtn = btn;
    dispatchedDoc = doc;
  });

  toolbar->setActiveDocIndex(1U);

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = static_cast<std::uint32_t>(ButtonId::SaveDoc);

  const bool consumed = toolbar->picked(pick, *state);
  EXPECT_TRUE(consumed);
  EXPECT_EQ(dispatchedBtn, ButtonId::SaveDoc);
  EXPECT_EQ(dispatchedDoc, 1U);
}

TEST_F(FloatingToolbar3DTest, AccessibilityActionPerformsClick) {
  using ButtonId              = gleditor::FloatingToolbar3D::ButtonId;
  ButtonId dispatchedBtn      = ButtonId::NewDoc;
  std::uint32_t dispatchedDoc = ~0U;

  toolbar->setActionHandler([&dispatchedBtn, &dispatchedDoc](
                                const ButtonId btn, const std::uint32_t doc) {
    dispatchedBtn = btn;
    dispatchedDoc = doc;
  });

  toolbar->setActiveDocIndex(2U);

  const std::uint64_t nodeId =
      0x5000U + static_cast<std::uint64_t>(ButtonId::Bold);
  const bool handled =
      toolbar->performAction(nodeId, gleditor::a11y::Action::Click, "");
  EXPECT_TRUE(handled);
  EXPECT_EQ(dispatchedBtn, ButtonId::Bold);
  EXPECT_EQ(dispatchedDoc, 2U);
}
