//#include <fuzztest/fuzztest.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

#include "../src/glyphcache/glyphpalette.hpp"
#include "../src/log.hpp"
#include "mocks/GL.hpp"

using testing::ElementsAre;
using testing::WhenSorted;

class GlyphPaletteTest : public testing::Test, public Loggable {
protected:
  std::shared_ptr<GL> gl;
  std::shared_ptr<testing::NiceMock<GLMock>> glMock;
  Rect dims{Length{1024}, Length{1024}};
  ~GlyphPaletteTest() override = default;

  void SetUp() override {
    glMock = std::make_shared<testing::NiceMock<GLMock>>();
    gl     = std::static_pointer_cast<GL>(glMock);
  }
};

// just to make coverage happy
TEST_F(GlyphPaletteTest, moveSelfAssignment) {
  GlyphPalette alice(dims, gl);
  alice = std::move(alice); // NOLINT
  EXPECT_EQ(alice, alice);
}

TEST_F(GlyphPaletteTest, equality) {
  GlyphPalette alice(dims, gl);
  GlyphPalette bob(dims, gl);
  EXPECT_EQ(alice, bob) << "GlyphPalette should equal GlyphPalette";
}

TEST_F(GlyphPaletteTest, less) {
  EXPECT_CALL(*glMock, texSubImage3D).Times(1);
  GlyphPalette alice(dims, gl);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  GlyphPalette bob(dims, gl);
  EXPECT_LT(alice, bob)
      << "alice should sort first as it has the least available height";
}

TEST_F(GlyphPaletteTest, greater) {
  EXPECT_CALL(*glMock, texSubImage3D).Times(1);
  GlyphPalette alice(dims, gl);
  GlyphPalette bob(dims, gl);
  bob.put(Rect{Length{10}, Length{10}}, nullptr);
  EXPECT_GT(alice, bob)
      << "alice should sort last as it has the most available height";
}

TEST_F(GlyphPaletteTest, ordering) {
  EXPECT_CALL(*glMock, texSubImage3D).Times(3);
  GlyphPalette alice(dims, gl);
  GlyphPalette bob(dims, gl);
  bob.put(Rect{Length{10}, Length{10}}, nullptr);
  bob.put(Rect{Length{10}, Length{20}},
          nullptr); // height 20 char to force into a new lane
  GlyphPalette carl(dims, gl);
  carl.put(Rect{Length{10}, Length{10}}, nullptr);
  std::vector<GlyphPalette> pals = {alice, bob, carl};
  EXPECT_THAT(pals, WhenSorted(ElementsAre(bob, carl, alice)));
}

TEST_F(GlyphPaletteTest, availHeight) {
  GlyphPalette alice(dims, gl);
  EXPECT_EQ(alice.availHeight(), dims.height);
}

TEST_F(GlyphPaletteTest, availHeightPut1Lane) {
  GlyphPalette alice(dims, gl);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 10});
}

TEST_F(GlyphPaletteTest, availHeightPut2Lanes) {
  GlyphPalette alice(dims, gl);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  alice.put(Rect{Length{10}, Length{10}}, nullptr);
  alice.put(Rect{Length{10}, Length{20}}, nullptr);
  alice.put(Rect{Length{10}, Length{20}}, nullptr);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 30});
}

TEST_F(GlyphPaletteTest, availHeightFullLane) {
  GlyphPalette alice(dims, gl);
  alice.put(Rect{dims.width, Length{10}}, nullptr);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 10});
}

TEST_F(GlyphPaletteTest, availHeightLaneExpand) {
  EXPECT_CALL(*glMock, texSubImage3D).Times(2);
  GlyphPalette alice(dims, gl);
  EXPECT_NE(alice.put(Rect{dims.width, Length{10}}, nullptr), std::nullopt);
  EXPECT_NE(alice.put(Rect{dims.width, Length{10}}, nullptr), std::nullopt);
  EXPECT_EQ(alice.availHeight(), Length{std::to_underlying(dims.height) - 20})
      << alice;
}

TEST_F(GlyphPaletteTest, canFit) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, Length{10}})) << alice;
}

TEST_F(GlyphPaletteTest, canFitNewLane) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, Length{10}})) << alice;
  EXPECT_TRUE(alice.put(
      Rect{Length{10}, Length{std::to_underlying(dims.height) - 10}}, nullptr))
      << alice;
  ASSERT_TRUE(alice.canFit(Rect{Length{10}, Length{11}})) << alice;
  EXPECT_TRUE(alice.put(Rect{Length{10}, Length{11}}, nullptr)) << alice;
}

TEST_F(GlyphPaletteTest, canFitMaxHeight) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(alice.canFit(Rect{Length{10}, dims.height})) << alice;
}

TEST_F(GlyphPaletteTest, canFitMaxWidth) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(alice.canFit(Rect{dims.width, Length{10}})) << alice;
}

TEST_F(GlyphPaletteTest, canFitMax) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(alice.canFit(dims)) << alice;
}

TEST_F(GlyphPaletteTest, cannotFitTooWide) {
  GlyphPalette alice(dims, gl);
  EXPECT_FALSE(alice.canFit(
      Rect{Length{std::to_underlying(dims.width) + 1}, Length{10}}))
      << alice;
}

TEST_F(GlyphPaletteTest, cannotFitTooTall) {
  GlyphPalette alice(dims, gl);
  EXPECT_FALSE(alice.canFit(
      Rect{Length{10}, Length{std::to_underlying(dims.height) + 1}}))
      << alice;
}

TEST_F(GlyphPaletteTest, cannotFitTooWideTall) {
  GlyphPalette alice(dims, gl);
  EXPECT_FALSE(alice.canFit(Rect{Length{std::to_underlying(dims.width) + 1},
                                 Length{std::to_underlying(dims.height) + 1}}))
      << alice;
}

TEST_F(GlyphPaletteTest, cannotFit2MaxLanes) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(alice.canFit(dims)) << alice;
  EXPECT_NE(alice.put(dims, nullptr), std::nullopt);
  EXPECT_FALSE(alice.canFit(dims)) << alice;
  EXPECT_EQ(alice.put(dims, nullptr), std::nullopt);
  EXPECT_FALSE(alice.canFit(dims)) << alice;
}

TEST_F(GlyphPaletteTest, cannotFitInLaneOrMoreLanes) {
  GlyphPalette alice(dims, gl);
  EXPECT_TRUE(
      alice.canFit(Rect{Length{std::to_underlying(dims.width) / 2},
                        Length{std::to_underlying(dims.height) / 2 + 1}}))
      << alice;
  EXPECT_NE(alice.put(Rect{Length{std::to_underlying(dims.width) / 2},
                           Length{std::to_underlying(dims.height) / 2 + 1}},
                      nullptr),
            std::nullopt)
      << alice;
  logger->info("alice put1: {}", alice);
  EXPECT_FALSE(
      alice.canFit(Rect{Length{std::to_underlying(dims.width) / 2 + 1},
                        Length{std::to_underlying(dims.height) / 2 + 1}}))
      << alice;
  EXPECT_EQ(alice.put(Rect{Length{std::to_underlying(dims.width) / 2 + 1},
                           Length{std::to_underlying(dims.height) / 2 + 1}},
                      nullptr),
            std::nullopt)
      << alice;
  logger->info("alice put2: {}", alice);
}

TEST_F(GlyphPaletteTest, toString) {
  GlyphPalette alice(dims, gl);
  std::stringstream str;
  str << alice;
  EXPECT_NE(str.str(), "");
}

/*
 * Get google fuzztest working someday
 * void hasNoUndefBehavior(int width, int height) {
  auto glMock = std::static_pointer_cast<GL>(
      std::make_shared<testing::NiceMock<GLMock>>());
  GlyphPalette alice(Rect{Length{width}, Length{height}}, glMock);
}

FUZZ_TEST(GlyphPaletteTest, hasNoUndefBehavior)
    .WithDomains(fuzztest::Arbitrary<int>(), fuzztest::Arbitrary<int>()); */
