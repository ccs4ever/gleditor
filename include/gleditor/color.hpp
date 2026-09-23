/**
 * @file color.hpp
 * @brief Common color representation, packing, and hex conversion utilities.
 */
#ifndef GLEDITOR_COLOR_H
#define GLEDITOR_COLOR_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gleditor::color {

/**
 * @brief Three-channel RGB float color with values in [0.0, 1.0].
 */
struct Color3 {
  float r{1.0F};
  float g{1.0F};
  float b{1.0F};

  bool operator==(const Color3 &) const = default;
};

/**
 * @brief Four-channel RGBA float color with values in [0.0, 1.0].
 */
struct Color4 {
  float r{1.0F};
  float g{1.0F};
  float b{1.0F};
  float a{1.0F};

  bool operator==(const Color4 &) const = default;
};

/**
 * @brief Pack RGBA float components into a 32-bit integer (0xRRGGBBAA).
 */
constexpr std::uint32_t packRgba(const float r, const float g, const float b,
                                 const float a = 1.0F) {
  const auto ur =
      static_cast<std::uint32_t>(std::clamp(r * 255.0F, 0.0F, 255.0F));
  const auto ug =
      static_cast<std::uint32_t>(std::clamp(g * 255.0F, 0.0F, 255.0F));
  const auto ub =
      static_cast<std::uint32_t>(std::clamp(b * 255.0F, 0.0F, 255.0F));
  const auto ua =
      static_cast<std::uint32_t>(std::clamp(a * 255.0F, 0.0F, 255.0F));
  return (ur << 24U) | (ug << 16U) | (ub << 8U) | ua;
}

constexpr std::uint32_t packRgba(const Color4 &c) {
  return packRgba(c.r, c.g, c.b, c.a);
}

constexpr std::uint32_t packRgb(const Color3 &c, const float a = 1.0F) {
  return packRgba(c.r, c.g, c.b, a);
}

/**
 * @brief Unpack a 32-bit integer (0xRRGGBBAA) into float RGBA components.
 */
constexpr Color4 unpackRgba(const std::uint32_t packed) {
  return Color4{
      .r = static_cast<float>((packed >> 24U) & 0xFFU) / 255.0F,
      .g = static_cast<float>((packed >> 16U) & 0xFFU) / 255.0F,
      .b = static_cast<float>((packed >> 8U) & 0xFFU) / 255.0F,
      .a = static_cast<float>(packed & 0xFFU) / 255.0F,
  };
}

/// The value of one hex digit, or nullopt for anything else.
[[nodiscard]] constexpr std::optional<int> hexDigit(const char chr) noexcept {
  if (chr >= '0' && chr <= '9') {
    return chr - '0';
  }
  if (chr >= 'a' && chr <= 'f') {
    return (chr - 'a') + 10;
  }
  if (chr >= 'A' && chr <= 'F') {
    return (chr - 'A') + 10;
  }
  return std::nullopt;
}

/**
 * @brief Parse a 6-digit (or optionally 3-digit) hex RGB string (e.g.
 *        "#RRGGBB" or "RRGGBB", and if @p allow3Digit is true, "#RGB" or
 * "RGB").
 */
inline std::optional<Color3> parseHexColor(const std::string_view text,
                                           const bool allow3Digit = false) {
  std::string_view hex = text;
  if (!hex.empty() && hex.front() == '#') {
    hex.remove_prefix(1);
  }
  const bool shortForm = 3 == hex.size();
  if (!(6 == hex.size() || (shortForm && allow3Digit))) {
    return std::nullopt;
  }

  std::array<std::optional<int>, 6> digits{};
  std::ranges::transform(hex, digits.begin(), hexDigit);
  if (!std::ranges::all_of(digits | std::views::take(hex.size()),
                           &std::optional<int>::has_value)) {
    return std::nullopt;
  }
  // "#RGB" is "#RRGGBB" with each digit doubled: 0xF is 0xFF, i.e. 15 * 17.
  const auto channel = [&](const std::size_t i) {
    const int value = shortForm
                          ? (*digits[i] * 17)
                          : ((*digits[2 * i] << 4) | *digits[(2 * i) + 1]);
    return static_cast<float>(value) / 255.0F;
  };
  return Color3{.r = channel(0), .g = channel(1), .b = channel(2)};
}

/**
 * @brief Format a Color3 into a lowercase "#rrggbb" string.
 */
inline std::string formatHexColor(const Color3 &color) {
  const auto r =
      static_cast<unsigned>(std::clamp(color.r * 255.0F, 0.0F, 255.0F));
  const auto g =
      static_cast<unsigned>(std::clamp(color.g * 255.0F, 0.0F, 255.0F));
  const auto b =
      static_cast<unsigned>(std::clamp(color.b * 255.0F, 0.0F, 255.0F));
  return std::format("#{:02x}{:02x}{:02x}", r, g, b);
}

/**
 * @brief Encode arbitrary binary bytes into a lowercase hex string.
 */
inline std::string toHex(const std::string_view bytes) {
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto chr : bytes) {
    const auto byte = static_cast<std::uint8_t>(chr);
    out.push_back(digits[byte >> 4U]);
    out.push_back(digits[byte & 0x0FU]);
  }
  return out;
}

/// Why a hex string did not decode.
enum class HexError : std::uint8_t {
  OddLength,   ///< two digits make a byte, and there was one left over
  NonHexDigit, ///< a character outside [0-9a-fA-F]
};

/// Decode a lowercase or uppercase hex string into binary bytes, or say why
/// it is not one.
[[nodiscard]] inline std::expected<std::string, HexError>
decodeHex(const std::string_view text) {
  if (0 != text.size() % 2) {
    return std::unexpected{HexError::OddLength};
  }
  std::string out;
  out.reserve(text.size() / 2);
  for (const auto pair : text | std::views::chunk(2)) {
    const auto high = hexDigit(pair[0]);
    const auto low  = hexDigit(pair[1]);
    if (!high || !low) {
      return std::unexpected{HexError::NonHexDigit};
    }
    out.push_back(static_cast<char>((*high << 4) | *low));
  }
  return out;
}

/**
 * @brief decodeHex() for callers with no use for the reason.
 * @throws std::runtime_error on an odd number of digits or non-hex characters.
 */
inline std::string fromHex(const std::string_view text) {
  auto decoded = decodeHex(text);
  if (!decoded) {
    throw std::runtime_error(HexError::OddLength == decoded.error()
                                 ? "fromHex: odd length"
                                 : "fromHex: non-hex digit");
  }
  return *std::move(decoded);
}

} // namespace gleditor::color

#endif // GLEDITOR_COLOR_H
