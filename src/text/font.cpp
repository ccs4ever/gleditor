#include <gleditor/text/font.hpp>

#include <cmath>
#include <fontconfig/fontconfig.h>
#include <format>
#include <hb-ft.h>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace gleditor::text {

namespace {

struct ParsedFontSpec {
  std::string family;
  double pointSize{16.0};
};

ParsedFontSpec parseFontSpec(const std::string &spec) {
  if (spec.empty()) {
    return {.family = "Monospace", .pointSize = 16.0};
  }

  // Find last space which usually separates family from size
  const auto lastSpace = spec.rfind(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < spec.size()) {
    try {
      std::size_t idx = 0;
      double sz       = std::stod(spec.substr(lastSpace + 1), &idx);
      if (idx == spec.size() - (lastSpace + 1) && sz > 0.0) {
        return {.family = spec.substr(0, lastSpace), .pointSize = sz};
      }
    } catch (...) { // NOLINT(bugprone-empty-catch)
      // Not a number; treat entire string as family
    }
  }

  return {.family = spec, .pointSize = 16.0};
}

std::expected<std::string, FontError> resolveFontPath(const std::string &spec) {
  FcInit();
  FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>(spec.c_str()));
  if (!pat) {
    return std::unexpected{FontError::BadSpec};
  }

  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);

  FcResult result  = FcResultNoMatch;
  FcPattern *match = FcFontMatch(nullptr, pat, &result);
  FcPatternDestroy(pat);

  if (!match) {
    return std::unexpected{FontError::NoMatch};
  }

  FcChar8 *file = nullptr;
  if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
    FcPatternDestroy(match);
    return std::unexpected{FontError::NoMatch};
  }

  std::string fontPath = reinterpret_cast<const char *>(file);
  FcPatternDestroy(match);
  return fontPath;
}

/// A FontFace for @p fontPath, or LoadFailed. The constructor throws because it
/// is a constructor; this is where that becomes a value.
FontResult openFace(FT_Library lib, const std::string &fontPath,
                    const double pointSize) {
  try {
    return std::make_shared<FontFace>(lib, fontPath, pointSize);
  } catch (const std::runtime_error &) {
    return std::unexpected{FontError::LoadFailed};
  }
}

} // namespace

FontFace::FontFace(FT_Library ftLib, const std::string &fontPath,
                   const double pointSize, const unsigned int dpi)
    : pointSize_(pointSize) {
  if (FT_New_Face(ftLib, fontPath.c_str(), 0, &face_) != 0) {
    throw std::runtime_error(
        std::format("FreeType failed to load font face: {}", fontPath));
  }

  // FreeType char size in 26.6 fractional points
  const auto charSize = static_cast<FT_F26Dot6>(std::round(pointSize * 64.0));
  if (FT_Set_Char_Size(face_, 0, charSize, dpi, dpi) != 0) {
    FT_Done_Face(face_);
    throw std::runtime_error(
        std::format("FreeType failed to set character size for: {}", fontPath));
  }

  family_ = face_->family_name ? face_->family_name : fontPath;
  key_    = std::format("{} {:.1f}", family_, pointSize);

  hbFont_ = hb_ft_font_create_referenced(face_);
  if (!hbFont_) {
    FT_Done_Face(face_);
    throw std::runtime_error(
        std::format("HarfBuzz failed to create font for: {}", fontPath));
  }

  // Populate scaled typographic metrics (in pixels)
  metrics_.ascent =
      static_cast<float>(face_->size->metrics.ascender) / kFixed26Dot6Scale;
  metrics_.descent =
      -static_cast<float>(face_->size->metrics.descender) / kFixed26Dot6Scale;
  metrics_.lineHeight =
      static_cast<float>(face_->size->metrics.height) / kFixed26Dot6Scale;
  if (metrics_.lineHeight <= 0.0F) {
    metrics_.lineHeight = metrics_.ascent + metrics_.descent;
  }

  // Space width
  const auto spaceIndex = FT_Get_Char_Index(face_, ' ');
  if (spaceIndex && FT_Load_Glyph(face_, spaceIndex, FT_LOAD_DEFAULT) == 0) {
    metrics_.spaceWidth =
        static_cast<float>(face_->glyph->advance.x) / kFixed26Dot6Scale;
  } else {
    metrics_.spaceWidth = metrics_.ascent * 0.5F;
  }
}

FontFace::~FontFace() {
  if (hbFont_) {
    hb_font_destroy(hbFont_);
    hbFont_ = nullptr;
  }
  if (face_) {
    FT_Done_Face(face_);
    face_ = nullptr;
  }
}

FontFace::FontFace(FontFace &&oth) noexcept
    : face_(std::exchange(oth.face_, nullptr)),
      hbFont_(std::exchange(oth.hbFont_, nullptr)), metrics_(oth.metrics_),
      family_(std::move(oth.family_)), pointSize_(oth.pointSize_),
      key_(std::move(oth.key_)) {}

FontFace &FontFace::operator=(FontFace &&oth) noexcept {
  if (this != &oth) {
    if (hbFont_) {
      hb_font_destroy(hbFont_);
    }
    if (face_) {
      FT_Done_Face(face_);
    }
    face_      = std::exchange(oth.face_, nullptr);
    hbFont_    = std::exchange(oth.hbFont_, nullptr);
    metrics_   = oth.metrics_;
    family_    = std::move(oth.family_);
    pointSize_ = oth.pointSize_;
    key_       = std::move(oth.key_);
  }
  return *this;
}

FontManager &FontManager::instance() {
  thread_local FontManager inst;
  return inst;
}

#include FT_LCD_FILTER_H

FontManager::FontManager() {
  if (FT_Init_FreeType(&ftLib_) != 0) {
    throw std::runtime_error("FreeType initialization failed");
  }
  // Initialize standard LCD subpixel decimation filters on the FreeType library
  // instance for subpixel font rendering and hinting compatibility.
  FT_Library_SetLcdFilter(ftLib_, FT_LCD_FILTER_DEFAULT);
}

FontManager::~FontManager() {
  cache_.clear();
  fallbackCache_.clear();
  if (ftLib_) {
    FT_Done_FreeType(ftLib_);
    ftLib_ = nullptr;
  }
}

FontResult FontManager::findFont(const std::string &fontSpec) {
  if (const auto it = cache_.find(fontSpec); it != cache_.end()) {
    return it->second;
  }
  const auto pointSize = parseFontSpec(fontSpec).pointSize;
  auto font = resolveFontPath(fontSpec).and_then([&](const std::string &path) {
    return openFace(ftLib_, path, pointSize);
  });
  cache_.emplace(fontSpec, font);
  return font;
}

FontFacePtr FontManager::getFont(const std::string &fontSpec) {
  auto font = findFont(fontSpec);
  if (!font) {
    throw std::runtime_error(
        std::format("{}: {}", toString(font.error()), fontSpec));
  }
  return *std::move(font);
}

namespace {
/// The file of the font Fontconfig would use for @p codepoint, as close to
/// @p primary as it can match.
std::expected<std::string, FontError>
fallbackPathFor(const FontFace &primary, const uint32_t codepoint) {
  FcInit();
  FcPattern *pat = FcPatternCreate();
  FcCharSet *cs  = FcCharSetCreate();
  FcCharSetAddChar(cs, codepoint);
  FcPatternAddCharSet(pat, FC_CHARSET, cs);
  FcPatternAddDouble(pat, FC_SIZE, primary.pointSize());
  FcPatternAddString(
      pat, FC_FAMILY,
      reinterpret_cast<const FcChar8 *>(primary.family().c_str()));

  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);

  FcResult result  = FcResultNoMatch;
  FcPattern *match = FcFontMatch(nullptr, pat, &result);
  FcCharSetDestroy(cs);
  FcPatternDestroy(pat);
  if (!match) {
    return std::unexpected{FontError::NoMatch};
  }

  FcChar8 *file = nullptr;
  if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
    FcPatternDestroy(match);
    return std::unexpected{FontError::NoMatch};
  }
  std::string fontPath = reinterpret_cast<const char *>(file);
  FcPatternDestroy(match);
  return fontPath;
}
} // namespace

FontResult FontManager::getFallbackFont(const FontFacePtr &primaryFont,
                                        const uint32_t codepoint) {
  if (!primaryFont) {
    return std::unexpected{FontError::NoPrimary};
  }
  const auto cacheKey = std::format("{}_{:#x}", primaryFont->key(), codepoint);
  if (const auto it = fallbackCache_.find(cacheKey);
      it != fallbackCache_.end()) {
    return it->second;
  }
  auto fallback = fallbackPathFor(*primaryFont, codepoint)
                      .and_then([&](const std::string &path) {
                        return openFace(ftLib_, path, primaryFont->pointSize());
                      });
  fallbackCache_.emplace(cacheKey, fallback);
  return fallback;
}

} // namespace gleditor::text
