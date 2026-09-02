/**
 * @file user_permascroll_test.cpp
 * @brief Unit tests for sovereign UserPermascroll, cross-document sharing,
 *        collaborative live ops, and DeviceDelegation.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <xudu/core/identity/identity_layout.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/provenance.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/scroll.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/user_permascroll.hpp>

#include "pgp_fixture.hpp"

namespace {

using xudu::MicroversionId;
using xudu::Op;
using xudu::OpKind;
using xudu::PermascrollRegistry;
using xudu::PrimediaSpan;
using xudu::PublicKey;
using xudu::SignedProvenance;
using xudu::Store;
using xudu::UserPermascroll;
using xudu::identity::Fingerprint;

TEST(UserPermascrollTest, BasicAppendAndRead) {
  UserPermascroll scroll;
  const auto span1 = scroll.append("Hello ");
  const auto span2 = scroll.append("Permascroll!");

  EXPECT_EQ(span1.scroll, 0U);
  EXPECT_EQ(span1.start, 0U);
  EXPECT_EQ(span1.length, 6U);

  EXPECT_EQ(span2.scroll, 0U);
  EXPECT_EQ(span2.start, 6U);
  EXPECT_EQ(span2.length, 12U);

  EXPECT_EQ(scroll.size(), 18U);
  EXPECT_EQ(scroll.read(span1), "Hello ");
  EXPECT_EQ(scroll.read(span2), "Permascroll!");
  EXPECT_EQ(scroll.readView(span2), "Permascroll!");
  EXPECT_EQ(scroll.bytes(), "Hello Permascroll!");
}

TEST(UserPermascrollTest, MultipleStoresSharingOnePermascroll) {
  auto sharedPermascroll = std::make_shared<UserPermascroll>();

  Store storeA(sharedPermascroll);
  Store storeB(sharedPermascroll);

  const auto verA1 = storeA.insert(MicroversionId{}, 0, "Alice chapter 1.");
  const auto verB1 = storeB.insert(MicroversionId{}, 0, "Bob notes.");
  const auto verA2 = storeA.insert(verA1, 16, " Alice chapter 2.");

  EXPECT_EQ(storeA.textOf(verA1), "Alice chapter 1.");
  EXPECT_EQ(storeB.textOf(verB1), "Bob notes.");
  EXPECT_EQ(storeA.textOf(verA2), "Alice chapter 1. Alice chapter 2.");

  // Monotonic, continuous global byte stream across documents
  EXPECT_EQ(sharedPermascroll->bytes(),
            "Alice chapter 1.Bob notes. Alice chapter 2.");
  EXPECT_EQ(sharedPermascroll->size(), 43U);
}

TEST(UserPermascrollTest, CrossDocumentSelfTransclusion) {
  auto sharedPermascroll = std::make_shared<UserPermascroll>();

  Store storeA(sharedPermascroll);
  Store storeB(sharedPermascroll);

  const auto verA =
      storeA.insert(MicroversionId{}, 0, "The foundational theorem.");
  const auto bytesBeforeTransclusion = sharedPermascroll->size();

  // Document B transcludes the exact span from the shared author's permascroll
  // (slot 0)
  Op op;
  op.kind         = OpKind::Insert;
  op.at           = 0;
  op.span         = PrimediaSpan{0, 4, 12}; // Points to "foundational"
  const auto verB = storeB.apply(MicroversionId{}, op);

  EXPECT_EQ(storeB.textOf(verB), "foundational");

  // Zero duplicate storage allocated on transclusion
  EXPECT_EQ(sharedPermascroll->size(), bytesBeforeTransclusion);
}

TEST(UserPermascrollTest, IncrementalSealing) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_permascroll_test_seal";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  UserPermascroll scroll;
  scroll.append("First segment content.");

  SignedProvenance prov1;
  prov1.yaml = "title: \"Permascroll Seg 1\"\n";
  prov1.signature =
      "-----BEGIN PGP SIGNATURE-----\ntest\n-----END PGP SIGNATURE-----\n";

  const auto seg1 = scroll.sealIncremental(tempDir, prov1);
  ASSERT_TRUE(seg1.has_value());
  EXPECT_EQ(seg1->at, 0U);
  EXPECT_EQ(seg1->length, 22U);
  EXPECT_EQ(seg1->fileIndex, 0U);

  scroll.append(" Second segment content.");

  SignedProvenance prov2;
  prov2.yaml = "title: \"Permascroll Seg 2\"\n";
  prov2.signature =
      "-----BEGIN PGP SIGNATURE-----\ntest2\n-----END PGP SIGNATURE-----\n";

  const auto seg2 = scroll.sealIncremental(tempDir, prov2);
  ASSERT_TRUE(seg2.has_value());
  EXPECT_EQ(seg2->at, 22U);
  EXPECT_EQ(seg2->length, 24U);

  const auto current = scroll.currentScroll();
  ASSERT_EQ(current.segments.size(), 2U);
  EXPECT_EQ(current.segments[0].at, 0U);
  EXPECT_EQ(current.segments[0].length, 22U);
  EXPECT_EQ(current.segments[1].at, 22U);
  EXPECT_EQ(current.segments[1].length, 24U);

  std::filesystem::remove_all(tempDir);
}

TEST(UserPermascrollTest, CollaborativeLiveEditingZeroPayload) {
  auto localPermascroll = std::make_shared<UserPermascroll>();
  Store bobStore(localPermascroll);

  // Bob writes his own text in slot 0
  const auto bobV1 = bobStore.insert(MicroversionId{}, 0, "Bob says hello. ");
  const auto bobBytesAfterTyping = localPermascroll->size();

  // Remote collaborator Alice sends a live operation
  const auto aliceKeys = xudu::createMutableKeys();
  const std::string aliceScrollKey =
      "btpk:" + aliceKeys.publicKey.hex() + ":permascroll";

  Op remoteOp;
  remoteOp.kind   = OpKind::Insert;
  remoteOp.parent = bobV1;
  remoteOp.at     = 16;
  remoteOp.span   = PrimediaSpan{0, 100, 15}; // Alice's offset 100, len 15

  // Apply remote live op with Alice's authorScrollKey (zero raw text passed)
  const auto bobV2 = bobStore.applyRemoteLiveOp(remoteOp, "", aliceScrollKey);

  EXPECT_EQ(bobV2.str(), "2");

  // Bob's local permascroll is NOT polluted with Alice's text
  EXPECT_EQ(localPermascroll->size(), bobBytesAfterTyping);

  // Alice's scroll is registered as an external scroll (> 0)
  const auto *node = bobStore.getCompactOp(bobV2);
  ASSERT_NE(node, nullptr);
  EXPECT_GT(node->scrollId, 0U); // External scroll slot!
  EXPECT_EQ(node->spanStart, 100U);
  EXPECT_EQ(node->spanLength, 15U);
}

namespace {

/// The delegation kDeviceDelegationSignature was generated over. Built by
/// hand rather than with createMutableKeys() because the signature covers the
/// device key, so it has to be the same key every run.
xudu::DeviceDelegation fixtureDelegation() {
  xudu::DeviceDelegation cert;
  cert.masterFingerprint =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  cert.devicePublicKey = xudu::PublicKey{};
  cert.devicePublicKey.bytes.fill(0x11);
  cert.deviceName      = "thinkpad-laptop";
  cert.issuedTimestamp = 1700000000;
  cert.gpgSignatureArmored =
      std::string(xudu::testing::kDeviceDelegationSignature);
  return cert;
}

} // namespace

TEST(UserPermascrollTest, DeviceDelegationCertificateRoundTrip) {
  const auto cert = fixtureDelegation();

  const auto yaml = cert.toYaml();
  EXPECT_NE(yaml.find("thinkpad-laptop"), std::string::npos);

  const auto decoded = xudu::DeviceDelegation::fromYaml(yaml);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, cert);
}

TEST(UserPermascrollTest, DeviceDelegationVerifiesAgainstItsMasterKey) {
  const auto cert = fixtureDelegation();
  EXPECT_TRUE(cert.verify(xudu::testing::kAuthorPublicKey));

  // Survives a round trip through YAML, which is how it reaches another
  // machine.
  const auto decoded = xudu::DeviceDelegation::fromYaml(cert.toYaml());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->verify(xudu::testing::kAuthorPublicKey));
}

// This case used to pass with the signature field set to the literal text
// "mock": verify() checked that three fields were non-empty. Every rejection
// below was accepted before there was anything here to reject it.
TEST(UserPermascrollTest, DeviceDelegationRejectsWhatItShould) {
  const auto good = fixtureDelegation();

  auto mockSignature = good;
  mockSignature.gpgSignatureArmored =
      "-----BEGIN PGP SIGNATURE-----\nmock\n-----END PGP SIGNATURE-----";
  EXPECT_FALSE(mockSignature.verify(xudu::testing::kAuthorPublicKey))
      << "the word 'mock' passed as an OpenPGP signature";

  // A different key, with a real signature of its own, is still not this
  // delegation's master.
  EXPECT_FALSE(good.verify(xudu::testing::kImpostorPublicKey));

  // Every signed field is covered: changing any one invalidates the whole.
  auto renamed       = good;
  renamed.deviceName = "someone-elses-laptop";
  EXPECT_FALSE(renamed.verify(xudu::testing::kAuthorPublicKey));

  auto reissued            = good;
  reissued.issuedTimestamp = 1700000001;
  EXPECT_FALSE(reissued.verify(xudu::testing::kAuthorPublicKey));

  auto swappedDevice = good;
  swappedDevice.devicePublicKey.bytes.fill(0x22);
  EXPECT_FALSE(swappedDevice.verify(xudu::testing::kAuthorPublicKey))
      << "a delegation was retargeted to a different device key";

  auto unsignedCert = good;
  unsignedCert.gpgSignatureArmored.clear();
  EXPECT_FALSE(unsignedCert.verify(xudu::testing::kAuthorPublicKey));

  EXPECT_FALSE(good.verify("")) << "no master key means no verification";
}

TEST(UserPermascrollTest, PermascrollRegistrySingleton) {
  auto &reg = PermascrollRegistry::instance();
  reg.clear();

  const auto fp1 =
      Fingerprint::fromString("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(fp1.has_value());

  const auto scroll1a = reg.getOrCreate(*fp1);
  const auto scroll1b = reg.getOrCreate(*fp1);
  EXPECT_EQ(scroll1a, scroll1b);

  const auto def1 = reg.defaultUser();
  const auto def2 = reg.defaultUser();
  EXPECT_EQ(def1, def2);

  reg.clear();
}

} // namespace
