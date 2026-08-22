#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>
#include <gtest/gtest.h>

using namespace gleditor;
using namespace gleditor::text;

TEST(TextLayoutTest, FontManagerLoadsStandardFont) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  EXPECT_GT(font->metrics().ascent, 0.0F);
  EXPECT_GT(font->metrics().descent, 0.0F);
  EXPECT_GT(font->metrics().lineHeight, 0.0F);
  EXPECT_GT(font->metrics().spaceWidth, 0.0F);
  EXPECT_NE(font->hbFont(), nullptr);
}

TEST(TextLayoutTest, LayoutSingleLineProducesValidShaping) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  const std::string text = "Hello World";
  auto shaping           = TextLayout::layoutSingleLine(text, font);

  EXPECT_EQ(shaping.limit, text.size());
  EXPECT_GT(shaping.textWidthPx, 0);
  EXPECT_GT(shaping.textHeightPx, 0);
  EXPECT_EQ(shaping.lineCount, 1);
  EXPECT_EQ(shaping.lines.size(), 1);
  EXPECT_FALSE(shaping.glyphs.empty());
  EXPECT_FALSE(shaping.clusters.empty());
}

TEST(TextLayoutTest, LayoutMultiLineWrapsOnWidth) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  const std::string text = "The quick brown fox jumps over the lazy dog";
  LayoutOptions opts{
      .maxWidthPx      = 120.0F, // narrow width forcing multiple lines
      .maxHeightPx     = 1000.0F,
      .singleParagraph = false,
      .ellipsize       = false,
  };

  auto shaping = TextLayout::layoutPage(text, font, opts);

  EXPECT_GT(shaping.lineCount, 1);
  EXPECT_GT(shaping.lines.size(), 1);
  EXPECT_EQ(shaping.limit, text.size());
}

TEST(TextLayoutTest, LayoutPaginatesOnMaxHeight) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  std::string text;
  for (int i = 0; i < 50; i++) {
    text += "Line " + std::to_string(i) + " of text content\n";
  }

  const float lineHeight = font->metrics().lineHeight;
  LayoutOptions opts{
      .maxWidthPx      = 500.0F,
      .maxHeightPx     = lineHeight * 5.5F, // only enough room for 5 lines
      .singleParagraph = false,
      .ellipsize       = false,
  };

  auto shaping = TextLayout::layoutPage(text, font, opts);

  EXPECT_LE(shaping.lineCount, 5);
  EXPECT_LT(shaping.limit, text.size()); // correctly sliced before end
  EXPECT_GT(shaping.limit, 0);
}

TEST(TextLayoutTest, glyphsOutsideAnyDecoratedRangeCarryNone) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  auto shaping = TextLayout::layoutPage("plain text", font, LayoutOptions{});

  ASSERT_FALSE(shaping.glyphs.empty());
  for (const auto &glyph : shaping.glyphs) {
    EXPECT_EQ(glyph.decorations, DecorationMask{0});
  }
}

TEST(TextLayoutTest, glyphsWithinADecoratedRangeCarryIt) {
  auto &fm               = FontManager::instance();
  auto font              = fm.getFont("Monospace 16");
  const std::string text = "plain bold plain";
  // "bold" is text[6..10).
  LayoutOptions opts;
  opts.decoratedRanges.push_back(DecoratedRange{
      .start = 6, .end = 10, .decorations = decorationBit(Decoration::Bold)});

  auto shaping = TextLayout::layoutPage(text, font, opts);

  bool sawInside  = false;
  bool sawOutside = false;
  for (const auto &glyph : shaping.glyphs) {
    const auto byteStart = shaping.clusters[glyph.clusterIndex].byteStart;
    if (byteStart >= 6 && byteStart < 10) {
      sawInside = true;
      EXPECT_TRUE(hasDecoration(glyph.decorations, Decoration::Bold))
          << "glyph at byte " << byteStart << " should be bold";
    } else {
      sawOutside = true;
      EXPECT_FALSE(hasDecoration(glyph.decorations, Decoration::Bold))
          << "glyph at byte " << byteStart << " should not be bold";
    }
  }
  EXPECT_TRUE(sawInside);
  EXPECT_TRUE(sawOutside);
}

TEST(TextLayoutTest, overlappingDecoratedRangesCombine) {
  auto &fm               = FontManager::instance();
  auto font              = fm.getFont("Monospace 16");
  const std::string text = "abcdef";
  LayoutOptions opts;
  // Overlap entirely at [2, 4): bold from the first range, italic from the
  // second, so a glyph in the overlap should carry both.
  opts.decoratedRanges.push_back(DecoratedRange{
      .start = 0, .end = 4, .decorations = decorationBit(Decoration::Bold)});
  opts.decoratedRanges.push_back(DecoratedRange{
      .start = 2, .end = 6, .decorations = decorationBit(Decoration::Italic)});

  auto shaping = TextLayout::layoutPage(text, font, opts);

  for (const auto &glyph : shaping.glyphs) {
    const auto byteStart = shaping.clusters[glyph.clusterIndex].byteStart;
    const bool bold      = hasDecoration(glyph.decorations, Decoration::Bold);
    const bool italic    = hasDecoration(glyph.decorations, Decoration::Italic);
    if (byteStart < 2) {
      EXPECT_TRUE(bold);
      EXPECT_FALSE(italic);
    } else if (byteStart < 4) {
      EXPECT_TRUE(bold);
      EXPECT_TRUE(italic);
    } else {
      EXPECT_FALSE(bold);
      EXPECT_TRUE(italic);
    }
  }
}
