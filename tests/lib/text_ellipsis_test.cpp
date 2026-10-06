#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {
using namespace gleditor;
using namespace gleditor::text;

bool markerPresent(const PageShaping &page) {
  return std::ranges::any_of(page.glyphs,
                             [](const auto &g) { return g.chr == "…"; });
}

TEST(TextEllipsisTest, SingleLineDoesNotWrapAndMarkerConsumesNoSource) {
  const auto font        = FontManager::instance().getFont("Sans 16");
  const std::string text = "A long label that used to wrap onto several lines";
  const auto page = TextLayout::layoutSingleLine(text, font, 100.0F, true);
  ASSERT_EQ(page.lineCount, 1U);
  EXPECT_LE(page.lines.front().barWidth, 100.0F);
  EXPECT_LT(page.limit, text.size());
  ASSERT_TRUE(markerPresent(page));
  for (const auto &glyph : page.glyphs) {
    const auto &cluster = page.clusters[glyph.clusterIndex];
    if (glyph.chr == "…") {
      EXPECT_EQ(cluster.byteLength, 0U);
    } else {
      EXPECT_EQ(glyph.chr, text.substr(cluster.byteStart, cluster.byteLength));
    }
  }
}

TEST(TextEllipsisTest, LastVisibleLineEllipsizesButPaginationStillContinues) {
  const auto font        = FontManager::instance().getFont("Sans 16");
  const std::string text = "one two three four five six seven eight nine ten";
  LayoutOptions options{.maxWidthPx  = 100.0F,
                        .maxHeightPx = font->metrics().lineHeight * 2.1F};
  const auto continued = TextLayout::layoutPage(text, font, options);
  EXPECT_FALSE(markerPresent(continued));
  ASSERT_GT(continued.limit, 0U);
  ASSERT_LT(continued.limit, text.size());
  EXPECT_FALSE(
      TextLayout::layoutPage(std::string_view{text}.substr(continued.limit),
                             font, options)
          .glyphs.empty());
  options.ellipsize = true;
  const auto label  = TextLayout::layoutPage(text, font, options);
  EXPECT_LE(label.lineCount, 2U);
  EXPECT_LE(label.textHeightPx, options.maxHeightPx + 1.0F);
  ASSERT_TRUE(markerPresent(label));
  for (const auto &g : label.glyphs) {
    if (g.chr == "…") {
      EXPECT_EQ(g.lineIndex, label.lineCount - 1);
    }
  }
}

TEST(TextEllipsisTest, DecorationsAndNamedPageSurviveFitting) {
  const auto font = FontManager::instance().getFont("Sans 16");
  LayoutOptions options{.maxWidthPx      = 100.0F,
                        .maxHeightPx     = 0.0F,
                        .singleParagraph = true,
                        .ellipsize       = true};
  options.decoratedRanges = {
      {.start = 0, .end = 100, .decorations = decorationBit(Decoration::Bold)}};
  options.page.widthPx = 300.0F;
  const auto page      = TextLayout::layoutPage(
      "A long decorated label needing ellipsis", font, options);
  EXPECT_EQ(page.page, options.page);
  ASSERT_TRUE(markerPresent(page));
  for (const auto &g : page.glyphs) {
    EXPECT_TRUE(hasDecoration(g.decorations, Decoration::Bold));
  }
}

TEST(TextEllipsisTest, StyledLineKeepsItsIndentAndFitsMarker) {
  const auto font = FontManager::instance().getFont("Sans 16");
  LayoutOptions options{.maxWidthPx      = 120.0F,
                        .maxHeightPx     = 0.0F,
                        .singleParagraph = true,
                        .ellipsize       = true};
  options.blockStyles = {{.start = 0, .end = 1000, .indentLeftPx = 10.0F}};
  const auto page = TextLayout::layoutPage("A long styled label with an indent",
                                           font, options);
  ASSERT_EQ(page.lineCount, 1U);
  EXPECT_GE(page.lines.front().left, 10.0F);
  EXPECT_LE(page.lines.front().left + page.lines.front().barWidth, 120.0F);
  EXPECT_TRUE(markerPresent(page));
}

TEST(TextEllipsisTest, StyledEllipsisKeepsRightAlignment) {
  const auto font = FontManager::instance().getFont("Sans 16");
  LayoutOptions options{.maxWidthPx      = 120.0F,
                        .maxHeightPx     = 0.0F,
                        .singleParagraph = true,
                        .ellipsize       = true};
  options.blockStyles = {{.start         = 0,
                          .end           = 1000,
                          .align         = TextAlign::Right,
                          .indentRightPx = 7.0F}};
  const auto page     = TextLayout::layoutPage(
      "A long right aligned label requiring ellipsis", font, options);
  ASSERT_EQ(page.lineCount, 1U);
  EXPECT_NEAR(page.lines.front().left + page.lines.front().barWidth, 113.0F,
              0.001F);
  EXPECT_TRUE(markerPresent(page));
}

TEST(TextEllipsisTest, EarlierMediaReservationSurvivesLastLineEllipsis) {
  const auto font = FontManager::instance().getFont("Sans 16");
  LayoutOptions options{.maxWidthPx  = 120.0F,
                        .maxHeightPx = font->metrics().lineHeight * 3.0F,
                        .ellipsize   = true};
  options.boxes = {{.anchor    = 0,
                    .widthPx   = 40.0F,
                    .heightPx  = font->metrics().lineHeight,
                    .placement = BoxPlacement::Block,
                    .id        = 77}};
  const std::string source =
      "\uFFFCone two three four five six seven eight nine ten eleven";
  const auto page = TextLayout::layoutPage(source, font, options);
  ASSERT_EQ(page.boxes.size(), 1U);
  EXPECT_EQ(page.boxes.front().id, 77U);
  EXPECT_EQ(page.boxes.front().anchorByteOffset, 0U);
  EXPECT_TRUE(markerPresent(page));
  EXPECT_LT(page.limit, source.size());
  EXPECT_GT(page.limit, 3U);
  for (const auto &g : page.glyphs) {
    EXPECT_NE(g.chr, "\uFFFC");
    EXPECT_LE(g.clusterLeft, 120.0F);
  }
}
} // namespace
