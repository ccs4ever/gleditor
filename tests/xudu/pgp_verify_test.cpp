/**
 * @file pgp_verify_test.cpp
 * @brief Tests for OpenPGP key and signature verification.
 *
 * These are the checks the whole identity ledger rests on, so most of the
 * cases here are the ones that must fail. Before this layer existed the only
 * question asked of a key was whether its fingerprint field held 40 hex
 * digits, and the only question asked of a signature was whether it was
 * non-zero.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xudu/core/identity/pgp_verify.hpp>

#include "pgp_fixture.hpp"

namespace xudu::identity::pgp {
namespace {

using ::testing::Eq;
using namespace xudu::testing;

TEST(PgpVerifyTest, ReadsTheFingerprintOutOfARealKey) {
  const auto fp = fingerprintOf(kAuthorPublicKey);
  ASSERT_TRUE(fp.has_value());
  EXPECT_THAT(fp->toString(), Eq(std::string(kAuthorFingerprint)));
}

TEST(PgpVerifyTest, RejectsInputThatIsNotAKey) {
  EXPECT_FALSE(fingerprintOf("").has_value());
  EXPECT_FALSE(fingerprintOf("not a key at all").has_value());
  // Right shape, wrong content: armor headers around garbage.
  EXPECT_FALSE(fingerprintOf("-----BEGIN PGP PUBLIC KEY BLOCK-----\n\nQUJD\n"
                             "-----END PGP PUBLIC KEY BLOCK-----\n")
                   .has_value());
}

// The check that stops a peer presenting its own key under somebody else's
// name. Without it every signature test downstream passes -- against the
// attacker's key.
TEST(PgpVerifyTest, AKeyDoesNotMatchAFingerprintItDoesNotOwn) {
  const auto author   = *Fingerprint::fromString(kAuthorFingerprint);
  const auto impostor = *Fingerprint::fromString(kImpostorFingerprint);

  EXPECT_TRUE(keyMatchesFingerprint(kAuthorPublicKey, author));
  EXPECT_TRUE(keyMatchesFingerprint(kImpostorPublicKey, impostor));

  EXPECT_FALSE(keyMatchesFingerprint(kImpostorPublicKey, author))
      << "the impostor's key answered to the author's fingerprint";
  EXPECT_FALSE(keyMatchesFingerprint(kAuthorPublicKey, impostor));

  // An unparseable key is an unusable one, not a trusted one.
  EXPECT_FALSE(keyMatchesFingerprint("garbage", author));
  EXPECT_FALSE(keyMatchesFingerprint("", author));
}

TEST(PgpVerifyTest, AcceptsAGenuineDetachedSignature) {
  EXPECT_TRUE(
      verifyDetached(kAuthorPublicKey, kAuthorMessage, kAuthorSignature));
}

TEST(PgpVerifyTest, RejectsASignatureOverDifferentBytes) {
  EXPECT_FALSE(verifyDetached(kAuthorPublicKey, "device-delegation-canonical-byteS",
                              kAuthorSignature));
  EXPECT_FALSE(verifyDetached(kAuthorPublicKey, "", kAuthorSignature));
}

// A well-formed signature by the wrong key. This is the case a naive
// implementation gets wrong: the packet parses, the maths checks out, and it
// still is not the signature we required.
TEST(PgpVerifyTest, RejectsAGenuineSignatureByTheWrongKey) {
  ASSERT_TRUE(
      verifyDetached(kImpostorPublicKey, kAuthorMessage, kImpostorSignature));

  EXPECT_FALSE(
      verifyDetached(kAuthorPublicKey, kAuthorMessage, kImpostorSignature))
      << "a signature by the impostor verified against the author's key";
}

TEST(PgpVerifyTest, RejectsMissingOrMalformedSignatures) {
  EXPECT_FALSE(verifyDetached(kAuthorPublicKey, kAuthorMessage, ""));
  EXPECT_FALSE(verifyDetached(kAuthorPublicKey, kAuthorMessage, "not armor"));
  EXPECT_FALSE(verifyDetached("", kAuthorMessage, kAuthorSignature));
}

} // namespace
} // namespace xudu::identity::pgp
