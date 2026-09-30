/**
 * @file fuzz_test.cpp
 * @brief In-process randomized property and mutation fuzz testing.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/binary_ops.hpp"
#include "common/xanadu/blessing.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/mutable_link.hpp"
#include "common/xanadu/publication.hpp"

namespace {

using xanadu::decodeBlessing;
using xanadu::decodeLinkPackage;
using xanadu::decodeMutablePointer;
using xanadu::decodePublication;
using xanadu::MicroversionId;
using xanadu::MutableLink;
using xanadu::Op;
using xanadu::PublicKey;
using xanadu::readBinaryOpsSpool;
using xanadu::readMicroversionId;
using xanadu::readOpsSpool;
using xanadu::readVarint;
using xanadu::SecretKey;
using xanadu::Signature;

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

} // namespace
