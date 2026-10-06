#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/text/fit.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>
#include <gtest/gtest.h>

namespace {

using namespace gleditor;
using namespace gleditor::text;

bool validUtf8(const std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80U) {
      continue;
    }
    unsigned count{};
    std::uint32_t value{};
    std::uint32_t minimum{};
    if (first >= 0xC2U && first <= 0xDFU) {
      count   = 1;
      value   = first & 0x1FU;
      minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      count   = 2;
      value   = first & 0x0FU;
      minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      count   = 3;
      value   = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (text.size() - i < count) {
      return false;
    }
    for (unsigned j = 0; j < count; ++j) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xC0U) != 0x80U) {
        return false;
      }
      value = (value << 6U) | (next & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
      return false;
    }
  }
  return true;
}

bool hasEllipsis(const PageShaping &shaping) {
  return std::ranges::any_of(shaping.glyphs, [](const auto &glyph) {
    return glyph.chr == "…" || glyph.chr == "...";
  });
}

void expectWithinWidth(const FittedText &fitted, const float width) {
  constexpr float tolerance = 0.001F;
  EXPECT_LE(fitted.widthPx, width + tolerance);
  EXPECT_GE(fitted.widthPx, 0.0F);
  EXPECT_TRUE(std::isfinite(fitted.widthPx));
  EXPECT_EQ(fitted.lines, fitted.shaping.lineCount);
  EXPECT_EQ(fitted.lines, fitted.shaping.lines.size());
  for (const auto &line : fitted.shaping.lines) {
    EXPECT_GE(line.left, -tolerance);
    EXPECT_LE(line.left + line.barWidth, width + tolerance);
  }
  for (const auto &glyph : fitted.shaping.glyphs) {
    EXPECT_TRUE(validUtf8(glyph.chr)) << "cluster at " << glyph.clusterIndex;
    EXPECT_LT(glyph.clusterIndex, fitted.shaping.clusters.size());
    EXPECT_LT(glyph.lineIndex, fitted.shaping.lineCount);
  }
}

class TextFitTest : public testing::Test {
protected:
  FontFacePtr font = FontManager::instance().getFont("Sans 16");

  void SetUp() override { ASSERT_NE(font, nullptr); }
};

TEST_F(TextFitTest, EmptyTextHasNoVisibleSourceOrTruncation) {
  const auto fitted = fit({}, font, {});
  EXPECT_EQ(fitted.visibleBytes, 0U);
  EXPECT_FALSE(fitted.truncated);
  EXPECT_EQ(fitted.widthPx, 0.0F);
  EXPECT_TRUE(fitted.shaping.glyphs.empty());
}

TEST_F(TextFitTest, ZeroWidthMeansUnboundedAndPreservesTheSource) {
  const std::string text = "A long label with combining é and العربية";
  const auto fitted      = fit(text, font, {.maxWidthPx = 0.0F});
  EXPECT_FALSE(fitted.truncated);
  EXPECT_EQ(fitted.visibleBytes, text.size());
  EXPECT_EQ(fitted.lines, 1U);
  EXPECT_GT(fitted.widthPx, 0.0F);
  EXPECT_FALSE(hasEllipsis(fitted.shaping));
}

TEST_F(TextFitTest, WidthSweepIncludesSubpixelAndNarrowerThanEllipsis) {
  const std::string text = "A deliberately long label to ellipsize";
  const auto natural     = fit(text, font, {});
  const int widest       = static_cast<int>(std::ceil(natural.widthPx)) + 2;
  for (const float width : {0.01F, 0.5F, 0.99F}) {
    SCOPED_TRACE(width);
    const auto fitted = fit(text, font, {.maxWidthPx = width});
    expectWithinWidth(fitted, width);
    EXPECT_TRUE(fitted.truncated);
    EXPECT_EQ(fitted.visibleBytes, 0U);
  }
  for (int width = 1; width <= widest; ++width) {
    SCOPED_TRACE(width);
    const auto fitted =
        fit(text, font, {.maxWidthPx = static_cast<float>(width)});
    expectWithinWidth(fitted, static_cast<float>(width));
    EXPECT_LE(fitted.lines, 1U);
    EXPECT_LE(fitted.visibleBytes, text.size());
    if (static_cast<float>(width) >= natural.widthPx) {
      EXPECT_FALSE(fitted.truncated);
      EXPECT_EQ(fitted.visibleBytes, text.size());
    }
  }
}

struct UnicodeCase {
  const char *name;
  std::vector<std::string> graphemes;
};

TEST_F(TextFitTest, UnicodeWidthSweepsCutOnlyCompleteGraphemesAndClusters) {
  const std::vector<UnicodeCase> cases{
      {"combining marks", {"é", "ö", "â", "ñ", "é", "ö"}},
      {"ZWJ emoji",
       {"👨‍👩‍👧‍👦", "👩‍💻", "👩🏽‍🚀",
        "👨‍👩‍👧‍👦"}},
      {"Devanagari", {"क्षि", "त्र", "क्षि", "त्र", "क्षि"}},
      {"Arabic RTL",
       {"م", "ر", "ح", "ب", "ا", "ب", "ا", "ل", "ع", "ا", "ل", "م"}},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    std::string text;
    std::set<std::size_t> boundaries{0};
    for (const auto &grapheme : test.graphemes) {
      text += grapheme;
      boundaries.insert(text.size());
    }
    const auto natural = fit(text, font, {});
    const auto shaped  = TextLayout::layoutSingleLine(text, font);
    std::set<std::size_t> clusters{0, text.size()};
    for (const auto &cluster : shaped.clusters) {
      clusters.insert(cluster.byteStart);
    }
    const int widest = static_cast<int>(std::ceil(natural.widthPx));
    for (int width = 1; width <= widest; ++width) {
      SCOPED_TRACE(width);
      const auto fitted =
          fit(text, font, {.maxWidthPx = static_cast<float>(width)});
      expectWithinWidth(fitted, static_cast<float>(width));
      EXPECT_TRUE(boundaries.contains(fitted.visibleBytes))
          << "cut at byte " << fitted.visibleBytes;
      EXPECT_TRUE(clusters.contains(fitted.visibleBytes))
          << "split HarfBuzz cluster at byte " << fitted.visibleBytes;
      EXPECT_TRUE(
          validUtf8(std::string_view{text}.substr(0, fitted.visibleBytes)));
    }
  }
}

TEST_F(TextFitTest, StartAndMiddleEllipsisRetainIdentifierSuffix) {
  const std::string text =
      "prefix/0123456789/abcdefghijklmnopqrstuvwxyz/suffix";
  const auto tail        = fit("…/suffix", font, {});
  const auto headAndTail = fit("prefix/…/suffix", font, {});
  for (const auto at : {EllipsisAt::Start, EllipsisAt::Middle}) {
    SCOPED_TRACE(static_cast<int>(at));
    const float width =
        at == EllipsisAt::Start ? tail.widthPx : headAndTail.widthPx;
    const auto fitted = fit(text, font, {.maxWidthPx = width, .at = at});
    expectWithinWidth(fitted, width);
    EXPECT_TRUE(fitted.truncated);
    ASSERT_TRUE(hasEllipsis(fitted.shaping));
    ASSERT_FALSE(fitted.shaping.glyphs.empty());
    EXPECT_EQ(fitted.shaping.glyphs.back().chr, "x");
    if (at == EllipsisAt::Middle) {
      EXPECT_EQ(fitted.shaping.glyphs.front().chr, "p");
    } else {
      EXPECT_EQ(fitted.shaping.glyphs.front().chr, "…");
    }
  }
}

TEST_F(TextFitTest, ArabicEndEllipsisAppearsAtTheVisualLeft) {
  const std::string text = "مرحبابالعالممرحبابالعالم";
  const float width      = fit(text, font, {}).widthPx * 0.6F;
  const auto fitted      = fit(text, font, {.maxWidthPx = width});
  EXPECT_TRUE(fitted.truncated);
  EXPECT_GT(fitted.visibleBytes, 0U);
  ASSERT_TRUE(hasEllipsis(fitted.shaping));
  const auto ellipsis =
      std::ranges::find_if(fitted.shaping.glyphs,
                           [](const auto &glyph) { return glyph.chr == "…"; });
  ASSERT_NE(ellipsis, fitted.shaping.glyphs.end());
  for (const auto &glyph : fitted.shaping.glyphs) {
    EXPECT_LE(ellipsis->clusterLeft, glyph.clusterLeft + 0.001F);
  }
}

TEST_F(TextFitTest, WrapRespectsMaxLinesAndEllipsizesItsLastLine) {
  const std::string text =
      "One two three four five six seven eight nine ten eleven twelve.";
  const auto fitted =
      fit(text, font,
          {.maxWidthPx = 110.0F, .maxLines = 2, .overflow = Overflow::Wrap});
  expectWithinWidth(fitted, 110.0F);
  EXPECT_EQ(fitted.lines, 2U);
  EXPECT_TRUE(fitted.truncated);
  EXPECT_LT(fitted.visibleBytes, text.size());
  ASSERT_TRUE(hasEllipsis(fitted.shaping));
  for (const auto &glyph : fitted.shaping.glyphs) {
    if (glyph.chr == "…") {
      EXPECT_EQ(glyph.lineIndex, 1U);
    }
  }
}

TEST_F(TextFitTest, HeightLimitWinsOverMaxLines) {
  const float height = font->metrics().lineHeight * 2.1F;
  const auto fitted =
      fit("One two three four five six seven eight nine ten", font,
          {.maxWidthPx  = 90.0F,
           .maxHeightPx = height,
           .maxLines    = 10,
           .overflow    = Overflow::Wrap});
  expectWithinWidth(fitted, 90.0F);
  EXPECT_LE(fitted.lines, 2U);
  EXPECT_LE(fitted.heightPx, height);
  EXPECT_TRUE(fitted.truncated);
}

TEST_F(TextFitTest, WrapDoesNotMarkExactlyFittingLinesAsTruncated) {
  const std::string text = "first\nsecond";
  const auto fitted =
      fit(text, font,
          {.maxWidthPx = 200.0F, .maxLines = 2, .overflow = Overflow::Wrap});
  EXPECT_EQ(fitted.lines, 2U);
  EXPECT_FALSE(fitted.truncated);
  EXPECT_EQ(fitted.visibleBytes, text.size());
  EXPECT_FALSE(hasEllipsis(fitted.shaping));
}

TEST_F(TextFitTest, AlignmentUsesTheConstrainedWidth) {
  for (const auto align : {TextAlign::Centre, TextAlign::Right}) {
    SCOPED_TRACE(static_cast<int>(align));
    const float width = 200.0F;
    const auto fitted =
        fit("label", font, {.maxWidthPx = width, .align = align});
    ASSERT_EQ(fitted.shaping.lines.size(), 1U);
    const auto &line = fitted.shaping.lines.front();
    const float expectedLeft =
        (width - line.barWidth) * (align == TextAlign::Right ? 1.0F : 0.5F);
    EXPECT_NEAR(line.left, expectedLeft, 0.001F);
    EXPECT_FALSE(fitted.truncated);
  }
}

TEST_F(TextFitTest, HeightBelowOneLineProducesNoOverflow) {
  const float height = font->metrics().lineHeight * 0.5F;
  const auto fitted  = fit("Cannot fit one whole line", font,
                           {.maxWidthPx = 300.0F, .maxHeightPx = height});
  EXPECT_TRUE(fitted.truncated);
  EXPECT_EQ(fitted.lines, 0U);
  EXPECT_EQ(fitted.visibleBytes, 0U);
  EXPECT_LE(fitted.heightPx, height);
  EXPECT_TRUE(fitted.shaping.glyphs.empty());
}

TEST_F(TextFitTest, UnboundedLineCountWrapsAllSource) {
  const std::string text = "One two three four five six seven eight nine ten";
  const auto fitted =
      fit(text, font,
          {.maxWidthPx = 100.0F, .maxLines = 0, .overflow = Overflow::Wrap});
  expectWithinWidth(fitted, 100.0F);
  EXPECT_GT(fitted.lines, 1U);
  EXPECT_FALSE(fitted.truncated);
  EXPECT_EQ(fitted.visibleBytes, text.size());
  EXPECT_FALSE(hasEllipsis(fitted.shaping));
}

TEST_F(TextFitTest, ClipUsesNoEllipsisAndKeepsASingleLine) {
  const auto fitted = fit("A long editable text field", font,
                          {.maxWidthPx = 80.0F, .overflow = Overflow::Clip});
  EXPECT_TRUE(fitted.truncated);
  EXPECT_LE(fitted.lines, 1U);
  EXPECT_FALSE(hasEllipsis(fitted.shaping));
}

TEST_F(TextFitTest, StringViewDoesNotReadOrShowSurroundingBytes) {
  const std::string storage = "SECRET_PREFIXhelloSECRET_SUFFIX";
  const std::string_view text{storage.data() + 13, 5};
  const auto fitted = fit(text, font, {});
  const auto plain  = fit("hello", font, {});
  EXPECT_EQ(fitted.visibleBytes, 5U);
  EXPECT_EQ(fitted.widthPx, plain.widthPx);
  ASSERT_EQ(fitted.shaping.glyphs.size(), plain.shaping.glyphs.size());
  for (std::size_t i = 0; i < plain.shaping.glyphs.size(); ++i) {
    EXPECT_EQ(fitted.shaping.glyphs[i].chr, plain.shaping.glyphs[i].chr);
  }
}

TEST_F(TextFitTest, FitOwnsDisplayedTextAfterTheInputIsDestroyed) {
  auto fitted = [&]() {
    const std::string text = "temporary source that needs truncation";
    return fit(text, font, {.maxWidthPx = 100.0F});
  }();
  EXPECT_TRUE(fitted.truncated);
  ASSERT_FALSE(fitted.shaping.glyphs.empty());
  EXPECT_TRUE(hasEllipsis(fitted.shaping));
  for (const auto &glyph : fitted.shaping.glyphs) {
    EXPECT_TRUE(validUtf8(glyph.chr));
  }
}

TEST_F(TextFitTest, MixedBidiPreservesLogicalSourceAndVisualRunOrder) {
  const std::string text = "left العربية right";
  const auto natural     = fit(text, font, {});
  const auto arabicBegin = text.find("العربية");
  const auto arabicEnd   = text.find(" right");
  std::vector<std::size_t> arabicOffsets;
  for (const auto &glyph : natural.shaping.glyphs) {
    const auto &cluster = natural.shaping.clusters[glyph.clusterIndex];
    EXPECT_EQ(glyph.chr, text.substr(cluster.byteStart, cluster.byteLength));
    if (cluster.byteStart >= arabicBegin && cluster.byteStart < arabicEnd) {
      arabicOffsets.push_back(cluster.byteStart);
    }
  }
  ASSERT_GT(arabicOffsets.size(), 1U);
  EXPECT_TRUE(std::ranges::is_sorted(arabicOffsets, std::greater<>{}));
  EXPECT_EQ(natural.shaping.glyphs.front().chr, "l");
  EXPECT_EQ(natural.shaping.glyphs.back().chr, "t");
  for (const auto at :
       {EllipsisAt::Start, EllipsisAt::Middle, EllipsisAt::End}) {
    const auto fitted =
        fit(text, font, {.maxWidthPx = natural.widthPx * 0.6F, .at = at});
    expectWithinWidth(fitted, natural.widthPx * 0.6F);
    EXPECT_TRUE(fitted.truncated);
    for (const auto &glyph : fitted.shaping.glyphs) {
      const auto &cluster = fitted.shaping.clusters[glyph.clusterIndex];
      if (cluster.byteLength == 0) {
        EXPECT_TRUE(glyph.chr == "…" || glyph.chr == "...");
      } else {
        EXPECT_EQ(glyph.chr,
                  text.substr(cluster.byteStart, cluster.byteLength));
      }
    }
  }
}

TEST_F(TextFitTest, FallbackStringViewRemainsWithinItsSourceSlice) {
  const std::string expected = "क्षि";
  const std::string storage  = "private prefix" + expected + "private suffix";
  const std::string_view source{storage.data() + 14, expected.size()};
  const auto bounded     = fit(source, font, {});
  const auto independent = fit(expected, font, {});
  EXPECT_EQ(bounded.visibleBytes, expected.size());
  EXPECT_EQ(bounded.widthPx, independent.widthPx);
  ASSERT_EQ(bounded.shaping.glyphs.size(), independent.shaping.glyphs.size());
  for (std::size_t i = 0; i < bounded.shaping.glyphs.size(); i++) {
    EXPECT_EQ(bounded.shaping.glyphs[i].chr, independent.shaping.glyphs[i].chr);
    EXPECT_TRUE(validUtf8(bounded.shaping.glyphs[i].chr));
  }
}

TEST_F(TextFitTest, EndEllipsisLineRangeExcludesOmittedSource) {
  const auto fitted = fit("A source with a deliberately omitted ending", font,
                          {.maxWidthPx = 130.0F});
  ASSERT_TRUE(fitted.truncated);
  ASSERT_EQ(fitted.shaping.lines.size(), 1U);
  const auto &line = fitted.shaping.lines.front();
  EXPECT_EQ(line.byteStart + line.byteLength, fitted.visibleBytes);
  for (const auto &glyph : fitted.shaping.glyphs) {
    if (glyph.chr == "…" || glyph.chr == "...") {
      EXPECT_EQ(fitted.shaping.clusters[glyph.clusterIndex].byteLength, 0U);
    }
  }
}

} // namespace
