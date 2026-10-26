/**
 * @file store_loader_test.cpp
 * @brief Unit tests for StoreLoader native import and air-gapped fat store
 *        reconstitution with minimal primedia fragments.
 */
#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/session_container.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/store_tables.hpp"
#include "common/xanadu/swarm.hpp"

#include <array>
#include <filesystem>
#include <string>
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

class StoreLoaderTest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir_ = std::filesystem::temp_directory_path() /
               ("store_loader_test_" + std::to_string(std::time(nullptr)));
    std::filesystem::create_directories(testDir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(testDir_, ec);
  }

  std::filesystem::path testDir_;
};

TEST_F(StoreLoaderTest, ImportStorePackageReconstitutesStoreAndFragments) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  Store sourceStore;
  auto v1 = sourceStore.insert(MicroversionId{}, 0,
                               "The sovereign hypermedia foundation.");
  auto v2 = sourceStore.insert(v1, 14, " Xanadu and ZigZag unified");

  // Create LinkPackage with curator and scrolls
  const auto curatorKeys = createMutableKeys();
  Scroll scrollA;
  scrollA.publisher      = curatorKeys.publicKey;
  scrollA.salt           = "concept_a";
  const std::string keyA = scrollKey(scrollA).value();

  Scroll scrollB;
  scrollB.publisher      = curatorKeys.publicKey;
  scrollB.salt           = "concept_b";
  const std::string keyB = scrollKey(scrollB).value();

  std::map<std::string, Scroll> pkgScrolls{{keyA, scrollA}, {keyB, scrollB}};

  GlobalLink glink;
  glink.type  = LinkType::Quotation;
  glink.owner = "curator";
  glink.left.push_back(GlobalSpan{.scroll = keyA, .start = 0, .length = 13});
  glink.right.push_back(GlobalSpan{.scroll = keyB, .start = 14, .length = 26});
  auto lp = publishLinkPackage(curatorKeys, "salt_42", "Foundational Concepts",
                               1, 1700000000, {glink}, pkgScrolls);

  auto pkgRes = createFatStorePackage(
      sourceStore, "laptop", deviceKeys.priv, masterFp, "0", v2.str(),
      /*encrypted=*/false, /*passphrase=*/"", {&lp, 1},
      /*existingDeviceCert=*/std::nullopt,
      /*masterPrivateKey=*/masterKeys.priv);
  ASSERT_TRUE(pkgRes.has_value());

  const auto destDir = testDir_ / "imported_station_store";
  ImportStorePackageOptions options;
  options.expectedMasterFingerprint = masterFp;
  options.verifySignature           = true;
  options.adoptIncludedLinkPackages = true;

  auto importRes = importStorePackage(*pkgRes, destDir, options);
  ASSERT_TRUE(importRes.has_value());
  const auto &result = *importRes;

  EXPECT_TRUE(std::filesystem::exists(destDir / "ops.nodes"));
  EXPECT_TRUE(std::filesystem::exists(destDir / "store.tables"));
  EXPECT_GT(result.materializedFragments, 0U);
  EXPECT_EQ(result.adoptedLinkPackages, 1U);
  EXPECT_GT(result.adoptedLinksCount, 0U);

  ASSERT_NE(result.store, nullptr);
  EXPECT_GE(result.store->opCount(), sourceStore.opCount());

  // Verify text resolution with exact zero-shift coordinates
  PrimediaSpan span1{.scroll = localScroll, .start = 0, .length = 13};
  auto res1 = result.store->resolve(span1);
  EXPECT_EQ(res1.status, ResolutionStatus::VerifiedBytes);
  EXPECT_EQ(res1.text, "The sovereign");

  PrimediaSpan span2{.scroll = localScroll, .start = 14, .length = 6};
  auto res2 = result.store->resolve(span2);
  EXPECT_EQ(res2.status, ResolutionStatus::VerifiedBytes);
  EXPECT_EQ(res2.text, "hyperm");

  PrimediaSpan span3{.scroll = localScroll, .start = 37, .length = 6};
  auto res3 = result.store->resolve(span3);
  EXPECT_EQ(res3.status, ResolutionStatus::VerifiedBytes);
  EXPECT_EQ(res3.text, "Xanadu");
}

TEST_F(StoreLoaderTest, ImportEncryptedPackageFromDisk) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  Store sourceStore;
  auto v1 = sourceStore.insert(MicroversionId{}, 0,
                               "Encrypted station package on disk");

  const std::string passphrase = "SecureAirgapStationKey!";
  auto pkgRes                  = createFatStorePackage(
      sourceStore, "laptop", deviceKeys.priv, masterFp, "0", v1.str(),
      /*encrypted=*/true, passphrase, /*selectedLinkPackages=*/{},
      /*existingDeviceCert=*/std::nullopt,
      /*masterPrivateKey=*/masterKeys.priv);
  ASSERT_TRUE(pkgRes.has_value());

  const auto pkgPath = testDir_ / "store.xuzzpkg";
  auto exportRes     = exportPackage(*pkgRes, pkgPath);
  ASSERT_TRUE(exportRes.has_value());
  EXPECT_TRUE(std::filesystem::exists(pkgPath));

  const auto destDir = testDir_ / "imported_encrypted_station";
  ImportStorePackageOptions options;
  options.expectedMasterFingerprint = masterFp;
  options.passphrase                = passphrase;

  auto importRes = importStorePackage(pkgPath, destDir, options);
  ASSERT_TRUE(importRes.has_value());
  const auto &result = *importRes;

  ASSERT_NE(result.store, nullptr);
  PrimediaSpan fullSpan{.scroll = localScroll, .start = 0, .length = 33};
  EXPECT_EQ(result.store->read(fullSpan), "Encrypted station package on disk");
}

TEST_F(StoreLoaderTest, ImportExternalScrollFragments) {
  const auto masterKeys = generateKeyPair();
  const auto deviceKeys = generateKeyPair();
  const auto masterFp   = computeSha256Hex(masterKeys.pub);

  Store sourceStore;
  sourceStore.insert(MicroversionId{}, 0, "Host text");

  // Add an external scroll with publisher and salt
  Scroll extScroll;
  extScroll.publisher = PublicKey{deviceKeys.pub};
  extScroll.salt      = "quote_salt";
  ASSERT_TRUE(extScroll.publisher);
  const auto extScrollId = sourceStore.addScroll(extScroll);

  // Set external live bytes on source
  const std::string authorKey =
      "btpk:" + extScroll.publisher->hex() + ":" + extScroll.salt;
  sourceStore.setExternalLiveBytes(authorKey, 1000,
                                   "Quoted remote scroll passage");

  // Analyze and package
  auto pkgRes = createFatStorePackage(
      sourceStore, "laptop", deviceKeys.priv, masterFp, "0", "1",
      /*encrypted=*/false, /*passphrase=*/"", /*selectedLinkPackages=*/{},
      /*existingDeviceCert=*/std::nullopt,
      /*masterPrivateKey=*/masterKeys.priv);
  ASSERT_TRUE(pkgRes.has_value());

  // Add the external fragment to the package manually to ensure multi-scroll
  // coverage
  ScrollFragment extFrag;
  extFrag.scrollId       = extScrollId;
  extFrag.start          = 1000;
  extFrag.length         = 28;
  const std::string text = "Quoted remote scroll passage";
  extFrag.bytes.assign(text.begin(), text.end());
  extFrag.sha256 = computeSha256Hex(extFrag.bytes);
  pkgRes->addScrollFragment(extFrag);

  auto reSignRes = pkgRes->signWithDeviceKey(deviceKeys.priv);
  ASSERT_TRUE(reSignRes.has_value());

  const auto destDir = testDir_ / "imported_ext_station";
  ImportStorePackageOptions options;
  options.expectedMasterFingerprint = masterFp;

  auto importRes = importStorePackage(*pkgRes, destDir, options);
  ASSERT_TRUE(importRes.has_value());
  const auto &result = *importRes;

  PrimediaSpan extSpan{.scroll = extScrollId, .start = 1000, .length = 28};
  EXPECT_EQ(result.store->read(extSpan), "Quoted remote scroll passage");
}

} // namespace
} // namespace xanadu
