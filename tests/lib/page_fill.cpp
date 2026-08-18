#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>

namespace {

using namespace gleditor;

struct PageFillTest : testing::Test {};

/// The page font and geometry.
[[nodiscard]] text::FontFacePtr pageFont() {
  return text::FontManager::instance().getFont("Monospace 16");
}

[[nodiscard]] text::LayoutOptions pageOptions() {
  return text::LayoutOptions{
      .maxWidthPx      = 139.70F * 8.5F,
      .maxHeightPx     = 139.70F * 11.0F,
      .singleParagraph = false,
      .ellipsize       = true,
  };
}

/// @p bytes of text with no line break anywhere in it.
[[nodiscard]] std::string unbrokenText(const std::size_t bytes) {
  const std::string sentence =
      "The quick brown fox jumped over the lazy dog, and then considered "
      "at some length whether the exercise had been worth the trouble. ";
  std::string out;
  out.reserve(bytes + sentence.size());
  while (out.size() < bytes) {
    out += sentence;
  }
  out.resize(bytes);
  return out;
}

TEST_F(PageFillTest, aDocumentWithNoLineBreaksStillWraps) {
  const auto inputText = unbrokenText(256 * 1024);
  const auto font      = pageFont();
  const auto page =
      text::TextLayout::layoutPage(inputText, font, pageOptions());

  EXPECT_GT(page.lineCount, 1);
  EXPECT_GT(page.lineCount, 20);
}

TEST_F(PageFillTest, thePageFillsUp) {
  const auto inputText = unbrokenText(256 * 1024);
  const auto font      = pageFont();
  const auto page =
      text::TextLayout::layoutPage(inputText, font, pageOptions());

  EXPECT_GT(page.limit, 0);
  EXPECT_LT(page.limit, inputText.size());
}

TEST_F(PageFillTest, aShortDocumentIsTakenWhole) {
  const std::string inputText = "The quick brown fox.\n";
  const auto font             = pageFont();
  const auto page =
      text::TextLayout::layoutPage(inputText, font, pageOptions());

  EXPECT_EQ(page.limit, inputText.size());
}

TEST_F(PageFillTest, shapingTheSameBytesTwiceGivesTheSameAnswers) {
  const auto inputText = unbrokenText(64 * 1024);
  const auto font      = pageFont();
  const auto first =
      text::TextLayout::layoutPage(inputText, font, pageOptions());
  const auto second =
      text::TextLayout::layoutPage(inputText, font, pageOptions());

  ASSERT_EQ(first.limit, second.limit);
  ASSERT_EQ(first.lineCount, second.lineCount);
  ASSERT_EQ(first.glyphs.size(), second.glyphs.size());
  for (std::size_t i = 0; i < first.glyphs.size(); i++) {
    EXPECT_EQ(first.glyphs[i].chr, second.glyphs[i].chr);
    EXPECT_FLOAT_EQ(first.glyphs[i].clusterLeft, second.glyphs[i].clusterLeft);
    EXPECT_FLOAT_EQ(first.glyphs[i].clusterTop, second.glyphs[i].clusterTop);
  }
}

} // namespace
