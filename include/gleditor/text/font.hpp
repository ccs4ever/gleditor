#pragma once

#include <cstdint>
#include <expected>
#include <ft2build.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include FT_FREETYPE_H
#include <hb.h>

namespace gleditor::text {

/// FreeType and HarfBuzz both report metrics in 26.6 fixed-point: the low 6
/// bits are a fractional pixel, so dividing by this converts to a plain
/// float pixel value.
inline constexpr float kFixed26Dot6Scale = 64.0F;

/**
 * @brief Scaled typographic metrics for a resolved font at a given pixel size.
 */
struct FontMetrics {
  float ascent{};     ///< Pixels above baseline (positive)
  float descent{};    ///< Pixels below baseline (positive)
  float lineHeight{}; ///< Full line pitch in pixels
  float spaceWidth{}; ///< Advance width of the space character
};

/**
 * @brief Represents a loaded FreeType font face paired with a HarfBuzz font
 * object.
 */
class FontFace {
public:
  FontFace(FT_Library ftLib, const std::string &fontPath, double pointSize,
           unsigned int dpi = 96);
  ~FontFace();

  FontFace(const FontFace &)            = delete;
  FontFace &operator=(const FontFace &) = delete;
  FontFace(FontFace &&oth) noexcept;
  FontFace &operator=(FontFace &&oth) noexcept;

  [[nodiscard]] FT_Face face() const { return face_; }
  [[nodiscard]] hb_font_t *hbFont() const { return hbFont_; }
  [[nodiscard]] const FontMetrics &metrics() const { return metrics_; }
  [[nodiscard]] const std::string &family() const { return family_; }
  [[nodiscard]] double pointSize() const { return pointSize_; }
  [[nodiscard]] const std::string &key() const { return key_; }

private:
  FT_Face face_{};
  hb_font_t *hbFont_{};
  FontMetrics metrics_{};
  std::string family_;
  double pointSize_{};
  std::string key_;
};

using FontFacePtr = std::shared_ptr<FontFace>;

/// Why a font could not be had.
enum class FontError : std::uint8_t {
  BadSpec,    ///< Fontconfig could not parse the description
  NoMatch,    ///< Fontconfig matched nothing, or nothing with a file
  LoadFailed, ///< FreeType or HarfBuzz could not open the matched file
  NoPrimary,  ///< a fallback was asked for with no primary font to follow
};

[[nodiscard]] constexpr std::string_view
toString(const FontError error) noexcept {
  switch (error) {
  case FontError::BadSpec:
    return "unparseable font description";
  case FontError::NoMatch:
    return "no matching font";
  case FontError::LoadFailed:
    return "font file could not be loaded";
  case FontError::NoPrimary:
    return "no primary font";
  }
  return "font unavailable";
}

using FontResult = std::expected<FontFacePtr, FontError>;

/**
 * @brief Resolves font descriptions (e.g. "Monospace 16", "Serif 14") via
 * Fontconfig and caches loaded FontFace objects per thread.
 */
class FontManager {
public:
  static FontManager &instance();

  /// The font @p fontSpec describes, or why there is none. Cached per
  /// thread, including the refusals.
  [[nodiscard]] FontResult findFont(const std::string &fontSpec);
  /// findFont() for callers with no fallback of their own.
  /// @throws std::runtime_error naming the FontError.
  FontFacePtr getFont(const std::string &fontSpec);
  /// A font covering @p codepoint that looks as much like @p primaryFont as
  /// Fontconfig can manage. A miss is cached too, so a codepoint no installed
  /// font covers costs one Fontconfig query rather than one per glyph.
  [[nodiscard]] FontResult getFallbackFont(const FontFacePtr &primaryFont,
                                           uint32_t codepoint);

private:
  FontManager();
  ~FontManager();

  FT_Library ftLib_{};
  std::unordered_map<std::string, FontResult> cache_;
  std::unordered_map<std::string, FontResult> fallbackCache_;
};

} // namespace gleditor::text
