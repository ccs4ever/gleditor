#include "mutable_link.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "bencode.hpp"
#include "torrent.hpp"

namespace xudu {

namespace {

/// Fill a fixed-size key or signature from hex, complaining in terms of what
/// was being read rather than in terms of bytes.
template <std::size_t N>
std::array<std::uint8_t, N> arrayFromHex(const std::string_view text,
                                         const std::string_view what) {
  if (text.size() != N * 2) {
    throw std::runtime_error(std::string{what} + ": expected " +
                             std::to_string(N * 2) + " hex digits, got " +
                             std::to_string(text.size()));
  }
  const auto raw = fromHex(text);
  std::array<std::uint8_t, N> out{};
  std::copy(raw.begin(), raw.end(), out.begin());
  return out;
}

template <std::size_t N>
std::string arrayToHex(const std::array<std::uint8_t, N> &bytes) {
  return toHex({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

} // namespace

std::string PublicKey::hex() const { return arrayToHex(bytes); }

PublicKey PublicKey::fromHex(const std::string_view text) {
  return {arrayFromHex<32>(text, "public key")};
}

bool PublicKey::isZero() const {
  return std::ranges::all_of(bytes,
                             [](const std::uint8_t byte) { return 0 == byte; });
}

std::string SecretKey::hex() const { return arrayToHex(bytes); }

SecretKey SecretKey::fromHex(const std::string_view text) {
  return {arrayFromHex<64>(text, "secret key")};
}

std::string Signature::hex() const { return arrayToHex(bytes); }

Signature Signature::fromHex(const std::string_view text) {
  return {arrayFromHex<64>(text, "signature")};
}

std::string DhtTarget::hex() const { return arrayToHex(bytes); }

bool MutableLink::looksLikeMutableLink(const std::string_view text) {
  if (!text.starts_with("magnet:?")) {
    return false;
  }
  bool found = false;
  forEachMagnetParameter(
      text, [&found](const std::string_view key, const std::string &value) {
        found = found || ("xs" == key && value.starts_with("urn:btpk:"));
      });
  return found;
}

MutableLink MutableLink::parse(const std::string_view uri) {
  if (!uri.starts_with("magnet:?")) {
    throw std::runtime_error("not a magnet link: " + std::string{uri});
  }

  MutableLink link;
  bool haveKey = false;
  forEachMagnetParameter(uri, [&link, &haveKey](const std::string_view key,
                                                const std::string &value) {
    if ("xs" == key) {
      constexpr std::string_view prefix = "urn:btpk:";
      if (value.starts_with(prefix)) {
        link.key =
            PublicKey::fromHex(std::string_view{value}.substr(prefix.size()));
        haveKey = true;
      }
    } else if ("s" == key) {
      // Hex in the link, bytes everywhere else: it is hashed into the target
      // and signed over, and both of those are byte operations.
      link.salt = fromHex(value);
    } else if ("xt" == key) {
      constexpr std::string_view prefix = "urn:btih:";
      if (value.starts_with(prefix)) {
        const auto digits = std::string_view{value}.substr(prefix.size());
        if (40 == digits.size()) {
          link.currentWhenWritten = InfoHash::fromHex(digits);
        }
      }
    } else if ("dn" == key) {
      link.displayName = value;
    } else if ("tr" == key) {
      link.trackers.push_back(value);
    }
  });

  if (!haveKey) {
    throw std::runtime_error("link names no public key (xs=urn:btpk:): " +
                             std::string{uri});
  }
  return link;
}

std::string MutableLink::uri() const {
  std::string out = "magnet:?xs=urn:btpk:" + key.hex();
  if (!salt.empty()) {
    out += "&s=" + toHex(salt);
  }
  if (!displayName.empty()) {
    out += "&dn=" + displayName;
  }
  return out;
}

DhtTarget MutableLink::target() const {
  // "the SHA-1 hash of the public key (as it appears in the bencoded dict) and
  // the salt (if present)" -- so the raw thirty-two bytes, then the salt, with
  // nothing between them and no length prefix.
  std::string material(reinterpret_cast<const char *>(key.bytes.data()),
                       key.bytes.size());
  material += salt;
  return {sha1(material)};
}

std::string mutableSigningBuffer(const std::string_view salt,
                                 const std::int64_t sequence,
                                 const std::string_view encodedValue) {
  std::string out;
  if (!salt.empty()) {
    out += "4:salt" + std::to_string(salt.size()) + ":";
    out += salt;
  }
  out += "3:seqi" + std::to_string(sequence) + "e1:v";
  out += encodedValue;
  return out;
}

std::string encodeMutablePointer(const InfoHash &hash) {
  const std::string raw(reinterpret_cast<const char *>(hash.bytes.data()),
                        hash.bytes.size());
  return bencode::Value::dict({{"ih", bencode::Value::string(raw)}}).encode();
}

std::optional<InfoHash>
decodeMutablePointer(const std::string_view encodedValue) {
  try {
    const auto value          = bencode::decode(encodedValue);
    const auto *const pointer = value.find("ih");
    if (nullptr == pointer || !pointer->isString()) {
      return std::nullopt;
    }
    const auto &raw = pointer->asString();
    if (raw.size() != std::tuple_size_v<decltype(InfoHash::bytes)>) {
      // Twenty bytes or it is not an info hash. A shorter one padded out, or a
      // longer one truncated, would be a reference to content nobody named.
      return std::nullopt;
    }
    InfoHash hash;
    std::copy(raw.begin(), raw.end(), hash.bytes.begin());
    return hash;
  } catch (const std::exception &) {
    // Not bencode, or not a dictionary. A name may hold anything at all; this
    // one is simply not pointing at a torrent.
    return std::nullopt;
  }
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
