#include <gleditor/glyphcache/lane.hpp>  // for GlyphLane
#include <gleditor/glyphcache/types.hpp> // for Rect, Point
#include <gtest/gtest.h>                 // for Test, TestInfo (ptr only)
// #include <spdlog/spdlog-inl.h>
#include <stdexcept> // for invalid_argument
#include <tuple>     // for tuple
#include <utility>   // for to_underlying
#include <vector>    // for vector

#include <gmock/gmock.h> // for ElementsAreMatcher, Element...
#include <gtest/gtest.h> // for Message, AssertionResult

using namespace gleditor;

using testing::ElementsAre;
using testing::WhenSorted;

// std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();

TEST(GlyphLane, equality) {
  GlyphLane alice(Offset{1}, Rect{Length{0}, Length{1}});
  GlyphLane bob(Offset{0}, Rect{Length{1}, Length{1}});
  EXPECT_EQ(alice, bob) << "GlyphLane should equal GlyphLane";
}

TEST(GlyphLane, less) {
  GlyphLane alice(Offset{1}, Rect{Length{0}, Length{0}});
  GlyphLane bob(Offset{0}, Rect{Length{1}, Length{1}});
  EXPECT_LT(alice, bob) << "GlyphLane alice should be less than GlyphLane bob";
}

TEST(GlyphLane, greater) {
  GlyphLane alice(Offset{1}, Rect{Length{0}, Length{9999}});
  GlyphLane bob(Offset{0}, Rect{Length{1}, Length{1}});
  EXPECT_GT(alice, bob)
      << "GlyphLane alice should be greater than GlyphLane bob";
}

TEST(GlyphLane, ordering) {
  GlyphLane alice(Offset{1}, Rect{Length{0}, Length{9999}});
  GlyphLane bob(Offset{0}, Rect{Length{1}, Length{1}});
  GlyphLane carl(Offset{888}, Rect{Length{1}, Length{8}});
  std::vector<GlyphLane> lanes = {alice, bob, carl};
  EXPECT_THAT(lanes, WhenSorted(ElementsAre(bob, carl, alice)));
}

TEST(GlyphLane, availWidth) {
  const auto width   = Length{100};
  const int putWidth = 10;
  GlyphLane alice(Offset{0}, Rect{width, Length{100}});
  GlyphLane bob(Offset{0}, Rect{width, Length{100}});
  const auto point = bob.put(Length{putWidth});
  // The lane gives up what the glyph took rounded to the next mip block, not
  // what it asked for: the glyph after it has to begin on a block boundary.
  const auto availWidth =
      Length{std::to_underlying(width) - alignedToMipBlock(putWidth)};
  EXPECT_EQ(alice.availWidth(), width);
  EXPECT_EQ(bob.availWidth(), availWidth);
  EXPECT_EQ(point, (Point{Offset{0}, Offset{0}}));
}

TEST(GlyphLane, putAtZero) {
  GlyphLane alice(Offset{0}, Rect{Length{100}, Length{100}});
  EXPECT_EQ(alice.put(Length{10}), (Point{Offset{0}, Offset{0}}));
  EXPECT_EQ(alice.availWidth(), Length{100 - alignedToMipBlock(10)});
}

TEST(GlyphLane, putAt10) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_EQ(alice.put(Length{10}), (Point{Offset{0}, Offset{10}}));
  EXPECT_EQ(alice.put(Length{10}),
            (Point{Offset{alignedToMipBlock(10)}, Offset{10}}));
  EXPECT_EQ(alice.availWidth(), Length{100 - (2 * alignedToMipBlock(10))});
}

// A glyph as wide as the lane fills it: what the rounding would have taken
// past the end is not space anything could have been put in anyway, and a
// lane reporting a negative amount of room left is a lane nothing can reason
// about.
TEST(GlyphLane, putMaxSize) {
  const int width = 100;
  GlyphLane alice(Offset{10}, Rect{Length{width}, Length{100}});
  EXPECT_EQ(alice.put(Length{width}), (Point{Offset{0}, Offset{10}}));
  EXPECT_EQ(alice.availWidth(), Length{0});
}

// Every glyph begins on a mip block boundary, whatever widths came before it.
// The mip chain averages 2^L blocks on the texture's own grid, so a glyph
// starting anywhere else has its own texels averaged together at whatever
// phase its neighbours happened to leave -- and the same letter then minifies
// to different pixels depending only on where it landed. See
// gleditor::glyphAlignment.
TEST(GlyphLane, everyGlyphStartsOnAMipBlockBoundary) {
  GlyphLane lane(Offset{0}, Rect{Length{4096}, Length{100}});
  // Deliberately awkward widths: primes, one, and exact multiples.
  for (const int width : {1, 3, 7, 8, 13, 16, 17, 29, 31, 32, 41}) {
    const auto placed = lane.put(Length{width});
    EXPECT_EQ(std::to_underlying(placed.x) % glyphAlignment, 0)
        << "a glyph " << width
        << " wide was placed at x = " << std::to_underlying(placed.x)
        << ", which no mip block starts at";
  }
}

TEST(GlyphLane, putOverMaxSize) {
  const int width = 100;
  GlyphLane alice(Offset{10}, Rect{Length{width}, Length{100}});
  EXPECT_THROW(alice.put(Length{width * 2}), std::invalid_argument);
}

TEST(GlyphLane, canFit) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, Length{10}}));
}

TEST(GlyphLane, canFitJustWide) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_TRUE(alice.canFit(Rect{Length{100}, Length{10}}));
}

TEST(GlyphLane, canFitJustTall) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, Length{100}}));
}

TEST(GlyphLane, canFitJust) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_TRUE(alice.canFit(Rect{Length{100}, Length{100}}));
}

TEST(GlyphLane, canFitTooWide) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_FALSE(alice.canFit(Rect{Length{1000}, Length{10}}));
}

TEST(GlyphLane, canFitTooTall) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_FALSE(alice.canFit(Rect{Length{10}, Length{1000}}));
}

TEST(GlyphLane, canFitTooTallAndWide) {
  GlyphLane alice(Offset{10}, Rect{Length{100}, Length{100}});
  EXPECT_FALSE(alice.canFit(Rect{Length{1000}, Length{1000}}));
}
