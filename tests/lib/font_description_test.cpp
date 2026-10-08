#include <fontconfig/fontconfig.h>
#include <gleditor/text/font.hpp>
#include <gtest/gtest.h>

#include <string>

using gleditor::text::FontManager;

namespace {

bool fixedWidth(const gleditor::text::FontFacePtr &font) {
  return font && FT_IS_FIXED_WIDTH(font->face());
}

bool styled(const gleditor::text::FontFacePtr &font, FT_Long flag) {
  return font && 0 != (font->face()->style_flags & flag);
}

/// Whether Fontconfig itself, asked in its own syntax ("Sans:slant=100"),
/// finds a face of the family with that style. A style the system lacks
/// cannot be asked of the library, and this does not ask the library.
bool installed(const char *pattern, const char *object, int atLeast) {
  FcInit();
  FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>(pattern));
  if (!pat) return false;
  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult result  = FcResultNoMatch;
  FcPattern *match = FcFontMatch(nullptr, pat, &result);
  FcPatternDestroy(pat);
  int value = -1;
  const bool found =
      match && FcPatternGetInteger(match, object, 0, &value) == FcResultMatch;
  if (match) FcPatternDestroy(match);
  return found && value >= atLeast;
}

} // namespace

// Every description in the tree is written "Family Style Size". Handed whole
// to Fontconfig, that is one family named "Monospace 16", which matches
// nothing: every role fell back to the default face, so a monospace role was
// proportional and a bold one was not bold, differently on every machine.
TEST(FontDescriptionTest, TheFamilyBeforeTheSizeIsTheOneLoaded) {
  const auto font = FontManager::instance().getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  EXPECT_TRUE(fixedWidth(font)) << font->family();
  EXPECT_DOUBLE_EQ(font->pointSize(), 16.0);
}

TEST(FontDescriptionTest, StyleWordsChooseTheFace) {
  auto &fonts = FontManager::instance();
  EXPECT_FALSE(styled(fonts.getFont("Sans 12"), FT_STYLE_FLAG_BOLD));
  if (installed("Sans:weight=200", FC_WEIGHT, FC_WEIGHT_BOLD)) {
    const auto bold = fonts.getFont("Sans Bold 12");
    EXPECT_TRUE(styled(bold, FT_STYLE_FLAG_BOLD)) << bold->family();
    EXPECT_DOUBLE_EQ(bold->pointSize(), 12.0);
  }
  if (installed("Sans:slant=100", FC_SLANT, FC_SLANT_ITALIC)) {
    const auto italic = fonts.getFont("Sans Italic 12");
    EXPECT_TRUE(styled(italic, FT_STYLE_FLAG_ITALIC)) << italic->family();
  }
}

// The glyph cache keys glyphs by FontFace::key(), so a family's regular and
// bold faces at one size must not share a key.
TEST(FontDescriptionTest, FacesOfOneFamilyKeepTheirStylesApart) {
  auto &fonts = FontManager::instance();
  if (!installed("Sans:weight=200", FC_WEIGHT, FC_WEIGHT_BOLD))
    GTEST_SKIP() << "no bold sans face is installed";
  EXPECT_NE(fonts.getFont("Sans 12")->key(),
            fonts.getFont("Sans Bold 12")->key());
}

TEST(FontDescriptionTest, AFamilyListFallsThroughToTheNextName) {
  const auto font =
      FontManager::instance().getFont("No Such Family Anywhere, Monospace 14");
  ASSERT_NE(font, nullptr);
  EXPECT_TRUE(fixedWidth(font)) << font->family();
  EXPECT_DOUBLE_EQ(font->pointSize(), 14.0);
}
