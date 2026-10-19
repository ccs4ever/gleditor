/**
 * @file fuzz_test.cpp
 * @brief In-process randomized property and mutation fuzz testing.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/binary_ops.hpp"
#include "common/xanadu/blessing.hpp"
#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/session_container.hpp"

namespace {

using xanadu::decodeBlessing;
using xanadu::decodeLinkPackage;
using xanadu::decodeMutablePointer;
using xanadu::decodePublication;
using xanadu::importPackage;
using xanadu::MicroversionId;
using xanadu::MutableLink;
using xanadu::Op;
using xanadu::PublicKey;
using xanadu::readBinaryOpsSpool;
using xanadu::readMicroversionId;
using xanadu::readOpsSpool;
using xanadu::readVarint;
using xanadu::SecretKey;
using xanadu::SessionManifest;
using xanadu::Signature;
using xanadu::identity::CertificateConstraints;
using xanadu::identity::PubKey32;
using xanadu::identity::StandardCryptoEngine;
using xanadu::identity::X509Certificate;

std::vector<std::uint8_t> generateRandomBytes(std::mt19937 &rng,
                                              std::size_t maxLen) {
  std::uniform_int_distribution<std::size_t> lenDist(0, maxLen);
  std::uniform_int_distribution<int> byteDist(0, 255);
  const auto len = lenDist(rng);
  std::vector<std::uint8_t> bytes(len);
  for (std::size_t i = 0; i < len; i++) {
    bytes[i] = static_cast<std::uint8_t>(byteDist(rng));
  }
  return bytes;
}

std::string generateRandomString(std::mt19937 &rng, std::size_t maxLen) {
  const auto bytes = generateRandomBytes(rng, maxLen);
  return std::string{reinterpret_cast<const char *>(bytes.data()),
                     bytes.size()};
}

TEST(FuzzTest, binaryOpsParserNeverCrashesOnRandomBytes) {
  std::mt19937 rng(42);
  for (int iter = 0; iter < 5000; iter++) {
    const auto bytes = generateRandomBytes(rng, 512);
    const std::string data{reinterpret_cast<const char *>(bytes.data()),
                           bytes.size()};

    // 1. readOpsSpool (auto-detecting binary vs text)
    std::istringstream in1(data);
    std::vector<xanadu::OpRecord> ops1;
    try {
      readOpsSpool(in1, ops1);
    } catch (const std::exception &) {
      // Graceful error expected
    }

    // 2. readBinaryOpsSpool directly
    std::istringstream in2(data);
    std::vector<xanadu::OpRecord> ops2;
    try {
      readBinaryOpsSpool(in2, ops2);
    } catch (const std::exception &) {
      // Graceful error expected
    }

    // 3. readVarint directly
    std::istringstream in3(data);
    std::uint64_t val = 0;
    try {
      std::ignore = readVarint(in3, val);
    } catch (const std::exception &) {
      // Graceful error expected
    }

    // 4. readMicroversionId directly
    std::istringstream in4(data);
    MicroversionId id;
    try {
      std::ignore = readMicroversionId(in4, id);
    } catch (const std::exception &) {
      // Graceful error expected
    }
  }
}

TEST(FuzzTest, bencodeAndManifestParsersNeverCrashOnRandomBytes) {
  std::mt19937 rng(1337);
  for (int iter = 0; iter < 5000; iter++) {
    const auto str = generateRandomString(rng, 1024);

    // 1. Raw Bencode decode
    try {
      std::ignore = xanadu::bencode::decode(str);
    } catch (const std::exception &) {
      // Graceful error
    }

    // 2. decodePublication
    std::ignore = decodePublication(str);

    // 3. decodeLinkPackage
    std::ignore = decodeLinkPackage(str);

    // 4. decodeBlessing
    std::ignore = decodeBlessing(str);

    // 5. decodeMutablePointer
    std::ignore = decodeMutablePointer(str);
  }
}

TEST(FuzzTest, mutableLinkAndHexParsersNeverCrashOnRandomStrings) {
  std::mt19937 rng(999);
  for (int iter = 0; iter < 5000; iter++) {
    const auto str = generateRandomString(rng, 256);

    // 1. MutableLink::parse
    try {
      std::ignore = MutableLink::parse(str);
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 2. PublicKey::fromHex
    try {
      std::ignore = PublicKey::fromHex(str);
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 3. SecretKey::fromHex
    try {
      std::ignore = SecretKey::fromHex(str);
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 4. Signature::fromHex
    try {
      std::ignore = Signature::fromHex(str);
    } catch (const std::exception &) {
      // Graceful exception expected
    }
  }
}

TEST(FuzzTest, microversionParserNeverCrashesOnRandomStrings) {
  std::mt19937 rng(2026);
  for (int iter = 0; iter < 5000; iter++) {
    const auto str = generateRandomString(rng, 256);

    // 1. MicroversionId::parse
    try {
      std::ignore = MicroversionId::parse(str);
    } catch (const std::exception &) {
      // Graceful exception expected
    }
  }
}

TEST(FuzzTest, SessionManifestParserNeverCrashesOnRandomBytes) {
  std::mt19937 rng(777);
  for (int iter = 0; iter < 4000; iter++) {
    const auto str = generateRandomString(rng, 1024);
    try {
      std::ignore = SessionManifest::parse(str);
    } catch (...) {
      FAIL() << "SessionManifest::parse threw an unhandled exception";
    }
  }

  // Mutated / semi-structured TSV lines
  static constexpr const char *kKeys[] = {
      "manifest_version",   "master_fingerprint",  "device_id",
      "device_key",         "base_version",        "head_version",
      "primedia_offset",    "primedia_length",     "timestamp",
      "encryption_cipher",  "encryption_salt",     "encryption_nonce",
      "payload_sha256",     "store_tables_sha256", "scroll_fragment_count",
      "link_package_count", "custom_key"};
  std::uniform_int_distribution<std::size_t> keyDist(0, std::size(kKeys) - 1);
  for (int iter = 0; iter < 1000; iter++) {
    std::string mutated;
    const int lines = 1 + (iter % 8);
    for (int l = 0; l < lines; ++l) {
      mutated += kKeys[keyDist(rng)];
      mutated.push_back('\t');
      mutated += generateRandomString(rng, 64);
      mutated.push_back('\n');
    }
    try {
      std::ignore = SessionManifest::parse(mutated);
    } catch (...) {
      FAIL() << "SessionManifest::parse threw on semi-structured TSV";
    }
  }
}

TEST(FuzzTest, ImportPackageNeverCrashesOnRandomBytes) {
  std::mt19937 rng(888);
  static constexpr std::string_view kDummyMasterFp =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  static constexpr std::array<std::uint8_t, 32> kDummyMasterKey = {
      0x3b, 0x6a, 0x27, 0xbc, 0xce, 0xb6, 0xa4, 0x2d, 0x62, 0xa3, 0xa8,
      0xd0, 0x2a, 0x6f, 0x0d, 0x73, 0x65, 0x32, 0x15, 0x77, 0x1d, 0xe2,
      0x43, 0xa6, 0x3a, 0xc0, 0x48, 0xa1, 0x8b, 0x59, 0xda, 0x29};

  for (int iter = 0; iter < 1000; iter++) {
    const auto bytes = generateRandomBytes(rng, 2048);
    const std::span<const std::uint8_t> span(bytes.data(), bytes.size());

    // 1. Default import
    try {
      std::ignore = importPackage(span, std::nullopt, false, std::nullopt, "");
    } catch (...) {
      FAIL() << "importPackage threw an unhandled exception";
    }

    // 2. Import with verification and dummy key / fingerprint / passphrase
    try {
      std::ignore = importPackage(span, kDummyMasterFp, true, kDummyMasterKey,
                                  "xuzz_passphrase");
    } catch (...) {
      FAIL() << "importPackage with verification threw an unhandled exception";
    }

    // 3. Permuted flags based on iteration
    const bool verifySig    = (iter & 1) != 0;
    const bool checkBinding = (iter & 2) != 0;
    const auto expectedFp =
        checkBinding ? std::optional<std::string_view>(kDummyMasterFp)
                     : std::nullopt;
    const auto masterKey =
        verifySig
            ? std::optional<std::span<const std::uint8_t, 32>>(kDummyMasterKey)
            : std::nullopt;
    const std::string_view pass = (iter & 4) != 0 ? "secret" : "";

    try {
      std::ignore = importPackage(span, expectedFp, verifySig, masterKey, pass);
    } catch (...) {
      FAIL()
          << "importPackage with permuted options threw an unhandled exception";
    }
  }
}

TEST(FuzzTest, VerifyDelegationNeverCrashesOnRandomBytes) {
  std::mt19937 rng(999);
  const auto masterEngineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(masterEngineRes.has_value());
  const auto &masterEngine = *masterEngineRes;
  const auto masterPub     = masterEngine.publicKey();

  StandardCryptoEngine engine;

  static constexpr std::uint64_t kTimestamps[] = {
      0, 1ULL, 1700000000ULL, 1791480000ULL,
      std::numeric_limits<std::uint64_t>::max()};

  for (int iter = 0; iter < 1000; iter++) {
    const auto bytes = generateRandomBytes(rng, 1024);
    X509Certificate cert;
    cert.der = bytes;

    // Test with boundary timestamps
    for (const auto ts : kTimestamps) {
      try {
        std::ignore = engine.verifyDelegation(cert, masterPub, ts);
      } catch (...) {
        FAIL() << "verifyDelegation threw an unhandled exception on timestamp "
               << ts;
      }
    }

    // Test with random master public key
    if (bytes.size() >= 32) {
      PubKey32 randomKey;
      std::memcpy(randomKey.bytes.data(), bytes.data(), 32);
      try {
        std::ignore = engine.verifyDelegation(cert, randomKey, 1700000000ULL);
      } catch (...) {
        FAIL() << "verifyDelegation threw an unhandled exception on random key";
      }
    }

    // Test with all-zeros key
    PubKey32 zeroKey{};
    try {
      std::ignore = engine.verifyDelegation(cert, zeroKey, 0);
    } catch (...) {
      FAIL() << "verifyDelegation threw an unhandled exception on zero key";
    }
  }
}

TEST(FuzzTest, AuthorizeCsrNeverCrashesOnRandomBytes) {
  std::mt19937 rng(1234);
  const auto masterEngineRes = StandardCryptoEngine::generate();
  ASSERT_TRUE(masterEngineRes.has_value());
  const auto &masterEngine = *masterEngineRes;

  StandardCryptoEngine engine;

  CertificateConstraints baseConstraints;
  baseConstraints.serialNumber = 1001;
  baseConstraints.notBefore    = 1000;
  baseConstraints.notAfter     = 2000000000;
  baseConstraints.deviceId     = "fuzz_device";
  baseConstraints.deviceName   = "Fuzz Test Device";
  baseConstraints.authorName   = "Fuzz Author";
  baseConstraints.authorEmail  = "author@fuzz.invalid";

  for (int iter = 0; iter < 1000; iter++) {
    const auto bytes = generateRandomBytes(rng, 1024);
    const std::span<const std::uint8_t> span(bytes.data(), bytes.size());

    // 1. Authorize CSR with base constraints
    try {
      std::ignore = engine.authorizeCsr(masterEngine, span, baseConstraints);
    } catch (...) {
      FAIL() << "authorizeCsr threw an unhandled exception on random bytes";
    }

    // 2. Authorize CSR with permuted constraints derived from random bytes
    if (bytes.size() >= 16) {
      CertificateConstraints permuted;
      std::memcpy(&permuted.notBefore, bytes.data(), sizeof(std::uint64_t));
      std::memcpy(&permuted.notAfter, bytes.data() + 8, sizeof(std::uint64_t));
      permuted.serialNumber = static_cast<std::uint64_t>(iter);
      permuted.deviceId =
          generateRandomString(rng, std::min<std::size_t>(bytes.size(), 32));
      try {
        std::ignore = engine.authorizeCsr(masterEngine, span, permuted);
      } catch (...) {
        FAIL() << "authorizeCsr threw on permuted constraints";
      }
    }
  }
}

} // namespace
