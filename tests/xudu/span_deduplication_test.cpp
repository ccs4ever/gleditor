#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "xudu/core/store.hpp"
#include "xudu/core/uncommitted_op_log.hpp"
#include "xudu/core/user_permascroll.hpp"

namespace fs = std::filesystem;

namespace xudu {
namespace {

class SpanDeduplicationTest : public testing::Test {
protected:
  fs::path testDir;

  void SetUp() override {
    testDir =
        fs::temp_directory_path() /
        ("xudu_dedup_test_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(testDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }
};

TEST_F(SpanDeduplicationTest, UserPermascrollFindsExactMatchingSpan) {
  UserPermascroll scroll;
  const std::string sample = "The quick brown fox jumps over the lazy dog.";
  const auto span = scroll.append(sample);

  EXPECT_EQ(span.start, 0U);
  EXPECT_EQ(span.length, sample.size());

  // Exact match >= 24 bytes
  const auto found = scroll.findExistingSpan(sample, 24);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->scroll, localScroll);
  EXPECT_EQ(found->start, 0U);
  EXPECT_EQ(found->length, sample.size());

  // Substring match >= 24 bytes: "quick brown fox jumps over"
  const std::string sub = "quick brown fox jumps over";
  const auto foundSub   = scroll.findExistingSpan(sub, 24);
  ASSERT_TRUE(foundSub.has_value());
  EXPECT_EQ(foundSub->start, 4U);
  EXPECT_EQ(foundSub->length, sub.size());

  // Match below threshold is ignored
  const auto shortMatch = scroll.findExistingSpan("The quick", 24);
  EXPECT_FALSE(shortMatch.has_value());

  // Non-existent text returns nullopt
  const auto nonExistent =
      scroll.findExistingSpan("A completely different text passage here.", 24);
  EXPECT_FALSE(nonExistent.has_value());
}

TEST_F(SpanDeduplicationTest, StoreInsertSpanPreservesMaterialization) {
  Store store;
  const auto span1 = store.primedia().append("Original master text passage.");
  const auto v1    = store.insertSpan(MicroversionId{}, 0, span1);

  EXPECT_EQ(v1.str(), "1");
  EXPECT_EQ(store.textOf(v1), "Original master text passage.");

  // Re-insert the same span in a branch
  const auto v2 = store.insertSpan(v1, 0, span1);
  EXPECT_EQ(v2.str(), "2");
  EXPECT_EQ(store.textOf(v2),
            "Original master text passage.Original master text passage.");
}

TEST_F(SpanDeduplicationTest, UncommittedLogAndStoreReuseExistingSpan) {
  Store store;
  const std::string quote =
      "Project Xanadu is a computer network with universal transclusion.";
  const auto v1               = store.insert(MicroversionId{}, 0, quote);
  const auto initialSpoolSize = store.primedia().size();
  EXPECT_GE(initialSpoolSize, quote.size());

  // Simulate typing the same passage into another document branch
  UncommittedOpLog log;
  for (std::size_t i = 0; i < quote.size(); ++i) {
    log.recordInsert(static_cast<std::uint32_t>(i),
                     std::string_view(&quote[i], 1));
  }

  const auto compacted = log.compact();
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_EQ(compacted[0].text, quote);

  // Apply with deduplication logic
  auto curVersion = MicroversionId{};
  if (const auto existing =
          store.userPermascroll().findExistingSpan(compacted[0].text, 24)) {
    curVersion = store.insertSpan(curVersion, compacted[0].at, *existing);
  } else {
    curVersion = store.insert(curVersion, compacted[0].at, compacted[0].text);
  }

  // Spool size should NOT have increased because existing span was reused!
  EXPECT_EQ(store.primedia().size(), initialSpoolSize);
  EXPECT_EQ(store.textOf(curVersion), quote);
}

} // namespace
} // namespace xudu

