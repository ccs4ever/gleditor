/**
 * @file gif_decoder_test.cpp
 * @brief Unit tests for GifDecoder.
 */
#include <gleditor/gif_decoder.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
#include <gif_lib.h>
#endif

namespace gleditor {
namespace {

struct MemoryWriter {
  std::vector<std::uint8_t> *buffer;
};

int gifMemoryWrite(GifFileType *gif, const GifByteType *buf, int len) {
  auto *dest = static_cast<MemoryWriter *>(gif->UserData);
  dest->buffer->insert(dest->buffer->end(), buf, buf + len);
  return len;
}

std::vector<std::uint8_t> makeTestGif(const int width, const int height,
                                      const int frameCount = 2) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
  std::vector<std::uint8_t> out;
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

TEST(GifDecoderTest, DecodeMultiFrameGif) {
  const auto bytes = makeTestGif(32, 24, 3);

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
  auto decoder = GifDecoder::decode(bytes);
  ASSERT_NE(nullptr, decoder);

  EXPECT_EQ(32, decoder->width());
  EXPECT_EQ(24, decoder->height());
  EXPECT_EQ(3U, decoder->frameCount());
  EXPECT_NEAR(0.60F, decoder->duration(), 0.01F);

  // Check frameAt timestamps
  const auto &f0 = decoder->frameAt(0.0F);
  EXPECT_EQ(32U * 24U, f0.rgba.size());
  EXPECT_NEAR(0.0F, f0.timestampSeconds, 0.001F);
  EXPECT_NEAR(0.20F, f0.durationSeconds, 0.01F);

  const auto &f1 = decoder->frameAt(0.25F);
  EXPECT_NEAR(0.20F, f1.timestampSeconds, 0.001F);

  const auto &f2 = decoder->frameAt(0.55F);
  EXPECT_NEAR(0.40F, f2.timestampSeconds, 0.001F);

  // Beyond duration clamps to last frame
  const auto &fEnd = decoder->frameAt(1.0F);
  EXPECT_NEAR(0.40F, fEnd.timestampSeconds, 0.001F);
#else
  auto decoder = GifDecoder::decode(bytes);
  EXPECT_EQ(nullptr, decoder);
#endif
}

TEST(GifDecoderTest, DecodeInvalidBytesReturnsNull) {
  std::vector<std::uint8_t> invalid = {0x00, 0x01, 0x02};
  auto decoder                      = GifDecoder::decode(invalid);
  EXPECT_EQ(nullptr, decoder);
}

} // namespace
} // namespace gleditor
