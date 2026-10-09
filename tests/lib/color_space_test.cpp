#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include <gleditor/color_space.hpp>

namespace {

namespace color = gleditor::color;

struct Primary {
  std::uint32_t rgba;
  color::Oklab oklab;
};

// Ottosson's own values for the sRGB primaries and white (also the CSS Color
// 4 reference implementation's).
constexpr std::array kPrimaries{
    Primary{0xFF0000FFU, {0.627955, 0.224863, 0.125846}},
    Primary{0x00FF00FFU, {0.866440, -0.233888, 0.179498}},
    Primary{0x0000FFFFU, {0.452014, -0.032457, -0.311528}},
    Primary{0xFFFFFFFFU, {1.0, 0.0, 0.0}},
};

TEST(ColorSpaceTest, OklabOfThePrimariesMatchesTheReference) {
  for (const auto &primary : kPrimaries) {
    const auto lab =
        color::oklabFromLinear(color::linearFromRgba(primary.rgba));
    EXPECT_NEAR(lab.l, primary.oklab.l, 1e-5) << std::hex << primary.rgba;
    EXPECT_NEAR(lab.a, primary.oklab.a, 1e-5) << std::hex << primary.rgba;
    EXPECT_NEAR(lab.b, primary.oklab.b, 1e-5) << std::hex << primary.rgba;
  }
}

TEST(ColorSpaceTest, OklchRoundTripsEveryEightBitColourItWasGiven) {
  for (std::uint32_t r = 0; r < 256; r += 17) {
    for (std::uint32_t g = 0; g < 256; g += 17) {
      for (std::uint32_t b = 0; b < 256; b += 17) {
        const std::uint32_t rgba = (r << 24U) | (g << 16U) | (b << 8U) | 0xFFU;
        const auto lch           = color::oklchFromRgba(rgba);
        EXPECT_GE(lch.h, 0.0);
        EXPECT_LT(lch.h, 360.0);
        const auto back = color::linearFromOklch(lch);
        EXPECT_TRUE(color::inGamut(back, 1e-6));
        EXPECT_EQ(color::rgbaFromLinear(back), rgba);
      }
    }
  }
}

TEST(ColorSpaceTest, HighChromaFallsOutOfGamut) {
  EXPECT_TRUE(
      color::inGamut(color::linearFromOklch({.l = 0.75, .c = 0.05, .h = 200})));
  EXPECT_FALSE(
      color::inGamut(color::linearFromOklch({.l = 0.75, .c = 0.35, .h = 200})));
}

TEST(ColorSpaceTest, AlphaIsKeptOutOfTheColour) {
  EXPECT_EQ(color::rgbaFromLinear(color::linearFromRgba(0x336699FFU), 0x40U),
            0x33669940U);
  EXPECT_DOUBLE_EQ(color::ciede2000(0x33669900U, 0x336699FFU), 0.0);
}

TEST(ColorSpaceTest, CieLabOfRedMatchesTheReference) {
  const auto lab = color::cieLabFromRgba(0xFF0000FFU);
  EXPECT_NEAR(lab.l, 53.2408, 1e-3);
  EXPECT_NEAR(lab.a, 80.0925, 1e-3);
  EXPECT_NEAR(lab.b, 67.2032, 1e-3);
}

struct Pair {
  color::CieLab one;
  color::CieLab two;
  double expected;
};

// From the 34 pairs Sharma, Wu and Dalal (2005) publish to test an
// implementation: the hue-angle, achromatic and near-black cases among them.
constexpr std::array kSharmaPairs{
    Pair{{50.0, 2.6772, -79.7751}, {50.0, 0.0, -82.7485}, 2.0425},
    Pair{{50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}, 2.3669},
    Pair{{50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}, 27.1492},
    Pair{{60.2574, -34.0099, 36.2677}, {60.4626, -34.1751, 39.4387}, 1.2644},
    Pair{{2.0776, 0.0795, -1.1350}, {0.9033, -0.0636, -0.5514}, 0.9082},
    Pair{{50.0, 2.49, -0.001}, {50.0, -2.49, 0.0011}, 7.2195},
};

TEST(ColorSpaceTest, Ciede2000MatchesSharmasPairs) {
  for (const auto &pair : kSharmaPairs) {
    EXPECT_NEAR(color::ciede2000(pair.one, pair.two), pair.expected, 1e-4);
    EXPECT_NEAR(color::ciede2000(pair.two, pair.one), pair.expected, 1e-4);
  }
}

} // namespace
