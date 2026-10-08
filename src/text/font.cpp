#include <gleditor/text/font.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fontconfig/fontconfig.h>
#include <format>
#include <hb-ft.h>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace gleditor::text {

namespace {

struct ParsedFontSpec {
  std::vector<std::string> families;
  std::optional<int> weight;
  std::optional<int> slant;
  double pointSize{16.0};
};

struct StyleWord {
  std::string_view word;
  std::optional<int> weight;
  std::optional<int> slant;
};

// The style words of a Pango-style description ("Sans Bold Italic 12"), the
// spelling every caller in the tree uses, mapped to Fontconfig's values.
// Fontconfig's own name syntax ("Sans-12:bold") reads a description like that
// as one family called "Sans Bold Italic 12", matches nothing, and quietly
// falls back to the default face, so the description must be taken apart
// here rather than handed to FcNameParse.
constexpr std::array kStyleWords{
    StyleWord{"thin", FC_WEIGHT_THIN},
    StyleWord{"ultra-light", FC_WEIGHT_EXTRALIGHT},
    StyleWord{"extra-light", FC_WEIGHT_EXTRALIGHT},
    StyleWord{"light", FC_WEIGHT_LIGHT},
    StyleWord{"semi-light", FC_WEIGHT_DEMILIGHT},
    StyleWord{"demi-light", FC_WEIGHT_DEMILIGHT},
    StyleWord{"book", FC_WEIGHT_BOOK},
    StyleWord{"regular", FC_WEIGHT_REGULAR},
    StyleWord{"normal", FC_WEIGHT_REGULAR},
    StyleWord{"medium", FC_WEIGHT_MEDIUM},
    StyleWord{"semi-bold", FC_WEIGHT_DEMIBOLD},
    StyleWord{"demi-bold", FC_WEIGHT_DEMIBOLD},
    StyleWord{"bold", FC_WEIGHT_BOLD},
    StyleWord{"ultra-bold", FC_WEIGHT_EXTRABOLD},
    StyleWord{"extra-bold", FC_WEIGHT_EXTRABOLD},
    StyleWord{"heavy", FC_WEIGHT_HEAVY},
    StyleWord{"black", FC_WEIGHT_BLACK},
    StyleWord{"ultra-heavy", FC_WEIGHT_EXTRABLACK},
    StyleWord{"roman", std::nullopt, FC_SLANT_ROMAN},
    StyleWord{"italic", std::nullopt, FC_SLANT_ITALIC},
    StyleWord{"oblique", std::nullopt, FC_SLANT_OBLIQUE},
};

std::optional<StyleWord> styleWord(std::string_view word) {
  std::string folded(word);
  std::ranges::transform(folded, folded.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const auto found = std::ranges::find(kStyleWords, folded, &StyleWord::word);
  if (found == kStyleWords.end()) return std::nullopt;
  return *found;
}

std::optional<double> pointSizeOf(const std::string &word) {
  try {
    std::size_t used  = 0;
    const double size = std::stod(word, &used);
    if (used == word.size() && size > 0.0) return size;
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // Not a number, so not a size.
  }
  return std::nullopt;
}

/// "[FAMILY,LIST] [STYLE-WORDS] [SIZE]", as Pango reads it: the size is the
/// last word if it is a number, the style words are those before it, and
/// what is left is a comma-separated family list.
ParsedFontSpec parseFontSpec(const std::string &spec) {
  std::vector<std::string> words;
  std::istringstream in(spec);
  for (std::string word; in >> word;) words.push_back(std::move(word));

  ParsedFontSpec parsed;
  if (!words.empty()) {
    if (const auto size = pointSizeOf(words.back())) {
      parsed.pointSize = *size;
      words.pop_back();
    }
  }
  while (!words.empty()) {
    const auto style = styleWord(words.back());
    if (!style) break;
    if (!parsed.weight) parsed.weight = style->weight;
    if (!parsed.slant) parsed.slant = style->slant;
    words.pop_back();
  }

  std::string family;
  for (const auto &word : words) family += (family.empty() ? "" : " ") + word;
  std::istringstream list(family);
  for (std::string name; std::getline(list, name, ',');) {
    const auto first = name.find_first_not_of(' ');
    const auto last  = name.find_last_not_of(' ');
    if (first != std::string::npos)
      parsed.families.push_back(name.substr(first, last - first + 1));
  }
  if (parsed.families.empty()) parsed.families.emplace_back("Monospace");
  return parsed;
}

std::expected<std::string, FontError> resolveFontPath(const std::string &spec) {
  FcInit();
  const auto parsed = parseFontSpec(spec);
  FcPattern *pat    = FcPatternCreate();
  if (!pat) {
    return std::unexpected{FontError::BadSpec};
  }
  for (const auto &family : parsed.families)
    FcPatternAddString(pat, FC_FAMILY,
                       reinterpret_cast<const FcChar8 *>(family.c_str()));
  if (parsed.weight) FcPatternAddInteger(pat, FC_WEIGHT, *parsed.weight);
  if (parsed.slant) FcPatternAddInteger(pat, FC_SLANT, *parsed.slant);
  FcPatternAddDouble(pat, FC_SIZE, parsed.pointSize);

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
FontResult openFace(const FontLibraryPtr &lib, const std::string &fontPath,
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
  // The style is part of the key: a family's bold and regular faces at one
  // size are different faces, and the glyph cache keys its glyphs by this.
  key_ = std::format("{} {} {:.1f}", family_,
                     face_->style_name ? face_->style_name : "", pointSize);

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

FontFace::FontFace(FontLibraryPtr library, const std::string &fontPath,
                   double pointSize, unsigned int dpi)
    : FontFace(library.get(), fontPath, pointSize, dpi) {
  library_ = std::move(library);
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
    : library_(std::move(oth.library_)),
      face_(std::exchange(oth.face_, nullptr)),
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
    library_   = std::move(oth.library_);
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
  FT_Library library{};
  if (FT_Init_FreeType(&library) != 0) {
    throw std::runtime_error("FreeType initialization failed");
  }
  ftLib_ = FontLibraryPtr(library,
                          [](FT_Library owned) { FT_Done_FreeType(owned); });
  // Initialize standard LCD subpixel decimation filters on the FreeType library
  // instance for subpixel font rendering and hinting compatibility.
  FT_Library_SetLcdFilter(ftLib_.get(), FT_LCD_FILTER_DEFAULT);
}

FontManager::~FontManager() {
  cache_.clear();
  fallbackCache_.clear();
  ftLib_.reset();
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
