/**
 * @file fuzz_test.cpp
 * @brief In-process randomized property and mutation fuzz testing.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <xudu/core/bencode.hpp>
#include <xudu/core/binary_ops.hpp>
#include <xudu/core/blessing.hpp>
#include <xudu/core/link_package.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/mutable_link.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/yaml.hpp>

namespace {

using xudu::decodeBlessing;
using xudu::decodeLinkPackage;
using xudu::decodeMutablePointer;
using xudu::decodePublication;
using xudu::MicroversionId;
using xudu::MutableLink;
using xudu::Op;
using xudu::PublicKey;
using xudu::readBinaryOpsSpool;
using xudu::readMicroversionId;
using xudu::readOpsSpool;
using xudu::readVarint;
using xudu::SecretKey;
using xudu::Signature;

std::vector<std::uint8_t> generateRandomBytes(std::mt19937 &rng, std::size_t maxLen) {
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
  return std::string{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

TEST(FuzzTest, binaryOpsParserNeverCrashesOnRandomBytes) {
  std::mt19937 rng(42);
  for (int iter = 0; iter < 5000; iter++) {
    const auto bytes = generateRandomBytes(rng, 512);
    const std::string data{reinterpret_cast<const char *>(bytes.data()), bytes.size()};

    // 1. readOpsSpool (auto-detecting binary vs text)
    std::istringstream in1(data);
    std::map<MicroversionId, Op> ops1;
    try {
      readOpsSpool(in1, ops1);
    } catch (const std::exception &) {
      // Graceful error expected
    }

    // 2. readBinaryOpsSpool directly
    std::istringstream in2(data);
    std::map<MicroversionId, Op> ops2;
    try {
      readBinaryOpsSpool(in2, ops2);
    } catch (const std::exception &) {
      // Graceful error expected
    }

    // 3. readVarint directly
    std::istringstream in3(data);
    std::uint64_t val = 0;
    try {
      static_cast<void>(readVarint(in3, val));
    } catch (const std::exception &) {
      // Graceful error expected
    }

    // 4. readMicroversionId directly
    std::istringstream in4(data);
    MicroversionId id;
    try {
      static_cast<void>(readMicroversionId(in4, id));
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
      static_cast<void>(xudu::bencode::decode(str));
    } catch (const std::exception &) {
      // Graceful error
    }

    // 2. decodePublication
    static_cast<void>(decodePublication(str));

    // 3. decodeLinkPackage
    static_cast<void>(decodeLinkPackage(str));

    // 4. decodeBlessing
    static_cast<void>(decodeBlessing(str));

    // 5. decodeMutablePointer
    static_cast<void>(decodeMutablePointer(str));
  }
}

TEST(FuzzTest, mutableLinkAndHexParsersNeverCrashOnRandomStrings) {
  std::mt19937 rng(999);
  for (int iter = 0; iter < 5000; iter++) {
    const auto str = generateRandomString(rng, 256);

    // 1. MutableLink::parse
    try {
      static_cast<void>(MutableLink::parse(str));
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 2. PublicKey::fromHex
    try {
      static_cast<void>(PublicKey::fromHex(str));
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 3. SecretKey::fromHex
    try {
      static_cast<void>(SecretKey::fromHex(str));
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 4. Signature::fromHex
    try {
      static_cast<void>(Signature::fromHex(str));
    } catch (const std::exception &) {
      // Graceful exception expected
    }
  }
}

TEST(FuzzTest, microversionAndYamlParsersNeverCrashOnRandomStrings) {
  std::mt19937 rng(2026);
  for (int iter = 0; iter < 5000; iter++) {
    const auto str = generateRandomString(rng, 256);

    // 1. MicroversionId::parse
    try {
      static_cast<void>(MicroversionId::parse(str));
    } catch (const std::exception &) {
      // Graceful exception expected
    }

    // 2. xudu::yaml::read
    static_cast<void>(xudu::yaml::read(str));
  }
}

} // namespace
