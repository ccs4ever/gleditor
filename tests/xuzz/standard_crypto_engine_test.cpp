/**
 * @file standard_crypto_engine_test.cpp
 * @brief Unit tests for StandardCryptoEngine: OpenSSL 3.0+ Ed25519, X.509 v3,
 *        CSR, and path validation.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "common/xanadu/identity/standard_crypto_engine.hpp"

namespace xanadu::identity {
namespace {

using ::testing::Eq;
using ::testing::Ne;
using ::testing::NotNull;

// ============================================================================
// Key Generation, Signing, and Verification
// ============================================================================

TEST(StandardCryptoEngineTest, KeyGenerationAndAlgorithmName) {
  auto engineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(engineRes.has_value());
  EXPECT_THAT(engineRes->algorithmName(), Eq("Ed25519"));
  EXPECT_TRUE(engineRes->hasPrivateKey());

  const auto pubKey = engineRes->publicKey();
  EXPECT_FALSE(pubKey.isZero());

  const auto rawPrivOpt = engineRes->rawPrivateKey();
  ASSERT_TRUE(rawPrivOpt.has_value());
  EXPECT_THAT(rawPrivOpt->size(), Eq(32UZ));
}

TEST(StandardCryptoEngineTest, SignVerifyAndTamperRejection) {
  auto engineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(engineRes.has_value());
  const auto &engine = *engineRes;

  const std::string msg = "Hello Xanadu World!";
  const auto msgSpan    = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(msg.data()), msg.size());

  // Valid sign and verify
  auto sigRes = engine.sign(msgSpan);
  ASSERT_TRUE(sigRes.has_value());
  EXPECT_FALSE(sigRes->isZero());

  auto verifyRes = engine.verify(engine.publicKey(), msgSpan, *sigRes);
  EXPECT_TRUE(verifyRes.has_value());

  // Tampered message must fail
  const std::string tamperedMsg = "Hello Xanadu World?";
  const auto tamperedSpan       = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(tamperedMsg.data()),
      tamperedMsg.size());
  EXPECT_THAT(engine.verify(engine.publicKey(), tamperedSpan, *sigRes).error(),
              Eq(ValidationError::CorruptSignature));

  // Wrong public key must fail
  auto otherEngine = StandardCryptoEngine::generate();
  ASSERT_TRUE(otherEngine.has_value());
  EXPECT_THAT(engine.verify(otherEngine->publicKey(), msgSpan, *sigRes).error(),
              Eq(ValidationError::CorruptSignature));

  // Corrupt signature bytes must fail
  auto corruptSig = *sigRes;
  corruptSig.bytes[10] ^= 0xFF;
  EXPECT_THAT(engine.verify(engine.publicKey(), msgSpan, corruptSig).error(),
              Eq(ValidationError::CorruptSignature));
}

// ============================================================================
// Canonical Scalar Verification (RFC 8032: S < L)
// ============================================================================

TEST(StandardCryptoEngineTest, RejectsNonCanonicalSignatureScalar) {
  auto engineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(engineRes.has_value());
  const auto &engine = *engineRes;

  const std::string msg = "Canonical Ed25519 Test";
  const auto msgSpan    = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(msg.data()), msg.size());

  auto sigRes = engine.sign(msgSpan);
  ASSERT_TRUE(sigRes.has_value());

  // In Ed25519, S is bytes 32..63 (little-endian scalar).
  // Prime L has MSB (byte 63) = 0x10.
  // Setting byte 63 to 0x20 makes S >= 2^253 > L, strictly non-canonical!
  auto nonCanonicalSig      = *sigRes;
  nonCanonicalSig.bytes[63] = 0x20;

  EXPECT_THAT(
      engine.verify(engine.publicKey(), msgSpan, nonCanonicalSig).error(),
      Eq(ValidationError::CorruptSignature));

  // Test isCanonicalEd25519Scalar directly
  std::span<const std::uint8_t, 32> validS(sigRes->bytes.data() + 32, 32);
  EXPECT_TRUE(isCanonicalEd25519Scalar(validS));

  std::span<const std::uint8_t, 32> badS(nonCanonicalSig.bytes.data() + 32, 32);
  EXPECT_FALSE(isCanonicalEd25519Scalar(badS));
}

// ============================================================================
// Domain Separation Prefixes
// ============================================================================

TEST(StandardCryptoEngineTest, DomainSeparationPreventsCrossProtocolReplay) {
  auto engineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(engineRes.has_value());
  const auto &engine = *engineRes;

  const std::string payload = "data-payload-1234";
  const auto payloadSpan    = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());

  // Sign as Manifest
  auto manifestSig = engine.signManifest(payloadSpan);
  ASSERT_TRUE(manifestSig.has_value());

  // Verify as Manifest succeeds
  EXPECT_TRUE(
      engine.verifyManifest(engine.publicKey(), payloadSpan, *manifestSig)
          .has_value());

  // Verify as PeerAuth must fail (domain separation prefix mismatch)
  EXPECT_THAT(
      engine.verifyPeerAuth(engine.publicKey(), payloadSpan, *manifestSig)
          .error(),
      Eq(ValidationError::CorruptSignature));

  // Verify as DeviceCsr must fail
  EXPECT_THAT(
      engine.verifyDeviceCsr(engine.publicKey(), payloadSpan, *manifestSig)
          .error(),
      Eq(ValidationError::CorruptSignature));

  // Raw verify without prefix must fail
  EXPECT_THAT(
      engine.verify(engine.publicKey(), payloadSpan, *manifestSig).error(),
      Eq(ValidationError::CorruptSignature));

  // Sign as Revocation and verify
  auto revSig = engine.signDeviceRevocation(payloadSpan);
  ASSERT_TRUE(revSig.has_value());
  EXPECT_TRUE(
      engine.verifyDeviceRevocation(engine.publicKey(), payloadSpan, *revSig)
          .has_value());
  EXPECT_THAT(
      engine.verifyManifest(engine.publicKey(), payloadSpan, *revSig).error(),
      Eq(ValidationError::CorruptSignature));
}

// ============================================================================
// PKCS#8 Encrypted Serialization & Deserialization
// ============================================================================

TEST(StandardCryptoEngineTest, Pkcs8EncryptedDerAndPemRoundTrip) {
  auto engineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(engineRes.has_value());
  const auto &engine = *engineRes;

  const std::string passphrase = "CorrectHorseBatteryStaple!";

  // 1. DER round-trip
  auto derEncRes = engine.exportPrivateKeyPkcs8Der(passphrase);
  ASSERT_TRUE(derEncRes.has_value());
  EXPECT_FALSE(derEncRes->empty());

  StandardCryptoEngine importedDerEngine;
  auto loadDerStatus =
      importedDerEngine.loadPrivateKeyPkcs8Der(*derEncRes, passphrase);
  ASSERT_TRUE(loadDerStatus.has_value());
  EXPECT_THAT(importedDerEngine.publicKey(), Eq(engine.publicKey()));

  // Wrong passphrase must fail
  StandardCryptoEngine badPassEngine;
  EXPECT_THAT(
      badPassEngine.loadPrivateKeyPkcs8Der(*derEncRes, "WrongPassword").error(),
      Eq(ValidationError::CorruptKey));

  // 2. PEM round-trip
  auto pemEncRes = engine.exportPrivateKeyPkcs8Pem(passphrase);
  ASSERT_TRUE(pemEncRes.has_value());
  EXPECT_FALSE(pemEncRes->empty());
  EXPECT_NE(pemEncRes->find("BEGIN ENCRYPTED PRIVATE KEY"), std::string::npos);

  StandardCryptoEngine importedPemEngine;
  auto loadPemStatus =
      importedPemEngine.loadPrivateKeyPkcs8Pem(*pemEncRes, passphrase);
  ASSERT_TRUE(loadPemStatus.has_value());
  EXPECT_THAT(importedPemEngine.publicKey(), Eq(engine.publicKey()));

  // Signing with imported engine must match original key
  const std::string testMsg = "PKCS#8 verification message";
  const auto testSpan       = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(testMsg.data()), testMsg.size());
  auto sig = importedPemEngine.sign(testSpan);
  ASSERT_TRUE(sig.has_value());
  EXPECT_TRUE(engine.verify(engine.publicKey(), testSpan, *sig).has_value());
}

// ============================================================================
// PKCS#10 CSR Generation & Air-Gapped Authorization
// ============================================================================

TEST(StandardCryptoEngineTest, CsrGenerationAndAuthorizationRoundTrip) {
  // Device keypair on local machine
  auto devEngine = StandardCryptoEngine::generate();
  ASSERT_TRUE(devEngine.has_value());

  // Master keypair on cold storage machine
  auto masterEngine = StandardCryptoEngine::generate();
  ASSERT_TRUE(masterEngine.has_value());

  // 1. Workstation generates PKCS#10 CSR
  auto csrDer =
      devEngine->generateCsr(*devEngine, "laptop-worker", "Alice MacBook");
  ASSERT_TRUE(csrDer.has_value());
  EXPECT_FALSE(csrDer->empty());

  // 2. Master authorizes CSR with constraints
  CertificateConstraints constraints;
  constraints.serialNumber = 1001;
  constraints.notBefore    = 1700000000;
  constraints.notAfter     = 1700000000 + 365 * 86400; // 1 year
  constraints.deviceId     = "laptop-worker";
  constraints.deviceName   = "Alice MacBook";
  constraints.authorName   = "Alice Adams";
  constraints.authorEmail  = "alice@example.org";

  auto certRes =
      masterEngine->authorizeCsr(*masterEngine, *csrDer, constraints);
  ASSERT_TRUE(certRes.has_value());
  EXPECT_FALSE(certRes->empty());
  EXPECT_THAT(certRes->serialNumber, Eq(1001ULL));
  EXPECT_THAT(certRes->subjectPublicKey, Eq(devEngine->publicKey()));
  EXPECT_THAT(certRes->issuerPublicKey, Eq(masterEngine->publicKey()));

  // 3. Full X.509 path validation
  const std::uint64_t validTime = 1700000000 + 1000;
  auto valRes                   = masterEngine->verifyDelegation(
      *certRes, masterEngine->publicKey(), validTime);
  EXPECT_TRUE(valRes.has_value());
}

// ============================================================================
// Full X.509 Path Validation: Expiration & Constraint Checks
// ============================================================================

TEST(StandardCryptoEngineTest, X509PathValidationRejectsExpiredCertificate) {
  auto devEngine    = StandardCryptoEngine::generate();
  auto masterEngine = StandardCryptoEngine::generate();
  ASSERT_TRUE(devEngine.has_value() && masterEngine.has_value());

  auto csrDer = devEngine->generateCsr(*devEngine, "dev1", "Device 1");
  ASSERT_TRUE(csrDer.has_value());

  CertificateConstraints constraints;
  constraints.serialNumber = 42;
  constraints.notBefore    = 1000;
  constraints.notAfter     = 2000;
  constraints.deviceId     = "dev1";
  constraints.deviceName   = "Device 1";

  auto certRes =
      masterEngine->authorizeCsr(*masterEngine, *csrDer, constraints);
  ASSERT_TRUE(certRes.has_value());

  // Before validity window
  EXPECT_THAT(
      masterEngine->verifyDelegation(*certRes, masterEngine->publicKey(), 500)
          .error(),
      Eq(ValidationError::CertificateNotYetValid));

  // Inside validity window
  EXPECT_TRUE(
      masterEngine->verifyDelegation(*certRes, masterEngine->publicKey(), 1500)
          .has_value());

  // After validity window
  EXPECT_THAT(
      masterEngine->verifyDelegation(*certRes, masterEngine->publicKey(), 2500)
          .error(),
      Eq(ValidationError::CertificateExpired));
}

TEST(StandardCryptoEngineTest, X509PathValidationRejectsWrongMasterKey) {
  auto devEngine    = StandardCryptoEngine::generate();
  auto masterEngine = StandardCryptoEngine::generate();
  auto rogueMaster  = StandardCryptoEngine::generate();
  ASSERT_TRUE(devEngine.has_value() && masterEngine.has_value() &&
              rogueMaster.has_value());

  auto csrDer = devEngine->generateCsr(*devEngine, "dev1", "Device 1");
  ASSERT_TRUE(csrDer.has_value());

  CertificateConstraints constraints;
  constraints.serialNumber = 1;
  constraints.notBefore    = 1000;
  constraints.notAfter     = 2000;

  auto certRes =
      masterEngine->authorizeCsr(*masterEngine, *csrDer, constraints);
  ASSERT_TRUE(certRes.has_value());

  // Verification with expectedMasterKey != actual signer key must fail
  EXPECT_THAT(
      masterEngine->verifyDelegation(*certRes, rogueMaster->publicKey(), 1500)
          .error(),
      Eq(ValidationError::DelegationSignatureInvalid));
}

TEST(StandardCryptoEngineTest, X509PathValidationRejectsCaCertificates) {
  // Construct a certificate with CA:TRUE to verify that verifyDelegation
  // rejects it
  auto devEngine    = StandardCryptoEngine::generate();
  auto masterEngine = StandardCryptoEngine::generate();
  ASSERT_TRUE(devEngine.has_value() && masterEngine.has_value());

  // Create a custom self-signed or CA certificate
  X509Ptr caCert(X509_new(), &X509_free);
  ASSERT_THAT(caCert, NotNull());
  X509_set_version(caCert.get(), 2);
  ASN1_INTEGER_set_uint64(X509_get_serialNumber(caCert.get()), 999);

  X509_NAME *name = X509_get_subject_name(caCert.get());
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_UTF8,
      reinterpret_cast<const unsigned char *>("Rogue Intermediate CA"), -1, -1,
      0);
  X509_set_issuer_name(caCert.get(), name);

  ASN1_TIME_set(X509_getm_notBefore(caCert.get()), 1000);
  ASN1_TIME_set(X509_getm_notAfter(caCert.get()), 2000);

  EvpPkeyPtr devPkey(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                  devEngine->publicKey().bytes.data(), 32),
      &EVP_PKEY_free);
  X509_set_pubkey(caCert.get(), devPkey.get());

  // Deliberately set CA:TRUE
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, caCert.get(), caCert.get(), nullptr, nullptr, 0);
  X509_EXTENSION *extBc = X509V3_EXT_conf_nid(
      nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE");
  ASSERT_THAT(extBc, NotNull());
  X509_add_ext(caCert.get(), extBc, -1);
  X509_EXTENSION_free(extBc);

  X509_EXTENSION *extKu = X509V3_EXT_conf_nid(
      nullptr, &ctx, NID_key_usage, "critical,digitalSignature,nonRepudiation");
  ASSERT_THAT(extKu, NotNull());
  X509_add_ext(caCert.get(), extKu, -1);
  X509_EXTENSION_free(extKu);

  // Sign with master key
  ASSERT_GT(X509_sign(caCert.get(), masterEngine->rawEvpPkey(), nullptr), 0);

  // Convert to DER
  const int derLen = i2d_X509(caCert.get(), nullptr);
  std::vector<std::uint8_t> der(derLen);
  unsigned char *p = der.data();
  i2d_X509(caCert.get(), &p);

  X509Certificate badCert;
  badCert.setDer(std::move(der));
  badCert.setValidity(1000, 2000);

  // verifyDelegation must reject CA:TRUE with PathValidationFailed!
  EXPECT_THAT(
      masterEngine->verifyDelegation(badCert, masterEngine->publicKey(), 1500)
          .error(),
      Eq(ValidationError::PathValidationFailed));
}

// ============================================================================
// Base64 DER Round-Trip
// ============================================================================

TEST(StandardCryptoEngineTest, Base64DerRoundTrip) {
  auto devEngine    = StandardCryptoEngine::generate();
  auto masterEngine = StandardCryptoEngine::generate();
  ASSERT_TRUE(devEngine.has_value() && masterEngine.has_value());

  auto csrDer = devEngine->generateCsr(*devEngine, "laptop", "Alice Laptop");
  ASSERT_TRUE(csrDer.has_value());

  CertificateConstraints constraints;
  constraints.serialNumber = 55;
  constraints.notBefore    = 1700000000;
  constraints.notAfter     = 1700000000 + 86400 * 30;
  constraints.deviceId     = "laptop";
  constraints.deviceName   = "Alice Laptop";
  constraints.authorName   = "Alice";
  constraints.authorEmail  = "alice@example.com";

  auto certRes =
      masterEngine->authorizeCsr(*masterEngine, *csrDer, constraints);
  ASSERT_TRUE(certRes.has_value());

  // 1. Convert to single-line Base64 DER
  const std::string b64 =
      StandardCryptoEngine::certificateToBase64Der(*certRes);
  EXPECT_FALSE(b64.empty());
  EXPECT_EQ(b64.find('\n'), std::string::npos);
  EXPECT_EQ(b64.find('\r'), std::string::npos);

  // 2. Decode from Base64 DER
  auto decodedCertRes = StandardCryptoEngine::certificateFromBase64Der(b64);
  ASSERT_TRUE(decodedCertRes.has_value());
  EXPECT_THAT(decodedCertRes->der, Eq(certRes->der));
  EXPECT_THAT(decodedCertRes->serialNumber, Eq(certRes->serialNumber));
  EXPECT_THAT(decodedCertRes->subjectPublicKey, Eq(certRes->subjectPublicKey));

  // Decoded certificate must validate successfully
  EXPECT_TRUE(masterEngine
                  ->verifyDelegation(*decodedCertRes, masterEngine->publicKey(),
                                     1700000000 + 100)
                  .has_value());

  // 3. Convert to and from PEM
  auto pemRes = StandardCryptoEngine::certificateToPem(*certRes);
  ASSERT_TRUE(pemRes.has_value());
  EXPECT_NE(pemRes->find("-----BEGIN CERTIFICATE-----"), std::string::npos);

  auto pemCertRes = StandardCryptoEngine::certificateFromPem(*pemRes);
  ASSERT_TRUE(pemCertRes.has_value());
  EXPECT_THAT(pemCertRes->der, Eq(certRes->der));
}

} // namespace
} // namespace xanadu::identity
