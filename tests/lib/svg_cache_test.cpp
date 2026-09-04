/**
 * @file svg_cache_test.cpp
 * @brief Unit tests for gleditor::SvgCache.
 *
 * Always compiled, matching decode_index_test.cpp's precedent for an
 * optional capability: peekSize()'s two failure-mode tests hold regardless
 * of whether GLEDITOR_HAVE_SVG_THORVG was compiled in (a build without
 * thorvg-1 fails every input, which is exactly what these two assert). The
 * rest -- real rasterization -- needs ThorVG to mean anything and is
 * compiled out otherwise, since there is no "Unsupported" enum value here to
 * runtime-skip on the way decode_index_test.cpp's format-keyed tests do.
 *
 * The GL/GlCanvas fast path itself has no test here: it needs a real,
 * current GL context bound to a real texture, which is exactly what
 * MockRenderDevice does not provide (nor does any other tests/lib/ harness
 * today -- tests/lib/canvas_image.cpp's own "framebuffer readback" test is
 * mock-based too). That path is proven by running the actual application
 * and reading back pixels -- see design/decode-index-spike.md's SVG
 * addendum. This file proves the CPU/SwCanvas fallback instead, using
 * MockRenderDevice the same way tests/lib/canvas_image.cpp's RecordingDevice
 * does, with real ThorVG rasterization underneath the mock.
 */
#include <gleditor/svg_cache.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include <gmock/gmock.h>

#include "mocks/device.hpp"

using testing::NiceMock;
using testing::Return;

namespace gleditor {
namespace {

std::vector<std::uint8_t> readFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

#ifdef GLEDITOR_HAVE_SVG_THORVG

/// Reports Backend::Vulkan (so SvgCache always takes the CPU/SwCanvas path,
/// never the GL one) and records every uploaded texture's bytes, keyed by
/// handle id, the same recording role tests/lib/canvas_image.cpp's
/// RecordingDevice plays for Canvas's own buffers.
class RecordingDevice : public NiceMock<MockRenderDevice> {
public:
  std::map<std::uint32_t, std::vector<std::byte>> uploads;
  std::uint32_t nextHandleId{1};
  int createCount{0};
  int destroyCount{0};

  RecordingDevice() {
    ON_CALL(*this, backend).WillByDefault(Return(render::Backend::Vulkan));
    ON_CALL(*this, createTextureArray)
        .WillByDefault([this](int, int, render::TextureFormat, int) {
          ++createCount;
          return render::TextureHandle{nextHandleId++};
        });
    ON_CALL(*this, destroyTexture)
        .WillByDefault([this](const render::TextureHandle handle) {
          ++destroyCount;
          uploads.erase(handle.id);
        });
    ON_CALL(*this, updateTextureLayer)
        .WillByDefault([this](const render::TextureHandle handle, int, int, int,
                              int, int, const std::span<const std::byte> data) {
          uploads[handle.id].assign(data.begin(), data.end());
        });
  }
};

/// One quadrant-plus-circle pixel read back from a RecordingDevice's upload,
/// as RGBA8.
struct Rgba {
  std::uint8_t r, g, b, a;
};

Rgba pixelAt(const std::vector<std::byte> &rgba, const int width, const int x,
             const int y) {
  const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
  return Rgba{
      static_cast<std::uint8_t>(rgba[offset + 0]),
      static_cast<std::uint8_t>(rgba[offset + 1]),
      static_cast<std::uint8_t>(rgba[offset + 2]),
      static_cast<std::uint8_t>(rgba[offset + 3]),
  };
}

#endif // GLEDITOR_HAVE_SVG_THORVG

} // namespace

TEST(SvgCacheTest, PeekSizeFailsOnEmptyBytes) {
  EXPECT_FALSE(SvgCache::peekSize({}).has_value());
}

TEST(SvgCacheTest, PeekSizeFailsOnGarbageBytes) {
  const std::vector<std::uint8_t> notSvg{'n', 'o', 't', ' ', 's', 'v', 'g'};
  EXPECT_FALSE(SvgCache::peekSize(notSvg).has_value());
}

#ifdef GLEDITOR_HAVE_SVG_THORVG

TEST(SvgCacheTest, PeekSizeReturnsIntrinsicDimensions) {
  const auto bytes = readFile("tests/samples/sample_image.svg");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  const auto size = SvgCache::peekSize(bytes);
  ASSERT_TRUE(size.has_value());
  EXPECT_FLOAT_EQ(size->first, 128.0F);
  EXPECT_FLOAT_EQ(size->second, 128.0F);
}

TEST(SvgCacheTest, LoadBufferFailsOnGarbageBytes) {
  RecordingDevice device;
  SvgCache cache(&device);
  const std::vector<std::uint8_t> notSvg{'n', 'o', 't', ' ', 's', 'v', 'g'};
  EXPECT_FALSE(cache.loadBuffer("bad", notSvg).has_value());
  // Nothing should have been left allocated: either no texture was ever
  // created, or one was created and then destroyed on the failure path.
  EXPECT_EQ(device.createCount, device.destroyCount);
}

/// The real correctness test: rasterizes tests/samples/sample_image.svg on
/// the CPU path (RecordingDevice reports Vulkan, so SvgCache never even
/// tries the GL path) and checks the uploaded RGBA bytes against pixels
/// read back from an independent, standalone ThorVG SwCanvas render of the
/// same fixture -- confirmed empirically before writing this test: row 0 is
/// the SVG's own top row, and (10,10)/(117,10)/(10,117)/(117,117)/(64,64)
/// land in the red/green/blue/gold quadrants and the white centre circle
/// respectively, matching the fixture's own colours (#dc2626, #22c55e,
/// #3b82f6, #facc15, white).
TEST(SvgCacheTest, LoadBufferRasterizesViaSwCanvasWithCorrectPixels) {
  const auto bytes = readFile("tests/samples/sample_image.svg");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  RecordingDevice device;
  SvgCache cache(&device);
  const auto resource = cache.loadBuffer("quadrants", bytes);
  ASSERT_TRUE(resource.has_value());
  EXPECT_TRUE(resource->valid());
  EXPECT_EQ(resource->width, 128);
  EXPECT_EQ(resource->height, 128);
  EXPECT_EQ(resource->layer, 0);

  const auto it = device.uploads.find(resource->texture.id);
  ASSERT_NE(it, device.uploads.end());
  const auto &rgba = it->second;
  ASSERT_EQ(rgba.size(), static_cast<std::size_t>(128 * 128 * 4));

  const auto topLeft     = pixelAt(rgba, 128, 10, 10);
  const auto topRight    = pixelAt(rgba, 128, 117, 10);
  const auto bottomLeft  = pixelAt(rgba, 128, 10, 117);
  const auto bottomRight = pixelAt(rgba, 128, 117, 117);
  const auto centre      = pixelAt(rgba, 128, 64, 64);

  EXPECT_EQ(topLeft.r, 220);
  EXPECT_EQ(topLeft.g, 38);
  EXPECT_EQ(topLeft.b, 38);
  EXPECT_EQ(topRight.r, 34);
  EXPECT_EQ(topRight.g, 197);
  EXPECT_EQ(topRight.b, 94);
  EXPECT_EQ(bottomLeft.r, 59);
  EXPECT_EQ(bottomLeft.g, 130);
  EXPECT_EQ(bottomLeft.b, 246);
  EXPECT_EQ(bottomRight.r, 250);
  EXPECT_EQ(bottomRight.g, 204);
  EXPECT_EQ(bottomRight.b, 21);
  EXPECT_EQ(centre.r, 255);
  EXPECT_EQ(centre.g, 255);
  EXPECT_EQ(centre.b, 255);
  EXPECT_EQ(centre.a, 255);
}

TEST(SvgCacheTest, LoadBufferCachesByIdAndRasterizesOnlyOnce) {
  const auto bytes = readFile("tests/samples/sample_image.svg");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  RecordingDevice device;
  SvgCache cache(&device);
  const auto first  = cache.loadBuffer("quadrants", bytes);
  const auto second = cache.loadBuffer("quadrants", bytes);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->texture, second->texture);
  EXPECT_EQ(device.createCount, 1);
}

TEST(SvgCacheTest, FindReturnsWhatLoadBufferCached) {
  const auto bytes = readFile("tests/samples/sample_image.svg");
  ASSERT_FALSE(bytes.empty()) << "fixture missing";

  RecordingDevice device;
  SvgCache cache(&device);
  EXPECT_FALSE(cache.find("quadrants").has_value());
  const auto loaded = cache.loadBuffer("quadrants", bytes);
  ASSERT_TRUE(loaded.has_value());
  const auto found = cache.find("quadrants");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->texture, loaded->texture);
}

#endif // GLEDITOR_HAVE_SVG_THORVG

} // namespace gleditor
