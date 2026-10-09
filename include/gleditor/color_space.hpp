/**
 * @file color_space.hpp
 * @brief Perceptual colour spaces and distance: OKLab and OKLCH (Ottosson
 *        2020), CIELAB under D65, and the CIEDE2000 colour difference
 *        (Sharma, Wu and Dalal 2005), all from and to 8-bit sRGB.
 *
 * Header-only and device-free so that code which links no graphics library
 * can choose and compare colours by how they look rather than by their
 * channel values.
 */
#ifndef GLEDITOR_COLOR_SPACE_HPP
#define GLEDITOR_COLOR_SPACE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace gleditor::color {

/// sRGB with the transfer curve removed, each channel nominally in [0, 1].
/// Outside that range the colour cannot be shown on an sRGB display.
struct LinearRgb {
  double r{};
  double g{};
  double b{};

  bool operator==(const LinearRgb &) const = default;
};

/// OKLab: l is perceived lightness in [0, 1]; a and b are the green-red and
/// blue-yellow opponent axes.
struct Oklab {
  double l{};
  double a{};
  double b{};

  bool operator==(const Oklab &) const = default;
};

/// OKLab in polar form: lightness, chroma, and hue in degrees in [0, 360).
struct Oklch {
  double l{};
  double c{};
  double h{};

  bool operator==(const Oklch &) const = default;
};

/// CIE 1976 L*a*b* relative to the D65 white point; l in [0, 100].
struct CieLab {
  double l{};
  double a{};
  double b{};

  bool operator==(const CieLab &) const = default;
};

namespace detail {
/// IEC 61966-2-1: the linear segment below the knee keeps the curve's slope
/// finite at black.
inline constexpr double kSrgbDecodeKnee  = 0.04045;
inline constexpr double kSrgbEncodeKnee  = 0.0031308;
inline constexpr double kSrgbLinearSlope = 12.92;
inline constexpr double kSrgbOffset      = 0.055;
inline constexpr double kSrgbGamma       = 2.4;
inline constexpr double kChannelMax      = 255.0;

/// CIE 1976: below (6/29)^3 the cube root is replaced by a line so that the
/// transform stays invertible near black.
inline constexpr double kLabDelta = 6.0 / 29.0;

/// D65 reference white in XYZ, Y normalised to 1.
inline constexpr double kWhiteX = 0.95047;
inline constexpr double kWhiteY = 1.0;
inline constexpr double kWhiteZ = 1.08883;

[[nodiscard]] inline double degrees(const double radians) noexcept {
  return radians * 180.0 / std::numbers::pi;
}
[[nodiscard]] inline double radians(const double degrees) noexcept {
  return degrees * std::numbers::pi / 180.0;
}
/// An angle in degrees brought into [0, 360).
[[nodiscard]] inline double wrapDegrees(const double degrees) noexcept {
  const double wrapped = std::fmod(degrees, 360.0);
  return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
}
} // namespace detail

/// One sRGB channel, encoded in [0, 1], to linear light.
[[nodiscard]] inline double srgbToLinear(const double encoded) noexcept {
  using namespace detail;
  return encoded <= kSrgbDecodeKnee
             ? encoded / kSrgbLinearSlope
             : std::pow((encoded + kSrgbOffset) / (1.0 + kSrgbOffset),
                        kSrgbGamma);
}

/// One linear channel to its sRGB encoding, in [0, 1] for a channel in
/// [0, 1].
[[nodiscard]] inline double linearToSrgb(const double linear) noexcept {
  using namespace detail;
  return linear <= kSrgbEncodeKnee
             ? linear * kSrgbLinearSlope
             : ((1.0 + kSrgbOffset) * std::pow(linear, 1.0 / kSrgbGamma)) -
                   kSrgbOffset;
}

/// A packed 0xRRGGBBAA colour's red, green and blue, as linear light. Alpha
/// is not a colour and is ignored.
[[nodiscard]] inline LinearRgb linearFromRgba(const std::uint32_t rgba) {
  const auto channel = [rgba](const unsigned shift) {
    return srgbToLinear(static_cast<double>((rgba >> shift) & 0xFFU) /
                        detail::kChannelMax);
  };
  return LinearRgb{.r = channel(24U), .g = channel(16U), .b = channel(8U)};
}

/// The nearest 8-bit sRGB colour to @p linear, packed as 0xRRGGBBAA with
/// @p alpha. Channels outside [0, 1] are clamped, which changes the colour:
/// check inGamut() first when that matters.
[[nodiscard]] inline std::uint32_t
rgbaFromLinear(const LinearRgb &linear, const std::uint8_t alpha = 0xFFU) {
  const auto channel = [](const double value) {
    const double encoded = linearToSrgb(std::clamp(value, 0.0, 1.0));
    return static_cast<std::uint32_t>(
        std::lround(encoded * detail::kChannelMax));
  };
  return (channel(linear.r) << 24U) | (channel(linear.g) << 16U) |
         (channel(linear.b) << 8U) | alpha;
}

/// Whether @p linear can be shown on an sRGB display, allowing @p tolerance
/// past either end for rounding in the transforms that produced it.
[[nodiscard]] inline bool inGamut(const LinearRgb &linear,
                                  const double tolerance = 1e-9) noexcept {
  const auto inside = [tolerance](const double value) {
    return value >= -tolerance && value <= 1.0 + tolerance;
  };
  return inside(linear.r) && inside(linear.g) && inside(linear.b);
}

/// Ottosson's matrices, as published: linear sRGB to cone response (LMS),
/// and cube-rooted LMS to Lab, with their published inverses.
[[nodiscard]] inline Oklab oklabFromLinear(const LinearRgb &rgb) noexcept {
  const double lms0 = std::cbrt(
      (0.4122214708 * rgb.r) + (0.5363325363 * rgb.g) + (0.0514459929 * rgb.b));
  const double lms1 = std::cbrt(
      (0.2119034982 * rgb.r) + (0.6806995451 * rgb.g) + (0.1073969566 * rgb.b));
  const double lms2 = std::cbrt(
      (0.0883024619 * rgb.r) + (0.2817188376 * rgb.g) + (0.6299787005 * rgb.b));
  return Oklab{
      .l =
          (0.2104542553 * lms0) + (0.7936177850 * lms1) - (0.0040720468 * lms2),
      .a =
          (1.9779984951 * lms0) - (2.4285922050 * lms1) + (0.4505937099 * lms2),
      .b =
          (0.0259040371 * lms0) + (0.7827717662 * lms1) - (0.8086757660 * lms2),
  };
}

[[nodiscard]] inline LinearRgb linearFromOklab(const Oklab &lab) noexcept {
  const double lms0 = lab.l + (0.3963377774 * lab.a) + (0.2158037573 * lab.b);
  const double lms1 = lab.l - (0.1055613458 * lab.a) - (0.0638541728 * lab.b);
  const double lms2 = lab.l - (0.0894841775 * lab.a) - (1.2914855480 * lab.b);
  const double l3   = lms0 * lms0 * lms0;
  const double m3   = lms1 * lms1 * lms1;
  const double s3   = lms2 * lms2 * lms2;
  return LinearRgb{
      .r = (4.0767416621 * l3) - (3.3077115913 * m3) + (0.2309699292 * s3),
      .g = (-1.2684380046 * l3) + (2.6097574011 * m3) - (0.3413193965 * s3),
      .b = (-0.0041960863 * l3) - (0.7034186147 * m3) + (1.7076147010 * s3),
  };
}

[[nodiscard]] inline Oklab oklabFromOklch(const Oklch &lch) noexcept {
  const double hue = detail::radians(lch.h);
  return Oklab{
      .l = lch.l, .a = lch.c * std::cos(hue), .b = lch.c * std::sin(hue)};
}

[[nodiscard]] inline Oklch oklchFromOklab(const Oklab &lab) noexcept {
  return Oklch{
      .l = lab.l,
      .c = std::hypot(lab.a, lab.b),
      .h = detail::wrapDegrees(detail::degrees(std::atan2(lab.b, lab.a)))};
}

[[nodiscard]] inline Oklch oklchFromRgba(const std::uint32_t rgba) {
  return oklchFromOklab(oklabFromLinear(linearFromRgba(rgba)));
}

[[nodiscard]] inline LinearRgb linearFromOklch(const Oklch &lch) noexcept {
  return linearFromOklab(oklabFromOklch(lch));
}

/// IEC 61966-2-1's linear sRGB to XYZ, then CIE 1976 relative to D65.
[[nodiscard]] inline CieLab cieLabFromLinear(const LinearRgb &rgb) noexcept {
  using namespace detail;
  const double x =
      ((0.4124564 * rgb.r) + (0.3575761 * rgb.g) + (0.1804375 * rgb.b)) /
      kWhiteX;
  const double y =
      ((0.2126729 * rgb.r) + (0.7151522 * rgb.g) + (0.0721750 * rgb.b)) /
      kWhiteY;
  const double z =
      ((0.0193339 * rgb.r) + (0.1191920 * rgb.g) + (0.9503041 * rgb.b)) /
      kWhiteZ;
  const auto f = [](const double t) {
    return t > kLabDelta * kLabDelta * kLabDelta
               ? std::cbrt(t)
               : (t / (3.0 * kLabDelta * kLabDelta)) + (4.0 / 29.0);
  };
  return CieLab{.l = (116.0 * f(y)) - 16.0,
                .a = 500.0 * (f(x) - f(y)),
                .b = 200.0 * (f(y) - f(z))};
}

[[nodiscard]] inline CieLab cieLabFromRgba(const std::uint32_t rgba) {
  return cieLabFromLinear(linearFromRgba(rgba));
}

/// CIEDE2000 with the parametric factors kL = kC = kH = 1, as Sharma, Wu and
/// Dalal (2005) give it. About 1 is the smallest difference most people see
/// side by side; colours that must be told apart at a glance want 10 or more.
[[nodiscard]] inline double ciede2000(const CieLab &one,
                                      const CieLab &two) noexcept {
  using detail::degrees;
  using detail::radians;
  using detail::wrapDegrees;
  // 25^7, the chroma at which the a* rescaling G and the rotation term R_C
  // reach half strength.
  constexpr double kChromaPivot7 = 6103515625.0;
  const auto pow7                = [](const double v) {
    const double v2 = v * v;
    return v2 * v2 * v2 * v;
  };

  const double chromaMean =
      (std::hypot(one.a, one.b) + std::hypot(two.a, two.b)) / 2.0;
  const double g =
      0.5 *
      (1.0 - std::sqrt(pow7(chromaMean) / (pow7(chromaMean) + kChromaPivot7)));
  const double a1       = (1.0 + g) * one.a;
  const double a2       = (1.0 + g) * two.a;
  const double c1       = std::hypot(a1, one.b);
  const double c2       = std::hypot(a2, two.b);
  const double h1       = wrapDegrees(degrees(std::atan2(one.b, a1)));
  const double h2       = wrapDegrees(degrees(std::atan2(two.b, a2)));
  const bool achromatic = 0.0 == c1 * c2;

  double hueStep = 0.0;
  if (!achromatic) {
    hueStep = h2 - h1;
    if (hueStep > 180.0) {
      hueStep -= 360.0;
    } else if (hueStep < -180.0) {
      hueStep += 360.0;
    }
  }
  const double deltaL = two.l - one.l;
  const double deltaC = c2 - c1;
  const double deltaH =
      2.0 * std::sqrt(c1 * c2) * std::sin(radians(hueStep / 2.0));

  const double lMean = (one.l + two.l) / 2.0;
  const double cMean = (c1 + c2) / 2.0;
  double hMean       = h1 + h2;
  if (!achromatic) {
    if (std::abs(h1 - h2) <= 180.0) {
      hMean = (h1 + h2) / 2.0;
    } else if (h1 + h2 < 360.0) {
      hMean = (h1 + h2 + 360.0) / 2.0;
    } else {
      hMean = (h1 + h2 - 360.0) / 2.0;
    }
  }

  const double t = 1.0 - (0.17 * std::cos(radians(hMean - 30.0))) +
                   (0.24 * std::cos(radians(2.0 * hMean))) +
                   (0.32 * std::cos(radians((3.0 * hMean) + 6.0))) -
                   (0.20 * std::cos(radians((4.0 * hMean) - 63.0)));
  const double hueRotation =
      30.0 * std::exp(-std::pow((hMean - 275.0) / 25.0, 2.0));
  const double rc =
      2.0 * std::sqrt(pow7(cMean) / (pow7(cMean) + kChromaPivot7));
  const double lOffset2 = (lMean - 50.0) * (lMean - 50.0);
  const double sl = 1.0 + ((0.015 * lOffset2) / std::sqrt(20.0 + lOffset2));
  const double sc = 1.0 + (0.045 * cMean);
  const double sh = 1.0 + (0.015 * cMean * t);
  const double rt = -std::sin(radians(2.0 * hueRotation)) * rc;

  const double termL = deltaL / sl;
  const double termC = deltaC / sc;
  const double termH = deltaH / sh;
  return std::sqrt((termL * termL) + (termC * termC) + (termH * termH) +
                   (rt * termC * termH));
}

/// CIEDE2000 between two packed 0xRRGGBBAA colours; alpha is ignored.
[[nodiscard]] inline double ciede2000(const std::uint32_t one,
                                      const std::uint32_t two) {
  return ciede2000(cieLabFromRgba(one), cieLabFromRgba(two));
}

} // namespace gleditor::color

#endif
