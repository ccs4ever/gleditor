/**
 * @file swarm_catalog_index_test.cpp
 * @brief Unit tests for SwarmCatalogIndex SQLite FTS5 search and NQL compiler.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "xudu/core/swarm_catalog_index.hpp"

namespace xudu {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsEmpty;
using ::testing::Not;

PublicationEntry makeEntry(const std::string &infoHash,
                           const std::string &title,
                           const std::string &author,
                           const std::vector<std::string> &topics,
                           const std::string &abstract,
                           const bool transcopyright) {
  PublicationEntry entry;
  entry.infoHash = infoHash;
  entry.bep46Uri = "btpk:key:doc:" + infoHash;
  entry.title = title;
  entry.authorName = author;
  entry.authorFingerprint = "FP_" + author;
  entry.topics = topics;
  entry.abstractText = abstract;
  entry.timestamp = 1700000000ULL;
  entry.sequence = 1;
  entry.totalBytes = 500000ULL;
  entry.microversions = 5;
  entry.hasTranscopyright = transcopyright;
  entry.transcopyrightTerms = transcopyright ? "2 nano-XU/byte" : "";
  entry.merkleRoot = {0x11, 0x22, 0x33, 0x44};
  entry.signature = "sig_" + infoHash;
  return entry;
}

TEST(SwarmCatalogIndexTest, BasicIndexingAndCount) {
  SwarmCatalogIndex index(":memory:");
  EXPECT_THAT(index.count(), Eq(0U));

  const auto doc1 = makeEntry("hash1", "Literary Machines", "Ted Nelson",
                              {"hypertext", "xanadu"},
                              "The report on Project Xanadu and universal literature.", false);
  index.indexPublication(doc1, 10, 2, true);
  EXPECT_THAT(index.count(), Eq(1U));

  const auto doc2 = makeEntry("hash2", "Augmenting Human Intellect", "Doug Engelbart",
                              {"ui", "augmentation"},
                              "Conceptual framework for augmentation of human intellect.", false);
  index.indexPublication(doc2, 8, 1, false);
  EXPECT_THAT(index.count(), Eq(2U));
}

TEST(SwarmCatalogIndexTest, PlainTextSearchAndSnippet) {
  SwarmCatalogIndex index(":memory:");
  const auto doc1 = makeEntry("hash1", "Foundations of Transclusion", "Ted Nelson",
                              {"hypertext", "transclusion"},
                              "Deep explanation of non-destructive quotation and coordinate spaces.", false);
  const auto doc2 = makeEntry("hash2", "Personal Dynamic Media", "Alan Kay",
                              {"dynabook", "smalltalk"},
                              "Dynamic personal media and active creative computing.", false);

  index.indexPublication(doc1, 15, 3, true);
  index.indexPublication(doc2, 12, 4, true);

  // Search by keyword in title
  const auto res1 = index.search("Transclusion", 10);
  ASSERT_THAT(res1.size(), Eq(1U));
  EXPECT_THAT(res1[0].entry.title, Eq("Foundations of Transclusion"));
  EXPECT_THAT(res1[0].isVerified, Eq(true));
  EXPECT_THAT(res1[0].seederCount, Eq(15));
  EXPECT_THAT(res1[0].snippet, Not(IsEmpty()));

  // Search by keyword in abstract
  const auto res2 = index.search("creative computing", 10);
  ASSERT_THAT(res2.size(), Eq(1U));
  EXPECT_THAT(res2[0].entry.title, Eq("Personal Dynamic Media"));

  // Empty or non-matching query
  const auto res3 = index.search("NonexistentTermXYZ", 10);
  EXPECT_THAT(res3, IsEmpty());
}

TEST(SwarmCatalogIndexTest, NelsonianQueryLanguageCompiling) {
  SwarmCatalogIndex index(":memory:");

  const auto doc1 = makeEntry("hash1", "Foundations of Transclusion", "Ted Nelson",
                              {"hypertext", "transclusion"},
                              "Coordinate space invariance and universal literature.", false);
  const auto doc2 = makeEntry("hash2", "Transcopyright Protocol", "Ted Nelson",
                              {"hypertext", "transcopyright"},
                              "Economic micropayments for permissionless reuse.", true);
  const auto doc3 = makeEntry("hash3", "Augmenting Intellect", "Doug Engelbart",
                              {"hypertext", "augmentation"},
                              "Collaborative knowledge augmentation.", false);

  index.indexPublication(doc1, 20, 5, true);
  index.indexPublication(doc2, 10, 2, true);
  index.indexPublication(doc3, 8, 1, false);

  // 1. Topic tag search `#transclusion`
  const auto topicRes = index.search("#transclusion", 10);
  ASSERT_THAT(topicRes.size(), Eq(1U));
  EXPECT_THAT(topicRes[0].entry.infoHash, Eq("hash1"));

  // 2. Author prefix filter `author:nelson`
  const auto authorRes = index.search("author:nelson", 10);
  EXPECT_THAT(authorRes.size(), Eq(2U));

  // 3. Combined `#hypertext author:nelson`
  const auto combinedRes = index.search("#hypertext author:nelson", 10);
  EXPECT_THAT(combinedRes.size(), Eq(2U));

  // 4. Combined `#transcopyright author:nelson`
  const auto tcRes = index.search("#transcopyright author:nelson", 10);
  ASSERT_THAT(tcRes.size(), Eq(1U));
  EXPECT_THAT(tcRes[0].entry.infoHash, Eq("hash2"));

  // 5. Filter `has:transcopyright`
  const auto transRes = index.search("has:transcopyright", 10);
  ASSERT_THAT(transRes.size(), Eq(1U));
  EXPECT_THAT(transRes[0].entry.infoHash, Eq("hash2"));

  // 6. Filter `is:verified`
  const auto verRes = index.search("is:verified", 10);
  EXPECT_THAT(verRes.size(), Eq(2U)); // hash1 and hash2
}

TEST(SwarmCatalogIndexTest, SwarmHealthUpdate) {
  SwarmCatalogIndex index(":memory:");
  const auto doc = makeEntry("hash1", "Title", "Author", {"topic"}, "Abstract", false);
  index.indexPublication(doc, 5, 1, false);

  auto res = index.search("Title", 10);
  ASSERT_THAT(res.size(), Eq(1U));
  EXPECT_THAT(res[0].seederCount, Eq(5));
  EXPECT_THAT(res[0].peerCount, Eq(1));

  // Update swarm health
  index.indexPublication(doc, 42, 18, false);
  res = index.search("Title", 10);
  ASSERT_THAT(res.size(), Eq(1U));
  EXPECT_THAT(res[0].seederCount, Eq(42));
  EXPECT_THAT(res[0].peerCount, Eq(18));
}

} // namespace
} // namespace xudu
