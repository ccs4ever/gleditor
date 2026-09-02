#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "xudu/core/uncommitted_op_log.hpp"

namespace xudu {
namespace {

TEST(UncommittedOpLogTest, EmptyLogReturnsEmptyCompactedList) {
  UncommittedOpLog log;
  EXPECT_TRUE(log.empty());
  EXPECT_EQ(log.size(), 0U);

  const auto compacted = log.compact();
  EXPECT_TRUE(compacted.empty());
}

TEST(UncommittedOpLogTest, CoalescesSingleKeystrokesIntoSingleInsert) {
  UncommittedOpLog log;
  // Simulate typing "Hello World" character by character
  const std::string text = "Hello World";
  for (std::size_t i = 0; i < text.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&text[i], 1));
  }

  EXPECT_EQ(log.size(), 11U);

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "Hello World");
}

TEST(UncommittedOpLogTest, CoalescesConsecutiveBackspacesIntoSingleDelete) {
  UncommittedOpLog log;
  // Simulate hitting backspace 4 times from offset 10 to 6
  log.recordErase(9, "d");
  log.recordErase(8, "c");
  log.recordErase(7, "b");
  log.recordErase(6, "a");

  EXPECT_EQ(log.size(), 4U);

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].kind, OpKind::Delete);
  EXPECT_EQ(compacted[0].at, 6U);
  EXPECT_EQ(compacted[0].length, 4U);
}

TEST(UncommittedOpLogTest, CoalescesForwardDeletesIntoSingleDelete) {
  UncommittedOpLog log;
  // Simulate hitting delete 3 times at offset 5
  log.recordErase(5, "x");
  log.recordErase(5, "y");
  log.recordErase(5, "z");

  EXPECT_EQ(log.size(), 3U);

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].kind, OpKind::Delete);
  EXPECT_EQ(compacted[0].at, 5U);
  EXPECT_EQ(compacted[0].length, 3U);
}

TEST(UncommittedOpLogTest, AnnihilatesTypoSuffix) {
  UncommittedOpLog log;
  // User types "tehst"
  const std::string text = "tehst";
  for (std::size_t i = 0; i < text.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&text[i], 1));
  }
  // User hits backspace twice ("st")
  log.recordErase(4, "t");
  log.recordErase(3, "s");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "teh");
}

TEST(UncommittedOpLogTest, AnnihilatesEntireTypoRun) {
  UncommittedOpLog log;
  // User types "mistake"
  const std::string text = "mistake";
  for (std::size_t i = 0; i < text.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&text[i], 1));
  }
  // User backspaces all 7 characters
  for (int i = 6; i >= 0; --i) {
    log.recordErase(static_cast<std::uint32_t>(i),
                    std::string_view(&text[static_cast<std::size_t>(i)], 1));
  }

  const auto compacted = log.compact();
  EXPECT_TRUE(compacted.empty());
}

TEST(UncommittedOpLogTest, HandlesDisjointInserts) {
  UncommittedOpLog log;
  log.recordInsert(0, "first");
  log.recordInsert(100, "second");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 2U);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "first");
  EXPECT_EQ(compacted[1].at, 100U);
  EXPECT_EQ(compacted[1].text, "second");
}

} // namespace
} // namespace xudu

// Offsets in the log are byte offsets, so coalescing a backspace into a
// preceding insert can cut a multi-byte character in half. It used to: resize
// took the byte count and asked no questions. Splitting is refused now, and
// the ops stay separate -- which is slower and always correct.
TEST(UncommittedOpLogTest, DoesNotCoalesceThroughAMultiByteCharacter) {
  xudu::UncommittedOpLog log;
  // "caf\u00e9" -- five bytes, four characters, the last two bytes are one
  // character.
  const std::string cafe = "caf\xc3\xa9";
  ASSERT_EQ(cafe.size(), 5U);

  log.recordInsert(0, cafe);
  // A backspace deleting only the trailing byte of the e-acute, which is not
  // something a text layer should ever ask for but is what a byte-indexed
  // delete can express.
  log.recordErase(4, "\xa9");

  const auto compacted = log.compact();
  for (const auto &op : compacted) {
    if (op.kind == xudu::OpKind::Insert) {
      // Whatever survived must still be decodable.
      for (std::size_t i = 0; i < op.text.size();) {
        const auto lead   = static_cast<unsigned char>(op.text[i]);
        std::size_t width = 1;
        if ((lead & 0xE0U) == 0xC0U)
          width = 2;
        else if ((lead & 0xF0U) == 0xE0U)
          width = 3;
        else if ((lead & 0xF8U) == 0xF0U)
          width = 4;
        ASSERT_LE(i + width, op.text.size())
            << "compaction left a truncated UTF-8 sequence";
        i += width;
      }
    }
  }
}

// The ordinary case still coalesces: a backspace over an ASCII tail folds
// into the insert, because that cut is on a character boundary.
TEST(UncommittedOpLogTest, StillCoalescesOnACharacterBoundary) {
  xudu::UncommittedOpLog log;
  log.recordInsert(0, "hello");
  log.recordErase(4, "o");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].kind, xudu::OpKind::Insert);
  EXPECT_EQ(compacted[0].text, "hell");
}
