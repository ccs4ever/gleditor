/**
 * @file publication_ledger_test.cpp
 * @brief Unit tests for PublicationLedger and Merkle inclusion proofs.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xudu/core/publication_ledger.hpp"

namespace xudu {
namespace {

using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Ne;
using ::testing::Not;

PublicationEntry makeSampleEntry(const std::string &title,
                                 const std::string &author,
                                 const uint64_t seq) {
  PublicationEntry entry;
  entry.infoHash          = "0123456789abcdef0123456789abcdef01234567";
  entry.bep46Uri          = "btpk:testpubkey:doc:" + title;
  entry.title             = title;
  entry.authorName        = author;
  entry.authorFingerprint = "9F4A28C108BE4D1F0E7A8B9C1D2E3F4A5B6C7D8E";
  entry.topics            = {"hypertext", "transclusion", "osmic"};
  entry.abstractText      = "A foundational document on universal hypermedia.";
  entry.timestamp         = 1700000000ULL + seq * 100ULL;
  entry.sequence          = seq;
  entry.totalBytes        = 1048576ULL;
  entry.microversions     = 12;
  entry.hasTranscopyright = true;
  entry.transcopyrightTerms = "1 nano-XU/byte";
  entry.merkleRoot          = {0xaa, 0xbb, 0xcc, 0xdd};
  entry.signature           = "sig_sample_" + std::to_string(seq);
  return entry;
}

TEST(PublicationLedgerTest, EntryLeafHashingDeterminism) {
  const auto e1 = makeSampleEntry("Literary Machines", "Ted Nelson", 1);
  const auto e2 = makeSampleEntry("Literary Machines", "Ted Nelson", 1);
  auto e3       = makeSampleEntry("Literary Machines", "Ted Nelson", 2);

  EXPECT_THAT(e1.leafHash(), Eq(e2.leafHash()));
  EXPECT_THAT(e1.leafHashHex(), Eq(e2.leafHashHex()));
  EXPECT_THAT(e1.leafHashHex().size(), Eq(64U));

  // Sequence change alters hash
  EXPECT_THAT(e1.leafHash(), Ne(e3.leafHash()));

  // Content alteration alters hash
  e3.sequence = 1;
  e3.title    = "Literary Machines (Revised)";
  EXPECT_THAT(e1.leafHash(), Ne(e3.leafHash()));
}

TEST(PublicationLedgerTest, LedgerAppendAndRootProgression) {
  PublicationLedger ledger;
  EXPECT_TRUE(ledger.empty());
  EXPECT_THAT(ledger.size(), Eq(0U));

  const auto e1            = makeSampleEntry("Doc 1", "Author A", 1);
  const auto [idx1, root1] = ledger.appendPublication(e1);
  EXPECT_THAT(idx1, Eq(1U));
  EXPECT_THAT(ledger.size(), Eq(1U));
  EXPECT_FALSE(ledger.empty());
  EXPECT_THAT(ledger.root(), Eq(root1));

  const auto e2            = makeSampleEntry("Doc 2", "Author B", 2);
  const auto [idx2, root2] = ledger.appendPublication(e2);
  EXPECT_THAT(idx2, Eq(2U));
  EXPECT_THAT(ledger.size(), Eq(2U));
  EXPECT_THAT(root2, Ne(root1));
  EXPECT_THAT(ledger.root(), Eq(root2));

  const auto e3            = makeSampleEntry("Doc 3", "Author C", 3);
  const auto [idx3, root3] = ledger.appendPublication(e3);
  EXPECT_THAT(idx3, Eq(3U));
  EXPECT_THAT(ledger.size(), Eq(3U));
  EXPECT_THAT(root3, Ne(root2));
}

TEST(PublicationLedgerTest, MerkleInclusionProofVerification) {
  PublicationLedger ledger;
  const auto e1 =
      makeSampleEntry("Foundations of Transclusion", "Ted Nelson", 1);
  const auto e2 =
      makeSampleEntry("Augmenting Human Intellect", "Doug Engelbart", 2);
  const auto e3 = makeSampleEntry("Personal Dynamic Media", "Alan Kay", 3);
  const auto e4 =
      makeSampleEntry("Computer Lib / Dream Machines", "Ted Nelson", 4);

  ledger.appendPublication(e1);
  ledger.appendPublication(e2);
  ledger.appendPublication(e3);
  ledger.appendPublication(e4);

  const auto root = ledger.root();

  // Verify proof for each entry
  for (std::size_t idx = 0; idx < ledger.size(); ++idx) {
    const auto &entry = ledger.entry(idx);
    const auto proof  = ledger.generateProof(idx);
    EXPECT_THAT(proof.leafHash, Eq(entry.leafHash()));
    EXPECT_THAT(proof.rootHash, Eq(root));
    EXPECT_TRUE(proof.verify(root));
    EXPECT_TRUE(PublicationLedger::verifyInclusion(entry, proof, root));
  }
}

TEST(PublicationLedgerTest, TamperDetection) {
  PublicationLedger ledger;
  const auto e1 = makeSampleEntry("Original Title", "Author", 1);
  ledger.appendPublication(e1);

  auto proof = ledger.generateProof(0);
  EXPECT_TRUE(proof.verify(ledger.root()));
  EXPECT_TRUE(PublicationLedger::verifyInclusion(e1, proof, ledger.root()));

  // Tamper with entry hash
  proof.leafHash[0] ^= 0xFF;
  EXPECT_FALSE(proof.verify(ledger.root()));
  EXPECT_FALSE(PublicationLedger::verifyInclusion(e1, proof, ledger.root()));

  // Restore and tamper with root
  proof.leafHash = e1.leafHash();
  auto badRoot   = ledger.root();
  badRoot[0] ^= 0xFF;
  EXPECT_FALSE(proof.verify(badRoot));
  EXPECT_FALSE(PublicationLedger::verifyInclusion(e1, proof, badRoot));
}

TEST(PublicationLedgerTest, YamlSerializationRoundTrip) {
  PublicationLedger original;
  original.appendPublication(makeSampleEntry("Doc Alpha", "Author Alpha", 1));
  original.appendPublication(makeSampleEntry("Doc Beta", "Author Beta", 2));

  const std::string yamlStr = original.toYaml();
  EXPECT_THAT(yamlStr, Not(IsEmpty()));

  const auto restored = PublicationLedger::fromYaml(yamlStr);
  EXPECT_THAT(restored.size(), Eq(original.size()));
  EXPECT_THAT(restored.root(), Eq(original.root()));
  EXPECT_THAT(restored.rootHex(), Eq(original.rootHex()));

  // File save/load
  namespace fs = std::filesystem;
  const fs::path tempPath =
      fs::temp_directory_path() / "test_publication_ledger.yaml";
  EXPECT_TRUE(original.saveToFile(tempPath.string()));

  const auto fromFile = PublicationLedger::loadFromFile(tempPath.string());
  ASSERT_TRUE(fromFile.has_value());
  EXPECT_THAT(fromFile->size(), Eq(original.size()));
  EXPECT_THAT(fromFile->rootHex(), Eq(original.rootHex()));

  fs::remove(tempPath);
}

} // namespace
} // namespace xudu
