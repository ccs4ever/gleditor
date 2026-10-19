/**
 * @file fuzz_x509_delegation.cpp
 * @brief LLVM libFuzzer harness for X.509 delegation certificate path
 *        validation, PKCS#10 CSR authorization, and DER/ASN.1 parsing.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }

  const std::span<const std::uint8_t> bytes(data, size);
  const std::string_view sv(reinterpret_cast<const char *>(data), size);

  using namespace xanadu::identity;

  // Statically initialized master key engine for authorization & verification
  static const auto masterEngineRes = StandardCryptoEngine::generate();
  if (!masterEngineRes) {
    return 0;
  }
  const auto &masterEngine = *masterEngineRes;
  const auto masterPub     = masterEngine.publicKey();

  StandardCryptoEngine engine;

  // 1. Fuzz X.509 Delegation Certificate Path Validation with varied timestamps
  X509Certificate cert;
  cert.der.assign(data, data + size);

  // Boundary timestamps: 0 (disabled), 1, Max, Epoch 1791480000
  static constexpr std::uint64_t kTimestamps[] = {
      0, 1ULL, 1791480000ULL, std::numeric_limits<std::uint64_t>::max()};
  for (const auto ts : kTimestamps) {
    try {
      std::ignore = engine.verifyDelegation(cert, masterPub, ts);
    } catch (...) {
    }
  }

  // Fuzz with timestamp and custom master key derived from fuzz input
  if (size >= 8) {
    std::uint64_t customTs = 0;
    std::memcpy(&customTs, data, sizeof(customTs));
    try {
      std::ignore = engine.verifyDelegation(cert, masterPub, customTs);
    } catch (...) {
    }
  }

  if (size >= 32) {
    PubKey32 customKey;
    std::memcpy(customKey.bytes.data(), data, 32);
    try {
      std::ignore = engine.verifyDelegation(cert, customKey, 0);
    } catch (...) {
    }
  }

  // Also test with all-zeros key
  PubKey32 zeroKey{};
  try {
    std::ignore = engine.verifyDelegation(cert, zeroKey, 0);
  } catch (...) {
  }

  // 2. Fuzz PKCS#10 CSR Authorization & Parsing
  CertificateConstraints constraints;
  constraints.notBefore  = 1000;
  constraints.notAfter   = 2000000000;
  constraints.deviceId   = "fuzz_device";
  constraints.deviceName = "Fuzz Test Device";

  try {
    std::ignore = engine.authorizeCsr(masterEngine, bytes, constraints);
  } catch (...) {
  }

  // Permute constraints if fuzz data allows
  if (size >= 16) {
    CertificateConstraints permutedConstraints;
    std::memcpy(&permutedConstraints.notBefore, data, 8);
    std::memcpy(&permutedConstraints.notAfter, data + 8, 8);
    permutedConstraints.deviceId =
        std::string(sv.substr(16, std::min<size_t>(size - 16, 32)));
    try {
      std::ignore =
          engine.authorizeCsr(masterEngine, bytes, permutedConstraints);
    } catch (...) {
    }
  }

  // 3. Fuzz Delegation Certificate creation with fuzzed device public key
  if (size >= 32) {
    PubKey32 devKey;
    std::memcpy(devKey.bytes.data(), data, 32);
    try {
      std::ignore =
          engine.createDelegationCertificate(masterEngine, devKey, constraints);
    } catch (...) {
    }
  }

  // 4. Fuzz String / DER decoding utilities
  try {
    std::ignore = base64Decode(sv);
  } catch (...) {
  }

  try {
    std::ignore = StandardCryptoEngine::publicKeyFromAnyFormat(sv);
  } catch (...) {
  }

  return 0;
}
