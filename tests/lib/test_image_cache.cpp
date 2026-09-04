/**
 * @file test_image_cache.cpp
 * @brief Unit tests for DecodedImage and ImageCache.
 */
#include <gleditor/image_cache.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <gmock/gmock.h>

#include "mocks/device.hpp"

namespace gleditor {

TEST(ImageCacheTest, DecodeInvalidBufferReturnsEmpty) {
  const std::vector<std::uint8_t> empty;
  const auto decoded = decodeImageBuffer(empty);
  EXPECT_FALSE(decoded.valid());
}

TEST(ImageCacheTest, ImageResourceAspectRatio) {
  ImageResource res;
  res.width  = 800;
  res.height = 600;
  EXPECT_FLOAT_EQ(res.aspectRatio(), 800.0F / 600.0F);
}

TEST(ImageCacheTest, PutAndFindImageWithoutDevice) {
  ImageCache cache(nullptr, 512, 4);

  DecodedImage img;
  img.width  = 64;
  img.height = 64;
  img.rgba.resize(64 * 64 * 4, 255); // 64x64 white

  const auto res = cache.put("test_image_1", img);
  EXPECT_EQ(res.width, 64);
  EXPECT_EQ(res.height, 64);
  EXPECT_EQ(res.layer, 0);

  const auto found = cache.find("test_image_1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->width, 64);
  EXPECT_EQ(found->height, 64);
  EXPECT_EQ(found->layer, 0);
}

namespace {

using testing::NiceMock;

/// A 4-byte-per-pixel image whose bytes encode their own (x, y) so a test
/// can tell a correctly-cropped upload from a stride mistake by content, not
/// just by size -- pixel (x, y) is (x & 0xFF, y & 0xFF, 0xAB, 0xFF).
DecodedImage markedImage(const int width, const int height) {
  DecodedImage img;
  img.width  = width;
  img.height = height;
  img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto *const px = &img.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
      px[0]          = static_cast<std::uint8_t>(x & 0xFF);
      px[1]          = static_cast<std::uint8_t>(y & 0xFF);
      px[2]          = 0xAB;
      px[3]          = 0xFF;
    }
  }
  return img;
}

/// Records every updateTextureLayer() call ImageCache::put() makes, keyed by
/// atlas layer -- what lets a stress test check the shelf-packer's own
/// output for overlapping rects (corruption a crash would not reveal) and
/// diff an upload's actual bytes against what should have been cropped from
/// a larger source image.
class RecordingDevice : public NiceMock<MockRenderDevice> {
public:
  struct Upload {
    int layer{};
    int x{};
    int y{};
    int width{};
    int height{};
    std::vector<std::byte> data;
  };
  std::vector<Upload> uploads;

  RecordingDevice() {
    ON_CALL(*this, createTextureArray)
        .WillByDefault(
            [](int, int, render::TextureFormat, int) -> render::TextureHandle {
              return render::TextureHandle{1};
            });
    ON_CALL(*this, updateTextureLayer)
        .WillByDefault([this](render::TextureHandle, const int layer,
                              const int xOffset, const int yOffset,
                              const int width, const int height,
                              const std::span<const std::byte> data) {
          uploads.push_back(
              Upload{layer, xOffset, yOffset, width, height,
                     std::vector<std::byte>(data.begin(), data.end())});
        });
  }
};

/// Whether two axis-aligned rects on the same atlas layer overlap -- the
/// property that matters for the shelf-packer, since two images sharing
/// pixels means one's upload silently overwrote part of the other's.
bool rectsOverlap(const RecordingDevice::Upload &a,
                  const RecordingDevice::Upload &b) {
  if (a.layer != b.layer) {
    return false;
  }
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}

} // namespace

// Packs enough differently-sized images into a small atlas (small enough
// that a handful of images force both new shelf rows and new layers) that a
// packing mistake -- the same class of bug this test exists to catch --
// would show up as two uploads landing on overlapping rects.
TEST(ImageCacheTest, ManyDifferentlySizedImagesNeverOverlapAcrossLayers) {
  auto device = std::make_unique<RecordingDevice>();
  ImageCache cache(device.get(), /*atlasSize=*/64, /*maxLayers=*/4);

  const std::vector<std::pair<int, int>> sizes = {
      {20, 20}, {40, 10}, {15, 30}, {50, 5}, {8, 8},   {30, 30},
      {12, 40}, {25, 25}, {60, 4},  {5, 50}, {33, 18}, {18, 33},
  };
  std::size_t placed = 0;
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    const auto [w, h] = sizes[i];
    const auto res = cache.put("img_" + std::to_string(i), markedImage(w, h));
    if (res.valid()) {
      ++placed;
    }
  }
  ASSERT_GT(placed, sizes.size() / 2U)
      << "too few images placed to exercise multi-layer packing at all";
  EXPECT_GT(cache.layerCount(), 1)
      << "test fixture should force at least one layer boundary to be "
         "meaningful";

  for (std::size_t i = 0; i < device->uploads.size(); ++i) {
    for (std::size_t j = i + 1; j < device->uploads.size(); ++j) {
      EXPECT_FALSE(rectsOverlap(device->uploads[i], device->uploads[j]))
          << "uploads " << i << " and " << j << " overlap on layer "
          << device->uploads[i].layer;
    }
  }
}

// The atlas has no eviction (confirmed by reading ImageCache::put(), not
// assumed): once every layer is full, put() returns an invalid resource
// rather than reclaiming space from an existing entry -- a real, standing
// limitation, not something this test can fix by itself. What it checks
// instead is narrower and non-negotiable: running out of room must fail
// closed (silently do nothing for the image that doesn't fit) rather than
// corrupt an entry that was already placed.
TEST(ImageCacheTest, AtlasFullFailsClosedWithoutCorruptingEarlierEntries) {
  auto device = std::make_unique<RecordingDevice>();
  // One layer, sized so exactly one 32x32 image fits with no room for a
  // second -- the smallest fixture that can force "full" deterministically.
  ImageCache cache(device.get(), /*atlasSize=*/32, /*maxLayers=*/1);

  const auto first = cache.put("first", markedImage(32, 32));
  ASSERT_TRUE(first.valid());

  const auto second = cache.put("second", markedImage(32, 32));
  EXPECT_FALSE(second.valid())
      << "atlas is provably full; a second same-size image must not "
         "silently reuse the first's space";

  // The one successful upload must be untouched: no later call rewrote any
  // part of it.
  ASSERT_EQ(device->uploads.size(), 1U)
      << "a failed placement must not still call updateTextureLayer()";
  const auto refetched = cache.find("first");
  ASSERT_TRUE(refetched.has_value());
  EXPECT_EQ(refetched->layer, first.layer);
  EXPECT_FLOAT_EQ(refetched->u0, first.u0);
  EXPECT_FLOAT_EQ(refetched->v0, first.v0);
}

// Found while writing this stress test, not assumed away: put() clamps an
// oversized image's width/height down to atlasSize_ before packing, but
// originally sliced the upload bytes as `imgW * imgH * 4` contiguous bytes
// straight off the front of image.rgba -- correct only when the source's
// own row stride already equals imgW*4. A source WIDER than the atlas has a
// stride of image.width*4, not imgW*4, so that slice reads across row
// boundaries at the wrong offsets: not a crash, a silently wrong-looking
// image. markedImage()'s per-pixel (x, y) encoding makes this detectable by
// content rather than merely by byte count.
TEST(ImageCacheTest, ImageWiderThanAtlasUploadsACorrectlyStridedCrop) {
  auto device             = std::make_unique<RecordingDevice>();
  constexpr int atlasSize = 64;
  ImageCache cache(device.get(), atlasSize, /*maxLayers=*/1);

  constexpr int sourceWidth  = 100; // wider than the atlas on purpose.
  constexpr int sourceHeight = 40;
  const auto source          = markedImage(sourceWidth, sourceHeight);

  const auto res = cache.put("wide", source);
  ASSERT_TRUE(res.valid());
  ASSERT_EQ(device->uploads.size(), 1U);
  const auto &upload = device->uploads.front();
  EXPECT_EQ(upload.width, atlasSize);
  EXPECT_EQ(upload.height, sourceHeight);
  ASSERT_EQ(upload.data.size(),
            static_cast<std::size_t>(atlasSize) * sourceHeight * 4);

  // The correct upload is the top-left atlasSize x sourceHeight crop of the
  // source, read with the *source's* own stride -- build that reference
  // crop directly from markedImage()'s pixel encoding and compare, rather
  // than re-deriving it from the source buffer's layout (which is exactly
  // what a stride bug would also get wrong).
  bool mismatch = false;
  for (int y = 0; y < sourceHeight && !mismatch; ++y) {
    for (int x = 0; x < atlasSize && !mismatch; ++x) {
      const auto *const got = reinterpret_cast<const std::uint8_t *>(
          upload.data.data() +
          (static_cast<std::size_t>(y) * atlasSize + x) * 4);
      const std::uint8_t expected[4] = {static_cast<std::uint8_t>(x & 0xFF),
                                        static_cast<std::uint8_t>(y & 0xFF),
                                        0xAB, 0xFF};
      if (0 != std::memcmp(got, expected, 4)) {
        mismatch = true;
        ADD_FAILURE() << "pixel (" << x << "," << y << ") uploaded ("
                      << int(got[0]) << "," << int(got[1]) << "," << int(got[2])
                      << "," << int(got[3]) << ") expected ("
                      << int(expected[0]) << "," << int(expected[1]) << ","
                      << int(expected[2]) << "," << int(expected[3]) << ")";
      }
    }
  }
}

} // namespace gleditor
