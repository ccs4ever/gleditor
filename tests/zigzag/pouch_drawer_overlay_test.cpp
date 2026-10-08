#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../lib/mocks/device.hpp"
#include "common/ui/xanadoc/pouch_drawer.hpp"
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <gleditor/caret.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <unistd.h>
#include <unordered_set>

namespace {
using namespace gleditor::ui;

struct PrivatePouchConfiguration {
  std::optional<std::string> previous;
  std::filesystem::path root;
  PrivatePouchConfiguration() {
    if (const auto *value = std::getenv("XDG_CONFIG_HOME")) previous = value;
    static std::atomic<unsigned> serial{};
    root = std::filesystem::temp_directory_path() /
           ("gleditor-pouch-test-" + std::to_string(getpid()) + "-" +
            std::to_string(serial.fetch_add(1)));
    std::filesystem::create_directories(root);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
  }
  ~PrivatePouchConfiguration() {
    if (previous)
      setenv("XDG_CONFIG_HOME", previous->c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    std::filesystem::remove_all(root);
  }
};

struct PouchFixture {
  PrivatePouchConfiguration config;
  testing::NiceMock<MockRenderDevice> device;
  RenderState state{&device};
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();
  xanadu::Session session{"", scroll};
  xanadu::PouchDrawer drawer{session, {}};
  ch::Timeline timeline;
  glm::mat4 projection{1};
  PouchFixture() {
    ON_CALL(device, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(device, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(device, createBuffer)
        .WillByDefault(testing::Return(render::BufferHandle{1}));
    ON_CALL(device, createPipeline)
        .WillByDefault(testing::Return(render::PipelineHandle{1}));
    drawer.setOpen(true, false);
  }
  void frame(const UiMetrics &metrics, const Theme &theme = Theme{}) {
    gleditor::FrameContext ctx{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = metrics.screenWidth,
                               .screenHeight   = metrics.screenHeight,
                               .timeline       = timeline,
                               .chrome         = metrics.chrome,
                               .metrics        = metrics,
                               .theme          = theme};
    drawer.drawFrame(ctx);
  }
  gleditor::a11y::Tree tree() {
    gleditor::a11y::Tree result;
    gleditor::a11y::Builder builder(result, 19);
    drawer.describe(builder);
    return result;
  }
};

TEST(PouchDrawerOverlayTest, scaledPartitionsAndAccessibilityUseVisibleBounds) {
  PouchFixture fixture;
  std::uint64_t itemIdentity = 0x1'0000'0012ULL;
  for (const auto &zone : fixture.drawer.zones())
    zone->addItem({.itemId      = itemIdentity++,
                   .previewText = "A full Unicode preview: 日本語 "
                                  "👩‍👩‍👧‍👦 café"});
  for (const auto size : {Size{640, 480}, Size{1280, 800}, Size{2560, 1440}})
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F})
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        UiMetrics metrics{.fontScale    = scale,
                          .screenWidth  = static_cast<int>(size.width),
                          .screenHeight = static_cast<int>(size.height),
                          .chrome       = {.top = 30, .bottom = 20}};
        Theme theme;
        for (auto &font : theme.fonts) font.family = family;
        fixture.frame(metrics, theme);
        const auto layout = fixture.drawer.focusLayout();
        ASSERT_TRUE(layout);
        EXPECT_FALSE(layout->focusOrder.empty());
        std::unordered_set<std::uint32_t> identities;
        for (const auto &box : layout->boxes)
          EXPECT_TRUE(identities.insert(box.id).second);
        const auto safe = metrics.pixelSafeArea();
        const auto tree = fixture.tree();
        bool preview    = false;
        for (const auto &node : tree.nodes) {
          if (node.label.find("A full Unicode") != std::string::npos) {
            preview = true;
            EXPECT_TRUE(node.bounds);
            EXPECT_TRUE(node.actions &
                        gleditor::a11y::bit(gleditor::a11y::Action::Click));
          }
          if (!node.bounds) continue;
          EXPECT_GE(node.bounds->left, safe.left - .01);
          EXPECT_GE(node.bounds->top,
                    metrics.screenHeight - safe.bottom - safe.height - .01);
          EXPECT_LE(node.bounds->right, safe.left + safe.width + .01);
          EXPECT_LE(node.bounds->bottom,
                    metrics.screenHeight - safe.bottom + .01);
        }
        EXPECT_TRUE(preview) << size.width << " " << scale << " " << family;
        for (const auto &zone : fixture.drawer.zones()) {
          if (zone->height() <= 0) continue;
          EXPECT_GE(zone->x(), safe.left);
          EXPECT_GE(zone->y(), safe.bottom - .01);
          EXPECT_LE(zone->x() + zone->width(), safe.left + safe.width + .01);
          EXPECT_LE(zone->y() + zone->height(),
                    safe.bottom + safe.height + .01);
        }
      }
}

TEST(PouchDrawerOverlayTest, repeatedFramesRetainLayoutAndShaping) {
  PouchFixture fixture;
  fixture.drawer.zones().front()->addItem(
      {.itemId = 42, .previewText = "Retained preview"});
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  fixture.frame(metrics);
  const auto first = fixture.drawer.focusLayout();
  gleditor::text::ShapingStatsScope stats;
  for (int frame = 0; frame < 100; ++frame) fixture.frame(metrics);
  EXPECT_EQ(fixture.drawer.focusLayout(), first);
  EXPECT_EQ(stats.stats().harfbuzzCalls, 0U);
}

TEST(PouchDrawerOverlayTest, fullItemIdsDispatchAndRetiredActionsDoNotMutate) {
  PouchFixture fixture;
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  auto &zone                 = *fixture.drawer.zones().front();
  const std::uint64_t itemId = 0x1'0000'0012ULL;
  zone.addItem({.itemId = itemId, .previewText = "Full-width identifier"});
  int origins = 0;
  fixture.drawer.setSwingBackHandler([&](const auto &item) {
    EXPECT_EQ(item.itemId, itemId);
    ++origins;
  });
  fixture.frame(metrics);
  auto tree = fixture.tree();
  auto node =
      std::ranges::find(tree.nodes, std::string("Full-width identifier"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(node, tree.nodes.end());
  const auto oldId = node->id;
  EXPECT_TRUE(
      fixture.drawer.performAction(oldId, gleditor::a11y::Action::Click, {}));
  EXPECT_EQ(origins, 0);
  fixture.frame(metrics);
  EXPECT_EQ(origins, 1);
  EXPECT_TRUE(
      fixture.drawer.performAction(oldId, gleditor::a11y::Action::Click, {}));
  fixture.drawer.setOpen(false, false);
  fixture.drawer.setOpen(true, false);
  fixture.frame(metrics);
  EXPECT_EQ(origins, 1);
  EXPECT_FALSE(
      fixture.drawer.performAction(oldId, gleditor::a11y::Action::Click, {}));
  fixture.drawer.setOpen(false, false);
  EXPECT_TRUE(fixture.tree().nodes.empty());
  EXPECT_FALSE(fixture.drawer.active());
}

TEST(PouchDrawerOverlayTest, dropUsesVisiblePartitionAndClosedDrawerRefuses) {
  PouchFixture fixture;
  fixture.session.views().push_back({});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  fixture.frame(metrics);
  const auto &zone = *fixture.drawer.zones().front();
  const auto x     = zone.x() + zone.width() / 2;
  const auto y     = zone.y() + zone.height() / 2;
  EXPECT_TRUE(fixture.drawer.handleGhostDrop(
      {.scroll = 0, .start = 0, .length = 0}, "Dropped", {}, x, y));
  EXPECT_EQ(zone.items().size(), 1U);
  fixture.drawer.setOpen(false, false);
  EXPECT_FALSE(fixture.drawer.handleGhostDrop({}, "Refused", {}, x, y));
}
TEST(PouchDrawerOverlayTest, horizontalResizeBothDocksPersistsOnlyOnRelease) {
  for (const auto side : {xanadu::PouchDrawer::DockSide::Left,
                          xanadu::PouchDrawer::DockSide::Right}) {
    PouchFixture fixture;
    fixture.drawer.setDockSide(side);
    FocusManager focus;
    auto registration = focus.registerScope(fixture.drawer);
    const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
    fixture.frame(metrics);
    auto &settings    = fixture.session.systemStore(xanadu::SystemDocKind::UI);
    const auto before = settings.opCount();
    const auto tree   = fixture.tree();
    const auto handle =
        std::ranges::find(tree.nodes, std::string("Resize pouch drawer"),
                          &gleditor::a11y::Node::label);
    ASSERT_NE(handle, tree.nodes.end());
    ASSERT_TRUE(handle->bounds);
    const float x =
        static_cast<float>((handle->bounds->left + handle->bounds->right) / 2);
    const float y =
        static_cast<float>((handle->bounds->top + handle->bounds->bottom) / 2);
    EXPECT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Press,
                                       .button    = 1,
                                       .x         = x,
                                       .y         = y,
                                       .pointerId = 7}));
    const float moved =
        x + (side == xanadu::PouchDrawer::DockSide::Left ? 100 : -100);
    EXPECT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Move,
                                       .button    = 1,
                                       .x         = moved,
                                       .y         = y,
                                       .pointerId = 7}));
    fixture.frame(metrics);
    EXPECT_FLOAT_EQ(fixture.drawer.currentWidth(), 420);
    EXPECT_EQ(settings.opCount(), before);
    EXPECT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Release,
                                       .button    = 1,
                                       .x         = moved,
                                       .y         = y,
                                       .pointerId = 7}));
    EXPECT_EQ(settings.opCount(), before);
    fixture.frame(metrics);
    EXPECT_GT(settings.opCount(), before);
    const auto config = xanadu::UIConfig::fromStore(settings);
    EXPECT_FLOAT_EQ(config.pouchPanel.widthPx, 420);
    const auto once = settings.opCount();
    fixture.frame(metrics);
    EXPECT_EQ(settings.opCount(), once);
  }
}

TEST(PouchDrawerOverlayTest, completedResizePersistsWhenClosedBeforeNextFrame) {
  PouchFixture fixture;
  const auto document    = fixture.session.createNewStore();
  const auto documentOps = fixture.session.store(document).opCount();
  FocusManager focus;
  auto registration = focus.registerScope(fixture.drawer);
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  fixture.frame(metrics);
  auto &settings    = fixture.session.systemStore(xanadu::SystemDocKind::UI);
  const auto before = settings.opCount();
  const auto tree   = fixture.tree();
  const auto handle =
      std::ranges::find(tree.nodes, std::string("Resize pouch drawer"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(handle, tree.nodes.end());
  ASSERT_TRUE(handle->bounds);
  const float x =
      static_cast<float>((handle->bounds->left + handle->bounds->right) / 2);
  const float y =
      static_cast<float>((handle->bounds->top + handle->bounds->bottom) / 2);
  for (const auto phase :
       {PointerPhase::Press, PointerPhase::Move, PointerPhase::Release})
    ASSERT_TRUE(
        focus.dispatchPointer({.phase  = phase,
                               .button = 1,
                               .x = phase == PointerPhase::Press ? x : x + 100,
                               .y = y,
                               .pointerId = 7}));
  fixture.drawer.setOpen(false, false);
  EXPECT_EQ(settings.opCount(), before);
  fixture.frame(metrics);
  EXPECT_FLOAT_EQ(xanadu::UIConfig::fromStore(settings).pouchPanel.widthPx,
                  420);
  EXPECT_EQ(fixture.session.store(document).opCount(), documentOps);
  const auto once = settings.opCount();
  fixture.frame(metrics);
  EXPECT_EQ(settings.opCount(), once);
}

TEST(PouchDrawerOverlayTest, cancelledAndStationaryResizeDoNotWriteSettings) {
  PouchFixture fixture;
  FocusManager focus;
  auto registration = focus.registerScope(fixture.drawer);
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  fixture.frame(metrics);
  auto &settings    = fixture.session.systemStore(xanadu::SystemDocKind::UI);
  const auto before = settings.opCount();
  auto tree         = fixture.tree();
  const auto handle =
      std::ranges::find(tree.nodes, std::string("Resize pouch drawer"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(handle, tree.nodes.end());
  ASSERT_TRUE(handle->bounds);
  const float x =
      static_cast<float>((handle->bounds->left + handle->bounds->right) / 2);
  const float y =
      static_cast<float>((handle->bounds->top + handle->bounds->bottom) / 2);
  ASSERT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Press,
                                     .button    = 1,
                                     .x         = x,
                                     .y         = y,
                                     .pointerId = 4}));
  EXPECT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Move,
                                     .button    = 1,
                                     .x         = x + 100,
                                     .y         = y,
                                     .pointerId = 4}));
  fixture.frame(metrics);
  focus.focusLost();
  fixture.frame(metrics);
  EXPECT_FLOAT_EQ(fixture.drawer.currentWidth(), 320);
  EXPECT_EQ(settings.opCount(), before);
  ASSERT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Press,
                                     .button    = 1,
                                     .x         = x,
                                     .y         = y,
                                     .pointerId = 4}));
  ASSERT_TRUE(focus.dispatchPointer({.phase     = PointerPhase::Release,
                                     .button    = 1,
                                     .x         = x,
                                     .y         = y,
                                     .pointerId = 4}));
  fixture.frame(metrics);
  EXPECT_EQ(settings.opCount(), before);
}

TEST(PouchDrawerOverlayTest, accessibleResizeClampsToSafeArea) {
  PouchFixture fixture;
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  fixture.frame(metrics);
  auto tree = fixture.tree();
  const auto handle =
      std::ranges::find(tree.nodes, std::string("Resize pouch drawer"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(handle, tree.nodes.end());
  EXPECT_TRUE(fixture.drawer.performAction(
      handle->id, gleditor::a11y::Action::SetValue, "100000"));
  fixture.frame(metrics);
  EXPECT_LE(fixture.drawer.currentWidth(),
            metrics.pixelSafeArea().width * .9F + .01F);
  EXPECT_FALSE(fixture.drawer.performAction(
      handle->id, gleditor::a11y::Action::SetValue, "nan"));
}
TEST(PouchDrawerOverlayTest, wheelUpdatesAccessibleRowsAndOwnerRevision) {
  PouchFixture fixture;
  for (std::uint64_t id = 1; id < 20; ++id)
    fixture.drawer.zones().front()->addItem(
        {.itemId = id, .previewText = "Preview " + std::to_string(id)});
  const UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  fixture.frame(metrics);
  auto tree      = fixture.tree();
  const auto row = std::ranges::find(tree.nodes, std::string("Preview 1"),
                                     &gleditor::a11y::Node::label);
  ASSERT_NE(row, tree.nodes.end());
  ASSERT_TRUE(row->bounds);
  const float x =
      static_cast<float>((row->bounds->left + row->bounds->right) / 2);
  const float y =
      static_cast<float>((row->bounds->top + row->bounds->bottom) / 2);
  const auto before = fixture.drawer.accessibilityRevision();
  EXPECT_TRUE(fixture.drawer.pointerEvent(
      {.phase = PointerPhase::Wheel, .x = x, .y = y, .deltaY = -3}));
  EXPECT_NE(fixture.drawer.accessibilityRevision(), before);
  const auto pending = fixture.drawer.accessibilityRevision();
  fixture.frame(metrics);
  EXPECT_NE(fixture.drawer.accessibilityRevision(), pending);
  tree = fixture.tree();
  EXPECT_EQ(std::ranges::find(tree.nodes, std::string("Preview 1"),
                              &gleditor::a11y::Node::label),
            tree.nodes.end());
  EXPECT_NE(std::ranges::find(tree.nodes, std::string("Preview 2"),
                              &gleditor::a11y::Node::label),
            tree.nodes.end());
}
TEST(PouchDrawerOverlayTest, registeredModalForwardsSpaceToCardsAndSelectors) {
  PouchFixture fixture;
  fixture.drawer.zones().front()->addItem(
      {.itemId = 42, .previewText = "Space activates origin"});
  int origins = 0;
  fixture.drawer.setSwingBackHandler([&](const auto &) { ++origins; });
  const UiMetrics metrics{.screenWidth = 1280, .screenHeight = 800};
  fixture.frame(metrics);
  FocusManager focus;
  auto registration = focus.registerScope(fixture.drawer);
  auto tree         = fixture.tree();
  auto row =
      std::ranges::find(tree.nodes, std::string("Space activates origin"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(row, tree.nodes.end());
  ASSERT_TRUE(focus.focusNode(
      static_cast<std::uint32_t>(gleditor::a11y::Ids::localOf(row->id))));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Space}));
  EXPECT_EQ(origins, 0);
  fixture.frame(metrics);
  EXPECT_EQ(origins, 1);
  tree          = fixture.tree();
  auto selector = std::ranges::find(
      tree.nodes, std::string(xanadu::linkTypeName(xanadu::LinkType::Comment)),
      &gleditor::a11y::Node::label);
  ASSERT_NE(selector, tree.nodes.end());
  ASSERT_TRUE(focus.focusNode(
      static_cast<std::uint32_t>(gleditor::a11y::Ids::localOf(selector->id))));
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Space}));
  fixture.frame(metrics);
  EXPECT_EQ(fixture.drawer.forge().linkType(), xanadu::LinkType::Illustration);
  EXPECT_TRUE(focus.dispatchKey({gleditor::Key::Space}));
  fixture.frame(metrics);
  EXPECT_EQ(fixture.drawer.forge().linkType(), xanadu::LinkType::Disagreement);
  const auto before = fixture.drawer.manager().store().opCount();
  EXPECT_TRUE(focus.dispatchText("Must not reach document"));
  EXPECT_EQ(fixture.drawer.manager().store().opCount(), before);
}
TEST(PouchDrawerOverlayTest, narrowResizeKeepsControlColumnsReadable) {
  PouchFixture fixture;
  fixture.drawer.zones().front()->addItem(
      {.itemId = 42, .previewText = "Readable after resize"});
  const UiMetrics metrics{
      .fontScale = 2, .screenWidth = 1280, .screenHeight = 800};
  Theme theme;
  for (auto &font : theme.fonts) font.family = "Monospace";
  fixture.frame(metrics, theme);
  auto tree = fixture.tree();
  auto handle =
      std::ranges::find(tree.nodes, std::string("Resize pouch drawer"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(handle, tree.nodes.end());
  ASSERT_TRUE(fixture.drawer.performAction(
      handle->id, gleditor::a11y::Action::SetValue, "1"));
  fixture.frame(metrics, theme);
  EXPECT_GE(fixture.drawer.currentWidth(),
            4 * metrics.px(theme.type.minTouchPx));
  const auto layout = fixture.drawer.focusLayout();
  ASSERT_TRUE(layout);
  tree = fixture.tree();
  for (const auto &node : tree.nodes) {
    if (!(node.actions & gleditor::a11y::bit(gleditor::a11y::Action::Click)))
      continue;
    const auto *box = layout->find(
        static_cast<std::uint32_t>(gleditor::a11y::Ids::localOf(node.id)));
    ASSERT_NE(box, nullptr);
    EXPECT_GT(box->rect.width, 0);
    EXPECT_GT(box->rect.height, 0);
  }
}

TEST(PouchDrawerOverlayTest, adaptiveWidthResizeStartsAtRenderedEdge) {
  PouchFixture fixture;
  const UiMetrics metrics{.fontScale    = 2,
                          .screenWidth  = 640,
                          .screenHeight = 480,
                          .chrome       = {.top = 30, .bottom = 20}};
  Theme theme;
  for (auto &font : theme.fonts) font.family = "Monospace";
  fixture.frame(metrics, theme);
  const auto initial = fixture.drawer.currentWidth();
  EXPECT_GE(initial, 320);
  auto tree = fixture.tree();
  const auto handle =
      std::ranges::find(tree.nodes, std::string("Resize pouch drawer"),
                        &gleditor::a11y::Node::label);
  ASSERT_NE(handle, tree.nodes.end());
  ASSERT_TRUE(handle->bounds);
  const float x =
      static_cast<float>((handle->bounds->left + handle->bounds->right) / 2);
  const float y =
      static_cast<float>((handle->bounds->top + handle->bounds->bottom) / 2);
  ASSERT_TRUE(fixture.drawer.pointerEvent({.phase     = PointerPhase::Press,
                                           .button    = 1,
                                           .x         = x,
                                           .y         = y,
                                           .pointerId = 5}));
  ASSERT_TRUE(fixture.drawer.pointerEvent({.phase     = PointerPhase::Move,
                                           .button    = 1,
                                           .x         = x + 10,
                                           .y         = y,
                                           .pointerId = 5}));
  fixture.frame(metrics, theme);
  EXPECT_FLOAT_EQ(fixture.drawer.currentWidth(), initial + 10);
  EXPECT_TRUE(fixture.drawer.pointerEvent(
      {.phase = PointerPhase::Cancel, .pointerId = 5}));
  fixture.frame(metrics, theme);
  EXPECT_FLOAT_EQ(fixture.drawer.currentWidth(), initial);
}
TEST(PouchDrawerOverlayTest, drawnForgeAuthorsIntoTheActiveCommentary) {
  PouchFixture fixture;
  auto &session            = fixture.session;
  const auto sourceVersion = session.store().insert({}, 0, "Alice");
  session.views().push_back({.version = sourceVersion,
                             .pieces = session.store().rebuild(sourceVersion)});
  auto commentary        = std::make_unique<xanadu::Store>(fixture.scroll);
  const auto version     = commentary->insert({}, 0, "Bob");
  const auto destination = session.addStore(std::move(commentary), "");
  session.views().push_back(
      {.version    = version,
       .storeIndex = destination,
       .pieces     = session.store(destination).rebuild(version)});
  auto &pouch     = fixture.drawer.manager().store();
  const auto left = xanadu::carrySpan(
      session.store(), pouch,
      session.store().rebuild(sourceVersion).spansFor(0, 5).front());
  const auto right = xanadu::carrySpan(
      session.store(destination), pouch,
      session.store(destination).rebuild(version).spansFor(0, 3).front());
  ASSERT_TRUE(left);
  ASSERT_TRUE(right);
  fixture.drawer.forge().dropLeft({.span = *left, .previewText = "Alice"});
  fixture.drawer.forge().dropRight({.span = *right, .previewText = "Bob"});
  Caret caret(&fixture.device);
  fixture.state.caret = &caret;
  caret.placeAt(1, 3);
  const auto before = session.store().opCount();
  const UiMetrics metrics{.screenWidth = 1024, .screenHeight = 768};
  fixture.frame(metrics);
  const auto tree   = fixture.tree();
  const auto button = std::ranges::find(tree.nodes, "Forge Clasp",
                                        &gleditor::a11y::Node::label);
  ASSERT_NE(button, tree.nodes.end());
  ASSERT_TRUE(fixture.drawer.performAction(button->id,
                                           gleditor::a11y::Action::Click, {}));
  fixture.frame(metrics);
  EXPECT_EQ(session.store().opCount(), before);
  EXPECT_TRUE(session.store().linkView().empty());
  ASSERT_EQ(session.store(destination).linkView().size(), 1U);
  const auto &link = *session.store(destination).linkView().begin();
  EXPECT_EQ(link.left, (std::vector<xanadu::PrimediaSpan>{*left}));
  EXPECT_EQ(link.right, (std::vector<xanadu::PrimediaSpan>{*right}));
}

} // namespace
