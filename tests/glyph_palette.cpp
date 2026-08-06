// #include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>                    // for TestInfo (ptr only), TEST_F, for AssertionResult, Message
#include <gleditor/glyphcache/palette.hpp>  // for GlyphPalette
#include <gleditor/log.hpp>                 // for operator<<, Loggable
#include <gleditor/glyphcache/types.hpp>    // for Rect
#include <cstddef>                          // for byte
#include <memory>                           // for allocator, shared_ptr
#include <span>                             // for span
#include <optional>                         // for nullopt
#include <sstream>                          // for basic_stringstream, char_...
#include <utility>                          // for to_underlying, move
#include <vector>                           // for vector
#include <tuple>                            // for tuple

#include "mocks/device.hpp"                 // for MockRenderDevice
#include <gmock/gmock.h>                    // for NiceMock, EXPECT_CALL

enum class Length : int;

using testing::ElementsAre;
using testing::WhenSorted;

/// Coverage bytes handed to put(). The palette only forwards them, so any
/// non-empty buffer large enough for the box under test will do.
static const std::vector<std::byte> kPixels(1024UL * 1024UL, std::byte{0xFF});

class GlyphPaletteTest : public testing::Test, public Loggable {
protected:
  std::unique_ptr<testing::NiceMock<MockRenderDevice>> device;
  render::TextureHandle texture{1};
  Rect dims{Length{1024}, Length{1024}};
  ~GlyphPaletteTest() override = default;

  void SetUp() override {
    device = std::make_unique<testing::NiceMock<MockRenderDevice>>();
  }

  /// Build a palette on the fixture's device, layer 0.
  GlyphPalette makePalette() { return {dims, device.get(), texture, 0}; }
};

// just to make coverage happy
TEST_F(GlyphPaletteTest, moveSelfAssignment) {
  GlyphPalette alice = makePalette();
  // deliberately exercising the self-assignment guard
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
  alice = std::move(alice); // NOLINT
#pragma clang diagnostic pop
  EXPECT_EQ(alice, alice);
}

// Moving must carry the device and layer across, otherwise the moved-to
// palette keeps uploading through whichever device it happened to be built
// with -- and into the wrong array layer.
TEST_F(GlyphPaletteTest, moveAssignmentKeepsDeviceOfSource) {
  testing::NiceMock<MockRenderDevice> otherDevice;
  EXPECT_CALL(otherDevice, updateTextureLayer).Times(1);
  EXPECT_CALL(*device, updateTextureLayer).Times(0);

  GlyphPalette source(dims, &otherDevice, texture, 3);
  GlyphPalette dest = makePalette();
  dest = std::move(source);
  EXPECT_EQ(dest.layerIndex(), 3);
  dest.put(Rect{Length{10}, Length{10}}, kPixels);
}

TEST_F(GlyphPaletteTest, moveConstructionKeepsDeviceOfSource) {
  testing::NiceMock<MockRenderDevice> otherDevice;
  EXPECT_CALL(otherDevice, updateTextureLayer).Times(1);
  EXPECT_CALL(*device, updateTextureLayer).Times(0);

  GlyphPalette source(dims, &otherDevice, texture, 3);
  GlyphPalette dest(std::move(source));
  EXPECT_EQ(dest.layerIndex(), 3);
  dest.put(Rect{Length{10}, Length{10}}, kPixels);
}

// The layer index is what ties a palette to its slice of the array texture,
// and sorting the palette vector must not shuffle that association.
TEST_F(GlyphPaletteTest, uploadsTargetOwnLayer) {
  GlyphPalette alice(dims, device.get(), texture, 7);
  EXPECT_CALL(*device, updateTextureLayer(texture, 7, testing::_, testing::_,
                                          testing::_, testing::_, testing::_))
      .Times(1);
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
}

TEST_F(GlyphPaletteTest, equality) {
  GlyphPalette alice = makePalette();
  GlyphPalette bob = makePalette();
  EXPECT_EQ(alice, bob) << "GlyphPalette should equal GlyphPalette";
}

TEST_F(GlyphPaletteTest, less) {
  EXPECT_CALL(*device, updateTextureLayer).Times(1);
  GlyphPalette alice = makePalette();
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  GlyphPalette bob = makePalette();
  EXPECT_LT(alice, bob)
      << "alice should sort first as it has the least available height";
}

TEST_F(GlyphPaletteTest, greater) {
  EXPECT_CALL(*device, updateTextureLayer).Times(1);
  GlyphPalette alice = makePalette();
  GlyphPalette bob = makePalette();
  bob.put(Rect{Length{10}, Length{10}}, kPixels);
  EXPECT_GT(alice, bob)
      << "alice should sort last as it has the most available height";
}

TEST_F(GlyphPaletteTest, ordering) {
  EXPECT_CALL(*device, updateTextureLayer).Times(3);
  GlyphPalette alice = makePalette();
  GlyphPalette bob = makePalette();
  bob.put(Rect{Length{10}, Length{10}}, kPixels);
  bob.put(Rect{Length{10}, Length{20}},
          kPixels); // height 20 char to force into a new lane
  GlyphPalette carl = makePalette();
  carl.put(Rect{Length{10}, Length{10}}, kPixels);
  std::vector<GlyphPalette> pals = {alice, bob, carl};
  EXPECT_THAT(pals, WhenSorted(ElementsAre(bob, carl, alice)));
}

TEST_F(GlyphPaletteTest, availHeight) {
  GlyphPalette alice = makePalette();
  EXPECT_EQ(alice.availHeight(), dims.height);
}

TEST_F(GlyphPaletteTest, availHeightPut1Lane) {
  GlyphPalette alice = makePalette();
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 10});
}

TEST_F(GlyphPaletteTest, availHeightPut2Lanes) {
  GlyphPalette alice = makePalette();
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  alice.put(Rect{Length{10}, Length{10}}, kPixels);
  alice.put(Rect{Length{10}, Length{20}}, kPixels);
  alice.put(Rect{Length{10}, Length{20}}, kPixels);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 30});
}

TEST_F(GlyphPaletteTest, availHeightFullLane) {
  GlyphPalette alice = makePalette();
  alice.put(Rect{dims.width, Length{10}}, kPixels);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 10});
}

TEST_F(GlyphPaletteTest, availHeightLaneExpand) {
  EXPECT_CALL(*device, updateTextureLayer).Times(2);
  GlyphPalette alice = makePalette();
  EXPECT_NE(alice.put(Rect{dims.width, Length{10}}, kPixels), std::nullopt);
  EXPECT_NE(alice.put(Rect{dims.width, Length{10}}, kPixels), std::nullopt);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 20})
      << alice;
}

TEST_F(GlyphPaletteTest, canFit) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, Length{10}})) << alice;
}

TEST_F(GlyphPaletteTest, canFitNewLane) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, Length{10}})) << alice;
  EXPECT_TRUE(alice.put(
      Rect{Length{10}, Length{std::to_underlying(dims.height) - 10}}, kPixels))
      << alice;
  ASSERT_TRUE(alice.canFit(Rect{Length{10}, Length{11}})) << alice;
  EXPECT_TRUE(alice.put(Rect{Length{10}, Length{11}}, kPixels)) << alice;
}

TEST_F(GlyphPaletteTest, canFitMaxHeight) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, dims.height})) << alice;
}

TEST_F(GlyphPaletteTest, canFitMaxWidth) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(alice.canFit(Rect{dims.width, Length{10}})) << alice;
}

TEST_F(GlyphPaletteTest, canFitMax) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(alice.canFit(dims)) << alice;
}

TEST_F(GlyphPaletteTest, cannotFitTooWide) {
  GlyphPalette alice = makePalette();
  EXPECT_FALSE(alice.canFit(
      Rect{Length{std::to_underlying(dims.width) + 1}, Length{10}}))
      << alice;
}

TEST_F(GlyphPaletteTest, cannotFitTooTall) {
  GlyphPalette alice = makePalette();
  EXPECT_FALSE(alice.canFit(
      Rect{Length{10}, Length{std::to_underlying(dims.height) + 1}}))
      << alice;
}

TEST_F(GlyphPaletteTest, cannotFitTooWideTall) {
  GlyphPalette alice = makePalette();
  EXPECT_FALSE(alice.canFit(Rect{Length{std::to_underlying(dims.width) + 1},
                                 Length{std::to_underlying(dims.height) + 1}}))
      << alice;
}

TEST_F(GlyphPaletteTest, cannotFit2MaxLanes) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(alice.canFit(dims)) << alice;
  EXPECT_NE(alice.put(dims, kPixels), std::nullopt);
  EXPECT_FALSE(alice.canFit(dims)) << alice;
  EXPECT_EQ(alice.put(dims, kPixels), std::nullopt);
  EXPECT_FALSE(alice.canFit(dims)) << alice;
}

TEST_F(GlyphPaletteTest, cannotFitInLaneOrMoreLanes) {
  GlyphPalette alice = makePalette();
  EXPECT_TRUE(
      alice.canFit(Rect{Length{std::to_underlying(dims.width) / 2},
                        Length{std::to_underlying(dims.height) / 2 + 1}}))
      << alice;
  EXPECT_NE(alice.put(Rect{Length{std::to_underlying(dims.width) / 2},
                           Length{std::to_underlying(dims.height) / 2 + 1}},
                      kPixels),
            std::nullopt)
      << alice;
  EXPECT_FALSE(
      alice.canFit(Rect{Length{std::to_underlying(dims.width) / 2 + 1},
                        Length{std::to_underlying(dims.height) / 2 + 1}}))
      << alice;
  EXPECT_EQ(alice.put(Rect{Length{std::to_underlying(dims.width) / 2 + 1},
                           Length{std::to_underlying(dims.height) / 2 + 1}},
                      kPixels),
            std::nullopt)
      << alice;
}

TEST_F(GlyphPaletteTest, toString) {
  GlyphPalette alice = makePalette();
  std::stringstream str;
  str << alice;
  EXPECT_NE(str.str(), "");
}

// vi: set sw=2 sts=2 ts=2 et:
