/**
 * @file media_widget_test.cpp
 * @brief Unit tests for the document-embedded interactive media player UI
 *        widget.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>

using gleditor::MediaPlayer;
using gleditor::MediaResource;
using gleditor::MediaWidget;
using gleditor::MemoryMediaStream;
using gleditor::PlaybackState;
using testing::NiceMock;
using testing::Return;

class MediaWidgetTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::shared_ptr<MediaPlayer> player;
  std::unique_ptr<MediaWidget> widget;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    ON_CALL(*device, createPipeline)
        .WillByDefault(Return(render::PipelineHandle{1}));

    state  = std::make_unique<RenderState>(device.get());
    player = std::make_shared<MediaPlayer>(true); // Dummy audio mode

    auto memStream =
        std::make_shared<MemoryMediaStream>("AUDIO_DATA_FOR_WIDGET");
    auto res = MediaResource::fromStream(memStream, "WidgetTrack");
    EXPECT_TRUE(player->load(res));

    widget = std::make_unique<MediaWidget>("Monospace 10", player);
  }

  void TearDown() override {
    widget.reset();
    player.reset();
    state.reset();
    device.reset();
  }
};

TEST_F(MediaWidgetTest, DefaultPropertiesAndVisibility) {
  EXPECT_TRUE(widget->isVisible());
  EXPECT_EQ(widget->player(), player);
  EXPECT_GT(widget->width(), 100.0F);
  EXPECT_GT(widget->height(), 50.0F);

  widget->setVisible(false);
  EXPECT_FALSE(widget->isVisible());

  widget->setTitle("Custom Title");
  EXPECT_EQ(widget->title(), "Custom Title");

  widget->setSize(400.0F, 180.0F);
  EXPECT_FLOAT_EQ(widget->width(), 400.0F);
  EXPECT_FLOAT_EQ(widget->height(), 180.0F);
}

TEST_F(MediaWidgetTest, DocumentAttachmentAndPositioning) {
  EXPECT_FALSE(widget->isDocumentAttached());

  widget->setScreenPosition(100.0F, 200.0F);
  widget->setWorldPosition(glm::vec3{10.0F, 20.0F, 5.0F});
  EXPECT_FLOAT_EQ(widget->worldPosition().x, 10.0F);
  EXPECT_FLOAT_EQ(widget->worldPosition().y, 20.0F);
  EXPECT_FLOAT_EQ(widget->worldPosition().z, 5.0F);

  // Test attachToDocument and attachToPage
  widget->attachToDocument(nullptr, 120);
  EXPECT_FALSE(widget->isDocumentAttached());

  widget->attachToPage(nullptr, 0, 50.0F, 100.0F);
  EXPECT_FALSE(widget->isDocumentAttached());

  widget->detachFromDocument();
  EXPECT_FALSE(widget->isDocumentAttached());
}

TEST_F(MediaWidgetTest, PickPlayPauseStopButtons) {
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = widget->tagBase() + MediaWidget::tagPlay;

  // 1. Pick Play Button
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Playing);

  // 2. Pick Pause Button
  pick.tag.clusterIndex = widget->tagBase() + MediaWidget::tagPause;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Paused);

  // 3. Pick Stop Button
  pick.tag.clusterIndex = widget->tagBase() + MediaWidget::tagStop;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Stopped);

  // 4. Pick Volume Button
  EXPECT_FALSE(player->isMuted());
  pick.tag.clusterIndex = widget->tagBase() + MediaWidget::tagVolume;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_TRUE(player->isMuted());

  // Pick Volume Button again to unmute
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FALSE(player->isMuted());
}

TEST_F(MediaWidgetTest, PickSeekBarCalculatesFractionAndSeeks) {
  render::PickingResult pick;
  pick.tag.kind = render::tagKindOverlay;
  // Tag corresponding to 50% seek: tagSeekBase + 500
  pick.tag.clusterIndex = widget->tagBase() + MediaWidget::tagSeekBase + 500U;

  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.5F);

  // Tag corresponding to 25% seek: tagSeekBase + 250
  pick.tag.clusterIndex = widget->tagBase() + MediaWidget::tagSeekBase + 250U;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.25F);
}

TEST_F(MediaWidgetTest, IgnoresUnrelatedPicks) {
  render::PickingResult pick;
  // Non-overlay
  pick.tag.kind         = render::tagKindGlyph;
  pick.tag.clusterIndex = widget->tagBase() + 0x800U;
  EXPECT_FALSE(widget->picked(pick, *state));

  // Unrelated overlay tag
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 50U;
  EXPECT_FALSE(widget->picked(pick, *state));
}

TEST_F(MediaWidgetTest, AccessibilityTreeAndActions) {
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 1);
  widget->setTitle("Audio Sample");
  widget->describe(builder);

  const auto rootId = static_cast<std::uint64_t>(widget->tagBase());
  // Perform Action: Click on Play
  EXPECT_TRUE(widget->performAction(rootId + MediaWidget::tagPlay,
                                    gleditor::a11y::Action::Click, ""));
  EXPECT_EQ(player->state(), PlaybackState::Playing);

  // Perform Action: Click on Pause
  EXPECT_TRUE(widget->performAction(rootId + MediaWidget::tagPause,
                                    gleditor::a11y::Action::Click, ""));
  EXPECT_EQ(player->state(), PlaybackState::Paused);

  // Perform Action: Click on Stop
  EXPECT_TRUE(widget->performAction(rootId + MediaWidget::tagStop,
                                    gleditor::a11y::Action::Click, ""));
  EXPECT_EQ(player->state(), PlaybackState::Stopped);
}

TEST_F(MediaWidgetTest, EachCardOwnsItsPickingAndAccessibilityRange) {
  auto secondPlayer = std::make_shared<MediaPlayer>(true);
  auto stream       = std::make_shared<MemoryMediaStream>("SECOND_AUDIO_DATA");
  ASSERT_TRUE(secondPlayer->load(MediaResource::fromStream(stream, "Second")));
  MediaWidget second("Monospace 10", secondPlayer);

  EXPECT_NE(widget->widgetId(), second.widgetId());
  EXPECT_NE(widget->tagBase(), second.tagBase());
  EXPECT_EQ(widget->tagBase() & 0xFFFU, 0U);
  EXPECT_EQ(second.tagBase() & 0xFFFU, 0U);

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = second.tagBase() + MediaWidget::tagPlay;

  EXPECT_FALSE(widget->picked(pick, *state));
  EXPECT_TRUE(second.picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Stopped);
  EXPECT_EQ(secondPlayer->state(), PlaybackState::Playing);

  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 1);
  widget->describe(builder);
  second.describe(builder);
  EXPECT_NE(widget->tagBase(), second.tagBase());
}

TEST_F(MediaWidgetTest, DeviceReadyAndDrawFrame) {
  widget->deviceReady(*device, render::PipelineDesc{});

  choreograph::Timeline timeline;
  glm::mat4 vp{1.0F};
  gleditor::FrameContext ctx{*state, vp, 1280, 720, timeline};

  // 1. Draw in screen overlay mode
  widget->setScreenPosition(50.0F, 50.0F);
  widget->drawFrame(ctx);

  // 2. Draw when playing
  player->play();
  EXPECT_TRUE(widget->busy());
  widget->drawFrame(ctx);

  // 3. Draw when paused
  player->pause();
  EXPECT_FALSE(widget->busy());
  widget->drawFrame(ctx);

  // 4. Draw when stopped
  player->stop();
  widget->drawFrame(ctx);

  // 5. Draw when invisible (early exit)
  widget->setVisible(false);
  widget->drawFrame(ctx);
  widget->setVisible(true);

  // 6. Draw with custom position
  widget->setWorldPosition(glm::vec3{1.0F, 2.0F, 3.0F});
  widget->drawFrame(ctx);
}

TEST_F(MediaWidgetTest, BusyAndLoadResource) {
  auto memStream = std::make_shared<MemoryMediaStream>("AUDIO_DATA_FOR_LOAD");
  auto res       = MediaResource::fromStream(memStream, "NewTrack");

  EXPECT_TRUE(widget->load(res));
  EXPECT_EQ(widget->title(), "NewTrack");

  auto newPlayer = std::make_shared<MediaPlayer>(true);
  widget->setPlayer(newPlayer);
  EXPECT_EQ(widget->player(), newPlayer);
}

TEST_F(MediaWidgetTest, SettledChromeShapesAndUploadsNothing) {
  widget->setScreenPosition(50.0F, 50.0F);
  widget->deviceReady(*device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1.0F};
  gleditor::FrameContext context{*state, projection, 1280, 720, timeline};
  widget->drawFrame(context);
  const auto built = widget->chromeRevision();
  const auto cache = widget->shapingStats();
  EXPECT_GT(built, 0U);
  EXPECT_GT(cache.entries, 0U);
  EXPECT_CALL(*device, updateBuffer).Times(0);
  EXPECT_CALL(*device, createPipeline).Times(0);
  gleditor::text::ShapingStatsScope shaping;
  for (int frame = 0; frame < 50; frame++) {
    state->beginPickScene();
    widget->drawFrame(context);
  }
  EXPECT_EQ(widget->chromeRevision(), built);
  EXPECT_EQ(widget->shapingStats().misses, cache.misses);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(MediaWidgetTest, PlayerChangesInvalidateChromeAndPositionOnlyMovesIt) {
  widget->setScreenPosition(50.0F, 50.0F);
  widget->deviceReady(*device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1.0F};
  gleditor::FrameContext context{*state, projection, 1280, 720, timeline};
  widget->drawFrame(context);
  auto built = widget->chromeRevision();
  player->setMuted(true);
  widget->drawFrame(context);
  EXPECT_GT(widget->chromeRevision(), built);
  built = widget->chromeRevision();
  player->setPlaybackRate(1.5F);
  widget->drawFrame(context);
  EXPECT_GT(widget->chromeRevision(), built);
  built = widget->chromeRevision();
  widget->setScreenPosition(150.0F, 175.0F);
  gleditor::text::ShapingStatsScope shaping;
  widget->drawFrame(context);
  EXPECT_EQ(widget->chromeRevision(), built);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 1);
  widget->describe(builder);
  const auto root = tree.find(builder.id(widget->tagBase()));
  ASSERT_TRUE(root.has_value());
  ASSERT_TRUE(root->bounds.has_value());
  EXPECT_NEAR(root->bounds->left, 150.0, 0.001);
  EXPECT_NEAR(root->bounds->bottom, 720.0 - 175.0, 0.001);
}

TEST_F(MediaWidgetTest,
       ResponsiveChromeStaysWithinTheCardAndKeepsFullA11yTitle) {
  widget = std::make_unique<MediaWidget>(std::string{}, player);
  const std::string title =
      "A long media title with العربية é and 👩‍💻 "
      "that needs fitting";
  widget->setTitle(title);
  widget->setScreenPosition(10.0F, 20.0F);
  widget->deviceReady(*device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1.0F};
  for (const auto dimensions :
       {gleditor::ui::Size{120, 60}, gleditor::ui::Size{360, 140},
        gleditor::ui::Size{700, 400}}) {
    widget->setSize(dimensions.width, dimensions.height);
    for (const float scale : {0.8F, 1.0F, 1.5F, 2.0F}) {
      SCOPED_TRACE(testing::Message()
                   << dimensions.width << 'x' << dimensions.height << " scale "
                   << scale);
      gleditor::FrameContext context{.state          = *state,
                                     .viewProjection = projection,
                                     .screenWidth    = 1280,
                                     .screenHeight   = 720,
                                     .timeline       = timeline,
                                     .metrics        = {.fontScale = scale}};
      widget->drawFrame(context);
      const auto &layout = widget->layout();
      for (const auto &box : layout.boxes) {
        EXPECT_GE(box.rect.left, 0.0F);
        EXPECT_GE(box.rect.bottom, 0.0F);
        EXPECT_LE(box.rect.left + box.rect.width, widget->width() + 0.01F);
        EXPECT_LE(box.rect.bottom + box.rect.height, widget->height() + 0.01F);
      }
      for (const auto tag : {MediaWidget::tagPlay, MediaWidget::tagPause,
                             MediaWidget::tagStop, MediaWidget::tagVolume,
                             MediaWidget::tagSpeed, MediaWidget::tagSeekBase})
        EXPECT_NE(layout.find(widget->tagBase() + tag), nullptr);
      gleditor::a11y::Tree tree;
      gleditor::a11y::Builder builder(tree, 1);
      widget->describe(builder);
      const auto root = tree.find(builder.id(widget->tagBase()));
      ASSERT_TRUE(root.has_value());
      EXPECT_EQ(root->label, title);
      EXPECT_TRUE(root->bounds.has_value());
    }
  }
}

TEST_F(MediaWidgetTest, AccessibleSeekUsesTheSharedScrubberClamp) {
  const auto seek = widget->tagBase() + MediaWidget::tagSeekBase;
  EXPECT_TRUE(
      widget->performAction(seek, gleditor::a11y::Action::SetValue, "0.25"));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.25F);
  EXPECT_TRUE(
      widget->performAction(seek, gleditor::a11y::Action::SetValue, "2.0"));
  EXPECT_FLOAT_EQ(player->progressFraction(), 1.0F);
  EXPECT_FALSE(widget->performAction(seek, gleditor::a11y::Action::SetValue,
                                     "not a number"));
  EXPECT_TRUE(widget->performAction(gleditor::a11y::Ids::of(16, seek),
                                    gleditor::a11y::Action::SetValue, "0.5"));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.5F);
}

TEST_F(MediaWidgetTest, MoreThanSixteenCardsKeepCapturedPickingIdentities) {
  std::vector<std::unique_ptr<MediaWidget>> cards;
  for (int index = 0; index < 24; index++) {
    auto card = std::make_unique<MediaWidget>("Monospace 10", player);
    card->setScreenPosition(0.0F, 0.0F);
    card->deviceReady(*device, {});
    cards.push_back(std::move(card));
  }
  ch::Timeline timeline;
  const glm::mat4 projection{1.0F};
  gleditor::FrameContext context{*state, projection, 1280, 720, timeline};
  state->beginPickScene();
  for (const auto &card : cards) card->drawFrame(context);
  const auto captured = state->overlayPickScene;
  ASSERT_EQ(captured.widgetOverlays.size(), cards.size());
  for (std::size_t index = 0; index < cards.size(); index++) {
    render::PickingResult pick{
        .requestId = 1,
        .tag = render::unpackPickingTag(captured.widgetOverlays[index].identity,
                                        MediaWidget::tagPlay, 0)};
    pick.overlayWidgetId = render::resolveOverlayWidget(captured, pick.tag);
    ASSERT_TRUE(pick.overlayWidgetId.has_value());
    EXPECT_EQ(*pick.overlayWidgetId,
              cards[index]->tagBase() + MediaWidget::tagPlay);
    for (std::size_t other = 0; other < cards.size(); other++)
      EXPECT_EQ(cards[other]->picked(pick, *state), other == index);
    if (index != 0)
      EXPECT_NE(captured.widgetOverlays[index].identity,
                captured.widgetOverlays[index - 1].identity);
  }
  // An unrelated radial-menu-looking tag cannot be interpreted as a card
  // control without this card's captured scope and full semantic target.
  render::PickingResult unrelated{
      .requestId = 2,
      .tag       = {.kind = render::tagKindOverlay, .clusterIndex = 0x8001U}};
  for (const auto &card : cards) EXPECT_FALSE(card->picked(unrelated, *state));
}

TEST_F(MediaWidgetTest, CapturedCardMeaningSurvivesPresentationChanges) {
  widget->setScreenPosition(50.0F, 50.0F);
  widget->deviceReady(*device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1.0F};
  gleditor::FrameContext context{*state, projection, 1280, 720, timeline};
  state->beginPickScene();
  widget->drawFrame(context);
  const auto captured = state->overlayPickScene;
  ASSERT_EQ(captured.widgetOverlays.size(), 1U);
  render::PickingResult pick{
      .requestId = 1,
      .tag = render::unpackPickingTag(captured.widgetOverlays[0].identity,
                                      MediaWidget::tagSeekBase + 750U, 0)};
  pick.overlayWidgetId = render::resolveOverlayWidget(captured, pick.tag);
  widget->setTitle("Changed after the click was requested");
  context.metrics.fontScale = 2.0F;
  state->beginPickScene();
  widget->drawFrame(context);
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.75F);
  pick.overlayWidgetId.reset();
  EXPECT_FALSE(widget->picked(pick, *state));
}

TEST_F(MediaWidgetTest, normalCardDrawsItsTitleAndPlaybackBadge) {
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::uint32_t next = 1;
  ON_CALL(*device, createBuffer)
      .WillByDefault([&](render::BufferKind, std::size_t) {
        return render::BufferHandle{next++};
      });
  ON_CALL(*device, resizeBuffer)
      .WillByDefault(
          [](render::BufferHandle handle, std::size_t) { return handle; });
  ON_CALL(*device, updateBuffer)
      .WillByDefault([&](render::BufferHandle handle, std::size_t offset,
                         std::span<const std::byte> bytes) {
        auto &storage = buffers[handle.id];
        storage.resize(std::max(storage.size(), offset + bytes.size()));
        std::memcpy(storage.data() + offset, bytes.data(), bytes.size());
      });
  std::size_t headerInk = 0;
  ON_CALL(*device, drawGlyphs)
      .WillByDefault([&](const render::DrawUniforms &,
                         render::BufferHandle handle, std::size_t offset,
                         std::uint32_t count) {
        const auto &storage = buffers.at(handle.id);
        for (std::uint32_t i = 0; i < count; ++i) {
          Doc::VBORow row{};
          std::memcpy(&row, storage.data() + offset + i * sizeof(row),
                      sizeof(row));
          if ((row.foreground & Doc::VBORow::solidFlag) == 0 &&
              row.pos[1] > 240)
            ++headerInk;
        }
      });
  widget = std::make_unique<MediaWidget>(std::string{}, player);
  widget->setTitle("Visible media title");
  widget->setSize(500, 280);
  widget->setScreenPosition(50, 150);
  widget->deviceReady(*device, {});
  ch::Timeline timeline;
  const glm::mat4 projection{1};
  gleditor::FrameContext context{*state, projection, 1280, 800, timeline};
  widget->drawFrame(context);
  EXPECT_GT(headerInk, 10U);
}
