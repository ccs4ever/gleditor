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

using namespace gleditor;

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
  /// The coverage bytes of the most recent updateTextureLayer() call, so a
  /// test can tell two rasterisations apart by their actual pixels rather
  /// than only by the mean-ink summary Sizes reports -- two decorations
  /// adding equal-sized bars at different rows still average to the same
  /// ink, so ink alone cannot tell them apart.
  std::vector<std::byte> lastUpload;

  void SetUp() override {}

  /// Build a cache over a device reporting @p maxSize / @p maxLayers.
  std::unique_ptr<GlyphCache> makeCache(const int maxSize,
                                        const int maxLayers) {
    device      = std::make_unique<NiceMock<MockRenderDevice>>();
    nextTexture = 0;
    uploads     = 0;
    allocations.clear();
    lastUpload.clear();
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
                              std::span<const std::byte> data) {
          uploads++;
          lastUpload.assign(data.begin(), data.end());
        });
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

// Every glyph starts on a mip block boundary, whatever order the glyphs were
// asked for in. The chain averages 2^L blocks on the texture's own grid, so a
// glyph that began anywhere else would have its own texels averaged together
// at whatever phase the glyphs before it happened to leave -- and since that
// order follows which of several documents loading at once reached the render
// thread first, the same page rendered differently from one run to the next.
// See gleditor::glyphAlignment.
TEST_F(GlyphCacheTest, everyGlyphLandsOnAMipBlockBoundary) {
  const auto cache = makeCache(4096, 8);
  // Two sizes, so the boxes are of assorted widths and heights and the lanes
  // are not all the same: a run of identical boxes would line up on any
  // alignment at all and prove nothing.
  for (const auto *const name : {"Serif 40", "Serif 90"}) {
    const auto face = font(name);
    for (const auto &chr : alphabet(20)) {
      const auto placed = cache->put(chr, face);
      EXPECT_EQ(static_cast<int>(placed.texCoords.topLeft.x) % glyphAlignment,
                0)
          << chr << " at " << name
          << " starts at x = " << placed.texCoords.topLeft.x
          << ", which no mip block starts at";
      EXPECT_EQ(static_cast<int>(placed.texCoords.topLeft.y) % glyphAlignment,
                0)
          << chr << " at " << name
          << " starts at y = " << placed.texCoords.topLeft.y
          << ", which no mip block starts at";
    }
  }
}

// The border a glyph is rasterised inside has to be a whole number of blocks
// too, or aligning the padded box would leave the glyph itself off the grid --
// the coordinates handed out point past the border, not at it.
TEST(GlyphAtlas, theBorderIsAWholeNumberOfMipBlocks) {
  EXPECT_EQ(glyphPadding % glyphAlignment, 0);
  EXPECT_GE(glyphPadding, 1 << (atlasMipLevels - 1))
      << "the border has to reach as far as the deepest level averages";
  EXPECT_GE(glyphAlignment, 1 << (atlasMipLevels - 1))
      << "a shallower alignment leaves the deepest level cutting across "
         "glyphs";
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

TEST_F(GlyphCacheTest, noDecorationsIsTheDefaultAndItsOwnEntry) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 40");

  const auto plain    = cache->put("a", face);
  const auto uploaded = uploads;
  // The two-argument call and an explicit empty set both mean "no
  // decorations", so this is the same entry: no second rasterisation.
  const auto again = cache->put("a", face, {});

  EXPECT_EQ(uploads, uploaded) << "an explicit empty set should hit the same "
                                  "entry the two-argument call made";
  EXPECT_EQ(plain.layer, again.layer);
  EXPECT_EQ(plain.texCoords.topLeft.x, again.texCoords.topLeft.x);
  EXPECT_EQ(plain.texCoords.topLeft.y, again.texCoords.topLeft.y);
}

TEST_F(GlyphCacheTest, theSameDecorationsAreOneCacheEntry) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 40");

  cache->put("a", face, {Decoration::Bold});
  const auto uploaded = uploads;
  cache->put("a", face, {Decoration::Bold});

  EXPECT_EQ(uploads, uploaded)
      << "asking for the same cluster, font and decorations twice must not "
         "rasterise a second time";
}

TEST_F(GlyphCacheTest, decorationSetMembershipOrderDoesNotMatter) {
  // std::unordered_set has no order to begin with, but the two insertion
  // orders below are worth pinning down: {Bold, Italic} and {Italic, Bold}
  // must be recognised as the same request.
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 40");

  const auto first =
      cache->put("a", face, {Decoration::Bold, Decoration::Italic});
  const auto uploaded = uploads;
  const auto second =
      cache->put("a", face, {Decoration::Italic, Decoration::Bold});

  EXPECT_EQ(uploads, uploaded);
  EXPECT_EQ(first.layer, second.layer);
  EXPECT_EQ(first.texCoords.topLeft.x, second.texCoords.topLeft.x);
}

TEST_F(GlyphCacheTest, differentDecorationsAreDifferentCacheEntries) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 40");

  cache->put("a", face);
  const auto afterPlain = uploads;
  cache->put("a", face, {Decoration::Bold});
  const auto afterBold = uploads;
  cache->put("a", face, {Decoration::Italic});
  const auto afterItalic = uploads;

  EXPECT_GT(afterBold, afterPlain)
      << "bold must rasterise separately from undecorated";
  EXPECT_GT(afterItalic, afterBold)
      << "italic is a third distinct entry, not the same as bold";
}

TEST_F(GlyphCacheTest, aSupersetOfDecorationsIsAnotherEntry) {
  // {Bold} and {Bold, Underline} must not be confused for one another just
  // because one's set contains the other's.
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 40");

  cache->put("a", face, {Decoration::Bold});
  const auto afterBold = uploads;
  cache->put("a", face, {Decoration::Bold, Decoration::Underline});

  EXPECT_GT(uploads, afterBold);
}

// Beyond this point, decorations are checked for actually changing the
// rasterised bitmap -- not just for landing in a separate cache entry, which
// the tests above already cover.

TEST_F(GlyphCacheTest, boldHasMoreInkThanPlain) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  const auto plain = cache->put("A", face);
  const auto bold  = cache->put("A", face, {Decoration::Bold});

  EXPECT_GT(bold.ink, plain.ink)
      << "FT_GlyphSlot_Embolden thickens the outline, which should ink more "
         "of the glyph's own box, not just occupy a different cache slot";
}

TEST_F(GlyphCacheTest, italicChangesTheGlyphsFootprint) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  const auto plain  = cache->put("A", face);
  const auto italic = cache->put("A", face, {Decoration::Italic});

  EXPECT_NE(std::to_underlying(italic.dims.width),
            std::to_underlying(plain.dims.width))
      << "FT_GlyphSlot_Oblique shears the outline, which widens an upright "
         "glyph's bounding box";
}

TEST_F(GlyphCacheTest, underlineAddsInkBelowTheGlyph) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  const auto plain      = cache->put("A", face);
  const auto underlined = cache->put("A", face, {Decoration::Underline});

  EXPECT_GT(underlined.ink, plain.ink);
}

TEST_F(GlyphCacheTest, overlineAddsInkAboveTheGlyph) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  const auto plain     = cache->put("A", face);
  const auto overlined = cache->put("A", face, {Decoration::Overline});

  EXPECT_GT(overlined.ink, plain.ink);
}

TEST_F(GlyphCacheTest, strikethroughAddsInkThroughTheGlyph) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  const auto plain  = cache->put("A", face);
  const auto struck = cache->put("A", face, {Decoration::Strikethrough});

  EXPECT_GT(struck.ink, plain.ink);
}

TEST_F(GlyphCacheTest, eachLineDecorationLandsAtADifferentHeight) {
  // Not just "adds ink somewhere" -- underline, overline and strikethrough
  // must not all be drawing over one another at the same row. Bars of equal
  // thickness and width add equal ink regardless of which row they land on,
  // so mean ink cannot tell three different heights apart; the actual
  // uploaded bytes can.
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  cache->put("A", face, {Decoration::Underline});
  const auto underlined = lastUpload;
  cache->put("A", face, {Decoration::Overline});
  const auto overlined = lastUpload;
  cache->put("A", face, {Decoration::Strikethrough});
  const auto struckThrough = lastUpload;

  ASSERT_FALSE(underlined.empty());
  EXPECT_NE(underlined, overlined);
  EXPECT_NE(underlined, struckThrough);
  EXPECT_NE(overlined, struckThrough);
}

TEST_F(GlyphCacheTest, superscriptAndSubscriptRaiseNoDecorationSpecificCode) {
  // Deliberately not tested for a different bitmap: per Decoration's own
  // comment, these name only a reduced rasterisation size, and that size is
  // expected to come from the caller requesting a smaller FontPtr -- the
  // ordinary per-font cache key already handles that -- rather than from
  // anything in addToCache(). This just pins down that passing them does not
  // throw or otherwise misbehave.
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  EXPECT_NO_THROW(cache->put("A", face, {Decoration::Superscript}));
  EXPECT_NO_THROW(cache->put("A", face, {Decoration::Subscript}));
}

// Beyond this point: resolveRealVariant() preferring a genuine bold/italic
// file over synthesising one. Nothing in GlyphCache's public API exposes
// which FT_Face ended up doing the rasterising, so these are necessarily
// indirect -- real hinted weights and synthetic embolden both increase ink,
// so a passing boldHasMoreInkThanPlain-style test does not by itself prove
// which path ran. What is checked here is behaviour a real-vs-synthetic
// switch could plausibly get wrong: that decoration still works at all for a
// family fontconfig actually has a matching file for (real path exercised,
// per fc-match against this test environment's fonts) and for one it does
// not (synthetic fallback exercised), and that requesting a real variant
// never throws even when none exists.

TEST_F(GlyphCacheTest, boldStillIncreasesInkWhenARealBoldFileExists) {
  // Confirmed via `fc-match "Serif:bold"` against this environment: resolves
  // to a genuine Bold file, not a re-served Regular one.
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Serif 60");

  const auto plain = cache->put("A", face);
  const auto bold  = cache->put("A", face, {Decoration::Bold});

  EXPECT_GT(bold.ink, plain.ink);
}

TEST_F(GlyphCacheTest, boldStillIncreasesInkWithNoRealBoldFile) {
  // Confirmed via `fc-match "Z003:bold"` against this environment: fontconfig
  // has no bold weight for this family and answers with its one italic style
  // instead, so this only ever exercises the synthetic embolden fallback.
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Z003 60");

  const auto plain = cache->put("A", face);
  const auto bold  = cache->put("A", face, {Decoration::Bold});

  EXPECT_GT(bold.ink, plain.ink)
      << "a family with no real bold file must still fall back to "
         "FT_GlyphSlot_Embolden rather than silently rendering plain";
}

TEST_F(GlyphCacheTest, aFamilyWithNoBoldFileStillCachesSeparately) {
  const auto cache = makeCache(1024, 2);
  const auto face  = font("Z003 60");

  cache->put("A", face);
  const auto afterPlain = uploads;
  cache->put("A", face, {Decoration::Bold});

  EXPECT_GT(uploads, afterPlain);
}
