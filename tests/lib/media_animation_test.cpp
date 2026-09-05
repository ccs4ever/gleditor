#include <gtest/gtest.h>

#include <gleditor/media.hpp>
#include <gleditor/media_stream.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
#include <gif_lib.h>
#endif

namespace gleditor {
namespace {

struct MemoryWriter {
  std::vector<std::byte> *buffer;
};

int gifMemoryWrite(GifFileType *gif, const GifByteType *buf, int len) {
  auto *dest    = static_cast<MemoryWriter *>(gif->UserData);
  const auto *b = reinterpret_cast<const std::byte *>(buf);
  dest->buffer->insert(dest->buffer->end(), b, b + len);
  return len;
}

std::vector<std::byte> makeTestGif(const int width, const int height,
                                   const int frameCount = 2) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
  std::vector<std::byte> out;
  MemoryWriter writer{&out};
  int err          = 0;
  GifFileType *gif = EGifOpen(&writer, gifMemoryWrite, &err);
  if (!gif) {
    return {};
  }

  ColorMapObject *cmap  = GifMakeMapObject(2, nullptr);
  cmap->Colors[0].Red   = 255;
  cmap->Colors[0].Green = 0;
  cmap->Colors[0].Blue  = 0;
  cmap->Colors[1].Red   = 0;
  cmap->Colors[1].Green = 255;
  cmap->Colors[1].Blue  = 0;

  EGifSetGifVersion(gif, true);
  EGifPutScreenDesc(gif, width, height, 8, 0, cmap);

  for (int f = 0; f < frameCount; ++f) {
    GraphicsControlBlock gcb{};
    gcb.DisposalMode     = DISPOSAL_UNSPECIFIED;
    gcb.UserInputFlag    = false;
    gcb.DelayTime        = 20; // 0.20s
    gcb.TransparentColor = NO_TRANSPARENT_COLOR;
    std::uint8_t ext[4];
    EGifGCBToExtension(&gcb, ext);
    EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, ext);

    EGifPutImageDesc(gif, 0, 0, width, height, false, nullptr);
    std::vector<GifPixelType> line(static_cast<std::size_t>(width),
                                   static_cast<GifPixelType>(f % 2));
    for (int y = 0; y < height; ++y) {
      EGifPutLine(gif, line.data(), width);
    }
  }

  EGifCloseFile(gif, &err);
  GifFreeMapObject(cmap);
  return out;
#else
  (void)width;
  (void)height;
  (void)frameCount;
  return {};
#endif
}

TEST(MediaAnimationTest, AnimatedGifPlaybackAndSpeed) {
  auto gifData = makeTestGif(32, 24, 2);
  auto stream  = std::make_shared<MemoryMediaStream>(std::move(gifData));
  auto res     = MediaResource::fromStream(stream, "test.gif");

  MediaPlayer player;
  ASSERT_TRUE(player.load(res));

  EXPECT_TRUE(player.hasVideo());
  EXPECT_EQ(player.videoWidth(), 32);
  EXPECT_EQ(player.videoHeight(), 24);
  EXPECT_NEAR(player.durationSeconds(), 0.40F, 0.01F);
  EXPECT_EQ(player.state(), PlaybackState::Stopped);

  EXPECT_TRUE(player.isNewFrameAvailable());
  auto f0 = player.latestFrame();
  ASSERT_NE(f0, nullptr);
  EXPECT_EQ(f0->width, 32);
  EXPECT_EQ(f0->height, 24);

  // Playback control
  EXPECT_TRUE(player.play());
  EXPECT_EQ(player.state(), PlaybackState::Playing);

  // Default playback rate is 1.0
  EXPECT_FLOAT_EQ(player.playbackRate(), 1.0F);

  // Advance time with playback rate 1.0
  player.update(0.05F);
  EXPECT_NEAR(player.positionSeconds(), 0.05F, 0.001F);

  // Change playback rate to 2.0x
  player.setPlaybackRate(2.0F);
  EXPECT_FLOAT_EQ(player.playbackRate(), 2.0F);

  // Advance time by 0.05s * 2.0x = +0.10s -> position = 0.15s
  player.update(0.05F);
  EXPECT_NEAR(player.positionSeconds(), 0.15F, 0.001F);

  // Pause
  player.pause();
  EXPECT_EQ(player.state(), PlaybackState::Paused);
  player.update(0.05F);
  EXPECT_NEAR(player.positionSeconds(), 0.15F,
              0.001F); // No advance while paused

  // Seeking
  player.seek(0.05F);
  EXPECT_NEAR(player.positionSeconds(), 0.05F, 0.001F);

  // Stop resets position to 0
  player.stop();
  EXPECT_EQ(player.state(), PlaybackState::Stopped);
  EXPECT_FLOAT_EQ(player.positionSeconds(), 0.0F);
}

TEST(MediaAnimationTest, AnimatedSvgPlaybackAndClamping) {
  const std::string_view animatedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
           <rect x="0" y="0" width="20" height="20" fill="#ff0000">
             <animate attributeName="x" from="0" to="80" dur="2s" repeatCount="indefinite"/>
           </rect>
         </svg>)";

  auto stream = std::make_shared<MemoryMediaStream>(animatedSvg);
  auto res    = MediaResource::fromStream(stream, "test.svg");

  MediaPlayer player;
  ASSERT_TRUE(player.load(res));

  EXPECT_TRUE(player.hasVideo());
  EXPECT_EQ(player.videoWidth(), 100);
  EXPECT_EQ(player.videoHeight(), 100);
  EXPECT_FLOAT_EQ(player.durationSeconds(), 2.0F);
  EXPECT_EQ(player.state(), PlaybackState::Stopped);

  // Set time range [0.5s, 1.5s]
  player.setTimeRange(0.5F, 1.5F);
  EXPECT_TRUE(player.hasRangeConstraint());
  EXPECT_NEAR(player.positionSeconds(), 0.5F, 0.001F);

  player.play();
  EXPECT_EQ(player.state(), PlaybackState::Playing);

  // Set playback rate to 0.5x
  player.setPlaybackRate(0.5F);
  EXPECT_FLOAT_EQ(player.playbackRate(), 0.5F);

  // Advance by 1.0s * 0.5 = 0.5s -> position = 1.0s
  player.update(1.0F);
  EXPECT_NEAR(player.positionSeconds(), 1.0F, 0.001F);

  // Seeking outside range clamps to [0.5, 1.5]
  player.seek(0.1F);
  EXPECT_NEAR(player.positionSeconds(), 0.5F, 0.001F);

  player.seek(1.9F);
  EXPECT_NEAR(player.positionSeconds(), 1.5F, 0.001F);

  // Looping behavior
  player.setLooping(true);
  player.seek(1.4F);
  player.update(0.4F); // 1.4 + 0.2 = 1.6 >= 1.5 -> loops to 0.5s
  EXPECT_NEAR(player.positionSeconds(), 0.5F, 0.001F);
}

} // namespace
} // namespace gleditor
