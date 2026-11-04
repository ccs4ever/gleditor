/**
 * @file health_grants.cpp
 * @brief Implementation of health grant tokens, issuers and validation
 *        (P19 I3-2c1).
 */
#include "common/xanadu/vagent/runtime/health_grants.hpp"

#include <algorithm>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace xanadu::vagent::runtime {

namespace {

bool isLowercaseHex64(std::string_view s) noexcept {
  return s.size() == 64 && std::ranges::all_of(s, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

} // namespace

std::optional<HealthIssuer>
HealthIssuer::fromPrincipal(std::string_view principal) {
  if (!isLowercaseHex64(principal)) return std::nullopt;
  return HealthIssuer(std::string(principal));
}

HealthGrantToken::HealthGrantToken(std::span<const std::byte, 16> bytes) {
  std::ranges::copy(bytes, bytes_.begin());
}

HealthGrantToken::HealthGrantToken(const HealthGrantToken &other)
    : bytes_(other.bytes_) {}

HealthGrantToken &HealthGrantToken::operator=(const HealthGrantToken &other) {
  if (this != &other) {
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
    bytes_ = other.bytes_;
  }
  return *this;
}

HealthGrantToken::HealthGrantToken(HealthGrantToken &&other) noexcept
    : bytes_(other.bytes_) {
  OPENSSL_cleanse(other.bytes_.data(), other.bytes_.size());
}

HealthGrantToken &
HealthGrantToken::operator=(HealthGrantToken &&other) noexcept {
  if (this != &other) {
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
    bytes_ = other.bytes_;
    OPENSSL_cleanse(other.bytes_.data(), other.bytes_.size());
  }
  return *this;
}

HealthGrantToken::~HealthGrantToken() {
  OPENSSL_cleanse(bytes_.data(), bytes_.size());
}

std::optional<HealthGrantToken>
HealthGrantToken::fromBytes(std::span<const std::byte, 16> bytes) {
  if (bytes.size() != 16) return std::nullopt;
  const bool allZero =
      std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
  if (allZero) return std::nullopt;
  return HealthGrantToken(bytes);
}

std::optional<HealthGrantToken> HealthGrantToken::generate() {
  std::array<std::byte, 16> buf{};
  if (RAND_bytes(reinterpret_cast<unsigned char *>(buf.data()),
                 static_cast<int>(buf.size())) != 1)
    return std::nullopt;
  const bool allZero =
      std::ranges::all_of(buf, [](std::byte b) { return b == std::byte{0}; });
  if (allZero) return std::nullopt;
  return HealthGrantToken(buf);
}

std::array<std::byte, 32> HealthGrantToken::digest() const {
  std::array<std::byte, 32> out{};
  SHA256(reinterpret_cast<const unsigned char *>(bytes_.data()), bytes_.size(),
         reinterpret_cast<unsigned char *>(out.data()));
  return out;
}

bool isReservedDerivedHealthCode(std::string_view code) noexcept {
  return code == "refusal_fraction" || code == "rejection_streak" ||
         code == "fault";
}

bool isValidHealthCode(std::string_view code) noexcept {
  if (code.empty() || code.size() > 64) return false;
  if (isReservedDerivedHealthCode(code)) return false;
  return std::ranges::all_of(code, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-';
  });
}

bool isValidHealthGrantId(std::string_view id) noexcept {
  if (id.empty() || id.size() > kMaxObservationNameBytes) return false;
  return std::ranges::all_of(id, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' ||
           c == '-';
  });
}

} // namespace xanadu::vagent::runtime
