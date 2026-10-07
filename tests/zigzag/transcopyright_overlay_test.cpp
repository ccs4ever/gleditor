#include "../lib/mocks/device.hpp"
#include "xudu/transcopyright_overlay.hpp"
#include <algorithm>
#include <cstring>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>

namespace {
class BadgeDevice : public testing::NiceMock<MockRenderDevice> {
public:
  std::uint32_t next{1};
  std::size_t uploads{}, draws{};
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::vector<Doc::VBORow> drawn;
  BadgeDevice() {
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
        .WillByDefault([this](const render::DrawUniforms &,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          if (count) ++draws;
          const auto &storage = buffers.at(buffer.id);
          for (std::uint32_t index = 0; index < count; ++index) {
            Doc::VBORow row{};
            std::memcpy(&row, storage.data() + offset + index * sizeof(row),
                        sizeof(row));
            drawn.push_back(row);
          }
        });
  }
};
struct TranscopyrightOverlayTest : testing::Test {
  BadgeDevice device;
  RenderState state{&device};
  ch::Timeline timeline;
  glm::mat4 projection{1};
  std::uint64_t generation{1};
  std::size_t sourceCalls{};
  std::vector<xanadu::HoleSpanInfo> holes;
  std::map<std::uint64_t, gleditor::ui::Rect> anchors;
  std::vector<std::pair<std::size_t, xanadu::PrimediaSpan>> unlocked;
  xudu::TranscopyrightOverlay overlay{
      [this](const RenderState &) {
        ++sourceCalls;
        return holes;
      },
      [this] { return generation; },
      [this](const auto &hole,
             const auto &) -> std::optional<gleditor::ui::Rect> {
        const auto at = anchors.find(hole.span.start);
        return at == anchors.end() ? std::nullopt : std::optional{at->second};
      }};
  void SetUp() override {
    overlay.deviceReady(device, {});
    overlay.setUnlockCallback([this](std::size_t store, const auto &span) {
      unlocked.emplace_back(store, span);
    });
  }
  void add(std::uint64_t start, std::string currency = "nano-XU",
           bool locked = true) {
    xanadu::HoleSpanInfo hole;
    hole.docIndex    = 0;
    hole.storeIndex  = static_cast<std::size_t>(start);
    hole.span        = {.scroll = 1, .start = start, .length = 30};
    hole.length      = 30;
    hole.charStart   = static_cast<std::uint32_t>(start);
    hole.charEnd     = hole.charStart + 30;
    hole.reason      = locked ? xanadu::HoleReason::TranscopyrightLock
                              : xanadu::HoleReason::Withheld;
    hole.reasonLabel = locked ? "PAYWALL" : "WITHHELD";
    if (locked)
      hole.transcopyright = xanadu::TranscopyrightDescriptor{
          .priceAtomicUnits = 250,
          .flatFee          = true,
          .currencySymbol   = std::move(currency)};
    holes.push_back(std::move(hole));
    anchors[start] = {50, 150 + static_cast<float>(start), 240, 0};
  }
  void draw(gleditor::ui::UiMetrics metrics  = {.screenWidth  = 800,
                                                .screenHeight = 600},
            const gleditor::ui::Theme &theme = gleditor::ui::defaultTheme()) {
    device.drawn.clear();
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
};
TEST_F(TranscopyrightOverlayTest,
       WarmFramesRetainLabelsBuffersAndDomainSnapshot) {
  add(10);
  add(20, "unit", false);
  draw();
  ASSERT_GT(device.draws, 0U);
  ASSERT_EQ(sourceCalls, 1U);
  const auto uploads  = device.uploads;
  const auto revision = overlay.accessibilityRevision();
  gleditor::text::ShapingStatsScope shaping;
  for (int index = 0; index < 100; ++index) draw();
  EXPECT_EQ(sourceCalls, 1U);
  EXPECT_EQ(device.uploads, uploads);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  EXPECT_EQ(overlay.accessibilityRevision(), revision);
  ++generation;
  draw();
  EXPECT_EQ(sourceCalls, 2U);
}
TEST_F(TranscopyrightOverlayTest, ScaledSafeBoxesPreserveFullAccessibleLabels) {
  const std::string currency = "عملة طويلة מאוד ";
  add(10, currency);
  anchors[10] = {790, 590, 2000, 0};
  for (const auto width : {120, 400, 800}) {
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F}) {
      gleditor::ui::UiMetrics metrics{.fontScale    = scale,
                                      .screenWidth  = width,
                                      .screenHeight = 240,
                                      .chrome = {.top = 25, .bottom = 20}};
      draw(metrics);
      const auto safe = metrics.pixelSafeArea();
      ASSERT_EQ(overlay.activeBadges().size(), 1U);
      const auto &badge = overlay.activeBadges().front();
      EXPECT_GE(badge.screenX, safe.left - .01F);
      EXPECT_GE(badge.screenY, safe.bottom - .01F);
      EXPECT_LE(badge.screenX + badge.width, safe.left + safe.width + .01F);
      EXPECT_LE(badge.screenY + badge.height, safe.bottom + safe.height + .01F);
      gleditor::a11y::Tree tree;
      gleditor::a11y::Builder into(tree, 16);
      overlay.describe(into);
      ASSERT_EQ(tree.nodes.size(), 1U);
      EXPECT_EQ(tree.nodes.front().label, holes.front().badgeText());
      EXPECT_EQ(tree.nodes.front().bounds->left, badge.screenX);
      EXPECT_EQ(tree.nodes.front().bounds->top,
                240 - badge.screenY - badge.height);
    }
  }
}
TEST_F(TranscopyrightOverlayTest,
       StandardScreenAndFontMatrixDrawsContainedInk) {
  add(10, "عملة طويلة מאוד ");
  for (const auto size :
       {std::pair{640, 480}, std::pair{1280, 800}, std::pair{2560, 1440}}) {
    for (const auto scale : {.8F, 1.F, 1.5F, 2.F}) {
      for (const auto *family :
           {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
        SCOPED_TRACE(std::to_string(size.first) + "x" +
                     std::to_string(size.second) +
                     " scale=" + std::to_string(scale) + " font=" + family);
        gleditor::ui::UiMetrics metrics{.fontScale    = scale,
                                        .screenWidth  = size.first,
                                        .screenHeight = size.second,
                                        .chrome = {.top = 30, .bottom = 20}};
        gleditor::ui::Theme theme;
        theme.fonts[static_cast<std::size_t>(gleditor::ui::FontRole::Caption)]
            .family = family;
        anchors[10] = {static_cast<float>(size.first - 10),
                       static_cast<float>(size.second - 10), 240, 0};
        draw(metrics, theme);
        ASSERT_EQ(overlay.activeBadges().size(), 1U);
        const auto &badge = overlay.activeBadges().front();
        const auto safe   = metrics.pixelSafeArea();
        EXPECT_GE(badge.screenX, safe.left - .01F);
        EXPECT_GE(badge.screenY, safe.bottom - .01F);
        EXPECT_LE(badge.screenX + badge.width, safe.left + safe.width + .01F);
        EXPECT_LE(badge.screenY + badge.height,
                  safe.bottom + safe.height + .01F);
        std::size_t glyphs{};
        for (const auto &row : device.drawn) {
          if ((row.foreground & Doc::VBORow::solidFlag) != 0) continue;
          ++glyphs;
          const float width  = (row.quad >> 20U) & 4095U;
          const float height = (row.quad >> 8U) & 4095U;
          const float left   = row.pos[0] - width * .5F;
          const float bottom = row.pos[1] - height * .5F;
          EXPECT_GE(left, badge.screenX - .01F);
          EXPECT_GE(bottom, badge.screenY - .01F);
          EXPECT_LE(left + width, badge.screenX + badge.width + .01F);
          EXPECT_LE(bottom + height, badge.screenY + badge.height + .01F);
        }
        EXPECT_GT(glyphs, 0U);
        gleditor::a11y::Tree tree;
        gleditor::a11y::Builder into(tree, 16);
        overlay.describe(into);
        ASSERT_EQ(tree.nodes.size(), 1U);
        const auto &node = tree.nodes.front();
        EXPECT_EQ(node.label, holes.front().badgeText());
        ASSERT_TRUE(node.bounds);
        EXPECT_DOUBLE_EQ(node.bounds->left, badge.screenX);
        EXPECT_DOUBLE_EQ(node.bounds->top,
                         size.second - badge.screenY - badge.height);
        EXPECT_DOUBLE_EQ(node.bounds->right, badge.screenX + badge.width);
        EXPECT_DOUBLE_EQ(node.bounds->bottom, size.second - badge.screenY);
      }
    }
  }
}
TEST_F(TranscopyrightOverlayTest,
       CapturedPicksKeepTheirMeaningAfterBadgeReorder) {
  add(10);
  add(20);
  draw();
  const auto captured = state.overlayPickScene.widgetOverlays.front();
  render::PickingResult pick;
  pick.requestId       = 1;
  pick.tag             = {.kind     = render::tagKindOverlay,
                          .docIndex = (captured.identity >> render::tagPageBits) &
                                      ((1U << render::tagDocBits) - 1U),
                          .pageIndex    = 0,
                          .clusterIndex = 1};
  pick.overlayWidgetId = captured.targets->front();
  std::ranges::reverse(holes);
  ++generation;
  draw();
  EXPECT_TRUE(overlay.picked(pick, state));
  ASSERT_EQ(unlocked.size(), 1U);
  EXPECT_EQ(unlocked.front().first, 10U);
  EXPECT_EQ(unlocked.front().second.start, 10U);
  pick.tag.docIndex = 1;
  EXPECT_FALSE(overlay.picked(pick, state));
  pick.tag.docIndex = (captured.identity >> render::tagPageBits) &
                      ((1U << render::tagDocBits) - 1U);
  pick.overlayWidgetId.reset();
  EXPECT_FALSE(overlay.picked(pick, state));
}
TEST_F(TranscopyrightOverlayTest,
       MovingBadgesReuseFittedLabelsAndEmptySafeAreaHidesThem) {
  add(10);
  draw();
  const auto id = overlay.activeBadges().front().tagId;
  anchors[10].left += 40;
  {
    gleditor::text::ShapingStatsScope shaping;
    draw();
    EXPECT_EQ(shaping.stats().layoutCalls, 0U);
    EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  }
  EXPECT_EQ(sourceCalls, 1U);
  EXPECT_EQ(overlay.activeBadges().front().tagId, id);
  draw({.screenWidth = 800, .screenHeight = 600, .chrome = {.top = 600}});
  EXPECT_TRUE(overlay.activeBadges().empty());
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder into(tree, 16);
  overlay.describe(into);
  EXPECT_TRUE(tree.empty());
}
TEST_F(TranscopyrightOverlayTest, RemovedBadgesCannotRetargetCapturedPicks) {
  add(10);
  draw();
  const auto captured = state.overlayPickScene.widgetOverlays.front();
  const auto oldId    = captured.targets->front();
  holes.clear();
  add(20);
  ++generation;
  draw();
  render::PickingResult pick;
  pick.requestId    = 2;
  pick.tag.kind     = render::tagKindOverlay;
  pick.tag.docIndex = (captured.identity >> render::tagPageBits) &
                      ((1U << render::tagDocBits) - 1U);
  pick.tag.clusterIndex = 1;
  pick.overlayWidgetId  = oldId;
  EXPECT_FALSE(overlay.picked(pick, state));
  EXPECT_TRUE(unlocked.empty());
  EXPECT_NE(overlay.activeBadges().front().tagId, oldId);
  pick                  = {};
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = oldId;
  EXPECT_FALSE(overlay.picked(pick, state));
}
TEST_F(TranscopyrightOverlayTest, QueuedAccessibilityCannotUnlockRemovedBadge) {
  add(10);
  draw();
  const auto id = overlay.activeBadges().front().tagId;
  EXPECT_TRUE(overlay.performAction(gleditor::a11y::Ids::of(16, id),
                                    gleditor::a11y::Action::Click, {}));
  holes.clear();
  add(20);
  ++generation;
  draw();
  EXPECT_TRUE(unlocked.empty());
  EXPECT_FALSE(overlay.busy());
}
TEST_F(TranscopyrightOverlayTest, PointAnchorsAllocateIntrinsicLabelWidth) {
  add(10);
  anchors[10].width = 0;
  draw();
  ASSERT_EQ(overlay.activeBadges().size(), 1U);
  const auto &badge = overlay.activeBadges().front();
  EXPECT_GT(badge.width, 200.F);
  EXPECT_LT(badge.width, 800.F);
}
TEST_F(TranscopyrightOverlayTest,
       AccessibilityDispatchesOnTheRenderThreadAndBloomDoesNotReshape) {
  add(10);
  draw();
  const auto badge = overlay.activeBadges().front();
  EXPECT_TRUE(overlay.performAction(gleditor::a11y::Ids::of(16, badge.tagId),
                                    gleditor::a11y::Action::Click, {}));
  EXPECT_TRUE(unlocked.empty());
  EXPECT_TRUE(overlay.busy());
  draw();
  ASSERT_EQ(unlocked.size(), 1U);
  overlay.notifyUnlocked(0, badge.span, 250);
  holes.clear();
  ++generation;
  draw();
  const auto sources = sourceCalls;
  gleditor::text::ShapingStatsScope shaping;
  for (int index = 0; index < 25; ++index) draw();
  EXPECT_EQ(sourceCalls, sources);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  EXPECT_FALSE(overlay.busy());
  EXPECT_TRUE(overlay.activeBadges().empty());
}
} // namespace
