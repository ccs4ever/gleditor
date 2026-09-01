/**
 * @file zz_merkle_test.cpp
 * @brief Unit tests for Zigzag slice author verification using MerkleLedger.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xudu/core/merkle_ledger.hpp>
#include <zigzag/core/zz_xudu_projector.hpp>
#include <zigzag/core/zzstructure.hpp>

namespace zigzag {
namespace {

using ::testing::Eq;

TEST(ZzMerkleTest, VerifySliceAuthorAgainstLedgerRoot) {
  xudu::MerkleLedger ledger;

  xudu::GpgKeyLink authorLink;
  authorLink.fingerprint = "E2B1A4D89C3F0174A55280BCFE491370D6A284E1";
  authorLink.identity    = "Ted Nelson <ted@xanadu.net>";
  authorLink.email       = "ted@xanadu.net";
  authorLink.gpgKeyId    = "FE491370D6A284E1";
  authorLink.timestamp   = 1700000000;
  ledger.appendKey(authorLink);

  const auto root = ledger.root();

  ZzStructureDocument doc;
  doc.meta.name   = "Hypergrid Design Slice";
  doc.meta.author = "Ted Nelson <ted@xanadu.net>";

  std::string error;
  EXPECT_TRUE(verifySliceAuthor(doc, ledger, root, &error));
  EXPECT_TRUE(error.empty());

  // Slice with unknown author must fail
  ZzStructureDocument unverifiedDoc;
  unverifiedDoc.meta.name   = "Unverified Slice";
  unverifiedDoc.meta.author = "Impostor <impostor@fake.net>";
  EXPECT_FALSE(verifySliceAuthor(unverifiedDoc, ledger, root, &error));
  EXPECT_FALSE(error.empty());

  // Slice with no author declared must fail
  ZzStructureDocument anonymousDoc;
  anonymousDoc.meta.name = "Anonymous Slice";
  EXPECT_FALSE(verifySliceAuthor(anonymousDoc, ledger, root, &error));
}

TEST(ZzMerkleTest, RevokedAuthorVerificationFails) {
  xudu::MerkleLedger ledger;

  xudu::GpgKeyLink authorLink;
  authorLink.fingerprint = "1111222233334444555566667777888899990000";
  authorLink.identity    = "Revoked Author <revoked@example.com>";
  authorLink.email       = "revoked@example.com";
  authorLink.revoked     = true;
  ledger.appendKey(authorLink);

  const auto root = ledger.root();

  ZzStructureDocument doc;
  doc.meta.author = "Revoked Author <revoked@example.com>";

  std::string error;
  EXPECT_FALSE(verifySliceAuthor(doc, ledger, root, &error));
  EXPECT_FALSE(error.empty());
}

} // namespace
} // namespace zigzag
