/**
 * @file media_widget_speed_test.cpp
 * @brief Unit tests for MediaWidget speed control button and rate cycling.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_widget.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

using gleditor::MediaPlayer;
using gleditor::MediaResource;
using gleditor::MediaWidget;
using gleditor::MemoryMediaStream;
using testing::NiceMock;
using testing::Return;

class MediaWidgetSpeedTest : public testing::Test {
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
    player = std::make_shared<MediaPlayer>(true);

    auto memStream =
        std::make_shared<MemoryMediaStream>("AUDIO_DATA_FOR_SPEED_TEST");
    auto res = MediaResource::fromStream(memStream, "SpeedTrack");
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

TEST_F(MediaWidgetSpeedTest, CyclePlaybackRatesByPicking) {
  constexpr std::uint32_t tagBase = 0x8000U;

  EXPECT_FLOAT_EQ(widget->playbackRate(), 1.0F);

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = tagBase + MediaWidget::tagSpeed;

  // 1.0x -> 1.5x
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 1.5F);
  EXPECT_FLOAT_EQ(player->playbackRate(), 1.5F);

  // 1.5x -> 2.0x
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 2.0F);

  // 2.0x -> 0.25x
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 0.25F);

  // 0.25x -> 0.5x
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 0.5F);

  // 0.5x -> 1.0x
  EXPECT_TRUE(widget->picked(pick, *state));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 1.0F);
}

TEST_F(MediaWidgetSpeedTest, SetPlaybackRateDirectly) {
  widget->setPlaybackRate(1.75F);
  EXPECT_FLOAT_EQ(widget->playbackRate(), 1.75F);
  EXPECT_FLOAT_EQ(player->playbackRate(), 1.75F);
}

TEST_F(MediaWidgetSpeedTest, AccessibilityPerformActionOnSpeedButton) {
  constexpr std::uint32_t tagBase = 0x8000U;
  const auto speedNodeId =
      static_cast<std::uint64_t>(tagBase + MediaWidget::tagSpeed);

  EXPECT_FLOAT_EQ(widget->playbackRate(), 1.0F);

  EXPECT_TRUE(
      widget->performAction(speedNodeId, gleditor::a11y::Action::Click, ""));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 1.5F);

  EXPECT_TRUE(
      widget->performAction(speedNodeId, gleditor::a11y::Action::Click, ""));
  EXPECT_FLOAT_EQ(widget->playbackRate(), 2.0F);
}
