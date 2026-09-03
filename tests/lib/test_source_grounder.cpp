/**
 * @file test_source_grounder.cpp
 * @brief Unit tests for SourceGrounder subspan matching and SHA-256 computation.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <gleditor/source_grounder.hpp>

using gleditor::SourceGrounder;
using gleditor::SubspanMatch;

TEST(SourceGrounderTest, LocateExactTextSubspan) {
  const std::string doc =
      "Project Xanadu is the original hypertext project founded in 1960 by Ted "
      "Nelson.\nTransclusion is the inclusion of part of a document into "
      "another document by reference.";
  const std::string quote = "Transclusion is the inclusion of part of a document";

  const auto match = SourceGrounder::locateTextSubspan(doc, quote);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->offset, 80U);
  EXPECT_EQ(match->length, quote.size());
  EXPECT_EQ(doc.substr(match->offset, match->length), quote);
  EXPECT_EQ(match->mimeType, "text/plain");
}

TEST(SourceGrounderTest, LocateNormalizedWhitespaceSubspan) {
  const std::string doc =
      "Parallel dimensions\n  in ZigZag allow multiple orthogonal\n  "
      "connections between cells.";
  // Quote has different whitespace formatting (single spaces instead of newlines and indents)
  const std::string quote = "in ZigZag allow multiple orthogonal connections";

  const auto match = SourceGrounder::locateTextSubspan(doc, quote);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(doc.substr(match->offset, match->length),
            "in ZigZag allow multiple orthogonal\n  connections");
}

TEST(SourceGrounderTest, RejectUnmatchedQuote) {
  const std::string doc = "Only the registered content resides here.";
  const std::string quote = "This text never existed in the parent work.";

  const auto match = SourceGrounder::locateTextSubspan(doc, quote);
  EXPECT_FALSE(match.has_value());
}

TEST(SourceGrounderTest, LocateBinarySubspan) {
  const std::vector<std::uint8_t> parent = {0x89, 0x50, 0x4E, 0x47, 0x0D,
                                            0x0A, 0x1A, 0x0A, 0x00, 0x00,
                                            0x00, 0x0D, 0x49, 0x48, 0x44,
                                            0x52};
  const std::vector<std::uint8_t> excerpt = {0x00, 0x00, 0x00, 0x0D, 0x49, 0x48};

  const auto match = SourceGrounder::locateBinarySubspan(parent, excerpt);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->offset, 8U);
  EXPECT_EQ(match->length, excerpt.size());
}

TEST(SourceGrounderTest, ComputeSha256) {
  const std::string text = "xanadu";
  const std::string expected =
      "99546aa80b4aff4c96a297de944fc891d387ea03ac84a526d9c2c4d053acfd4c";
  EXPECT_EQ(SourceGrounder::computeSha256(text), expected);
}
