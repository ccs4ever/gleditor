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

TEST(SegmentedPrimediaSpoolTest, LifecycleAndActiveSegments) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_primedia_lifecycle_test";
  std::filesystem::create_directories(tempDir);
  const auto active1 = tempDir / "active1.spool";
  const auto active2 = tempDir / "active2.spool";

  SegmentedPrimediaSpool spool;
  EXPECT_TRUE(spool.openActiveSegment(active1));
  spool.append("Active segment text 1.");
  EXPECT_TRUE(spool.flush());

  // Seal active segment and rotate to active2
  EXPECT_TRUE(spool.sealActive(active2));
  spool.append("Active segment text 2.");
  EXPECT_TRUE(spool.flush());

  // Move constructor
  SegmentedPrimediaSpool moved(std::move(spool));
  EXPECT_GT(moved.size(), 0U);

  // Move assignment
  SegmentedPrimediaSpool assigned;
  assigned = std::move(moved);
  EXPECT_GT(assigned.size(), 0U);

  std::filesystem::remove_all(tempDir);
}

TEST(SegmentedPrimediaSpoolTest, AdoptAndFlush) {
  SegmentedPrimediaSpool spool;
  spool.append("Initial text");
  EXPECT_EQ(spool.bytes(), "Initial text");

  // Adopt replaces contents
  spool.adopt("Replaced text");
  EXPECT_EQ(spool.bytes(), "Replaced text");
  EXPECT_EQ(spool.size(), 13U);
  EXPECT_TRUE(spool.flush());
}

TEST(SegmentedPrimediaSpoolTest, ErrorsAndBounds) {
  SegmentedPrimediaSpool spool;
  spool.append("Sample text");

  // Non-local span throws
  PrimediaSpan nonLocal{42U, 0, 5};
  EXPECT_THROW(static_cast<void>(spool.read(nonLocal)), std::runtime_error);
  EXPECT_TRUE(spool.readView(nonLocal).empty());

  // Empty span
  PrimediaSpan emptySpan{xudu::localScroll, 0, 0};
  EXPECT_EQ(spool.read(emptySpan), "");
  EXPECT_EQ(spool.readView(emptySpan), "");

  // Out of bounds span
  PrimediaSpan oobSpan{xudu::localScroll, 1000, 50};
  EXPECT_EQ(spool.read(oobSpan), "");
  EXPECT_EQ(spool.readView(oobSpan), "");

  // Empty append returns 0-length span
  auto emptyAppended = spool.append("");
  EXPECT_EQ(emptyAppended.length, 0U);

  // Non-existent sealed segment fails
  EXPECT_FALSE(spool.addSealedSegment("/non/existent/path/xyz_123.bin"));
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

// -- the active segment, which is the permascroll's persistence --------------
//
// A store is an edit decision list and holds no primedia of its own, so this
// file is the only copy of what an author typed. It did nothing for a while:
// append() writes into anonymous pages, flush() msync'd them (which writes
// nowhere) and then fsync'd a file no byte had reached, and openActiveSegment()
// created the file without ever reading it back. Every one of these covers a
// half of that.

struct ActiveSegmentTest : testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    dir =
        std::filesystem::temp_directory_path() /
        ("xudu-primedia-" +
         std::string(
             ::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
};

TEST_F(ActiveSegmentTest, whatWasAppendedComesBackInANewSpool) {
  const auto path = dir / "active.primedia";
  {
    SegmentedPrimediaSpool spool;
    ASSERT_TRUE(spool.openActiveSegment(path));
    spool.append("the quick brown fox");
    ASSERT_TRUE(spool.flush());
  }

  SegmentedPrimediaSpool reopened;
  ASSERT_TRUE(reopened.openActiveSegment(path));
  EXPECT_EQ(reopened.size(), 19U);
  EXPECT_EQ(reopened.bytes(), "the quick brown fox");
}

TEST_F(ActiveSegmentTest, aSpanKeepsItsAddressAcrossASession) {
  // The whole point of persisting at all: an operation recorded in one session
  // names a primedia address, and the document is only still readable in the
  // next one if that address means the same thing.
  const auto path = dir / "active.primedia";
  PrimediaSpan quoted;
  {
    SegmentedPrimediaSpool spool;
    ASSERT_TRUE(spool.openActiveSegment(path));
    spool.append("see: ");
    quoted = spool.append("quick");
    ASSERT_TRUE(spool.flush());
  }

  SegmentedPrimediaSpool reopened;
  ASSERT_TRUE(reopened.openActiveSegment(path));
  EXPECT_EQ(reopened.read(quoted), "quick");
}

TEST_F(ActiveSegmentTest, appendingAfterAReopenContinuesTheSameAddresses) {
  const auto path = dir / "active.primedia";
  {
    SegmentedPrimediaSpool spool;
    ASSERT_TRUE(spool.openActiveSegment(path));
    spool.append("one");
    ASSERT_TRUE(spool.flush());
  }

  PrimediaSpan second;
  {
    SegmentedPrimediaSpool spool;
    ASSERT_TRUE(spool.openActiveSegment(path));
    second = spool.append(" two");
    // Past what the file already held, not over it.
    EXPECT_EQ(second.start, 3U);
    ASSERT_TRUE(spool.flush());
  }

  SegmentedPrimediaSpool reopened;
  ASSERT_TRUE(reopened.openActiveSegment(path));
  EXPECT_EQ(reopened.bytes(), "one two");
  EXPECT_EQ(reopened.read(second), " two");
}

TEST_F(ActiveSegmentTest, flushingTwiceWritesOnlyTheTail) {
  // A flush costs what was typed since the last one, not the length of the
  // document. A whole-spool rewrite would still pass every assertion above,
  // so this checks the file rather than what it reads as.
  const auto path = dir / "active.primedia";
  SegmentedPrimediaSpool spool;
  ASSERT_TRUE(spool.openActiveSegment(path));
  spool.append("one");
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(path), 3U);
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(path), 3U)
      << "a flush with nothing appended since the last one wrote again";
  spool.append(" two");
  ASSERT_TRUE(spool.flush());
  EXPECT_EQ(std::filesystem::file_size(path), 7U);
}

TEST_F(ActiveSegmentTest, nothingIsDurableUntilItIsFlushed) {
  const auto path = dir / "active.primedia";
  SegmentedPrimediaSpool spool;
  ASSERT_TRUE(spool.openActiveSegment(path));
  spool.append("typed but not flushed");
  EXPECT_EQ(std::filesystem::file_size(path), 0U);
}

} // namespace
