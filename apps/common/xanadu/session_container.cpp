/**
 * @file session_container.cpp
 * @brief Implementation of .xuzzpkg container packaging, unpacking, manifest
 *        parsing, and cryptographic verification.
 */
#include "session_container.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <unordered_set>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <zstd.h>

#include <gleditor/logging.hpp>

namespace xanadu {

namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::string base64Encode(std::span<const std::uint8_t> data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < data.size(); i += 3) {
    const std::uint32_t b0     = data[i];
    const std::uint32_t b1     = (i + 1 < data.size()) ? data[i + 1] : 0;
    const std::uint32_t b2     = (i + 2 < data.size()) ? data[i + 2] : 0;
    const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    if (i + 1 < data.size()) {
      out.push_back(kBase64Alphabet[(triple >> 6) & 0x3F]);
    } else {
      out.push_back('=');
    }
    if (i + 2 < data.size()) {
      out.push_back(kBase64Alphabet[triple & 0x3F]);
    } else {
      out.push_back('=');
    }
  }
  return out;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
base64Decode(std::string_view b64) {
  std::vector<std::uint8_t> out;
  out.reserve((b64.size() * 3) / 4);

  auto decodeChar = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  std::uint32_t val = 0;
  int valb          = -8;
  for (const char c : b64) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    if (c == '=') break;
    const int d = decodeChar(c);
    if (d < 0) return std::nullopt;
    val = (val << 6) | static_cast<std::uint32_t>(d);
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

[[nodiscard]] bool isAllHex(std::string_view s) noexcept {
  if (s.empty()) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string toHex(std::span<const std::uint8_t> data) {
  static constexpr char hexChars[] = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (const auto b : data) {
    s.push_back(hexChars[(b >> 4) & 0x0F]);
    s.push_back(hexChars[b & 0x0F]);
  }
  return s;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
fromHex(std::string_view hex) {
  if (hex.size() % 2 != 0) return std::nullopt;
  std::vector<std::uint8_t> out;
  out.reserve(hex.size() / 2);
  auto decodeNibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = decodeNibble(hex[i]);
    const int lo = decodeNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view a,
                                    std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

struct alignas(512) TarHeader {
  char name[100]{};
  char mode[8]{};
  char uid[8]{};
  char gid[8]{};
  char size[12]{};
  char mtime[12]{};
  char chksum[8]{};
  char typeflag{'0'};
  char linkname[100]{};
  char magic[6]{'u', 's', 't', 'a', 'r', '\0'};
  char version[2]{'0', '0'};
  char uname[32]{"xuzz"};
  char gname[32]{"xuzz"};
  char devmajor[8]{};
  char devminor[8]{};
  char prefix[155]{};
  char padding[12]{};
};
static_assert(sizeof(TarHeader) == 512, "TarHeader must be exactly 512 bytes");

void formatTarHeader(TarHeader &header, std::string_view name, std::size_t size,
                     std::uint64_t mtime) {
  std::memset(&header, 0, sizeof(header));
  const auto copyNameLen = std::min(name.size(), sizeof(header.name) - 1);
  std::memcpy(header.name, name.data(), copyNameLen);
  std::snprintf(header.mode, sizeof(header.mode), "%07o", 0644);
  std::snprintf(header.uid, sizeof(header.uid), "%07o", 0);
  std::snprintf(header.gid, sizeof(header.gid), "%07o", 0);
  std::snprintf(header.size, sizeof(header.size), "%011zo", size);
  std::snprintf(header.mtime, sizeof(header.mtime), "%011lo",
                static_cast<unsigned long>(mtime));
  header.typeflag = '0';
  std::memcpy(header.magic, "ustar\0", 6);
  std::memcpy(header.version, "00", 2);
  std::memcpy(header.uname, "xuzz\0", 5);
  std::memcpy(header.gname, "xuzz\0", 5);

  std::memset(header.chksum, ' ', sizeof(header.chksum));
  unsigned int sum = 0;
  const auto *raw  = reinterpret_cast<const unsigned char *>(&header);
  for (std::size_t i = 0; i < 512; ++i) {
    sum += raw[i];
  }
  std::snprintf(header.chksum, sizeof(header.chksum), "%06o", sum);
  header.chksum[6] = '\0';
  header.chksum[7] = ' ';
}

[[nodiscard]] bool verifyTarHeaderChecksum(const TarHeader &header) {
  char *endptr            = nullptr;
  unsigned long storedSum = std::strtoul(header.chksum, &endptr, 8);
  if (endptr == header.chksum) {
    return false;
  }

  unsigned int sumUnsigned = 0;
  int sumSigned            = 0;
  const auto *raw          = reinterpret_cast<const unsigned char *>(&header);
  for (std::size_t i = 0; i < 512; ++i) {
    if (i >= 148 && i < 156) {
      sumUnsigned += ' ';
      sumSigned += ' ';
    } else {
      sumUnsigned += raw[i];
      sumSigned += static_cast<signed char>(raw[i]);
    }
  }

  return (storedSum == sumUnsigned ||
          static_cast<long>(storedSum) == sumSigned);
}

void appendTarEntry(std::vector<std::uint8_t> &out, std::string_view name,
                    std::span<const std::uint8_t> content,
                    std::uint64_t timestamp) {
  TarHeader header;
  formatTarHeader(header, name, content.size(), timestamp);
  const auto *hBytes = reinterpret_cast<const std::uint8_t *>(&header);
  out.insert(out.end(), hBytes, hBytes + 512);
  out.insert(out.end(), content.begin(), content.end());
  const std::size_t pad = (512 - (content.size() % 512)) % 512;
  out.insert(out.end(), pad, static_cast<std::uint8_t>(0));
}

[[nodiscard]] std::expected<std::map<std::string, std::vector<std::uint8_t>>,
                            SessionValidationError>
unpackTar(std::span<const std::uint8_t> archive) {
  std::map<std::string, std::vector<std::uint8_t>> entries;
  std::size_t offset = 0;

  while (offset + 512 <= archive.size()) {
    bool allZeros = true;
    for (std::size_t i = 0; i < 512; ++i) {
      if (archive[offset + i] != 0) {
        allZeros = false;
        break;
      }
    }
    if (allZeros) {
      break;
    }

    TarHeader header;
    std::memcpy(&header, archive.data() + offset, 512);
    offset += 512;

    if (!verifyTarHeaderChecksum(header)) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    std::string name;
    if (header.prefix[0] != '\0') {
      name.append(header.prefix, strnlen(header.prefix, sizeof(header.prefix)));
      name.push_back('/');
    }
    name.append(header.name, strnlen(header.name, sizeof(header.name)));

    if (name.starts_with("./")) {
      name.erase(0, 2);
    }

    char *endptr     = nullptr;
    std::size_t size = std::strtoull(header.size, &endptr, 8);
    if (endptr == header.size) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    if (offset + size > archive.size()) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    std::vector<std::uint8_t> data(archive.data() + offset,
                                   archive.data() + offset + size);
    entries[name] = std::move(data);

    offset += size;
    const std::size_t pad = (512 - (size % 512)) % 512;
    if (offset + pad > archive.size()) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    offset += pad;
  }

  return entries;
}

[[nodiscard]] bool
isZstdCompressed(std::span<const std::uint8_t> data) noexcept {
  return data.size() >= 4 && data[0] == 0x28 && data[1] == 0xB5 &&
         data[2] == 0x2F && data[3] == 0xFD;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, SessionValidationError>
decompressIfZstd(std::span<const std::uint8_t> data) {
  if (!isZstdCompressed(data)) {
    return std::vector<std::uint8_t>(data.begin(), data.end());
  }

  const unsigned long long contentSize =
      ZSTD_getFrameContentSize(data.data(), data.size());
  if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
    return std::unexpected(SessionValidationError::CorruptArchive);
  }

  std::vector<std::uint8_t> out;
  if (contentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
      contentSize <= 512ULL * 1024 * 1024) {
    out.resize(static_cast<std::size_t>(contentSize));
    const std::size_t res =
        ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(res)) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    out.resize(res);
  } else {
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (!dctx) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    out.resize(1024 * 1024);
    ZSTD_inBuffer inBuf   = {data.data(), data.size(), 0};
    ZSTD_outBuffer outBuf = {out.data(), out.size(), 0};

    while (inBuf.pos < inBuf.size) {
      const size_t ret = ZSTD_decompressStream(dctx, &outBuf, &inBuf);
      if (ZSTD_isError(ret)) {
        ZSTD_freeDCtx(dctx);
        return std::unexpected(SessionValidationError::CorruptArchive);
      }
      if (outBuf.pos == outBuf.size) {
        out.resize(out.size() * 2);
        outBuf.dst  = out.data();
        outBuf.size = out.size();
      }
    }
    out.resize(outBuf.pos);
    ZSTD_freeDCtx(dctx);
  }
  return out;
}

} // namespace

std::string SessionManifest::serialize() const {
  std::string out;
  out.reserve(256);
  out += "manifest_version\t" + std::to_string(manifestVersion) + "\n";
  out += "master_fingerprint\t" + masterFingerprint + "\n";
  out += "device_id\t" + deviceId + "\n";
  out += "device_key\t" + deviceKey + "\n";
  out += "base_version\t" + baseVersion + "\n";
  out += "head_version\t" + headVersion + "\n";
  out += "primedia_offset\t" + std::to_string(primediaOffset) + "\n";
  out += "primedia_length\t" + std::to_string(primediaLength) + "\n";
  out += "timestamp\t" + std::to_string(timestamp) + "\n";
  for (const auto &[k, v] : extraFields) {
    out += k + "\t" + v + "\n";
  }
  return out;
}

std::expected<SessionManifest, SessionValidationError>
SessionManifest::parse(std::string_view tsvContent) {
  // Reject carriage returns strictly
  if (tsvContent.find('\r') != std::string_view::npos) {
    return std::unexpected(SessionValidationError::CarriageReturnRejected);
  }

  static const std::regex keyRegex("^[a-z_][a-z0-9_]*$");

  SessionManifest manifest;
  std::unordered_set<std::string> seenKeys;
  bool hasManifestVersion   = false;
  bool hasMasterFingerprint = false;
  bool hasDeviceId          = false;
  bool hasDeviceKey         = false;
  bool hasBaseVersion       = false;
  bool hasHeadVersion       = false;
  bool hasPrimediaOffset    = false;
  bool hasPrimediaLength    = false;
  bool hasTimestamp         = false;

  std::size_t start = 0;
  while (start < tsvContent.size()) {
    const auto end              = tsvContent.find('\n', start);
    const std::string_view line = (end == std::string_view::npos)
                                      ? tsvContent.substr(start)
                                      : tsvContent.substr(start, end - start);
    start = (end == std::string_view::npos) ? tsvContent.size() : end + 1;

    if (line.empty()) {
      continue;
    }

    const auto tab = line.find('\t');
    if (tab == std::string_view::npos || tab == 0) {
      return std::unexpected(SessionValidationError::CorruptManifest);
    }

    const std::string_view key = line.substr(0, tab);
    const std::string_view val = line.substr(tab + 1);

    if (!std::regex_match(key.begin(), key.end(), keyRegex)) {
      return std::unexpected(SessionValidationError::InvalidKeyFormat);
    }

    if (!seenKeys.insert(std::string(key)).second) {
      return std::unexpected(SessionValidationError::DuplicateKey);
    }

    if (key == "manifest_version") {
      std::uint32_t v = 0;
      const auto [ptr, ec] =
          std::from_chars(val.data(), val.data() + val.size(), v);
      if (ec != std::errc{} || ptr != val.data() + val.size() || v == 0) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.manifestVersion = v;
      hasManifestVersion       = true;
    } else if (key == "master_fingerprint") {
      if (val.size() != 64 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.masterFingerprint = std::string(val);
      hasMasterFingerprint       = true;
    } else if (key == "device_id") {
      if (val.empty()) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.deviceId = std::string(val);
      hasDeviceId       = true;
    } else if (key == "device_key") {
      if (val.size() != 64 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.deviceKey = std::string(val);
      hasDeviceKey       = true;
    } else if (key == "base_version") {
      if (val.empty()) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.baseVersion = std::string(val);
      hasBaseVersion       = true;
    } else if (key == "head_version") {
      if (val.empty()) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.headVersion = std::string(val);
      hasHeadVersion       = true;
    } else if (key == "primedia_offset") {
      std::uint64_t v = 0;
      const auto [ptr, ec] =
          std::from_chars(val.data(), val.data() + val.size(), v);
      if (ec != std::errc{} || ptr != val.data() + val.size()) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.primediaOffset = v;
      hasPrimediaOffset       = true;
    } else if (key == "primedia_length") {
      std::uint64_t v = 0;
      const auto [ptr, ec] =
          std::from_chars(val.data(), val.data() + val.size(), v);
      if (ec != std::errc{} || ptr != val.data() + val.size()) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.primediaLength = v;
      hasPrimediaLength       = true;
    } else if (key == "timestamp") {
      std::uint64_t v = 0;
      const auto [ptr, ec] =
          std::from_chars(val.data(), val.data() + val.size(), v);
      if (ec != std::errc{} || ptr != val.data() + val.size()) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.timestamp = v;
      hasTimestamp       = true;
    } else {
      manifest.extraFields.emplace_back(std::string(key), std::string(val));
    }
  }

  if (!hasManifestVersion || !hasMasterFingerprint || !hasDeviceId ||
      !hasDeviceKey || !hasBaseVersion || !hasHeadVersion ||
      !hasPrimediaOffset || !hasPrimediaLength || !hasTimestamp) {
    return std::unexpected(SessionValidationError::MissingRequiredField);
  }

  return manifest;
}

std::string SessionPackage::serializeManifest() const {
  SessionManifest manifest;
  manifest.manifestVersion   = manifestVersion;
  manifest.masterFingerprint = masterFingerprint;
  manifest.deviceId          = deviceId;
  manifest.deviceKey         = deviceKey;
  manifest.baseVersion       = baseVersion;
  manifest.headVersion       = headVersion;
  manifest.primediaOffset    = primediaOffset;
  manifest.primediaLength    = primediaSlice.size();
  manifest.timestamp         = timestamp;
  manifest.extraFields       = extraFields;
  return manifest.serialize();
}

std::expected<void, SessionValidationError> SessionPackage::signWithDeviceKey(
    std::span<const std::uint8_t, 32> devicePrivateKey) {
  EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                devicePrivateKey.data(),
                                                devicePrivateKey.size());
  if (!pkey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  std::array<std::uint8_t, 32> pub{};
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(pkey, pub.data(), &pubLen) <= 0 ||
      pubLen != 32) {
    EVP_PKEY_free(pkey);
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }
  deviceKey      = toHex(pub);
  primediaLength = primediaSlice.size();

  rawManifest = serializeManifest();

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    EVP_PKEY_free(pkey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  std::size_t sigLen = signature.size();
  if (EVP_DigestSign(
          ctx, signature.data(), &sigLen,
          reinterpret_cast<const unsigned char *>(rawManifest.data()),
          rawManifest.size()) <= 0 ||
      sigLen != 64) {
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return {};
}

std::expected<void, SessionValidationError>
SessionPackage::verifySignature() const {
  if (deviceKey.size() != 64 || !isAllHex(deviceKey)) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  auto pubBytes = fromHex(deviceKey);
  if (!pubBytes || pubBytes->size() != 32) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  EVP_PKEY *vkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                               pubBytes->data(), 32);
  if (!vkey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  EVP_MD_CTX *vctx = EVP_MD_CTX_new();
  if (!vctx) {
    EVP_PKEY_free(vkey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_DigestVerifyInit(vctx, nullptr, nullptr, nullptr, vkey) <= 0) {
    EVP_MD_CTX_free(vctx);
    EVP_PKEY_free(vkey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  const std::string manifestContent =
      !rawManifest.empty() ? rawManifest : serializeManifest();

  const int rc = EVP_DigestVerify(
      vctx, signature.data(), signature.size(),
      reinterpret_cast<const unsigned char *>(manifestContent.data()),
      manifestContent.size());

  EVP_MD_CTX_free(vctx);
  EVP_PKEY_free(vkey);

  if (rc != 1) {
    return std::unexpected(SessionValidationError::InvalidSignature);
  }
  return {};
}

std::expected<void, SessionValidationError>
SessionPackage::validateAuthorBinding(
    std::string_view expectedMasterFingerprint) const {
  if (!equalsIgnoreCase(masterFingerprint, expectedMasterFingerprint)) {
    return std::unexpected(SessionValidationError::IdentityMismatch);
  }
  return {};
}

std::expected<void, SessionValidationError> SessionPackage::verifyDeviceCert(
    std::optional<std::span<const std::uint8_t, 32>> masterPubKey) const {
  if (deviceCert.empty()) {
    return std::unexpected(SessionValidationError::MissingDeviceCert);
  }

  auto der = base64Decode(deviceCert);
  if (!der || der->empty()) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  const unsigned char *p = der->data();
  X509 *x = d2i_X509(nullptr, &p, static_cast<long>(der->size()));
  if (!x) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  EVP_PKEY *pkey = X509_get0_pubkey(x);
  if (!pkey) {
    X509_free(x);
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  std::array<std::uint8_t, 32> certPub{};
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(pkey, certPub.data(), &pubLen) <= 0 ||
      pubLen != 32) {
    X509_free(x);
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  auto devKeyBytes = fromHex(deviceKey);
  if (!devKeyBytes || devKeyBytes->size() != 32) {
    X509_free(x);
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  if (std::memcmp(certPub.data(), devKeyBytes->data(), 32) != 0) {
    X509_free(x);
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  if (masterPubKey) {
    EVP_PKEY *caKey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                  masterPubKey->data(), 32);
    if (!caKey) {
      X509_free(x);
      return std::unexpected(SessionValidationError::InvalidDeviceCert);
    }
    const int verifyRes = X509_verify(x, caKey);
    EVP_PKEY_free(caKey);
    if (verifyRes <= 0) {
      X509_free(x);
      return std::unexpected(SessionValidationError::InvalidDeviceCert);
    }
  }

  X509_free(x);
  return {};
}

std::expected<std::vector<std::uint8_t>, SessionValidationError>
exportPackage(const SessionPackage &pkg) {
  if (pkg.manifestVersion == 0) {
    return std::unexpected(SessionValidationError::InvalidFieldFormat);
  }
  if (pkg.masterFingerprint.size() != 64 || !isAllHex(pkg.masterFingerprint)) {
    return std::unexpected(SessionValidationError::InvalidFieldFormat);
  }
  if (pkg.deviceId.empty()) {
    return std::unexpected(SessionValidationError::MissingRequiredField);
  }
  if (pkg.deviceKey.size() != 64 || !isAllHex(pkg.deviceKey)) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }
  if (pkg.baseVersion.empty() || pkg.headVersion.empty()) {
    return std::unexpected(SessionValidationError::MissingRequiredField);
  }
  if (pkg.primediaLength != pkg.primediaSlice.size()) {
    return std::unexpected(SessionValidationError::PrimediaLengthMismatch);
  }
  if (pkg.deviceCert.empty()) {
    return std::unexpected(SessionValidationError::MissingDeviceCert);
  }

  std::vector<std::uint8_t> tar;
  tar.reserve(65536);

  const std::string manifestStr =
      !pkg.rawManifest.empty() ? pkg.rawManifest : pkg.serializeManifest();
  std::span<const std::uint8_t> manifestBytes(
      reinterpret_cast<const std::uint8_t *>(manifestStr.data()),
      manifestStr.size());

  appendTarEntry(tar, "MANIFEST.tsv", manifestBytes, pkg.timestamp);
  appendTarEntry(tar, "primedia.slice", pkg.primediaSlice, pkg.timestamp);
  appendTarEntry(tar, "ops.nodes", pkg.opsNodesBytes(), pkg.timestamp);

  std::span<const std::uint8_t> certBytes(
      reinterpret_cast<const std::uint8_t *>(pkg.deviceCert.data()),
      pkg.deviceCert.size());
  appendTarEntry(tar, "device.crt", certBytes, pkg.timestamp);

  appendTarEntry(tar, "signature.sig", pkg.signature, pkg.timestamp);

  tar.insert(tar.end(), 1024, 0);

  return tar;
}

std::expected<void, SessionValidationError>
exportPackage(const SessionPackage &pkg,
              const std::filesystem::path &filePath) {
  auto bytes = exportPackage(pkg);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  std::ofstream ofs(filePath, std::ios::binary);
  if (!ofs) {
    return std::unexpected(SessionValidationError::IoError);
  }
  ofs.write(reinterpret_cast<const char *>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
  if (!ofs.good()) {
    return std::unexpected(SessionValidationError::IoError);
  }
  return {};
}

std::expected<SessionPackage, SessionValidationError>
importPackage(std::span<const std::uint8_t> archiveBytes,
              std::optional<std::string_view> expectedMasterFingerprint,
              bool verifySignature) {
  if (archiveBytes.size() < 512) {
    return std::unexpected(SessionValidationError::CorruptArchive);
  }

  auto decompressed = decompressIfZstd(archiveBytes);
  if (!decompressed) {
    return std::unexpected(decompressed.error());
  }

  auto entriesRes = unpackTar(*decompressed);
  if (!entriesRes) {
    return std::unexpected(entriesRes.error());
  }
  const auto &entries = *entriesRes;

  if (!entries.contains("MANIFEST.tsv")) {
    return std::unexpected(SessionValidationError::MissingManifest);
  }
  if (!entries.contains("primedia.slice")) {
    return std::unexpected(SessionValidationError::MissingPrimedia);
  }
  if (!entries.contains("ops.nodes")) {
    return std::unexpected(SessionValidationError::MissingOps);
  }
  if (!entries.contains("device.crt")) {
    return std::unexpected(SessionValidationError::MissingDeviceCert);
  }
  if (!entries.contains("signature.sig")) {
    return std::unexpected(SessionValidationError::MissingSignature);
  }

  const auto &manifestBytes = entries.at("MANIFEST.tsv");
  const std::string_view manifestView(
      reinterpret_cast<const char *>(manifestBytes.data()),
      manifestBytes.size());

  auto manifestRes = SessionManifest::parse(manifestView);
  if (!manifestRes) {
    return std::unexpected(manifestRes.error());
  }
  const auto &manifest = *manifestRes;

  SessionPackage pkg;
  pkg.manifestVersion   = manifest.manifestVersion;
  pkg.masterFingerprint = manifest.masterFingerprint;
  pkg.deviceId          = manifest.deviceId;
  pkg.deviceKey         = manifest.deviceKey;
  pkg.baseVersion       = manifest.baseVersion;
  pkg.headVersion       = manifest.headVersion;
  pkg.primediaOffset    = manifest.primediaOffset;
  pkg.primediaLength    = manifest.primediaLength;
  pkg.timestamp         = manifest.timestamp;
  pkg.extraFields       = manifest.extraFields;
  pkg.rawManifest       = std::string(manifestView);

  if (expectedMasterFingerprint) {
    auto bindingRes = pkg.validateAuthorBinding(*expectedMasterFingerprint);
    if (!bindingRes) {
      return std::unexpected(bindingRes.error());
    }
  }

  const auto &primediaBytes = entries.at("primedia.slice");
  if (primediaBytes.size() != manifest.primediaLength) {
    return std::unexpected(SessionValidationError::PrimediaLengthMismatch);
  }
  pkg.primediaSlice = primediaBytes;

  const auto &opsBytes = entries.at("ops.nodes");
  if (opsBytes.size() % sizeof(CompactOpNode) != 0) {
    return std::unexpected(SessionValidationError::CorruptOpsNodes);
  }
  pkg.setOpsNodesBytes(opsBytes);

  const auto &certBytes = entries.at("device.crt");
  pkg.deviceCert = std::string(reinterpret_cast<const char *>(certBytes.data()),
                               certBytes.size());

  const auto &sigBytes = entries.at("signature.sig");
  if (sigBytes.size() != 64) {
    return std::unexpected(SessionValidationError::InvalidSignature);
  }
  std::copy(sigBytes.begin(), sigBytes.end(), pkg.signature.begin());

  if (verifySignature) {
    auto sigRes = pkg.verifySignature();
    if (!sigRes) {
      return std::unexpected(sigRes.error());
    }
  }

  return pkg;
}

std::expected<SessionPackage, SessionValidationError>
importPackage(const std::filesystem::path &filePath,
              std::optional<std::string_view> expectedMasterFingerprint,
              bool verifySignature) {
  std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
  if (!ifs) {
    return std::unexpected(SessionValidationError::IoError);
  }
  const auto size = ifs.tellg();
  if (size < 0) {
    return std::unexpected(SessionValidationError::IoError);
  }
  ifs.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
  ifs.read(reinterpret_cast<char *>(buffer.data()), size);
  if (!ifs.good()) {
    return std::unexpected(SessionValidationError::IoError);
  }
  return importPackage(buffer, expectedMasterFingerprint, verifySignature);
}

std::expected<std::string, SessionValidationError>
createX509DelegationCertificate(std::span<const std::uint8_t, 32> devicePubKey,
                                std::span<const std::uint8_t, 32> masterPrivKey,
                                std::string_view deviceId,
                                std::uint64_t validSeconds) {
  EVP_PKEY *devKey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                 devicePubKey.data(), 32);
  if (!devKey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  EVP_PKEY *caKey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                 masterPrivKey.data(), 32);
  if (!caKey) {
    EVP_PKEY_free(devKey);
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  X509 *x509 = X509_new();
  if (!x509) {
    EVP_PKEY_free(devKey);
    EVP_PKEY_free(caKey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  X509_set_version(x509, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), static_cast<long>(validSeconds));
  X509_set_pubkey(x509, devKey);

  X509_NAME *name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>(deviceId.data()),
      static_cast<int>(deviceId.size()), -1, 0);

  X509_NAME *issuer = X509_NAME_new();
  X509_NAME_add_entry_by_txt(
      issuer, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>("Xanadu Master Identity"), -1, -1,
      0);
  X509_set_issuer_name(x509, issuer);
  X509_NAME_free(issuer);

  if (X509_sign(x509, caKey, nullptr) <= 0) {
    X509_free(x509);
    EVP_PKEY_free(devKey);
    EVP_PKEY_free(caKey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  const int len = i2d_X509(x509, nullptr);
  if (len <= 0) {
    X509_free(x509);
    EVP_PKEY_free(devKey);
    EVP_PKEY_free(caKey);
    return std::unexpected(SessionValidationError::SerializationError);
  }

  std::vector<std::uint8_t> der(len);
  unsigned char *p = der.data();
  i2d_X509(x509, &p);

  X509_free(x509);
  EVP_PKEY_free(devKey);
  EVP_PKEY_free(caKey);

  return base64Encode(der);
}

} // namespace xanadu
