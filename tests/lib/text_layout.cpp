#include <algorithm>

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

TEST(TextLayoutTest, WithoutAtomicRangesAPlaceholderCanSplitAcrossPages) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  // Three real lines of text, then a ten-newline run standing in for a
  // media placeholder -- the exact shape apps/xudu/session.cpp's
  // placeholderFor() produces.
  const std::string prefix = "Line one\nLine two\nLine three\n";
  const std::string placeholder(10, '\n');
  const std::string text = prefix + placeholder;

  LayoutOptions opts{
      .maxWidthPx  = 500.0F,
      .maxHeightPx = lineHeight * 5.5F, // room for the 3 lines plus 2 more
  };

  auto shaping = TextLayout::layoutPage(text, font, opts);

  // With no atomicRanges, the layout engine has no way to know these ten
  // newlines belong together -- it paginates by line count alone and stops
  // partway through them, exactly the split a widget drawn over that space
  // must not be handed.
  EXPECT_EQ(shaping.lineCount, 5);
  EXPECT_GT(shaping.limit, prefix.size());
  EXPECT_LT(shaping.limit, text.size());
}

TEST(TextLayoutTest, AtomicRangeMovesWholeToNextPageWhenItDoesNotFit) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string prefix = "Line one\nLine two\nLine three\n";
  const std::string placeholder(10, '\n');
  const std::string text = prefix + placeholder;

  LayoutOptions opts{
      .maxWidthPx  = 500.0F,
      .maxHeightPx = lineHeight * 5.5F, // same budget as the test above
  };
  opts.atomicRanges.push_back(gleditor::AtomicRange{
      .start = static_cast<std::uint32_t>(prefix.size()),
      .end   = static_cast<std::uint32_t>(prefix.size() + placeholder.size()),
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  // Now the whole placeholder rolls to the next page's call instead of
  // splitting: this page ends exactly where the real lines did, and a
  // second layoutPage() call starting at shaping.limit would see the whole
  // ten-newline run fresh, with a full page's height to work with.
  EXPECT_EQ(shaping.lineCount, 3);
  EXPECT_EQ(shaping.limit, prefix.size());
}

TEST(TextLayoutTest, AtomicRangeDoesNotForceABreakWhenItFits) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string prefix = "Line one\nLine two\nLine three\n";
  const std::string placeholder(10, '\n');
  const std::string text = prefix + placeholder;

  LayoutOptions opts{
      .maxWidthPx  = 500.0F,
      .maxHeightPx = lineHeight * 20.0F, // comfortably fits all 13 lines
  };
  opts.atomicRanges.push_back(gleditor::AtomicRange{
      .start = static_cast<std::uint32_t>(prefix.size()),
      .end   = static_cast<std::uint32_t>(prefix.size() + placeholder.size()),
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  EXPECT_EQ(shaping.lineCount, 13);
  EXPECT_EQ(shaping.limit, text.size());
}

TEST(TextLayoutTest, AtomicRangeMinWidthWidensThePage) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  // A short line of real text, far narrower than the media placeholder that
  // follows it -- the shape a small caption above a wide image takes.
  const std::string prefix = "Fig 1\n";
  const std::string placeholder(3, '\n');
  const std::string text = prefix + placeholder;

  LayoutOptions opts{
      .maxWidthPx  = 2000.0F,
      .maxHeightPx = 1000.0F,
  };
  auto shaping = TextLayout::layoutPage(text, font, opts);
  // Baseline: with no atomic range at all, the page is only as wide as the
  // short text line -- a blank placeholder line has no glyphs to widen it.
  const auto narrowWidthPx = shaping.textWidthPx;

  opts.atomicRanges.push_back(gleditor::AtomicRange{
      .start = static_cast<std::uint32_t>(prefix.size()),
      .end   = static_cast<std::uint32_t>(prefix.size() + placeholder.size()),
      .minWidthPx = 900.0F,
  });
  const auto widened = TextLayout::layoutPage(text, font, opts);

  EXPECT_LT(narrowWidthPx, 900);
  EXPECT_GE(widened.textWidthPx, 900);
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

TEST(TextLayoutTest, MultiFontFallbackShaping) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  // Test Unicode text containing CJK and symbol glyphs
  const std::string text = "Hello 世界 🌍 test";
  auto shaping           = TextLayout::layoutSingleLine(text, font);

  EXPECT_GT(shaping.limit, 0);
  EXPECT_GT(shaping.textWidthPx, 0);
  EXPECT_FALSE(shaping.glyphs.empty());
  EXPECT_FALSE(shaping.clusters.empty());
}

TEST(TextLayoutTest, PageGeometryIsEchoedBackUnchanged) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  const gleditor::PageSize page{
      .mode     = gleditor::PageSizing::Fixed,
      .widthPx  = 1235.45F,
      .heightPx = 1584.7F,
      .marginPx = 24.0F,
  };
  LayoutOptions opts{
      .maxWidthPx = 500.0F, .maxHeightPx = 1000.0F, .page = page};

  auto shaping = TextLayout::layoutPage("short document", font, opts);

  EXPECT_EQ(shaping.page, page);
}

TEST(TextLayoutTest, UnnamedPageDefaultsToFitContentAndIsNotEchoedAsFixed) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  // No .page set at all -- every caller before PageSize existed, and every
  // caller measuring a single line (Canvas, Toast) still today.
  auto shaping =
      TextLayout::layoutPage("short document", font, LayoutOptions{});

  EXPECT_EQ(shaping.page.mode, gleditor::PageSizing::FitContent);
}

TEST(TextLayoutTest, BlockBoxConsumesHeightAndIsReportedInShapingBoxes) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  // Three real lines, then one anchor byte a media figure attaches its
  // LayoutBox to -- the same "prefix + anchor" shape the AtomicRange tests
  // above use, but the anchor is a single character rather than a run sized
  // to the media's height.
  const std::string prefix = "Line one\nLine two\nLine three\n";
  const std::string text   = prefix + "\n";

  LayoutOptions opts{
      .maxWidthPx  = 500.0F,
      .maxHeightPx = lineHeight * 20.0F, // comfortably fits everything
  };
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = static_cast<std::uint32_t>(prefix.size()),
      .widthPx   = 200.0F,
      .heightPx  = 150.0F,
      .marginPx  = 0.0F,
      .placement = gleditor::BoxPlacement::Block,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  const auto &placed = shaping.boxes.front();
  EXPECT_EQ(placed.anchorByteOffset, prefix.size());
  EXPECT_FLOAT_EQ(placed.top, lineHeight * 3.0F);
  EXPECT_FLOAT_EQ(placed.width, 200.0F);
  EXPECT_FLOAT_EQ(placed.height, 150.0F);
  EXPECT_EQ(placed.placement, gleditor::BoxPlacement::Block);
  // Default alignment is Left -- the box sits flush with the column start.
  EXPECT_FLOAT_EQ(placed.left, 0.0F);
}

TEST(TextLayoutTest, BlockBoxMovesWholeToNextPageWhenItDoesNotFit) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string prefix = "Line one\nLine two\nLine three\n";
  const std::string text   = prefix + "\n";

  LayoutOptions opts{
      .maxWidthPx  = 500.0F,
      .maxHeightPx = lineHeight * 3.5F, // room for the 3 lines, not the box
  };
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = static_cast<std::uint32_t>(prefix.size()),
      .widthPx   = 200.0F,
      .heightPx  = 150.0F,
      .placement = gleditor::BoxPlacement::Block,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  // The box rolls to the next page's call in its entirety rather than being
  // cut off partway through -- same rule an AtomicRange follows.
  EXPECT_EQ(shaping.lineCount, 3u);
  EXPECT_EQ(shaping.limit, prefix.size());
  EXPECT_TRUE(shaping.boxes.empty());
}

TEST(TextLayoutTest, BlockBoxCentresUnderACentreBlockStyleRange) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  const std::string text = "\n";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = 1000.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 200.0F,
      .heightPx  = 150.0F,
      .placement = gleditor::BoxPlacement::Block,
  });
  opts.blockStyles.push_back(gleditor::BlockStyleRange{
      .start = 0, .end = 1, .align = gleditor::TextAlign::Centre});

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  EXPECT_FLOAT_EQ(shaping.boxes.front().left, (500.0F - 200.0F) / 2.0F);
}

namespace {

// A page-worth of word-wrapping text, its own anchor byte at the very
// front for a FloatLeft/FloatRight box to attach to -- long enough that
// some lines fall entirely within the float's vertical extent and some
// fall entirely below it.
std::string floatWrapFixture() {
  std::string words = "\n";
  for (int i = 0; i < 60; i++) {
    words += "wordword ";
  }
  return words;
}

} // namespace

TEST(TextLayoutTest, TextWrapsBesideAFloat) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string text = floatWrapFixture();
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = lineHeight * 20.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 200.0F,
      .heightPx  = lineHeight * 3.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  const auto &placedFloat = shaping.boxes.front();
  EXPECT_FLOAT_EQ(placedFloat.left, 0.0F);
  EXPECT_FLOAT_EQ(placedFloat.width, 200.0F);

  // Every real line (skipping the float's own zero-width anchor line) whose
  // vertical span overlaps the float is pushed right of it and narrowed to
  // what remains of the column.
  bool sawNarrowedLine    = false;
  const float floatBottom = placedFloat.top + placedFloat.height;
  for (const auto &line : shaping.lines) {
    if (line.barWidth > 0.0F && line.top < floatBottom) {
      EXPECT_FLOAT_EQ(line.left, 200.0F);
      EXPECT_LE(line.barWidth, 300.0F);
      sawNarrowedLine = true;
    }
  }
  EXPECT_TRUE(sawNarrowedLine);
}

TEST(TextLayoutTest, FloatRetiresBelowItsBottom) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string text = floatWrapFixture();
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = lineHeight * 20.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 200.0F,
      .heightPx  = lineHeight * 3.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  const auto &placedFloat = shaping.boxes.front();
  const float floatBottom = placedFloat.top + placedFloat.height;

  // Once a line's span is entirely past the float's bottom, the band is back
  // to the full column -- the float does not narrow anything below it.
  bool sawFullWidthLine = false;
  for (const auto &line : shaping.lines) {
    if (line.top >= floatBottom) {
      EXPECT_FLOAT_EQ(line.left, 0.0F);
      sawFullWidthLine = true;
    }
  }
  EXPECT_TRUE(sawFullWidthLine);
}

TEST(TextLayoutTest, TwoFloatsStackBesideEachOtherWhenTheColumnHasRoom) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  // Two anchor bytes back to back, then real text so the page has a normal
  // line too.
  const std::string text = "\n\nHello";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = lineHeight * 20.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 150.0F,
      .heightPx  = lineHeight * 2.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 1,
      .widthPx   = 150.0F,
      .heightPx  = lineHeight * 2.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 2u);
  // 150 + 150 = 300, comfortably inside the 500px column -- the second float
  // sits beside the first at the same top rather than dropping below it.
  EXPECT_FLOAT_EQ(shaping.boxes[0].top, 0.0F);
  EXPECT_FLOAT_EQ(shaping.boxes[0].left, 0.0F);
  EXPECT_FLOAT_EQ(shaping.boxes[1].top, 0.0F);
  EXPECT_FLOAT_EQ(shaping.boxes[1].left, 150.0F);
}

TEST(TextLayoutTest, ASecondFloatThatDoesNotFitBesideTheFirstDropsBelowIt) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string text = "\n\nHello";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = lineHeight * 20.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 150.0F,
      .heightPx  = lineHeight * 2.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });
  // 150 + 400 > 500 -- does not fit beside the first, so it drops below the
  // first float's own bottom instead.
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 1,
      .widthPx   = 400.0F,
      .heightPx  = lineHeight * 2.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 2u);
  EXPECT_FLOAT_EQ(shaping.boxes[0].top, 0.0F);
  EXPECT_FLOAT_EQ(shaping.boxes[1].top, lineHeight * 2.0F);
  EXPECT_FLOAT_EQ(shaping.boxes[1].left, 0.0F);
}

TEST(TextLayoutTest, OverWideFloatClampsToTheColumn) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string text = "\nHello";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = lineHeight * 20.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 700.0F, // wider than the column
      .heightPx  = lineHeight * 2.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  EXPECT_FLOAT_EQ(shaping.boxes.front().width, 500.0F);
  EXPECT_FLOAT_EQ(shaping.boxes.front().left, 0.0F);
}

TEST(TextLayoutTest, ALineWithNoUsableBandPushesPastTheShorterFloat) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string text = "\n\nSome text here";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = lineHeight * 20.0F};
  // A left float and a right float that together consume the whole column
  // for the first line's worth of height -- the right one retires after one
  // line, the left one after two.
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 0,
      .widthPx   = 250.0F,
      .heightPx  = lineHeight * 2.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 1,
      .widthPx   = 250.0F,
      .heightPx  = lineHeight * 1.0F,
      .placement = gleditor::BoxPlacement::FloatRight,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 2u);
  // Find the real text line -- the one with actual ink, not one of the two
  // empty anchor placeholders (each of which still covers a nonzero byte
  // range, just with nothing drawn over it).
  const auto realLine =
      std::find_if(shaping.lines.begin(), shaping.lines.end(),
                   [](const auto &ln) { return ln.barWidth > 0.0F; });
  ASSERT_NE(realLine, shaping.lines.end());
  // Pushed past the right float's bottom (one line down), where the left
  // float alone leaves a usable, narrowed band.
  EXPECT_FLOAT_EQ(realLine->top, lineHeight);
  EXPECT_FLOAT_EQ(realLine->left, 250.0F);
}

TEST(TextLayoutTest, FloatMovesWholeToNextPageWhenItDoesNotFit) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string prefix = "Line one\nLine two\nLine three\n";
  const std::string text   = prefix + "\n";

  LayoutOptions opts{
      .maxWidthPx  = 500.0F,
      .maxHeightPx = lineHeight * 3.5F, // room for the 3 lines, not the float
  };
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = static_cast<std::uint32_t>(prefix.size()),
      .widthPx   = 200.0F,
      .heightPx  = 150.0F,
      .placement = gleditor::BoxPlacement::FloatLeft,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  EXPECT_EQ(shaping.lineCount, 3u);
  EXPECT_EQ(shaping.limit, prefix.size());
  EXPECT_TRUE(shaping.boxes.empty());
}

TEST(TextLayoutTest, InlineBoxAdvancesThePenAndIsSkippedByLaterGlyphs) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);

  // 'X' stands in for the anchor byte -- any character works, since an
  // Inline box's own width overrides whatever that byte would otherwise
  // have shaped to, and the assembly pass suppresses its glyph outright.
  const std::string text = "ABXCD";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = 1000.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 2,
      .widthPx   = 50.0F,
      .heightPx  = 10.0F,
      .placement = gleditor::BoxPlacement::Inline,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  // Four real characters (A, B, C, D) -- no glyph or cluster for the
  // suppressed anchor byte.
  EXPECT_EQ(shaping.glyphs.size(), 4u);
  EXPECT_EQ(shaping.clusters.size(), 4u);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  EXPECT_EQ(shaping.boxes.front().anchorByteOffset, 2u);
  EXPECT_FLOAT_EQ(shaping.boxes.front().width, 50.0F);

  const auto glyphFor = [&](const char *chr) {
    return std::find_if(
        shaping.glyphs.begin(), shaping.glyphs.end(),
        [&](const auto &g) { return g.chr == std::string{chr}; });
  };
  const auto aGlyph = glyphFor("A");
  const auto bGlyph = glyphFor("B");
  const auto cGlyph = glyphFor("C");
  ASSERT_NE(aGlyph, shaping.glyphs.end());
  ASSERT_NE(bGlyph, shaping.glyphs.end());
  ASSERT_NE(cGlyph, shaping.glyphs.end());

  // Monospace, so B's own advance over A is exactly one character's width;
  // C should sit that same distance past B, plus the inline box's own
  // width in between.
  const float charWidth = bGlyph->clusterLeft - aGlyph->clusterLeft;
  EXPECT_FLOAT_EQ(cGlyph->clusterLeft, bGlyph->clusterLeft + charWidth + 50.0F);
}

TEST(TextLayoutTest,
     InlineBoxIsPlacedRelativeToTheBaselineWithBaselineOffsetPx) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float ascent = font->metrics().ascent;

  const std::string text = "AXB";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = 1000.0F};
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor           = 1,
      .widthPx          = 30.0F,
      .heightPx         = 40.0F,
      .placement        = gleditor::BoxPlacement::Inline,
      .baselineOffsetPx = 5.0F,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_EQ(shaping.boxes.size(), 1u);
  const auto &placed = shaping.boxes.front();
  // top is the box's own top edge: baseline (line.top + ascent) plus the
  // offset its bottom hangs below it, minus its own height.
  EXPECT_FLOAT_EQ(placed.top, ascent + 5.0F - 40.0F);
  EXPECT_FLOAT_EQ(placed.height, 40.0F);
}

TEST(TextLayoutTest, InlineBoxGrowsTheLinesBarHeightAndPushesTheNextLineDown) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;
  const float ascent     = font->metrics().ascent;

  const std::string text = "AXB\nMore";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = 1000.0F};
  // Hangs far enough below the baseline that ascent + offset exceeds the
  // font's own lineHeight -- the only way an Inline box makes a line
  // taller than it would otherwise be (see the comment in layoutPage()).
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor           = 1,
      .widthPx          = 30.0F,
      .heightPx         = 10.0F,
      .placement        = gleditor::BoxPlacement::Inline,
      .baselineOffsetPx = lineHeight,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_GE(shaping.lines.size(), 2u);
  const float grownHeight = ascent + lineHeight;
  EXPECT_FLOAT_EQ(shaping.lines[0].barHeight, grownHeight);
  // The second line starts exactly where the grown first line ends, not one
  // plain lineHeight down.
  EXPECT_FLOAT_EQ(shaping.lines[1].top, grownHeight);
}

TEST(TextLayoutTest, InlineBoxFlushOnTheBaselineDoesNotGrowBarHeight) {
  auto &fm  = FontManager::instance();
  auto font = fm.getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const float lineHeight = font->metrics().lineHeight;

  const std::string text = "AXB";
  LayoutOptions opts{.maxWidthPx = 500.0F, .maxHeightPx = 1000.0F};
  // baselineOffsetPx defaults to 0 -- sitting on the baseline, same as any
  // ordinary glyph, regardless of how tall the box itself is.
  opts.boxes.push_back(gleditor::LayoutBox{
      .anchor    = 1,
      .widthPx   = 30.0F,
      .heightPx  = 200.0F,
      .placement = gleditor::BoxPlacement::Inline,
  });

  auto shaping = TextLayout::layoutPage(text, font, opts);

  ASSERT_FALSE(shaping.lines.empty());
  EXPECT_FLOAT_EQ(shaping.lines.front().barHeight, lineHeight);
}
