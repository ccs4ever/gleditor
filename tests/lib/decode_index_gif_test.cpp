/**
 * @file decode_index_gif_test.cpp
 * @brief Unit tests for GIF decode index, peekGifSize, and isAnimatedGif.
 */
#include <gleditor/decode_index.hpp>
#include <gtest/gtest.h>

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
#include <gif_lib.h>
#endif

#include <cstdint>
#include <vector>

namespace gleditor {
namespace {

#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
struct MemoryWriter {
  std::vector<std::uint8_t> *buffer;
};

int gifMemoryWrite(GifFileType *gif, const GifByteType *buf, int len) {
  auto *dest = static_cast<MemoryWriter *>(gif->UserData);
  dest->buffer->insert(dest->buffer->end(), buf, buf + len);
  return len;
}

std::vector<std::uint8_t> makeValidGif(const int width, const int height,
                                       const int frameCount = 2) {
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
}
#endif

/// Construct a minimal valid single-frame GIF in memory.
std::vector<std::uint8_t> makeMinimalGif(const std::uint16_t width,
                                         const std::uint16_t height,
                                         const bool animated = false) {
  std::vector<std::uint8_t> gif;
  // Header: GIF89a
  const char header[] = "GIF89a";
  gif.insert(gif.end(), header, header + 6);

  // Logical Screen Descriptor: Width (2 bytes), Height (2 bytes)
  gif.push_back(static_cast<std::uint8_t>(width & 0xFFU));
  gif.push_back(static_cast<std::uint8_t>((width >> 8U) & 0xFFU));
  gif.push_back(static_cast<std::uint8_t>(height & 0xFFU));
  gif.push_back(static_cast<std::uint8_t>((height >> 8U) & 0xFFU));

  // Packed fields: Global Color Table present, 2 colors (1 bit)
  // 0x80 (GCT flag) | 0x00 (color res) | 0x00 (sort) | 0x00 (2^1 = 2 entries)
  gif.push_back(0x80U);
  gif.push_back(0x00U); // Background color index
  gif.push_back(0x00U); // Pixel aspect ratio

  // Global Color Table: 2 RGB entries (6 bytes)
  gif.push_back(0xFFU);
  gif.push_back(0x00U);
  gif.push_back(0x00U); // Red
  gif.push_back(0x00U);
  gif.push_back(0x00U);
  gif.push_back(0xFFU); // Blue

  const auto appendFrame = [&]() {
    // Image Descriptor: 0x2C
    gif.push_back(0x2CU);
    // Left (2), Top (2), Width (2), Height (2)
    gif.push_back(0x00U);
    gif.push_back(0x00U);
    gif.push_back(0x00U);
    gif.push_back(0x00U);
    gif.push_back(static_cast<std::uint8_t>(width & 0xFFU));
    gif.push_back(static_cast<std::uint8_t>((width >> 8U) & 0xFFU));
    gif.push_back(static_cast<std::uint8_t>(height & 0xFFU));
    gif.push_back(static_cast<std::uint8_t>((height >> 8U) & 0xFFU));
    // Packed fields: no local color table
    gif.push_back(0x00U);

    // LZW minimum code size
    gif.push_back(0x02U);
    // Data sub-block with 1 byte (clear code)
    gif.push_back(0x01U);
    gif.push_back(0x04U); // clear code for 2-bit
    // Block terminator
    gif.push_back(0x00U);
  };

  appendFrame();
  if (animated) {
    appendFrame();
  }

  // Trailer: 0x3B
  gif.push_back(0x3BU);
  return gif;
}

TEST(DecodeIndexGifTest, PeekGifSizeValidHeader) {
  const auto gif  = makeMinimalGif(128, 64);
  const auto size = peekGifSize(gif);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(128U, size->first);
  EXPECT_EQ(64U, size->second);
}

TEST(DecodeIndexGifTest, PeekGifSizeInvalidBuffers) {
  EXPECT_FALSE(peekGifSize({}).has_value());

  const std::vector<std::uint8_t> tooShort = {'G', 'I', 'F', '8', '9', 'a'};
  EXPECT_FALSE(peekGifSize(tooShort).has_value());

  std::vector<std::uint8_t> badMagic = {'B', 'A',  'D',  'M',  'A',
                                        'G', 0x10, 0x00, 0x20, 0x00};
  EXPECT_FALSE(peekGifSize(badMagic).has_value());
}

TEST(DecodeIndexGifTest, IsAnimatedGifDistinguishesSingleFromMultiFrame) {
  const auto single = makeMinimalGif(32, 32, /*animated=*/false);
  EXPECT_FALSE(isAnimatedGif(single));

  const auto multi = makeMinimalGif(32, 32, /*animated=*/true);
  EXPECT_TRUE(isAnimatedGif(multi));
}

TEST(DecodeIndexGifTest, BuildDecodeIndexGif) {
#ifdef GLEDITOR_HAVE_DECODE_INDEX_GIF
  const auto multi = makeValidGif(64, 48, 2);
  const auto index = buildDecodeIndex(multi, MimeType::ImageGif);

  EXPECT_EQ(DecodeIndexFormat::Gif, index.format);
  EXPECT_TRUE(index.seekable);
  EXPECT_TRUE(index.durableIndex);
  EXPECT_EQ(2U, index.uncompressedExtent);
  EXPECT_EQ(2U, index.seekPoints.size());
  EXPECT_EQ(0U, index.seekPoints[0].uncompressedPosition);
  EXPECT_EQ(1U, index.seekPoints[1].uncompressedPosition);
#else
  const auto multi = makeMinimalGif(64, 48, /*animated=*/true);
  const auto index = buildDecodeIndex(multi, MimeType::ImageGif);
  EXPECT_EQ(DecodeIndexFormat::Unsupported, index.format);
#endif
}

} // namespace
} // namespace gleditor
