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
    return {"Monospace", 16.0};
  }

  // Find last space which usually separates family from size
  const auto lastSpace = spec.rfind(' ');
  if (lastSpace != std::string::npos && lastSpace + 1 < spec.size()) {
    try {
      std::size_t idx = 0;
      double sz       = std::stod(spec.substr(lastSpace + 1), &idx);
      if (idx == spec.size() - (lastSpace + 1) && sz > 0.0) {
        return {spec.substr(0, lastSpace), sz};
      }
    } catch (...) {
      // Not a number; treat entire string as family
    }
  }

  return {spec, 16.0};
}

std::string resolveFontPath(const std::string &spec) {
  FcInit();
  FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>(spec.c_str()));
  if (!pat) {
    throw std::runtime_error(
        std::format("Failed to parse font spec: {}", spec));
  }

  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);

  FcResult result  = FcResultNoMatch;
  FcPattern *match = FcFontMatch(nullptr, pat, &result);
  FcPatternDestroy(pat);

  if (!match) {
    throw std::runtime_error(
        std::format("Fontconfig found no match for: {}", spec));
  }

  FcChar8 *file = nullptr;
  if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
    FcPatternDestroy(match);
    throw std::runtime_error(
        std::format("Fontconfig matched font with no file path: {}", spec));
  }

  std::string fontPath = reinterpret_cast<const char *>(file);
  FcPatternDestroy(match);
  return fontPath;
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
  metrics_.ascent = static_cast<float>(face_->size->metrics.ascender) / 64.0F;
  metrics_.descent =
      -static_cast<float>(face_->size->metrics.descender) / 64.0F;
  metrics_.lineHeight = static_cast<float>(face_->size->metrics.height) / 64.0F;
  if (metrics_.lineHeight <= 0.0F) {
    metrics_.lineHeight = metrics_.ascent + metrics_.descent;
  }

  // Space width
  const auto spaceIndex = FT_Get_Char_Index(face_, ' ');
  if (spaceIndex && FT_Load_Glyph(face_, spaceIndex, FT_LOAD_DEFAULT) == 0) {
    metrics_.spaceWidth = static_cast<float>(face_->glyph->advance.x) / 64.0F;
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

FontManager::FontManager() {
  if (FT_Init_FreeType(&ftLib_) != 0) {
    throw std::runtime_error("FreeType initialization failed");
  }
}

FontManager::~FontManager() {
  cache_.clear();
  fallbackCache_.clear();
  if (ftLib_) {
    FT_Done_FreeType(ftLib_);
    ftLib_ = nullptr;
  }
}

FontFacePtr FontManager::getFont(const std::string &fontSpec) {
  const auto it = cache_.find(fontSpec);
  if (it != cache_.end()) {
    return it->second;
  }

  const auto parsed   = parseFontSpec(fontSpec);
  const auto fontPath = resolveFontPath(fontSpec);
  auto font = std::make_shared<FontFace>(ftLib_, fontPath, parsed.pointSize);
  cache_[fontSpec] = font;
  return font;
}

FontFacePtr FontManager::getFallbackFont(const FontFacePtr &primaryFont,
                                         const uint32_t codepoint) {
  if (!primaryFont) {
    return nullptr;
  }
  const auto cacheKey = std::format("{}_{:#x}", primaryFont->key(), codepoint);
  const auto it       = fallbackCache_.find(cacheKey);
  if (it != fallbackCache_.end()) {
    return it->second;
  }

  FcInit();
  FcPattern *pat = FcPatternCreate();
  FcCharSet *cs  = FcCharSetCreate();
  FcCharSetAddChar(cs, codepoint);
  FcPatternAddCharSet(pat, FC_CHARSET, cs);
  FcPatternAddDouble(pat, FC_SIZE, primaryFont->pointSize());
  FcPatternAddString(
      pat, FC_FAMILY,
      reinterpret_cast<const FcChar8 *>(primaryFont->family().c_str()));

  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);

  FcResult result  = FcResultNoMatch;
  FcPattern *match = FcFontMatch(nullptr, pat, &result);
  FcCharSetDestroy(cs);
  FcPatternDestroy(pat);

  if (!match) {
    fallbackCache_[cacheKey] = nullptr;
    return nullptr;
  }

  FcChar8 *file = nullptr;
  if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
    FcPatternDestroy(match);
    fallbackCache_[cacheKey] = nullptr;
    return nullptr;
  }

  std::string fontPath = reinterpret_cast<const char *>(file);
  FcPatternDestroy(match);

  try {
    auto fallback =
        std::make_shared<FontFace>(ftLib_, fontPath, primaryFont->pointSize());
    fallbackCache_[cacheKey] = fallback;
    return fallback;
  } catch (...) {
    fallbackCache_[cacheKey] = nullptr;
    return nullptr;
  }
}

} // namespace gleditor::text
