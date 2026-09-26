/**
 * @file caret_motion.cpp
 * @brief Where the caret goes for a key, from the text and a page's shaping.
 */
#include <gtest/gtest.h>

#include <string_view>

#include <gleditor/caret_motion.hpp>
#include <gleditor/doc.hpp>

namespace {

using gleditor::offsetOnLine;
using gleditor::placeOnLines;
using gleditor::stepCharacter;
using gleditor::stepWord;

TEST(CaretMotionTest, aCharacterStepNeverSplitsACodePoint) {
  // "é" is two bytes and "€" three: a step lands on either side of each.
  constexpr std::string_view text = "aé€b";
  EXPECT_EQ(stepCharacter(text, 0, true), 1U);
  EXPECT_EQ(stepCharacter(text, 1, true), 3U);
  EXPECT_EQ(stepCharacter(text, 3, true), 6U);
  EXPECT_EQ(stepCharacter(text, 6, false), 3U);
  EXPECT_EQ(stepCharacter(text, 3, false), 1U);
  EXPECT_EQ(stepCharacter(text, 0, false), 0U);
  EXPECT_EQ(stepCharacter(text, 7, true), 7U);
}

TEST(CaretMotionTest, aWordStepSkipsSpaceThenAWord) {
  constexpr std::string_view text = "one  two\nthree";
  EXPECT_EQ(stepWord(text, 0, true), 5U);
  EXPECT_EQ(stepWord(text, 5, true), 9U);
  EXPECT_EQ(stepWord(text, 9, false), 5U);
  EXPECT_EQ(stepWord(text, 5, false), 0U);
  EXPECT_EQ(stepWord(text, 3, false), 0U);
}

/// Two lines, "ab\n" and "cdef", one glyph per byte ten pixels wide.
PageShaping twoLines() {
  PageShaping shaping;
  constexpr std::uint32_t starts[] = {0, 1, 3, 4, 5, 6};
  constexpr std::size_t lines[]    = {0, 0, 1, 1, 1, 1};
  constexpr float lefts[]          = {0, 10, 0, 10, 20, 30};
  for (std::size_t i = 0; i < 6; ++i) {
    shaping.clusters.push_back(
        {.byteStart = starts[i], .byteLength = 1, .charCount = 1});
    shaping.glyphs.push_back({.clusterLeft  = lefts[i],
                              .clusterTop   = 0.0F,
                              .clusterIndex = i,
                              .lineIndex    = lines[i]});
  }
  shaping.lines.push_back({.barWidth   = 20.0F,
                           .barHeight  = 10.0F,
                           .left       = 0.0F,
                           .top        = 0.0F,
                           .lineIndex  = 0,
                           .byteStart  = 0,
                           .byteLength = 3});
  shaping.lines.push_back({.barWidth   = 40.0F,
                           .barHeight  = 10.0F,
                           .left       = 0.0F,
                           .top        = 10.0F,
                           .lineIndex  = 1,
                           .byteStart  = 3,
                           .byteLength = 4});
  shaping.lineCount = 2;
  return shaping;
}

TEST(CaretMotionTest, aByteIsPlacedOnItsLineAtItsGlyph) {
  const auto shaping = twoLines();
  EXPECT_EQ(placeOnLines(shaping, 1).line, 0U);
  EXPECT_FLOAT_EQ(placeOnLines(shaping, 1).x, 10.0F);
  EXPECT_EQ(placeOnLines(shaping, 5).line, 1U);
  EXPECT_FLOAT_EQ(placeOnLines(shaping, 5).x, 20.0F);
  // The newline ending a line, and the end of the text, are that line's end.
  EXPECT_EQ(placeOnLines(shaping, 2).line, 0U);
  EXPECT_FLOAT_EQ(placeOnLines(shaping, 2).x, 20.0F);
  EXPECT_FLOAT_EQ(placeOnLines(shaping, 7).x, 40.0F);
}

TEST(CaretMotionTest, movingToALineKeepsTheColumnOrStopsAtItsEnd) {
  const auto shaping              = twoLines();
  constexpr std::string_view text = "ab\ncdef";
  // Down from "b" keeps its column; up from "f" stops at the end of "ab",
  // before its newline rather than past it onto the next line.
  EXPECT_EQ(offsetOnLine(shaping, text, 1, 10.0F), 4U);
  EXPECT_EQ(offsetOnLine(shaping, text, 0, 30.0F), 2U);
  EXPECT_EQ(offsetOnLine(shaping, text, 0, 0.0F), 0U);
}

} // namespace
