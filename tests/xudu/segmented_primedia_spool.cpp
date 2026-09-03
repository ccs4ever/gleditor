/**
 * @file segmented_primedia_spool.cpp
 * @brief Tests for contiguous segmented virtual memory primedia spool.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <xudu/core/segmented_primedia_spool.hpp>

namespace {

using xudu::PrimediaSpan;
using xudu::SegmentedPrimediaSpool;

TEST(SegmentedPrimediaSpoolTest, appendAndReadLocalSpans) {
  SegmentedPrimediaSpool spool;
  const auto span1 = spool.append("Hello, ");
  const auto span2 = spool.append("World!");

  EXPECT_EQ(span1.start, 0U);
  EXPECT_EQ(span1.length, 7U);
  EXPECT_EQ(span2.start, 7U);
  EXPECT_EQ(span2.length, 6U);
  EXPECT_EQ(spool.size(), 13U);

  EXPECT_EQ(spool.read(span1), "Hello, ");
  EXPECT_EQ(spool.read(span2), "World!");
  EXPECT_EQ(spool.readView(span1), "Hello, ");
  EXPECT_EQ(spool.readView(span2), "World!");
  EXPECT_EQ(spool.bytes(), "Hello, World!");

  const auto combined = PrimediaSpan{xudu::localScroll, 0, 13};
  EXPECT_EQ(spool.read(combined), "Hello, World!");
}

TEST(SegmentedPrimediaSpoolTest, multiSegmentContinuityAcrossSeals) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_primedia_seal_test";
  std::filesystem::create_directories(tempDir);
  const auto seg1Path = tempDir / "seg1.spool";

  // Create segment 1 file
  const std::string text1 = "The first sealed torrent segment of text. ";
  {
    std::ofstream out(seg1Path, std::ios::binary | std::ios::trunc);
    out << text1;
  }

  SegmentedPrimediaSpool spool;
  ASSERT_TRUE(spool.addSealedSegment(seg1Path));
  EXPECT_EQ(spool.size(), text1.size());

  // Append new text to the active part
  const std::string text2 = "The second active stretch of text.";
  const auto span2        = spool.append(text2);

  EXPECT_EQ(span2.start, text1.size());
  EXPECT_EQ(span2.length, text2.size());
  EXPECT_EQ(spool.size(), text1.size() + text2.size());

  // Verify full span reading across segment boundaries
  const auto fullSpan =
      PrimediaSpan{xudu::localScroll, 0, text1.size() + text2.size()};
  EXPECT_EQ(spool.read(fullSpan), text1 + text2);
  EXPECT_EQ(spool.readView(fullSpan), text1 + text2);

  std::filesystem::remove_all(tempDir);
}

TEST(SegmentedPrimediaSpoolTest, appendAndReadBinaryMediaPayload) {
  SegmentedPrimediaSpool spool;
  const std::vector<std::uint8_t> pngBytes = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
      0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
  };

  const auto span = spool.append(pngBytes);
  EXPECT_EQ(span.start, 0U);
  EXPECT_EQ(span.length, pngBytes.size());
  EXPECT_EQ(spool.size(), pngBytes.size());

  const std::string readBack = spool.read(span);
  ASSERT_EQ(readBack.size(), pngBytes.size());
  EXPECT_EQ(std::memcmp(readBack.data(), pngBytes.data(), pngBytes.size()), 0);
}

} // namespace
