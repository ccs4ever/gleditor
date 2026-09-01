/**
 * @file media_test.cpp
 * @brief Unit tests for media resources, playback controls, and range bounds.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <gleditor/audio.hpp>
#include <gleditor/media.hpp>
#include <gleditor/media_stream.hpp>

using gleditor::AudioMedia;
using gleditor::AudioPlayer;
using gleditor::ByteRange;
using gleditor::MediaPlayer;
using gleditor::MediaResource;
using gleditor::MemoryMediaStream;
using gleditor::PlaybackState;
using gleditor::TimeRange;

// -- MediaResource ------------------------------------------------------------

TEST(MediaTest, MediaResourceFromFileAndStream) {
  auto fileRes = MediaResource::fromFile("tests/samples/kjv.txt");
  EXPECT_TRUE(fileRes->isValid());
  EXPECT_EQ(fileRes->type(), MediaResource::Type::File);
  EXPECT_EQ(fileRes->name(), "kjv.txt");
  EXPECT_NE(fileRes->stream(), nullptr);

  // Subspan
  auto subRes = fileRes->subspan(100, 50);
  ASSERT_NE(subRes, nullptr);
  EXPECT_TRUE(subRes->isValid());
  EXPECT_EQ(subRes->stream()->size(), 50U);

  // Stream-based
  const std::string dummyPayload = "AUDIO_STREAM_DUMMY_DATA_BYTES";
  auto memStream = std::make_shared<MemoryMediaStream>(dummyPayload);
  auto streamRes = MediaResource::fromStream(memStream, "MemoryAudio");
  EXPECT_TRUE(streamRes->isValid());
  EXPECT_EQ(streamRes->name(), "MemoryAudio");
  EXPECT_EQ(streamRes->type(), MediaResource::Type::Stream);
}

// -- MediaPlayer Controls & State ---------------------------------------------

TEST(MediaTest, MediaPlayerPlaybackStateCycle) {
  MediaPlayer player(true); // Dummy audio mode

  auto memStream = std::make_shared<MemoryMediaStream>("DUMMY_WAV_HEADER_DATA");
  auto res       = MediaResource::fromStream(memStream, "TestTrack");

  EXPECT_TRUE(player.load(res));
  EXPECT_EQ(player.state(), PlaybackState::Stopped);

  EXPECT_TRUE(player.play());
  EXPECT_EQ(player.state(), PlaybackState::Playing);

  player.pause();
  EXPECT_EQ(player.state(), PlaybackState::Paused);

  player.toggle();
  EXPECT_EQ(player.state(), PlaybackState::Playing);

  player.stop();
  EXPECT_EQ(player.state(), PlaybackState::Stopped);
  EXPECT_FLOAT_EQ(player.positionSeconds(), 0.0F);
}

TEST(MediaTest, MediaPlayerVolumeAndMute) {
  MediaPlayer player(true);

  player.setVolume(80);
  EXPECT_EQ(player.volume(), 80);

  // Clamping
  player.setVolume(150);
  EXPECT_EQ(player.volume(), 100);

  player.setVolume(-10);
  EXPECT_EQ(player.volume(), 0);

  EXPECT_FALSE(player.isMuted());
  player.setMuted(true);
  EXPECT_TRUE(player.isMuted());
}

TEST(MediaTest, MediaPlayerTimeRangeBoundsAndSeeking) {
  MediaPlayer player(true);
  auto memStream =
      std::make_shared<MemoryMediaStream>("DUMMY_AUDIO_DATA_FOR_RANGE");
  auto res = MediaResource::fromStream(memStream, "RangeTrack");
  player.load(res);

  // Constrain playback to [10.0s, 30.0s]
  player.setTimeRange(10.0F, 30.0F);
  EXPECT_TRUE(player.hasRangeConstraint());
  ASSERT_TRUE(player.activeTimeRange().has_value());
  EXPECT_FLOAT_EQ(player.activeTimeRange()->startSeconds, 10.0F);
  EXPECT_FLOAT_EQ(player.activeTimeRange()->endSeconds, 30.0F);

  // Playing automatically begins at start of range
  player.play();
  EXPECT_GE(player.positionSeconds(), 10.0F);

  // Seeking outside range is clamped
  player.seek(5.0F);
  EXPECT_FLOAT_EQ(player.positionSeconds(), 10.0F);

  player.seek(35.0F);
  EXPECT_FLOAT_EQ(player.positionSeconds(), 30.0F);

  // Seeking fraction (0.5 within [10.0, 30.0] -> 20.0s)
  player.seekFraction(0.5F);
  EXPECT_FLOAT_EQ(player.positionSeconds(), 20.0F);

  // Range enforcement on update
  player.seek(29.5F);
  player.update(1.0F); // Position reaches 30.5F > 30.0F
  EXPECT_EQ(player.state(), PlaybackState::Ended);

  // With looping enabled
  player.setLooping(true);
  player.play();
  player.seek(29.5F);
  player.update(1.0F);
  EXPECT_FLOAT_EQ(player.positionSeconds(), 10.0F); // Looped back to start
}

TEST(MediaTest, AudioAliasesCompatibility) {
  AudioPlayer player(true);
  auto memStream = std::make_shared<MemoryMediaStream>("AUDIO_TRACK_PCM");
  auto audio     = AudioMedia::fromStream(memStream, "AudioAliasTrack");

  EXPECT_TRUE(player.load(audio));
  EXPECT_TRUE(player.play());
  EXPECT_EQ(player.state(), PlaybackState::Playing);
}

TEST(MediaTest, VideoPropertiesAndFrames) {
  MediaPlayer player(true);
  EXPECT_FALSE(player.hasVideo());
  EXPECT_EQ(player.videoWidth(), 0);
  EXPECT_EQ(player.videoHeight(), 0);
  EXPECT_FLOAT_EQ(player.aspectRatio(), 16.0F / 9.0F);
  EXPECT_EQ(player.latestFrame(), nullptr);
  EXPECT_FALSE(player.isNewFrameAvailable());

  // Test VideoFrame struct
  gleditor::VideoFrame frame;
  frame.width  = 640;
  frame.height = 480;
  frame.rgba.resize(640 * 480, 0xFF0000FFU);
  frame.timestampMs = 1234;

  EXPECT_EQ(frame.width, 640);
  EXPECT_EQ(frame.height, 480);
  EXPECT_EQ(frame.rgba.size(), 640U * 480U);
  EXPECT_EQ(frame.timestampMs, 1234U);
}

TEST(MediaTest, PlaybackStateNames) {
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Stopped), "Stopped");
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Playing), "Playing");
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Paused), "Paused");
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Opening), "Opening");
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Buffering),
               "Buffering");
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Ended), "Ended");
  EXPECT_STREQ(gleditor::playbackStateName(PlaybackState::Error), "Error");
}

TEST(MediaTest, MediaResourceLocationAndSubspan) {
  auto locRes = MediaResource::fromLocation("http://example.com/audio.opus");
  EXPECT_TRUE(locRes->isValid());
  EXPECT_EQ(locRes->type(), MediaResource::Type::Location);
  EXPECT_EQ(locRes->location(), "http://example.com/audio.opus");
  EXPECT_EQ(locRes->stream(), nullptr);
  EXPECT_EQ(locRes->subspan(0, 100), nullptr);

  auto fileRes = MediaResource::fromFile("tests/samples/kjv.txt");
  auto subFile = fileRes->subspan(10, 50);
  ASSERT_NE(subFile, nullptr);
  EXPECT_TRUE(subFile->isValid());
  EXPECT_EQ(subFile->stream()->size(), 50U);
}

TEST(MediaTest, MediaPlayerUnloadAndByteRanges) {
  MediaPlayer player(true);
  auto memStream =
      std::make_shared<MemoryMediaStream>("AUDIO_DATA_FOR_BYTE_RANGE");
  auto res = MediaResource::fromStream(memStream, "ByteRangeTrack");

  EXPECT_TRUE(player.load(res));
  EXPECT_EQ(player.currentResource(), res);

  player.setByteRange(100, 500);
  EXPECT_TRUE(player.hasRangeConstraint());
  ASSERT_TRUE(player.activeByteRange().has_value());
  EXPECT_EQ(player.activeByteRange()->start, 100U);
  EXPECT_EQ(player.activeByteRange()->length, 500U);

  player.clearRange();
  EXPECT_FALSE(player.hasRangeConstraint());
  EXPECT_FALSE(player.activeByteRange().has_value());
  EXPECT_FALSE(player.activeTimeRange().has_value());

  EXPECT_FALSE(player.isLooping());
  player.setLooping(true);
  EXPECT_TRUE(player.isLooping());

  player.unload();
  EXPECT_EQ(player.state(), PlaybackState::Stopped);
}
