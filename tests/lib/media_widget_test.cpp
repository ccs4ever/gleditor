/**
 * @file media_widget_test.cpp
 * @brief Unit tests for the document-embedded interactive media player UI
 *        widget.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/audio_widget.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

using gleditor::AudioWidget;
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

    state  = std::make_unique<RenderState>(device.get());
    player = std::make_shared<MediaPlayer>(true); // Dummy audio mode

    auto memStream =
        std::make_shared<MemoryMediaStream>("AUDIO_DATA_FOR_WIDGET");
    auto res = MediaResource::fromStream(memStream, "WidgetTrack");
    player->load(res);

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
  constexpr std::uint32_t tagBase = 0x8000U;

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = tagBase + MediaWidget::tagPlay;

  // 1. Pick Play Button
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Playing);

  // 2. Pick Pause Button
  pick.tag.clusterIndex = tagBase + MediaWidget::tagPause;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Paused);

  // 3. Pick Stop Button
  pick.tag.clusterIndex = tagBase + MediaWidget::tagStop;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_EQ(player->state(), PlaybackState::Stopped);

  // 4. Pick Volume Button
  EXPECT_FALSE(player->isMuted());
  pick.tag.clusterIndex = tagBase + MediaWidget::tagVolume;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_TRUE(player->isMuted());

  // Pick Volume Button again to unmute
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FALSE(player->isMuted());
}

TEST_F(MediaWidgetTest, PickSeekBarCalculatesFractionAndSeeks) {
  constexpr std::uint32_t tagBase = 0x8000U;

  render::PickingResult pick;
  pick.tag.kind = render::tagKindOverlay;
  // Tag corresponding to 50% seek: tagSeekBase + 500
  pick.tag.clusterIndex = tagBase + MediaWidget::tagSeekBase + 500U;

  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.5F);

  // Tag corresponding to 25% seek: tagSeekBase + 250
  pick.tag.clusterIndex = tagBase + MediaWidget::tagSeekBase + 250U;
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(player->progressFraction(), 0.25F);
}

TEST_F(MediaWidgetTest, IgnoresUnrelatedPicks) {
  render::PickingResult pick;
  // Non-overlay
  pick.tag.kind         = render::tagKindGlyph;
  pick.tag.clusterIndex = 0x8000U + MediaWidget::tagPlay;
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

  constexpr std::uint64_t rootId = 0x8000U;
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

TEST_F(MediaWidgetTest, AudioWidgetAliasCompatibility) {
  auto audioWidget = std::make_unique<AudioWidget>("Sans 10", player);
  EXPECT_TRUE(audioWidget->isVisible());
  EXPECT_EQ(audioWidget->player(), player);
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
