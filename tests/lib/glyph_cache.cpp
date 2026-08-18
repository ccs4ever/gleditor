#include <gtest/gtest.h>

#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/render/types.hpp>

#include <gleditor/text/font.hpp>
#include <gmock/gmock.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mocks/device.hpp"

using testing::NiceMock;
using testing::Return;

/**
 * @brief GlyphCache against a mocked device with a deliberately small atlas.
 *
 * Real documents no longer make the atlas grow -- the whole of the King James
 * Bible packs into one 512x512 layer -- so growth has to be provoked here or it
 * is not covered at all. The device is a mock reporting whatever limits a test
 * asks for, and the glyphs are rasterised for real, since Pango and Cairo need
 * no graphics device.
 */
class GlyphCacheTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  /// Every size createTextureArray() was asked for, in order.
  std::vector<std::pair<int, int>> allocations;
  /// Textures handed out, so a test can tell a fresh one from a reused one.
  int nextTexture{};
  int uploads{};

  void SetUp() override {}

  /// Build a cache over a device reporting @p maxSize / @p maxLayers.
  std::unique_ptr<GlyphCache> makeCache(const int maxSize,
                                        const int maxLayers) {
    device      = std::make_unique<NiceMock<MockRenderDevice>>();
    nextTexture = 0;
    uploads     = 0;
    allocations.clear();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{maxSize, maxLayers}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault([this](const int size, const int layers,
                              render::TextureFormat, int) {
          allocations.emplace_back(size, layers);
          return render::TextureHandle{
              static_cast<std::uint32_t>(++nextTexture)};
        });
    ON_CALL(*device,
            updateTextureLayer(testing::_, testing::_, testing::_, testing::_,
                               testing::_, testing::_, testing::_))
        .WillByDefault([this](render::TextureHandle, int, int, int, int, int,
                              std::span<const std::byte>) { uploads++; });
    return std::make_unique<GlyphCache>(device.get());
  }

  /// Load a font at @p spec, e.g. "Serif 200". Big sizes are how a handful of
  /// glyphs can fill an atlas that a document's worth of text does not.
  static FontPtr font(const std::string &spec) {
    return gleditor::text::FontManager::instance().getFont(spec);
  }

  /// Distinct clusters, so that each one is rasterised rather than found in
  /// the cache.
  static std::vector<std::string> alphabet(const std::size_t count) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; i++) {
      out.push_back(std::string(1, static_cast<char>('A' + (i % 26))) +
                    std::string(i / 26, '\''));
    }
    return out;
  }
};

TEST_F(GlyphCacheTest, opensSmallerThanTheHardwareAllows) {
  const auto cache = makeCache(16384, 2048);
  EXPECT_LT(cache->atlasSize(), cache->atlasMaxSize());
  EXPECT_EQ(cache->atlasLayers(), 1);
  ASSERT_EQ(allocations.size(), 1U);
  EXPECT_EQ(allocations.front().first, cache->atlasSize());
}

// A device that cannot make the opening allocation must not be asked to. The
// opening size is a preference; the reported limit is not.
TEST_F(GlyphCacheTest, opensNoLargerThanTheHardwareAllows) {
  const auto cache = makeCache(256, 4);
  EXPECT_EQ(cache->atlasSize(), 256);
  EXPECT_EQ(cache->atlasMaxSize(), 256);
  ASSERT_EQ(allocations.size(), 1U);
  EXPECT_EQ(allocations.front(), std::make_pair(256, 1));
}

// The encoding, not the driver, is what bounds the layer count: the layer index
// reaches the shader in six bits.
TEST_F(GlyphCacheTest, neverPromisesMoreLayersThanAVertexCanName) {
  const auto cache = makeCache(16384, 2048);
  EXPECT_EQ(cache->atlasMaxLayers(), GlyphCache::maxEncodableLayers);
}

TEST_F(GlyphCacheTest, growsTheLayerBeforeAddingAnother) {
  const auto cache = makeCache(4096, 8);
  const auto face  = font("Serif 150");
  const auto start = cache->atlasSize();

  for (const auto &chr : alphabet(12)) {
    cache->put(chr, face);
  }

  EXPECT_GT(cache->atlasSize(), start)
      << "a dozen 150-point glyphs should not fit in the opening atlas";
  EXPECT_EQ(cache->atlasLayers(), 1)
      << "a second layer costs as much as doubling the first and buys half as "
         "much room";
}

TEST_F(GlyphCacheTest, addsLayersOnceTheLayerCannotGrow) {
  // maxSize equals the opening size, so growing sideways is not an option and
  // layers are the only room left.
  const auto cache = makeCache(512, 8);
  const auto face  = font("Serif 150");

  for (const auto &chr : alphabet(16)) {
    cache->put(chr, face);
  }

  EXPECT_EQ(cache->atlasSize(), 512);
  EXPECT_GT(cache->atlasLayers(), 1);
  EXPECT_LE(cache->atlasLayers(), 8);
}

TEST_F(GlyphCacheTest, everyAllocationStaysWithinTheReportedLimits) {
  const auto cache = makeCache(1024, 4);
  const auto face  = font("Serif 150");

  for (const auto &chr : alphabet(30)) {
    cache->put(chr, face);
  }

  for (const auto &[size, layers] : allocations) {
    EXPECT_LE(size, 1024);
    EXPECT_LE(layers, 4);
  }
}

// The texture coordinates of a glyph are already sitting in the vertex buffer
// of every page that drew it, so growth must leave them alone. Texels rather
// than a fraction of the texture is what makes that possible.
TEST_F(GlyphCacheTest, growingLeavesEarlierGlyphsWhereTheyWere) {
  const auto cache = makeCache(4096, 8);
  const auto face  = font("Serif 150");

  const auto first      = cache->put("A", face);
  const auto sizeBefore = cache->atlasSize();

  for (const auto &chr : alphabet(20)) {
    cache->put(chr, face);
  }
  ASSERT_GT(cache->atlasSize(), sizeBefore) << "the atlas did not grow";

  const auto again = cache->put("A", face);
  EXPECT_EQ(again.texCoords.topLeft.x, first.texCoords.topLeft.x);
  EXPECT_EQ(again.texCoords.topLeft.y, first.texCoords.topLeft.y);
  EXPECT_EQ(again.texCoords.box.width, first.texCoords.box.width);
  EXPECT_EQ(again.texCoords.box.height, first.texCoords.box.height);
  EXPECT_EQ(again.layer, first.layer);
}

// A new texture object starts empty, so a grown atlas is only correct if every
// glyph already packed is written into it again.
TEST_F(GlyphCacheTest, growingWritesEveryGlyphIntoTheNewTexture) {
  const auto cache = makeCache(4096, 8);
  const auto face  = font("Serif 150");

  const auto glyphs = alphabet(12);
  for (const auto &chr : glyphs) {
    cache->put(chr, face);
  }

  ASSERT_GT(allocations.size(), 1U) << "the atlas did not grow";
  EXPECT_GT(uploads, static_cast<int>(glyphs.size()))
      << "growth re-uploaded nothing";
}

TEST_F(GlyphCacheTest, refusesAGlyphNoAtlasCouldEverHold) {
  const auto cache = makeCache(64, 2);
  EXPECT_THROW(cache->put("W", font("Serif 400")), std::overflow_error);
}

// Failing to place one glyph must not take the cache with it: the atlas is
// still the size it grew to, and the next glyph still packs.
TEST_F(GlyphCacheTest, survivesAGlyphItCannotHold) {
  const auto cache = makeCache(256, 2);
  const auto small = font("Serif 12");

  EXPECT_THROW(cache->put("W", font("Serif 400")), std::overflow_error);
  EXPECT_NO_THROW(cache->put("a", small));
  EXPECT_LE(cache->atlasSize(), 256);
}
