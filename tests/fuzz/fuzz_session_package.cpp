/**
 * @file fuzz_session_package.cpp
 * @brief LLVM libFuzzer harness for .xuzzpkg session packages, AEAD decryption,
 *        tar unpacking, and hardened MANIFEST.tsv parsing.
 */
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/session_container.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (0 == size) {
    return 0;
  }

  const std::span<const std::uint8_t> bytes(data, size);
  const std::string_view sv(reinterpret_cast<const char *>(data), size);

  using namespace xanadu;

  // 1. Direct hardened MANIFEST.tsv parser fuzzing
  try {
    std::ignore = SessionManifest::parse(sv);
  } catch (...) {
  }

  // 2. Direct package container unpacking & import fuzzing with raw payload
  try {
    std::ignore = importPackage(bytes, std::nullopt, false, std::nullopt, "");
  } catch (...) {
  }

  // 3. Import fuzzing with signature verification and author binding checks
  static constexpr std::string_view kDummyMasterFp =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  static constexpr std::array<std::uint8_t, 32> kDummyMasterKey = {
      0x3b, 0x6a, 0x27, 0xbc, 0xce, 0xb6, 0xa4, 0x2d, 0x62, 0xa3, 0xa8,
      0xd0, 0x2a, 0x6f, 0x0d, 0x73, 0x65, 0x32, 0x15, 0x77, 0x1d, 0xe2,
      0x43, 0xa6, 0x3a, 0xc0, 0x48, 0xa1, 0x8b, 0x59, 0xda, 0x29};

  try {
    std::ignore =
        importPackage(bytes, kDummyMasterFp, true, kDummyMasterKey, "secret");
  } catch (...) {
  }

  // 4. Parameterized permutation fuzzing driven by leading byte
  if (size > 4) {
    const std::uint8_t flags = data[0];
    const bool verifySig     = (flags & 0x01) != 0;
    const bool checkBinding  = (flags & 0x02) != 0;
    const bool useMasterKey  = (flags & 0x04) != 0;
    const bool usePassphrase = (flags & 0x08) != 0;

    std::optional<std::string_view> expectedFp = std::nullopt;
    if (checkBinding) {
      expectedFp = kDummyMasterFp;
    }

    std::optional<std::span<const std::uint8_t, 32>> masterKeySpan =
        std::nullopt;
    if (useMasterKey) {
      masterKeySpan = kDummyMasterKey;
    }

    std::string_view passphrase = usePassphrase ? "xuzz_passphrase" : "";

    const std::span<const std::uint8_t> payloadBytes(data + 1, size - 1);
    try {
      std::ignore = importPackage(payloadBytes, expectedFp, verifySig,
                                  masterKeySpan, passphrase);
    } catch (...) {
    }
  }

  return 0;
}
