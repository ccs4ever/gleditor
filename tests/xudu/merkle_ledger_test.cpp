/**
 * @file merkle_ledger_test.cpp
 * @brief Unit tests for the append-only Merkle ledger and GPG identity linking.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xudu/core/merkle_ledger.hpp>
#include <xudu/core/provenance.hpp>
#include <xudu/core/torrent.hpp>

namespace xudu {
namespace {

using ::testing::Eq;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

TEST(MerkleLedgerTest, EntryNormalizationAndLeafHashing) {
  GpgKeyLink link1;
  link1.fingerprint =
      "4a 5b 6c 7d 8e 9f 00 11 22 33 44 55 66 77 88 99 aa bb cc dd";
  link1.identity  = "Ada Lovelace";
  link1.email     = "Ada@Example.ORG";
  link1.gpgKeyId  = "0x8E9F001122334455";
  link1.timestamp = 1700000000;
  link1.sequence  = 0;

  const auto hash1 = link1.leafHash();
  EXPECT_THAT(link1.leafHashHex().size(), Eq(64U));

  // Same content with different case/spaces in fingerprint and email must
  // produce identical hash
  GpgKeyLink link2;
  link2.fingerprint = "4A5B6C7D8E9F00112233445566778899AABBCCDD";
  link2.identity    = "Ada Lovelace";
  link2.email       = "ada@example.org";
  link2.gpgKeyId    = "0x8E9F001122334455";
  link2.timestamp   = 1700000000;
  link2.sequence    = 0;

  EXPECT_THAT(link1.leafHash(), Eq(link2.leafHash()));

  // Different sequence must produce different leaf hash
  link2.sequence = 1;
  EXPECT_THAT(link1.leafHash(), Ne(link2.leafHash()));
}

TEST(MerkleLedgerTest, AppendEntriesAndRootProgression) {
  MerkleLedger ledger;
  EXPECT_TRUE(ledger.empty());
  EXPECT_THAT(ledger.size(), Eq(0U));

  GpgKeyLink entry1;
  entry1.fingerprint = "1111222233334444555566667777888899990000";
  entry1.identity    = "Alice";
  entry1.email       = "alice@example.com";
  entry1.timestamp   = 1000;

  const auto [seq1, root1] = ledger.appendKey(entry1);
  EXPECT_THAT(seq1, Eq(0U));
  EXPECT_FALSE(ledger.empty());
  EXPECT_THAT(ledger.size(), Eq(1U));
  EXPECT_THAT(ledger.root(), Eq(root1));
  EXPECT_FALSE(ledger.rootHex().empty());

  GpgKeyLink entry2;
  entry2.fingerprint = "AAAA22223333444455556666777788889999BBBB";
  entry2.identity    = "Bob";
  entry2.email       = "bob@example.com";
  entry2.timestamp   = 2000;

  const auto [seq2, root2] = ledger.appendKey(entry2);
  EXPECT_THAT(seq2, Eq(1U));
  EXPECT_THAT(ledger.size(), Eq(2U));
  EXPECT_THAT(root2, Ne(root1));
  EXPECT_THAT(ledger.root(), Eq(root2));
}

TEST(MerkleLedgerTest, InclusionProofGenerationAndVerification) {
  MerkleLedger ledger;

  const std::vector<std::string> emails = {"user0@test.org", "user1@test.org",
                                           "user2@test.org", "user3@test.org",
                                           "user4@test.org", "user5@test.org"};

  for (std::size_t i = 0; i < emails.size(); ++i) {
    GpgKeyLink link;
    link.fingerprint = std::format("{:040d}", i + 1);
    link.identity    = std::format("User {}", i);
    link.email       = emails[i];
    link.timestamp   = 1700000000 + i * 100;
    ledger.appendKey(link);
  }

  const auto expectedRoot = ledger.root();

  // Verify inclusion proofs for each leaf
  for (std::size_t i = 0; i < ledger.size(); ++i) {
    const auto &entry = ledger.entry(i);
    const auto proof  = ledger.generateProof(i);

    EXPECT_THAT(proof.leafIndex, Eq(i));
    EXPECT_THAT(proof.rootHash, Eq(expectedRoot));
    EXPECT_TRUE(proof.verify(expectedRoot));
    EXPECT_TRUE(MerkleLedger::verifyInclusion(entry, proof, expectedRoot));

    // Tampered entry must fail verification
    auto tampered  = entry;
    tampered.email = "attacker@evil.org";
    EXPECT_FALSE(MerkleLedger::verifyInclusion(tampered, proof, expectedRoot));
  }
}

TEST(MerkleLedgerTest, ProofSerializationRoundTrip) {
  MerkleLedger ledger;
  for (int i = 0; i < 4; ++i) {
    GpgKeyLink link;
    link.fingerprint =
        std::format("111122223333444455556666777788889999000{}", i);
    link.identity  = std::format("Person {}", i);
    link.email     = std::format("person{}@test.net", i);
    link.timestamp = 1000 + i;
    ledger.appendKey(link);
  }

  const auto originalProof = ledger.generateProof(2);
  const std::string yaml   = originalProof.toYaml();
  EXPECT_FALSE(yaml.empty());

  const auto deserialized = MerkleProof::fromYaml(yaml);
  ASSERT_TRUE(deserialized.has_value());
  EXPECT_THAT(deserialized->leafIndex, Eq(originalProof.leafIndex));
  EXPECT_THAT(deserialized->maxIndex, Eq(originalProof.maxIndex));
  EXPECT_THAT(deserialized->leafHash, Eq(originalProof.leafHash));
  EXPECT_THAT(deserialized->rootHash, Eq(originalProof.rootHash));
  EXPECT_THAT(deserialized->path.size(), Eq(originalProof.path.size()));
  EXPECT_TRUE(deserialized->verify(ledger.root()));
}

TEST(MerkleLedgerTest, LedgerYamlSerializationAndLookup) {
  MerkleLedger ledger;

  GpgKeyLink link1;
  link1.fingerprint = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  link1.identity    = "Alice Adams";
  link1.email       = "alice@wonderland.org";
  link1.gpgKeyId    = "0xAAAA111122223333";
  link1.timestamp   = 500;
  ledger.appendKey(link1);

  GpgKeyLink link2;
  link2.fingerprint = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
  link2.identity    = "Bob Baker";
  link2.email       = "bob@bakery.org";
  link2.timestamp   = 600;
  ledger.appendKey(link2);

  // Second key for Alice (revoking old key)
  GpgKeyLink link3;
  link3.fingerprint = "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";
  link3.identity    = "Alice Adams";
  link3.email       = "alice@wonderland.org";
  link3.timestamp   = 700;
  ledger.appendKey(link3);

  EXPECT_THAT(
      ledger.findByFingerprint("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
      NotNull());
  EXPECT_THAT(
      ledger.findByFingerprint("0000000000000000000000000000000000000000"),
      IsNull());

  const auto aliceKeys = ledger.findByEmail("alice@wonderland.org");
  EXPECT_THAT(aliceKeys.size(), Eq(2U));

  const std::string yaml = ledger.toYaml();
  EXPECT_FALSE(yaml.empty());

  const MerkleLedger restored = MerkleLedger::fromYaml(yaml);
  EXPECT_THAT(restored.size(), Eq(ledger.size()));
  EXPECT_THAT(restored.root(), Eq(ledger.root()));
  EXPECT_THAT(restored.rootHex(), Eq(ledger.rootHex()));
}

TEST(MerkleLedgerTest, SealToTorrent) {
  MerkleLedger ledger;
  GpgKeyLink link;
  link.fingerprint = "1234567890123456789012345678901234567890";
  link.identity    = "Test Author";
  link.email       = "author@test.com";
  link.publicKeyArmored =
      "-----BEGIN PGP PUBLIC KEY BLOCK-----\ntest\n-----END PGP PUBLIC KEY "
      "BLOCK-----";
  link.timestamp = 1000;
  ledger.appendKey(link);

  const MadeTorrent made = ledger.sealToTorrent("test_ledger", 16384);
  EXPECT_FALSE(made.hash.isZero());
  EXPECT_FALSE(made.file.empty());

  const Metainfo meta = Metainfo::parse(made.file);
  EXPECT_THAT(meta.hash(), Eq(made.hash));
  EXPECT_THAT(meta.files().size(), Eq(3U)); // LEDGER.yaml, ROOT.hex, KEYS.pub
  EXPECT_THAT(meta.files()[0].path, Eq("LEDGER.yaml"));
  EXPECT_THAT(meta.files()[1].path, Eq("ROOT.hex"));
  EXPECT_THAT(meta.files()[2].path, Eq("KEYS.pub"));
}

TEST(MerkleLedgerTest, VerifyProvenanceAuthorAgainstRoot) {
  MerkleLedger ledger;
  GpgKeyLink link;
  link.fingerprint = "E2B1A4D89C3F0174A55280BCFE491370D6A284E1";
  link.identity    = "Ada Lovelace <ada@example.org>";
  link.email       = "ada@example.org";
  link.gpgKeyId    = "FE491370D6A284E1";
  link.timestamp   = 1700000000;
  ledger.appendKey(link);

  const auto root = ledger.root();

  Author author;
  author.name   = "Ada Lovelace";
  author.email  = "ada@example.org";
  author.gpgKey = "FE491370D6A284E1";

  SignedProvenance prov; // unsigned/empty signature path
  std::string error;
  EXPECT_TRUE(ledger.verifyProvenanceAuthor(author, prov, root, &error));

  // Unknown email fails
  Author unknownAuthor;
  unknownAuthor.name  = "Eve";
  unknownAuthor.email = "eve@unknown.com";
  EXPECT_FALSE(
      ledger.verifyProvenanceAuthor(unknownAuthor, prov, root, &error));
  EXPECT_FALSE(error.empty());

  // Wrong root hash fails
  std::array<std::uint8_t, 32> fakeRoot{};
  fakeRoot.fill(0xFF);
  EXPECT_FALSE(ledger.verifyProvenanceAuthor(author, prov, fakeRoot, &error));
}

} // namespace
} // namespace xudu
