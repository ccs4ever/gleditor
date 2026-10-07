#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/uncommitted_op_log.hpp"

namespace xanadu {
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

TEST(UncommittedOpLogTest, PreservesInsertHistoryWhenBackspacingTypoSuffix) {
  UncommittedOpLog log;
  // User types "tehst"
  const std::string text = "tehst";
  for (std::size_t i = 0; i < text.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&text[i], 1));
  }
  // User hits backspace twice ("st") from where the inserts left off
  log.recordErase(4, "t");
  log.recordErase(3, "s");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 2U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "tehst");
  EXPECT_EQ(compacted[1].kind, OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 3U);
  EXPECT_EQ(compacted[1].length, 2U);
}

TEST(UncommittedOpLogTest, PreservesInsertHistoryWhenBackspacingEntireRun) {
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
  ASSERT_EQ(compacted.size(), 2U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "mistake");
  EXPECT_EQ(compacted[1].kind, OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 0U);
  EXPECT_EQ(compacted[1].length, 7U);
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

TEST(UncommittedOpLogTest, ConsecutiveInsertsDeletesAndNewInsertsConsolidate) {
  UncommittedOpLog log;
  // User types "hello"
  const std::string first = "hello";
  for (std::size_t i = 0; i < first.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&first[i], 1));
  }
  // User hits backspace twice ("lo")
  log.recordErase(4, "o");
  log.recordErase(3, "l");

  // User types "ping"
  const std::string second = "ping";
  for (std::size_t i = 0; i < second.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(3 + i),
                     std::string_view(&second[i], 1));
  }

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 3U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "hello");

  EXPECT_EQ(compacted[1].kind, OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 3U);
  EXPECT_EQ(compacted[1].length, 2U);

  EXPECT_EQ(compacted[2].kind, OpKind::Insert);
  EXPECT_EQ(compacted[2].at, 3U);
  EXPECT_EQ(compacted[2].text, "ping");
}

TEST(UncommittedOpLogTest,
     ForwardDeletesAfterInsertConsolidateAndAllowNewInsert) {
  UncommittedOpLog log;
  // User types "abc"
  log.recordInsert(0, "a");
  log.recordInsert(1, "b");
  log.recordInsert(2, "c");

  // User hits forward delete 3 times at offset 3
  log.recordErase(3, "x");
  log.recordErase(3, "y");
  log.recordErase(3, "z");

  // User types "def"
  log.recordInsert(3, "d");
  log.recordInsert(4, "e");
  log.recordInsert(5, "f");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 3U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "abc");

  EXPECT_EQ(compacted[1].kind, OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 3U);
  EXPECT_EQ(compacted[1].length, 3U);

  EXPECT_EQ(compacted[2].kind, OpKind::Insert);
  EXPECT_EQ(compacted[2].at, 3U);
  EXPECT_EQ(compacted[2].text, "def");
}

TEST(UncommittedOpLogTest,
     MixedBackspaceAndDeleteKeyConsolidatesIntoSingleDeleteRange) {
  UncommittedOpLog log;
  // User types "testing"
  const std::string text = "testing";
  for (std::size_t i = 0; i < text.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&text[i], 1));
  }
  // Backspace at 6 ('g') -> cursor moves to 6
  log.recordErase(6, "g");
  // Delete key at 6 (deleting next character)
  log.recordErase(6, "x");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 2U);
  EXPECT_EQ(compacted[0].kind, OpKind::Insert);
  EXPECT_EQ(compacted[0].at, 0U);
  EXPECT_EQ(compacted[0].text, "testing");

  EXPECT_EQ(compacted[1].kind, OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 6U);
  EXPECT_EQ(compacted[1].length, 2U);
}

TEST(UncommittedOpLogTest, StoreReplayConsistencyAndHypertimeRetrieval) {
  Store store;
  UncommittedOpLog log;

  // 1. User types "secret typo" character by character
  const std::string typo = "secret typo";
  for (std::size_t i = 0; i < typo.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&typo[i], 1));
  }
  // 2. User backspaces all 11 characters
  for (int i = static_cast<int>(typo.size()) - 1; i >= 0; --i) {
    log.recordErase(static_cast<std::uint32_t>(i),
                    std::string_view(&typo[static_cast<std::size_t>(i)], 1));
  }
  // 3. User types "final draft"
  const std::string draft = "final draft";
  for (std::size_t i = 0; i < draft.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&draft[i], 1));
  }

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 3U);

  // Apply compacted operations to store
  auto ver = MicroversionId{};
  for (const auto &op : compacted) {
    if (op.kind == OpKind::Insert) {
      ver = store.insert(ver, op.at, op.text);
    } else if (op.kind == OpKind::Delete) {
      ver = store.erase(ver, op.at, op.length);
    }
  }

  // Active document displays final draft
  EXPECT_EQ(store.textOf(ver), "final draft");

  // Hypertime history preserves all microversions:
  // Step 1 had "secret typo", step 2 deleted it, step 3 added "final draft".
  const auto hist = ver.path();
  ASSERT_EQ(hist.size(), 3U);
  EXPECT_EQ(store.textOf(hist[0]), "secret typo");
  EXPECT_EQ(store.textOf(hist[1]), "");
  EXPECT_EQ(store.textOf(hist[2]), "final draft");
}

TEST(UncommittedOpLogTest, PreservesMultiByteInsertOnBackspace) {
  xanadu::UncommittedOpLog log;
  const std::string cafe = "caf\xc3\xa9";
  ASSERT_EQ(cafe.size(), 5U);

  log.recordInsert(0, cafe);
  log.recordErase(4, "\xa9");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 2U);
  EXPECT_EQ(compacted[0].kind, xanadu::OpKind::Insert);
  EXPECT_EQ(compacted[0].text, cafe);
  EXPECT_EQ(compacted[1].kind, xanadu::OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 4U);
  EXPECT_EQ(compacted[1].length, 1U);
}

TEST(UncommittedOpLogTest, PreservesHistoryOnCharacterBoundaryBackspace) {
  xanadu::UncommittedOpLog log;
  log.recordInsert(0, "hello");
  log.recordErase(4, "o");

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 2U);
  EXPECT_EQ(compacted[0].kind, xanadu::OpKind::Insert);
  EXPECT_EQ(compacted[0].text, "hello");
  EXPECT_EQ(compacted[1].kind, xanadu::OpKind::Delete);
  EXPECT_EQ(compacted[1].at, 4U);
  EXPECT_EQ(compacted[1].length, 1U);
}

} // namespace
} // namespace xanadu
