#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>
#include <gtest/gtest.h>

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
