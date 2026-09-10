/**
 * @file tests/xudu/page_break_test.cpp
 * @brief Unit tests for PageBreak operations, forced breaks, and permascroll
 * invariance.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "xudu/core/microversion.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/user_permascroll.hpp"

namespace fs = std::filesystem;
using namespace xudu;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class PageBreakTest : public ::testing::Test {
protected:
  fs::path testDir;

  void SetUp() override {
    testDir =
        fs::temp_directory_path() /
        ("xudu_page_break_test_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(testDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }
};

TEST_F(PageBreakTest, InsertBreakDoesNotGrowPermascroll) {
  Store store;
  const auto v1 =
      store.insert(MicroversionId{}, 0,
                   "Paragraph one.\n\nParagraph two.\n\nParagraph three.");
  const auto textLen = store.textOf(v1).size();

  // Permascroll size before break
  const auto initialSpoolSize = store.rebuild(v1).length();
  EXPECT_EQ(initialSpoolSize, textLen);

  // Insert page break at the first double-newline
  const auto breakPos =
      static_cast<std::uint32_t>(store.textOf(v1).find("\n\n") + 2);
  const auto v2 = store.insertBreak(v1, breakPos);

  EXPECT_NE(v2, v1);
  EXPECT_EQ(v2.str(), "2");

  // Rebuilt version length and materialized text are bit-identical
  const auto rebuiltV2 = store.rebuild(v2);
  EXPECT_EQ(rebuiltV2.length(), textLen);
  EXPECT_EQ(store.textOf(v2), store.textOf(v1));

  // Forced break offset is precisely recorded
  EXPECT_THAT(rebuiltV2.forcedBreaks(), ElementsAre(breakPos));
}

TEST_F(PageBreakTest, MultipleForcedBreaksInOrder) {
  Store store;
  const auto v1 = store.insert(MicroversionId{}, 0, "Alpha Beta Gamma Delta");
  const auto v2 = store.insertBreak(v1, 6);  // After Alpha_
  const auto v3 = store.insertBreak(v2, 11); // After Beta_
  const auto v4 = store.insertBreak(v3, 17); // After Gamma_

  EXPECT_EQ(store.textOf(v4), "Alpha Beta Gamma Delta");
  EXPECT_THAT(store.rebuild(v4).forcedBreaks(), ElementsAre(6U, 11U, 17U));
}

TEST_F(PageBreakTest, TextInsertionShiftsForcedBreaks) {
  Store store;
  const auto v1 = store.insert(MicroversionId{}, 0, "HelloWorld");
  const auto v2 = store.insertBreak(v1, 5); // between Hello and World
  EXPECT_THAT(store.rebuild(v2).forcedBreaks(), ElementsAre(5U));

  // Insert "Prefix_" at the very start (7 characters)
  const auto v3 = store.insert(v2, 0, "Prefix_");
  EXPECT_EQ(store.textOf(v3), "Prefix_HelloWorld");
  // The break at 5 should now be shifted forward to 12
  EXPECT_THAT(store.rebuild(v3).forcedBreaks(), ElementsAre(12U));

  // Insert "Middle_" right after Hello (at offset 12)
  const auto v4 = store.insert(v3, 12, "Middle_");
  EXPECT_EQ(store.textOf(v4), "Prefix_HelloMiddle_World");
  const auto breaksV4 = store.rebuild(v4).forcedBreaks();
  EXPECT_EQ(breaksV4.size(), 1U);
}

TEST_F(PageBreakTest, TextDeletionAffectsForcedBreaks) {
  Store store;
  const auto v1 = store.insert(MicroversionId{}, 0, "ABCDEFGHIJ");
  const auto v2 = store.insertBreak(v1, 5); // Break between E and F
  EXPECT_THAT(store.rebuild(v2).forcedBreaks(), ElementsAre(5U));

  // Erase 3 characters from start: "ABC"
  const auto v3 = store.erase(v2, 0, 3);
  EXPECT_EQ(store.textOf(v3), "DEFGHIJ");
  // Break at 5 shifts backward by 3 -> 2
  EXPECT_THAT(store.rebuild(v3).forcedBreaks(), ElementsAre(2U));
}

TEST_F(PageBreakTest, TranscludedPassageExcludesBreaks) {
  Store store;
  const auto doc1_v1 =
      store.insert(MicroversionId{}, 0, "Source text with page break.");
  const auto doc1_v2 = store.insertBreak(doc1_v1, 11);
  EXPECT_THAT(store.rebuild(doc1_v2).forcedBreaks(), ElementsAre(11U));

  // Destination doc transcludes from doc1_v2
  const auto doc2_v1 = store.insert(MicroversionId{}, 0, "Quoting: ");
  const auto srcText = store.textOf(doc1_v2);
  const auto doc2_v2 = store.transclude(
      doc2_v1, 9, doc1_v2, 0, static_cast<std::uint32_t>(srcText.size()));

  EXPECT_EQ(store.textOf(doc2_v2), "Quoting: Source text with page break.");
  // The quotation must NOT carry over the page break from the source document
  // layout
  EXPECT_THAT(store.rebuild(doc2_v2).forcedBreaks(), IsEmpty());
}

TEST_F(PageBreakTest, StoreSaveAndReloadPreservesBreaks) {
  // Both stores share one permascroll: a document's local spans are addresses
  // in the author's, and it keeps no copy of the bytes itself.
  const auto perma = std::make_shared<xudu::UserPermascroll>();
  MicroversionId savedVer;
  {
    Store store(perma);
    const auto v1 = store.insert(MicroversionId{}, 0,
                                 "First page text.\n\nSecond page text.");
    savedVer      = store.insertBreak(v1, 18);
    store.save(testDir.string());
  }

  Store reloaded(perma);
  reloaded.load(testDir.string());
  EXPECT_EQ(reloaded.textOf(savedVer), "First page text.\n\nSecond page text.");
  EXPECT_THAT(reloaded.rebuild(savedVer).forcedBreaks(), ElementsAre(18U));
}
