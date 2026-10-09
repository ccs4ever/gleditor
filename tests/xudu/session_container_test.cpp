/**
 * @file session_container_test.cpp
 * @brief Unit tests for SessionPackage container exchange format (.xuzzpkg),
 *        hardened TSV parsing, signature verification, and zero-shift
 * invariant.
 */
#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/session_container.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace xanadu {
namespace {

struct TestKeyPair {
  std::array<std::uint8_t, 32> priv{};
  std::array<std::uint8_t, 32> pub{};
};

TestKeyPair generateKeyPair() {
  EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  EXPECT_NE(kctx, nullptr);
  EXPECT_GT(EVP_PKEY_keygen_init(kctx), 0);
  EVP_PKEY *pkey = nullptr;
  EXPECT_GT(EVP_PKEY_keygen(kctx, &pkey), 0);
  EVP_PKEY_CTX_free(kctx);

  TestKeyPair kp;
  std::size_t pubLen  = 32;
  std::size_t privLen = 32;
  EXPECT_GT(EVP_PKEY_get_raw_public_key(pkey, kp.pub.data(), &pubLen), 0);
  EXPECT_GT(EVP_PKEY_get_raw_private_key(pkey, kp.priv.data(), &privLen), 0);
  EVP_PKEY_free(pkey);
  return kp;
}

std::string computeSha256Hex(std::span<const std::uint8_t> data) {
  std::array<std::uint8_t, 32> hash{};
  SHA256(data.data(), data.size(), hash.data());
  static constexpr char hexChars[] = "0123456789abcdef";
  std::string s;
  s.reserve(64);
  for (const auto b : hash) {
    s.push_back(hexChars[(b >> 4) & 0x0F]);
    s.push_back(hexChars[b & 0x0F]);
  }
  return s;
}

std::string toHexStr(std::span<const std::uint8_t> data) {
  static constexpr char hexChars[] = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (const auto b : data) {
    s.push_back(hexChars[(b >> 4) & 0x0F]);
    s.push_back(hexChars[b & 0x0F]);
  }
  return s;
}

CompactOpNode makeTestOp(std::uint32_t at, std::uint64_t spanStart,
                         std::uint64_t spanLength,
                         OpKind kind = OpKind::Insert) {
  CompactOpNode op{};
  op.kind       = kind;
  op.at         = at;
  op.spanStart  = spanStart;
  op.spanLength = spanLength;
  op.scrollId   = 0;
  return op;
}

TEST(SessionContainerTest, ManifestSerializationAndParsingRoundTrip) {
  const auto masterKeys   = generateKeyPair();
  const auto deviceKeys   = generateKeyPair();
  const auto masterFp     = computeSha256Hex(masterKeys.pub);
  const auto deviceKeyHex = toHexStr(deviceKeys.pub);

  SessionManifest manifest;
  manifest.manifestVersion   = 1;
  manifest.masterFingerprint = masterFp;
  manifest.deviceId          = "laptop";
  manifest.deviceKey         = deviceKeyHex;
  manifest.baseVersion       = "4";
  manifest.headVersion       = "4a2";
  manifest.primediaOffset    = 1048576;
  manifest.primediaLength    = 4096;
  manifest.timestamp         = 1791480000;
  manifest.extraFields.emplace_back("custom_field", "value_123");

  const std::string serialized = manifest.serialize();
  EXPECT_FALSE(serialized.empty());
  EXPECT_FALSE(serialized.contains('\r'));

  auto parseRes = SessionManifest::parse(serialized);
  ASSERT_TRUE(parseRes.has_value());
  const auto &parsed = *parseRes;

  EXPECT_EQ(parsed.manifestVersion, 1U);
  EXPECT_EQ(parsed.masterFingerprint, masterFp);
  EXPECT_EQ(parsed.deviceId, "laptop");
  EXPECT_EQ(parsed.deviceKey, deviceKeyHex);
  EXPECT_EQ(parsed.baseVersion, "4");
  EXPECT_EQ(parsed.headVersion, "4a2");
  EXPECT_EQ(parsed.primediaOffset, 1048576ULL);
  EXPECT_EQ(parsed.primediaLength, 4096ULL);
  EXPECT_EQ(parsed.timestamp, 1791480000ULL);
  ASSERT_EQ(parsed.extraFields.size(), 1U);
  EXPECT_EQ(parsed.extraFields[0].first, "custom_field");
  EXPECT_EQ(parsed.extraFields[0].second, "value_123");
}

TEST(SessionContainerTest, HardenedTsvRejectsCarriageReturns) {
  const std::string crlfManifest =
      "manifest_version\t1\r\n"
      "master_fingerprint\t"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
  auto res = SessionManifest::parse(crlfManifest);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), SessionValidationError::CarriageReturnRejected);
}

TEST(SessionContainerTest, HardenedTsvRejectsDuplicateKeys) {
  const std::string duplicateKeyManifest =
      "manifest_version\t1\n"
      "master_fingerprint\t"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
      "device_id\tlaptop\n"
      "device_id\tduplicate_laptop\n"
      "device_key\t"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
      "base_version\t4\n"
      "head_version\t4a2\n"
      "primedia_offset\t1048576\n"
      "primedia_length\t4096\n"
      "timestamp\t1791480000\n";
  auto res = SessionManifest::parse(duplicateKeyManifest);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), SessionValidationError::DuplicateKey);
}

TEST(SessionContainerTest, HardenedTsvRejectsInvalidKeyFormats) {
  // Uppercase key
  {
    const std::string tsv = "MANIFEST_VERSION\t1\n";
    auto res              = SessionManifest::parse(tsv);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), SessionValidationError::InvalidKeyFormat);
  }
  // Key with hyphen
  {
    const std::string tsv = "manifest-version\t1\n";
    auto res              = SessionManifest::parse(tsv);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), SessionValidationError::InvalidKeyFormat);
  }
  // Key with spaces
  {
    const std::string tsv = "manifest version\t1\n";
    auto res              = SessionManifest::parse(tsv);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), SessionValidationError::InvalidKeyFormat);
  }
  // Key starting with digit
  {
    const std::string tsv = "1version\t1\n";
    auto res              = SessionManifest::parse(tsv);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), SessionValidationError::InvalidKeyFormat);
  }
}

TEST(SessionContainerTest, HardenedTsvRejectsMissingRequiredFields) {
  // Missing timestamp
  const std::string incompleteManifest =
      "manifest_version\t1\n"
      "master_fingerprint\t"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
      "device_id\tlaptop\n"
      "device_key\t"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
      "base_version\t4\n"
      "head_version\t4a2\n"
      "primedia_offset\t1048576\n"
      "primedia_length\t4096\n";
  auto res = SessionManifest::parse(incompleteManifest);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), SessionValidationError::MissingRequiredField);
}

TEST(SessionContainerTest, HardenedTsvRejectsMalformedFieldValues) {
  // Invalid fingerprint length (not 64 hex)
  const std::string badFpManifest =
      "manifest_version\t1\n"
      "master_fingerprint\tshort_fp\n"
      "device_id\tlaptop\n"
      "device_key\t"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
      "base_version\t4\n"
      "head_version\t4a2\n"
      "primedia_offset\t1048576\n"
      "primedia_length\t4096\n"
      "timestamp\t1791480000\n";
  auto res = SessionManifest::parse(badFpManifest);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), SessionValidationError::InvalidFieldFormat);
}

TEST(SessionContainerTest, BundleCreationExportAndImportRoundTrip) {
  const auto masterKeys   = generateKeyPair();
  const auto deviceKeys   = generateKeyPair();
  const auto masterFp     = computeSha256Hex(masterKeys.pub);
  const auto deviceKeyHex = toHexStr(deviceKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());
  const std::string deviceCert = *certRes;

  const std::string primediaText =
      "The quick brown fox jumps over the lazy dog";
  const std::vector<std::uint8_t> primediaBytes(primediaText.begin(),
                                                primediaText.end());

  CompactOpNode op1                    = makeTestOp(0, 1048576, 20);
  CompactOpNode op2                    = makeTestOp(20, 1048596, 24);
  const std::vector<CompactOpNode> ops = {op1, op2};

  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(deviceKeyHex)
      ->setBaseVersion("4")
      ->setHeadVersion("4a2")
      ->setPrimediaOffset(1048576)
      ->setPrimediaLength(primediaBytes.size())
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(primediaBytes)
      ->setOpsNodes(ops)
      ->setDeviceCert(deviceCert);

  auto signRes = pkg.signWithDeviceKey(deviceKeys.priv);
  ASSERT_TRUE(signRes.has_value());

  auto verifySigRes = pkg.verifySignature();
  EXPECT_TRUE(verifySigRes.has_value());

  auto verifyCertRes = pkg.verifyDeviceCert(masterKeys.pub);
  EXPECT_TRUE(verifyCertRes.has_value());

  auto exportRes = exportPackage(pkg);
  ASSERT_TRUE(exportRes.has_value());
  const auto &archiveBytes = *exportRes;
  EXPECT_GT(archiveBytes.size(), 512U);

  // Import package and validate author binding
  auto importRes = importPackage(archiveBytes, masterFp);
  ASSERT_TRUE(importRes.has_value());
  const auto &importedPkg = *importRes;

  EXPECT_EQ(importedPkg.manifestVersion, 1U);
  EXPECT_EQ(importedPkg.masterFingerprint, masterFp);
  EXPECT_EQ(importedPkg.deviceId, "laptop");
  EXPECT_EQ(importedPkg.deviceKey, deviceKeyHex);
  EXPECT_EQ(importedPkg.baseVersion, "4");
  EXPECT_EQ(importedPkg.headVersion, "4a2");
  EXPECT_EQ(importedPkg.primediaOffset, 1048576ULL);
  EXPECT_EQ(importedPkg.primediaLength, primediaBytes.size());
  EXPECT_EQ(importedPkg.timestamp, 1791480000ULL);

  EXPECT_EQ(importedPkg.primediaSlice, primediaBytes);
  EXPECT_EQ(importedPkg.deviceCert, deviceCert);
  EXPECT_EQ(importedPkg.signature, pkg.signature);

  ASSERT_EQ(importedPkg.opsNodes.size(), 2U);
  EXPECT_EQ(importedPkg.opsNodes[0].spanStart, 1048576ULL);
  EXPECT_EQ(importedPkg.opsNodes[0].spanLength, 20ULL);
  EXPECT_EQ(importedPkg.opsNodes[1].spanStart, 1048596ULL);
  EXPECT_EQ(importedPkg.opsNodes[1].spanLength, 24ULL);
}

TEST(SessionContainerTest, AuthorBindingValidation) {
  const auto masterKeys = generateKeyPair();
  const auto otherKeys  = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);
  const auto otherFp    = computeSha256Hex(otherKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(toHexStr(deviceKeys.pub))
      ->setBaseVersion("4")
      ->setHeadVersion("4a2")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(4)
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view("test"))
      ->setDeviceCert(*certRes);

  ASSERT_TRUE(pkg.signWithDeviceKey(deviceKeys.priv).has_value());

  auto archive = exportPackage(pkg);
  ASSERT_TRUE(archive.has_value());

  // Correct author fingerprint matches
  EXPECT_TRUE(importPackage(*archive, masterFp).has_value());

  // Incorrect author fingerprint rejected
  auto mismatchRes = importPackage(*archive, otherFp);
  ASSERT_FALSE(mismatchRes.has_value());
  EXPECT_EQ(mismatchRes.error(), SessionValidationError::IdentityMismatch);
}

TEST(SessionContainerTest, SignatureVerificationRejectsTampering) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(toHexStr(deviceKeys.pub))
      ->setBaseVersion("4")
      ->setHeadVersion("4a2")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(4)
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view("test"))
      ->setDeviceCert(*certRes);

  ASSERT_TRUE(pkg.signWithDeviceKey(deviceKeys.priv).has_value());
  ASSERT_TRUE(pkg.verifySignature().has_value());

  // Tamper with signature bytes
  pkg.signature[10] ^= 0xFF;
  EXPECT_FALSE(pkg.verifySignature().has_value());
  EXPECT_EQ(pkg.verifySignature().error(),
            SessionValidationError::InvalidSignature);
}

TEST(SessionContainerTest, ZeroShiftInvariantPreservation) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  // Text authored offline on laptop
  const std::string authoredText =
      "Zero-shift invariant preservation for concurrent offline editing across "
      "laptops and workstations in Xanadu hypertime.";
  const std::vector<std::uint8_t> unsealedPrimedia(authoredText.begin(),
                                                   authoredText.end());

  const std::uint64_t baseOffset = 1048576; // P_0 = 1 MiB

  // Construct edit decision list operations addressing exact spans in P_0
  CompactOpNode opA =
      makeTestOp(0, baseOffset + 0, 31); // "Zero-shift invariant preservation"
  CompactOpNode opB =
      makeTestOp(31, baseOffset + 31, 28); // " for concurrent offline edit"
  CompactOpNode opC = makeTestOp(59, baseOffset + 59, 58); // remainder
  const std::vector<CompactOpNode> originalOps = {opA, opB, opC};

  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(toHexStr(deviceKeys.pub))
      ->setBaseVersion("4")
      ->setHeadVersion("4a2")
      ->setPrimediaOffset(baseOffset)
      ->setPrimediaLength(unsealedPrimedia.size())
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(unsealedPrimedia)
      ->setOpsNodes(originalOps)
      ->setDeviceCert(*certRes);

  ASSERT_TRUE(pkg.signWithDeviceKey(deviceKeys.priv).has_value());

  auto archiveRes = exportPackage(pkg);
  ASSERT_TRUE(archiveRes.has_value());

  auto importedRes = importPackage(*archiveRes, masterFp);
  ASSERT_TRUE(importedRes.has_value());
  const auto &importedPkg = *importedRes;

  // Zero-shift invariant 1: Primedia slice round-trips bit-for-bit
  ASSERT_EQ(importedPkg.primediaSlice.size(), unsealedPrimedia.size());
  EXPECT_EQ(std::memcmp(importedPkg.primediaSlice.data(),
                        unsealedPrimedia.data(), unsealedPrimedia.size()),
            0);

  // Zero-shift invariant 2: CompactOpNode records round-trip bit-for-bit
  ASSERT_EQ(importedPkg.opsNodes.size(), originalOps.size());
  for (std::size_t i = 0; i < originalOps.size(); ++i) {
    EXPECT_EQ(std::memcmp(&importedPkg.opsNodes[i], &originalOps[i],
                          sizeof(CompactOpNode)),
              0);
  }

  // Zero-shift invariant 3: Character spans index exact expected text with 0
  // coordinate shift
  for (const auto &op : importedPkg.opsNodes) {
    const std::uint64_t localOffset = op.spanStart - baseOffset;
    ASSERT_LE(localOffset + op.spanLength, importedPkg.primediaSlice.size());

    const std::string_view sliceSpan(
        reinterpret_cast<const char *>(importedPkg.primediaSlice.data()) +
            localOffset,
        op.spanLength);
    const std::string_view originalSpan(authoredText.data() + localOffset,
                                        op.spanLength);
    EXPECT_EQ(sliceSpan, originalSpan);
  }
}

TEST(SessionContainerTest, FileExportAndImportRoundTrip) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  const std::string text = "Session file persistence test";
  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(toHexStr(deviceKeys.pub))
      ->setBaseVersion("1")
      ->setHeadVersion("1a1")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(text.size())
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view(text))
      ->setDeviceCert(*certRes);

  ASSERT_TRUE(pkg.signWithDeviceKey(deviceKeys.priv).has_value());

  const auto tempPath =
      std::filesystem::temp_directory_path() / "test_session_package.xuzzpkg";

  auto exportFileRes = exportPackage(pkg, tempPath);
  ASSERT_TRUE(exportFileRes.has_value());
  EXPECT_TRUE(std::filesystem::exists(tempPath));

  auto importFileRes = importPackage(tempPath, masterFp);
  ASSERT_TRUE(importFileRes.has_value());
  EXPECT_EQ(importFileRes->primediaSliceString(), text);

  std::filesystem::remove(tempPath);
}

TEST(SessionContainerTest, EncryptedPackageExportAndImportRoundTrip) {
  const auto masterKeys   = generateKeyPair();
  const auto deviceKeys   = generateKeyPair();
  const auto masterFp     = computeSha256Hex(masterKeys.pub);
  const auto deviceKeyHex = toHexStr(deviceKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  const std::string text = "Confidential hypermedia content";
  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(deviceKeyHex)
      ->setBaseVersion("1")
      ->setHeadVersion("1a1")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(text.size())
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view(text))
      ->setDeviceCert(*certRes)
      ->setEncrypted(true)
      ->setPassphrase("super-secret-passphrase-2026");

  auto signRes = pkg.signWithDeviceKey(deviceKeys.priv);
  ASSERT_TRUE(signRes.has_value());

  auto exportRes = exportPackage(pkg);
  ASSERT_TRUE(exportRes.has_value());
  const auto &archive = *exportRes;

  // Import with correct passphrase
  auto importRes =
      importPackage(archive, masterFp, true,
                    std::span<const std::uint8_t, 32>(masterKeys.pub),
                    "super-secret-passphrase-2026");
  ASSERT_TRUE(importRes.has_value());
  EXPECT_EQ(importRes->primediaSliceString(), text);
  EXPECT_TRUE(importRes->isEncrypted());
}

TEST(SessionContainerTest, EncryptedPackageRejectsWrongOrMissingPassphrase) {
  const auto masterKeys   = generateKeyPair();
  const auto deviceKeys   = generateKeyPair();
  const auto masterFp     = computeSha256Hex(masterKeys.pub);
  const auto deviceKeyHex = toHexStr(deviceKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  const std::string text = "Encrypted test data";
  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(deviceKeyHex)
      ->setBaseVersion("1")
      ->setHeadVersion("1a1")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(text.size())
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view(text))
      ->setDeviceCert(*certRes)
      ->setEncrypted(true)
      ->setPassphrase("correct-passphrase");

  ASSERT_TRUE(pkg.signWithDeviceKey(deviceKeys.priv).has_value());

  auto exportRes = exportPackage(pkg);
  ASSERT_TRUE(exportRes.has_value());
  const auto &archive = *exportRes;

  // Wrong passphrase
  auto wrongRes = importPackage(
      archive, masterFp, true,
      std::span<const std::uint8_t, 32>(masterKeys.pub), "wrong-passphrase");
  ASSERT_FALSE(wrongRes.has_value());
  EXPECT_EQ(wrongRes.error(), SessionValidationError::DecryptionFailed);

  // Missing passphrase
  auto missingRes =
      importPackage(archive, masterFp, true,
                    std::span<const std::uint8_t, 32>(masterKeys.pub), "");
  ASSERT_FALSE(missingRes.has_value());
  EXPECT_EQ(missingRes.error(), SessionValidationError::MissingDecryptionKey);
}

TEST(SessionContainerTest, EncryptedPackageRejectsTamperedCiphertext) {
  const auto masterKeys   = generateKeyPair();
  const auto deviceKeys   = generateKeyPair();
  const auto masterFp     = computeSha256Hex(masterKeys.pub);
  const auto deviceKeyHex = toHexStr(deviceKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  const std::string text = "Tamper test";
  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("laptop")
      ->setDeviceKey(deviceKeyHex)
      ->setBaseVersion("1")
      ->setHeadVersion("1a1")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(text.size())
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view(text))
      ->setDeviceCert(*certRes)
      ->setEncrypted(true)
      ->setPassphrase("passphrase");

  ASSERT_TRUE(pkg.signWithDeviceKey(deviceKeys.priv).has_value());

  auto exportRes = exportPackage(pkg);
  ASSERT_TRUE(exportRes.has_value());
  auto archive = *exportRes;

  // Find "payload.enc" in tar archive and tamper with one byte of the content
  // past header
  const std::string tarName = "payload.enc";
  auto pos                  = archive.size();
  for (std::size_t i = 0; i + tarName.size() < archive.size(); ++i) {
    if (std::memcmp(archive.data() + i, tarName.data(), tarName.size()) == 0) {
      pos = i + 512; // skip header to payload data
      break;
    }
  }
  ASSERT_LT(pos, archive.size());
  archive[pos] ^= 0x55;

  auto tamperRes = importPackage(
      archive, masterFp, true,
      std::span<const std::uint8_t, 32>(masterKeys.pub), "passphrase");
  ASSERT_FALSE(tamperRes.has_value());
  // Either payload hash mismatch or decryption failed
  EXPECT_TRUE(tamperRes.error() ==
                  SessionValidationError::PayloadHashMismatch ||
              tamperRes.error() == SessionValidationError::DecryptionFailed);
}

TEST(SessionContainerTest, ContainerExportRejectsProtectedMainOrEmptyDeviceId) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  auto certRes = createX509DelegationCertificate(deviceKeys.pub,
                                                 masterKeys.priv, "laptop");
  ASSERT_TRUE(certRes.has_value());

  SessionPackage pkg;
  pkg.setManifestVersion(1)
      ->setMasterFingerprint(masterFp)
      ->setDeviceId("main") // Protected!
      ->setDeviceKey(toHexStr(deviceKeys.pub))
      ->setBaseVersion("1")
      ->setHeadVersion("1a1")
      ->setPrimediaOffset(0)
      ->setPrimediaLength(4)
      ->setTimestamp(1791480000)
      ->setPrimediaSlice(std::string_view("test"))
      ->setDeviceCert(*certRes);

  auto exportMainRes = exportPackage(pkg);
  ASSERT_FALSE(exportMainRes.has_value());
  EXPECT_EQ(exportMainRes.error(), SessionValidationError::InvalidFieldFormat);

  pkg.setDeviceId("");
  auto exportEmptyRes = exportPackage(pkg);
  ASSERT_FALSE(exportEmptyRes.has_value());
  EXPECT_EQ(exportEmptyRes.error(), SessionValidationError::InvalidFieldFormat);
}

TEST(SessionContainerTest, ContainerImportRejectsPathTraversal) {
  // Construct a minimal corrupt tar archive containing a traversal filename
  // "../evil.txt"
  std::vector<std::uint8_t> corruptTar(1024, 0);
  std::memcpy(corruptTar.data(), "../evil.txt", 11);
  // Set size octal string "0000010"
  std::memcpy(corruptTar.data() + 124, "0000010\0", 8);
  // Compute checksum
  std::memset(corruptTar.data() + 148, ' ', 8);
  unsigned int sum = 0;
  for (std::size_t i = 0; i < 512; ++i) {
    sum += corruptTar[i];
  }
  char chk[8];
  std::snprintf(chk, sizeof(chk), "%06o", sum);
  std::memcpy(corruptTar.data() + 148, chk, 7);

  auto res = importPackage(corruptTar, "master_fp");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), SessionValidationError::CorruptArchive);
}

} // namespace
} // namespace xanadu
