/**
 * @file utf8.cpp
 * @brief Moving a byte offset onto a character boundary.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include <gleditor/utf8.hpp>

namespace {

using gleditor::alignToCharacterEnd;
using gleditor::alignToCharacterStart;

/// "hello" with an e-acute: h, C3 A9, l, l, o. The acute occupies bytes 1 and
/// 2, so offset 2 is the one that lands inside a character.
const std::string accented = "h\xC3\xA9llo";
/// A four-byte character: an emoji, occupying bytes 0 through 3.
const std::string emoji = "\xF0\x9F\x99\x82!";

TEST(Utf8Test, anOffsetOnABoundaryDoesNotMove) {
  for (const std::uint32_t offset : {0U, 1U, 3U, 4U, 5U}) {
    EXPECT_EQ(alignToCharacterStart(accented, offset), offset) << offset;
    EXPECT_EQ(alignToCharacterEnd(accented, offset), offset) << offset;
  }
}

TEST(Utf8Test, theStartMovesBackOntoTheCharacter) {
  EXPECT_EQ(alignToCharacterStart(accented, 2), 1U);
}

TEST(Utf8Test, theEndMovesForwardPastTheCharacter) {
  EXPECT_EQ(alignToCharacterEnd(accented, 2), 3U);
}

TEST(Utf8Test, theTwoDirectionsBracketTheWholeCharacter) {
  // The property an edit depends on: a range naming any byte of a character
  // widens to the whole of it rather than to nothing.
  const auto start = alignToCharacterStart(accented, 2);
  const auto end   = alignToCharacterEnd(accented, 2 + 1);
  EXPECT_EQ(accented.substr(start, end - start), "\xC3\xA9");
}

TEST(Utf8Test, aFourByteCharacterIsCrossedInOneStep) {
  EXPECT_EQ(alignToCharacterStart(emoji, 3), 0U);
  EXPECT_EQ(alignToCharacterEnd(emoji, 1), 4U);
}

TEST(Utf8Test, anOffsetPastTheEndIsLeftAlone) {
  // Neither direction may walk off the end of the buffer looking for a
  // boundary that is not there.
  EXPECT_EQ(alignToCharacterStart(accented, 99), 99U);
  EXPECT_EQ(alignToCharacterEnd(accented, 99), 99U);
  EXPECT_EQ(alignToCharacterStart(accented, 6), 6U);
  EXPECT_EQ(alignToCharacterEnd(accented, 6), 6U);
}

TEST(Utf8Test, emptyTextIsNotSearched) {
  EXPECT_EQ(alignToCharacterStart("", 0), 0U);
  EXPECT_EQ(alignToCharacterEnd("", 0), 0U);
}

TEST(Utf8Test, aLeadingContinuationByteTerminates) {
  // Malformed input: text beginning mid-character, which a caller slicing a
  // buffer can produce. Moving back must stop at zero rather than underflow.
  const std::string truncated = "\xA9llo";
  EXPECT_EQ(alignToCharacterStart(truncated, 0), 0U);
  EXPECT_EQ(alignToCharacterEnd(truncated, 0), 1U);
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
