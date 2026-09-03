/**
 * @file test_image_cache.cpp
 * @brief Unit tests for DecodedImage and ImageCache.
 */
#include <gleditor/image_cache.hpp>
#include <gtest/gtest.h>

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

} // namespace gleditor
