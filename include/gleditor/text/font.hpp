#pragma once

#include <ft2build.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include FT_FREETYPE_H
#include <hb.h>

namespace gleditor::text {

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

/**
 * @brief Resolves font descriptions (e.g. "Monospace 16", "Serif 14") via
 * Fontconfig and caches loaded FontFace objects per thread.
 */
class FontManager {
public:
  static FontManager &instance();

  FontFacePtr getFont(const std::string &fontSpec);
  FontFacePtr getFallbackFont(const FontFacePtr &primaryFont,
                              uint32_t codepoint);

private:
  FontManager();
  ~FontManager();

  FT_Library ftLib_{};
  std::unordered_map<std::string, FontFacePtr> cache_;
  std::unordered_map<std::string, FontFacePtr> fallbackCache_;
};

} // namespace gleditor::text
