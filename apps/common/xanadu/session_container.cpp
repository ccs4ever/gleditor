/**
 * @file session_container.cpp
 * @brief Implementation of .xuzzpkg container packaging, unpacking, manifest
 *        parsing, and cryptographic verification.
 */
#include "session_container.hpp"
#include "identity/standard_crypto_engine.hpp"

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
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <zstd.h>

#include <gleditor/logging.hpp>

namespace xanadu {

SessionPackage::SessionPackage() = default;

SessionPackage::~SessionPackage() {
  if (!passphrase.empty()) {
    OPENSSL_cleanse(passphrase.data(), passphrase.size());
  }
}

SessionPackage::SessionPackage(SessionPackage &&other) noexcept
    : manifestVersion(other.manifestVersion),
      masterFingerprint(std::move(other.masterFingerprint)),
      deviceId(std::move(other.deviceId)),
      deviceKey(std::move(other.deviceKey)),
      baseVersion(std::move(other.baseVersion)),
      headVersion(std::move(other.headVersion)),
      primediaOffset(other.primediaOffset),
      primediaLength(other.primediaLength), timestamp(other.timestamp),
      encrypted(other.encrypted),
      encryptionCipher(std::move(other.encryptionCipher)),
      encryptionSalt(std::move(other.encryptionSalt)),
      encryptionNonce(std::move(other.encryptionNonce)),
      payloadSha256(std::move(other.payloadSha256)),
      passphrase(std::move(other.passphrase)),
      primediaSha256(std::move(other.primediaSha256)),
      opsSha256(std::move(other.opsSha256)),
      deviceCertSha256(std::move(other.deviceCertSha256)),
      extraFields(std::move(other.extraFields)),
      primediaSlice(std::move(other.primediaSlice)),
      opsNodes(std::move(other.opsNodes)),
      deviceCert(std::move(other.deviceCert)), signature(other.signature),
      rawManifest(std::move(other.rawManifest)) {}

SessionPackage &SessionPackage::operator=(SessionPackage &&other) noexcept {
  if (this != &other) {
    if (!passphrase.empty()) {
      OPENSSL_cleanse(passphrase.data(), passphrase.size());
    }
    manifestVersion   = other.manifestVersion;
    masterFingerprint = std::move(other.masterFingerprint);
    deviceId          = std::move(other.deviceId);
    deviceKey         = std::move(other.deviceKey);
    baseVersion       = std::move(other.baseVersion);
    headVersion       = std::move(other.headVersion);
    primediaOffset    = other.primediaOffset;
    primediaLength    = other.primediaLength;
    timestamp         = other.timestamp;
    encrypted         = other.encrypted;
    encryptionCipher  = std::move(other.encryptionCipher);
    encryptionSalt    = std::move(other.encryptionSalt);
    encryptionNonce   = std::move(other.encryptionNonce);
    payloadSha256     = std::move(other.payloadSha256);
    passphrase        = std::move(other.passphrase);
    primediaSha256    = std::move(other.primediaSha256);
    opsSha256         = std::move(other.opsSha256);
    deviceCertSha256  = std::move(other.deviceCertSha256);
    extraFields       = std::move(other.extraFields);
    primediaSlice     = std::move(other.primediaSlice);
    opsNodes          = std::move(other.opsNodes);
    deviceCert        = std::move(other.deviceCert);
    signature         = other.signature;
    rawManifest       = std::move(other.rawManifest);
  }
  return *this;
}

SessionPackage::SessionPackage(const SessionPackage &) = default;

SessionPackage &SessionPackage::operator=(const SessionPackage &other) {
  if (this != &other) {
    if (!passphrase.empty()) {
      OPENSSL_cleanse(passphrase.data(), passphrase.size());
    }
    manifestVersion   = other.manifestVersion;
    masterFingerprint = other.masterFingerprint;
    deviceId          = other.deviceId;
    deviceKey         = other.deviceKey;
    baseVersion       = other.baseVersion;
    headVersion       = other.headVersion;
    primediaOffset    = other.primediaOffset;
    primediaLength    = other.primediaLength;
    timestamp         = other.timestamp;
    encrypted         = other.encrypted;
    encryptionCipher  = other.encryptionCipher;
    encryptionSalt    = other.encryptionSalt;
    encryptionNonce   = other.encryptionNonce;
    payloadSha256     = other.payloadSha256;
    passphrase        = other.passphrase;
    primediaSha256    = other.primediaSha256;
    opsSha256         = other.opsSha256;
    deviceCertSha256  = other.deviceCertSha256;
    extraFields       = other.extraFields;
    primediaSlice     = other.primediaSlice;
    opsNodes          = other.opsNodes;
    deviceCert        = other.deviceCert;
    signature         = other.signature;
    rawManifest       = other.rawManifest;
  }
  return *this;
}

SessionPackage *SessionPackage::setPassphrase(std::string pass) noexcept {
  if (!passphrase.empty()) {
    OPENSSL_cleanse(passphrase.data(), passphrase.size());
  }
  passphrase = std::move(pass);
  return this;
}

namespace {

using identity::base64Decode;
using identity::base64Encode;
using identity::EvpMdCtxPtr;
using identity::EvpPkeyPtr;
using identity::isCanonicalEd25519Scalar;
using identity::isSmallOrderPoint;
using identity::X509Ptr;
using X509NamePtr = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;

[[nodiscard]] bool isValidDeviceId(std::string_view id) noexcept {
  if (id.empty() || id.size() > 64 || id == "main") {
    return false;
  }
  return std::ranges::all_of(id, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  });
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
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(
        std::tolower(static_cast<unsigned char>(a[i])) ^
        std::tolower(static_cast<unsigned char>(b[i])));
  }
  return diff == 0;
}

[[nodiscard]] std::string
computePackageAad(std::string_view masterFingerprint, std::string_view deviceId,
                  std::string_view baseVersion, std::string_view headVersion,
                  std::uint64_t primediaOffset, std::uint64_t primediaLength,
                  std::string_view encryptionSalt,
                  std::string_view encryptionNonce,
                  std::string_view primediaSha256, std::string_view opsSha256) {
  return std::string("xuzzpkg-aad-v1\n") +
         "master:" + std::string(masterFingerprint) + "\n" +
         "device:" + std::string(deviceId) + "\n" +
         "base:" + std::string(baseVersion) + "\n" +
         "head:" + std::string(headVersion) + "\n" +
         "offset:" + std::to_string(primediaOffset) + "\n" +
         "length:" + std::to_string(primediaLength) + "\n" +
         "salt:" + std::string(encryptionSalt) + "\n" +
         "nonce:" + std::string(encryptionNonce) + "\n" +
         "primedia_hash:" + std::string(primediaSha256) + "\n" +
         "ops_hash:" + std::string(opsSha256) + "\n";
}

[[nodiscard]] std::string computeSha256Hex(std::span<const std::uint8_t> data) {
  std::array<std::uint8_t, 32> hash{};
  SHA256(data.data(), data.size(), hash.data());
  return toHex(hash);
}

[[nodiscard]] std::string computeSha256Hex(std::string_view data) {
  return computeSha256Hex(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(data.data()), data.size()));
}

struct SensitiveCleanseGuard {
  std::span<std::uint8_t> mem;
  ~SensitiveCleanseGuard() {
    if (!mem.empty()) {
      OPENSSL_cleanse(mem.data(), mem.size());
    }
  }
};
using EvpCipherCtxPtr =
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

[[nodiscard]] std::expected<std::vector<std::uint8_t>, SessionValidationError>
encryptChaCha20Poly1305(std::span<const std::uint8_t> plaintext,
                        std::string_view passphrase,
                        std::span<const std::uint8_t, 16> salt,
                        std::span<const std::uint8_t, 12> nonce,
                        std::string_view aad) {
  if (passphrase.empty()) {
    return std::unexpected(SessionValidationError::MissingDecryptionKey);
  }

  std::array<std::uint8_t, 32> key{};
  SensitiveCleanseGuard keyGuard{key};
  if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()),
                        salt.data(), static_cast<int>(salt.size()), 600000,
                        EVP_sha256(), static_cast<int>(key.size()),
                        key.data()) != 1) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!ctx) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr,
                         nullptr) <= 0 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) <=
          0 ||
      EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(),
                         nonce.data()) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  int len = 0;
  if (!aad.empty()) {
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len,
                          reinterpret_cast<const unsigned char *>(aad.data()),
                          static_cast<int>(aad.size())) <= 0) {
      return std::unexpected(SessionValidationError::SerializationError);
    }
  }

  std::vector<std::uint8_t> out(plaintext.size() + 16);
  if (EVP_EncryptUpdate(ctx.get(), out.data(), &len, plaintext.data(),
                        static_cast<int>(plaintext.size())) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }
  int totalLen = len;

  if (EVP_EncryptFinal_ex(ctx.get(), out.data() + totalLen, &len) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }
  totalLen += len;

  std::array<std::uint8_t, 16> tag{};
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16, tag.data()) <=
      0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  out.resize(totalLen);
  out.insert(out.end(), tag.begin(), tag.end());
  return out;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, SessionValidationError>
decryptChaCha20Poly1305(std::span<const std::uint8_t> ciphertextWithTag,
                        std::string_view passphrase,
                        std::span<const std::uint8_t, 16> salt,
                        std::span<const std::uint8_t, 12> nonce,
                        std::string_view aad) {
  if (passphrase.empty()) {
    return std::unexpected(SessionValidationError::MissingDecryptionKey);
  }
  if (ciphertextWithTag.size() < 16) {
    return std::unexpected(SessionValidationError::DecryptionFailed);
  }

  const std::size_t ciphertextLen = ciphertextWithTag.size() - 16;
  const unsigned char *tagPtr     = ciphertextWithTag.data() + ciphertextLen;

  std::array<std::uint8_t, 32> key{};
  SensitiveCleanseGuard keyGuard{key};
  if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()),
                        salt.data(), static_cast<int>(salt.size()), 600000,
                        EVP_sha256(), static_cast<int>(key.size()),
                        key.data()) != 1) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!ctx) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_DecryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr,
                         nullptr) <= 0 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) <=
          0 ||
      EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(),
                         nonce.data()) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                          const_cast<unsigned char *>(tagPtr)) <= 0) {
    return std::unexpected(SessionValidationError::DecryptionFailed);
  }

  int len = 0;
  if (!aad.empty()) {
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len,
                          reinterpret_cast<const unsigned char *>(aad.data()),
                          static_cast<int>(aad.size())) <= 0) {
      return std::unexpected(SessionValidationError::DecryptionFailed);
    }
  }

  std::vector<std::uint8_t> out(ciphertextLen);
  if (EVP_DecryptUpdate(ctx.get(), out.data(), &len, ciphertextWithTag.data(),
                        static_cast<int>(ciphertextLen)) <= 0) {
    OPENSSL_cleanse(out.data(), out.size());
    return std::unexpected(SessionValidationError::DecryptionFailed);
  }
  int totalLen = len;

  if (EVP_DecryptFinal_ex(ctx.get(), out.data() + totalLen, &len) <= 0) {
    OPENSSL_cleanse(out.data(), out.size());
    return std::unexpected(SessionValidationError::DecryptionFailed);
  }
  totalLen += len;

  out.resize(totalLen);
  return out;
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
  char chkBuf[9] = {};
  std::memcpy(chkBuf, header.chksum, sizeof(header.chksum));
  chkBuf[8] = '\0';

  const char *pChk = chkBuf;
  while (*pChk == ' ') ++pChk;
  if (*pChk == '\0') {
    return false;
  }

  char *endptr                  = nullptr;
  errno                         = 0;
  const unsigned long storedSum = std::strtoul(pChk, &endptr, 8);
  if (errno != 0 || endptr == pChk) {
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

    // Path traversal and filename whitelist enforcement:
    // Reject empty, '..', leading slash, backslashes, or any character outside
    // [a-zA-Z0-9_.-]
    if (name.empty() || name.find("..") != std::string::npos ||
        name.starts_with('/') || name.find('\\') != std::string::npos) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    if (!std::ranges::all_of(name, [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                 c == '.' || c == '-';
        })) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    if (entries.contains(name)) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    char sizeBuf[13] = {};
    std::memcpy(sizeBuf, header.size, sizeof(header.size));
    sizeBuf[12] = '\0';

    const char *pSize = sizeBuf;
    while (*pSize == ' ') ++pSize;
    if (*pSize == '-' || *pSize == '+' || *pSize == '\0') {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    char *endptr                        = nullptr;
    errno                               = 0;
    const unsigned long long parsedSize = std::strtoull(pSize, &endptr, 8);
    if (errno != 0 || endptr == pSize || parsedSize > 256ULL * 1024 * 1024) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    const std::size_t size = static_cast<std::size_t>(parsedSize);

    if (size > archive.size() - offset) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    std::vector<std::uint8_t> data(archive.data() + offset,
                                   archive.data() + offset + size);
    entries[name] = std::move(data);

    offset += size;
    const std::size_t pad = (512 - (size % 512)) % 512;
    if (pad > archive.size() - offset) {
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

  static constexpr std::size_t kMaxDecompressedSize =
      256ULL * 1024 * 1024; // 256 MiB limit

  const unsigned long long contentSize =
      ZSTD_getFrameContentSize(data.data(), data.size());
  if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
    return std::unexpected(SessionValidationError::CorruptArchive);
  }

  if (contentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
      contentSize > kMaxDecompressedSize) {
    return std::unexpected(SessionValidationError::CorruptArchive);
  }

  std::vector<std::uint8_t> out;
  if (contentSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    out.resize(static_cast<std::size_t>(contentSize));
    const std::size_t res =
        ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(res)) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    out.resize(res);
  } else {
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx(ZSTD_createDCtx(),
                                                              &ZSTD_freeDCtx);
    if (!dctx) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }

    out.resize(1024 * 1024);
    ZSTD_inBuffer inBuf   = {data.data(), data.size(), 0};
    ZSTD_outBuffer outBuf = {out.data(), out.size(), 0};

    size_t lastRet = 1;
    while (inBuf.pos < inBuf.size) {
      lastRet = ZSTD_decompressStream(dctx.get(), &outBuf, &inBuf);
      if (ZSTD_isError(lastRet)) {
        return std::unexpected(SessionValidationError::CorruptArchive);
      }
      if (outBuf.pos == outBuf.size) {
        if (out.size() >= kMaxDecompressedSize) {
          return std::unexpected(SessionValidationError::CorruptArchive);
        }
        const std::size_t nextSize =
            std::min(out.size() * 2, kMaxDecompressedSize);
        out.resize(nextSize);
        outBuf.dst  = out.data();
        outBuf.size = out.size();
      }
    }
    if (lastRet != 0) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    out.resize(outBuf.pos);
  }
  return out;
}

} // namespace

std::string SessionManifest::serialize() const {
  std::string out;
  out.reserve(512);
  out += "manifest_version\t" + std::to_string(manifestVersion) + "\n";
  out += "master_fingerprint\t" + masterFingerprint + "\n";
  out += "device_id\t" + deviceId + "\n";
  out += "device_key\t" + deviceKey + "\n";
  out += "base_version\t" + baseVersion + "\n";
  out += "head_version\t" + headVersion + "\n";
  out += "primedia_offset\t" + std::to_string(primediaOffset) + "\n";
  out += "primedia_length\t" + std::to_string(primediaLength) + "\n";
  out += "timestamp\t" + std::to_string(timestamp) + "\n";
  if (encrypted) {
    out += "encrypted\ttrue\n";
    if (!encryptionCipher.empty()) {
      out += "encryption_cipher\t" + encryptionCipher + "\n";
    }
    if (!encryptionSalt.empty()) {
      out += "encryption_salt\t" + encryptionSalt + "\n";
    }
    if (!encryptionNonce.empty()) {
      out += "encryption_nonce\t" + encryptionNonce + "\n";
    }
    if (!payloadSha256.empty()) {
      out += "payload_sha256\t" + payloadSha256 + "\n";
    }
  }
  if (!primediaSha256.empty()) {
    out += "primedia_sha256\t" + primediaSha256 + "\n";
  }
  if (!opsSha256.empty()) {
    out += "ops_sha256\t" + opsSha256 + "\n";
  }
  if (!deviceCertSha256.empty()) {
    out += "device_cert_sha256\t" + deviceCertSha256 + "\n";
  }
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
      if (!isValidDeviceId(val)) {
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
    } else if (key == "encrypted") {
      manifest.encrypted = (val == "true" || val == "1");
    } else if (key == "encryption_cipher") {
      if (val != "chacha20-poly1305") {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.encryptionCipher = std::string(val);
    } else if (key == "encryption_salt") {
      if (val.size() != 32 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.encryptionSalt = std::string(val);
    } else if (key == "encryption_nonce") {
      if (val.size() != 24 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.encryptionNonce = std::string(val);
    } else if (key == "payload_sha256") {
      if (val.size() != 64 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.payloadSha256 = std::string(val);
    } else if (key == "primedia_sha256") {
      if (val.size() != 64 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.primediaSha256 = std::string(val);
    } else if (key == "ops_sha256") {
      if (val.size() != 64 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.opsSha256 = std::string(val);
    } else if (key == "device_cert_sha256") {
      if (val.size() != 64 || !isAllHex(val)) {
        return std::unexpected(SessionValidationError::InvalidFieldFormat);
      }
      manifest.deviceCertSha256 = std::string(val);
    } else {
      manifest.extraFields.emplace_back(std::string(key), std::string(val));
    }
  }

  if (!hasManifestVersion || !hasMasterFingerprint || !hasDeviceId ||
      !hasDeviceKey || !hasBaseVersion || !hasHeadVersion ||
      !hasPrimediaOffset || !hasPrimediaLength || !hasTimestamp) {
    return std::unexpected(SessionValidationError::MissingRequiredField);
  }

  if (manifest.encrypted) {
    if (manifest.encryptionCipher.empty() || manifest.encryptionSalt.empty() ||
        manifest.encryptionNonce.empty() || manifest.payloadSha256.empty()) {
      return std::unexpected(SessionValidationError::MissingRequiredField);
    }
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
  manifest.encrypted         = encrypted;
  manifest.encryptionCipher  = encryptionCipher;
  manifest.encryptionSalt    = encryptionSalt;
  manifest.encryptionNonce   = encryptionNonce;
  manifest.payloadSha256     = payloadSha256;
  manifest.primediaSha256    = primediaSha256;
  manifest.opsSha256         = opsSha256;
  manifest.deviceCertSha256  = deviceCertSha256;
  manifest.extraFields       = extraFields;
  return manifest.serialize();
}

std::expected<void, SessionValidationError> SessionPackage::signWithDeviceKey(
    std::span<const std::uint8_t, 32> devicePrivateKey) {
  if (!isValidDeviceId(deviceId)) {
    return std::unexpected(SessionValidationError::InvalidFieldFormat);
  }

  EvpPkeyPtr pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               devicePrivateKey.data(),
                                               devicePrivateKey.size()),
                  &EVP_PKEY_free);
  if (!pkey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  std::array<std::uint8_t, 32> pub{};
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(pkey.get(), pub.data(), &pubLen) <= 0 ||
      pubLen != 32) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }
  if (isSmallOrderPoint(pub)) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }
  deviceKey      = toHex(pub);
  primediaLength = primediaSlice.size();

  // Compute content verification hashes
  primediaSha256 = computeSha256Hex(primediaSlice);
  opsSha256      = computeSha256Hex(opsNodesBytes());
  if (!deviceCert.empty()) {
    deviceCertSha256 = computeSha256Hex(deviceCert);
  }

  if (encrypted) {
    if (passphrase.empty()) {
      return std::unexpected(SessionValidationError::MissingDecryptionKey);
    }
    encryptionCipher = "chacha20-poly1305";
    if (encryptionSalt.empty()) {
      std::array<std::uint8_t, 16> saltBytes{};
      if (RAND_bytes(saltBytes.data(), 16) <= 0) {
        return std::unexpected(SessionValidationError::SerializationError);
      }
      encryptionSalt = toHex(saltBytes);
    }
    if (encryptionNonce.empty()) {
      std::array<std::uint8_t, 12> nonceBytes{};
      if (RAND_bytes(nonceBytes.data(), 12) <= 0) {
        return std::unexpected(SessionValidationError::SerializationError);
      }
      encryptionNonce = toHex(nonceBytes);
    }

    auto saltVec  = fromHex(encryptionSalt);
    auto nonceVec = fromHex(encryptionNonce);
    if (!saltVec || saltVec->size() != 16 || !nonceVec ||
        nonceVec->size() != 12) {
      return std::unexpected(SessionValidationError::InvalidFieldFormat);
    }

    // Pack inner payload
    std::vector<std::uint8_t> innerTar;
    innerTar.reserve(primediaSlice.size() + opsNodesBytes().size() + 2048);
    appendTarEntry(innerTar, "primedia.slice", primediaSlice, timestamp);
    appendTarEntry(innerTar, "ops.nodes", opsNodesBytes(), timestamp);
    innerTar.insert(innerTar.end(), 1024, 0);

    const std::string aad =
        computePackageAad(masterFingerprint, deviceId, baseVersion, headVersion,
                          primediaOffset, primediaLength, encryptionSalt,
                          encryptionNonce, primediaSha256, opsSha256);

    std::span<const std::uint8_t, 16> saltSpan(saltVec->data(), 16);
    std::span<const std::uint8_t, 12> nonceSpan(nonceVec->data(), 12);
    auto encRes =
        encryptChaCha20Poly1305(innerTar, passphrase, saltSpan, nonceSpan, aad);
    if (!encRes) {
      return std::unexpected(encRes.error());
    }
    payloadSha256 = computeSha256Hex(*encRes);
  }

  rawManifest = serializeManifest();

  EvpMdCtxPtr ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!ctx) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) <=
      0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  std::size_t sigLen = signature.size();
  if (EVP_DigestSign(
          ctx.get(), signature.data(), &sigLen,
          reinterpret_cast<const unsigned char *>(rawManifest.data()),
          rawManifest.size()) <= 0 ||
      sigLen != 64) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

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

  std::span<const std::uint8_t, 32> pubSpan(pubBytes->data(), 32);
  if (isSmallOrderPoint(pubSpan)) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  std::span<const std::uint8_t, 32> sSpan(signature.data() + 32, 32);
  if (!isCanonicalEd25519Scalar(sSpan)) {
    return std::unexpected(SessionValidationError::InvalidSignature);
  }

  EvpPkeyPtr vkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              pubSpan.data(), 32),
                  &EVP_PKEY_free);
  if (!vkey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  EvpMdCtxPtr vctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!vctx) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (EVP_DigestVerifyInit(vctx.get(), nullptr, nullptr, nullptr, vkey.get()) <=
      0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  const std::string manifestContent =
      !rawManifest.empty() ? rawManifest : serializeManifest();

  const int rc = EVP_DigestVerify(
      vctx.get(), signature.data(), signature.size(),
      reinterpret_cast<const unsigned char *>(manifestContent.data()),
      manifestContent.size());

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
  X509Ptr x(d2i_X509(nullptr, &p, static_cast<long>(der->size())), &X509_free);
  if (!x) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  EVP_PKEY *pkey = X509_get0_pubkey(x.get());
  if (!pkey) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  std::array<std::uint8_t, 32> certPub{};
  std::size_t pubLen = 32;
  if (EVP_PKEY_get_raw_public_key(pkey, certPub.data(), &pubLen) <= 0 ||
      pubLen != 32) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }
  if (isSmallOrderPoint(certPub)) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  auto devKeyBytes = fromHex(deviceKey);
  if (!devKeyBytes || devKeyBytes->size() != 32) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  if (CRYPTO_memcmp(certPub.data(), devKeyBytes->data(), 32) != 0) {
    return std::unexpected(SessionValidationError::InvalidDeviceCert);
  }

  if (masterPubKey) {
    if (isSmallOrderPoint(*masterPubKey)) {
      return std::unexpected(SessionValidationError::InvalidDeviceCert);
    }
    EvpPkeyPtr caKey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                 masterPubKey->data(), 32),
                     &EVP_PKEY_free);
    if (!caKey) {
      return std::unexpected(SessionValidationError::InvalidDeviceCert);
    }
    const int verifyRes = X509_verify(x.get(), caKey.get());
    if (verifyRes <= 0) {
      return std::unexpected(SessionValidationError::InvalidDeviceCert);
    }
  }

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
  if (!isValidDeviceId(pkg.deviceId)) {
    return std::unexpected(SessionValidationError::InvalidFieldFormat);
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

  std::string manifestStr =
      !pkg.rawManifest.empty() ? pkg.rawManifest : pkg.serializeManifest();

  if (pkg.encrypted) {
    if (pkg.passphrase.empty() && pkg.payloadSha256.empty()) {
      return std::unexpected(SessionValidationError::MissingDecryptionKey);
    }

    auto saltVec  = fromHex(pkg.encryptionSalt);
    auto nonceVec = fromHex(pkg.encryptionNonce);
    if (!saltVec || saltVec->size() != 16 || !nonceVec ||
        nonceVec->size() != 12) {
      return std::unexpected(SessionValidationError::InvalidFieldFormat);
    }

    std::vector<std::uint8_t> innerTar;
    innerTar.reserve(pkg.primediaSlice.size() + pkg.opsNodesBytes().size() +
                     2048);
    appendTarEntry(innerTar, "primedia.slice", pkg.primediaSlice,
                   pkg.timestamp);
    appendTarEntry(innerTar, "ops.nodes", pkg.opsNodesBytes(), pkg.timestamp);
    innerTar.insert(innerTar.end(), 1024, 0);

    const std::string aad = computePackageAad(
        pkg.masterFingerprint, pkg.deviceId, pkg.baseVersion, pkg.headVersion,
        pkg.primediaOffset, pkg.primediaLength, pkg.encryptionSalt,
        pkg.encryptionNonce, pkg.primediaSha256, pkg.opsSha256);

    std::span<const std::uint8_t, 16> saltSpan(saltVec->data(), 16);
    std::span<const std::uint8_t, 12> nonceSpan(nonceVec->data(), 12);
    auto encRes = encryptChaCha20Poly1305(innerTar, pkg.passphrase, saltSpan,
                                          nonceSpan, aad);
    if (!encRes) {
      return std::unexpected(encRes.error());
    }

    if (!pkg.payloadSha256.empty() &&
        computeSha256Hex(*encRes) != pkg.payloadSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }

    std::span<const std::uint8_t> manifestBytes(
        reinterpret_cast<const std::uint8_t *>(manifestStr.data()),
        manifestStr.size());

    appendTarEntry(tar, "MANIFEST.tsv", manifestBytes, pkg.timestamp);
    appendTarEntry(tar, "payload.enc", *encRes, pkg.timestamp);
  } else {
    std::span<const std::uint8_t> manifestBytes(
        reinterpret_cast<const std::uint8_t *>(manifestStr.data()),
        manifestStr.size());

    appendTarEntry(tar, "MANIFEST.tsv", manifestBytes, pkg.timestamp);
    appendTarEntry(tar, "primedia.slice", pkg.primediaSlice, pkg.timestamp);
    appendTarEntry(tar, "ops.nodes", pkg.opsNodesBytes(), pkg.timestamp);
  }

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
              bool verifySignature,
              std::optional<std::span<const std::uint8_t, 32>> masterPubKey,
              std::string_view passphrase) {
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
  pkg.encrypted         = manifest.encrypted;
  pkg.encryptionCipher  = manifest.encryptionCipher;
  pkg.encryptionSalt    = manifest.encryptionSalt;
  pkg.encryptionNonce   = manifest.encryptionNonce;
  pkg.payloadSha256     = manifest.payloadSha256;
  pkg.primediaSha256    = manifest.primediaSha256;
  pkg.opsSha256         = manifest.opsSha256;
  pkg.deviceCertSha256  = manifest.deviceCertSha256;
  pkg.extraFields       = manifest.extraFields;
  pkg.rawManifest       = std::string(manifestView);

  if (expectedMasterFingerprint) {
    auto bindingRes = pkg.validateAuthorBinding(*expectedMasterFingerprint);
    if (!bindingRes) {
      return std::unexpected(bindingRes.error());
    }
  }

  const auto &certBytes = entries.at("device.crt");
  pkg.deviceCert = std::string(reinterpret_cast<const char *>(certBytes.data()),
                               certBytes.size());

  if (!manifest.deviceCertSha256.empty()) {
    if (computeSha256Hex(pkg.deviceCert) != manifest.deviceCertSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }
  }

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

  // Verify device certificate (optionally validated with master public key)
  auto certRes = pkg.verifyDeviceCert(masterPubKey);
  if (!certRes) {
    return std::unexpected(certRes.error());
  }

  if (manifest.encrypted) {
    if (!entries.contains("payload.enc")) {
      return std::unexpected(SessionValidationError::CorruptArchive);
    }
    const auto &encBytes = entries.at("payload.enc");
    if (computeSha256Hex(encBytes) != manifest.payloadSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }

    if (passphrase.empty()) {
      return std::unexpected(SessionValidationError::MissingDecryptionKey);
    }

    auto saltVec  = fromHex(manifest.encryptionSalt);
    auto nonceVec = fromHex(manifest.encryptionNonce);
    if (!saltVec || saltVec->size() != 16 || !nonceVec ||
        nonceVec->size() != 12) {
      return std::unexpected(SessionValidationError::InvalidFieldFormat);
    }

    const std::string aad = computePackageAad(
        manifest.masterFingerprint, manifest.deviceId, manifest.baseVersion,
        manifest.headVersion, manifest.primediaOffset, manifest.primediaLength,
        manifest.encryptionSalt, manifest.encryptionNonce,
        manifest.primediaSha256, manifest.opsSha256);

    std::span<const std::uint8_t, 16> saltSpan(saltVec->data(), 16);
    std::span<const std::uint8_t, 12> nonceSpan(nonceVec->data(), 12);
    auto decRes =
        decryptChaCha20Poly1305(encBytes, passphrase, saltSpan, nonceSpan, aad);
    if (!decRes) {
      return std::unexpected(decRes.error());
    }

    auto innerEntriesRes = unpackTar(*decRes);
    if (!innerEntriesRes) {
      return std::unexpected(innerEntriesRes.error());
    }
    const auto &innerEntries = *innerEntriesRes;

    if (!innerEntries.contains("primedia.slice")) {
      return std::unexpected(SessionValidationError::MissingPrimedia);
    }
    if (!innerEntries.contains("ops.nodes")) {
      return std::unexpected(SessionValidationError::MissingOps);
    }

    const auto &primediaBytes = innerEntries.at("primedia.slice");
    if (primediaBytes.size() != manifest.primediaLength) {
      return std::unexpected(SessionValidationError::PrimediaLengthMismatch);
    }
    if (!manifest.primediaSha256.empty() &&
        computeSha256Hex(primediaBytes) != manifest.primediaSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }
    pkg.primediaSlice = primediaBytes;

    const auto &opsBytes = innerEntries.at("ops.nodes");
    if (opsBytes.size() % sizeof(CompactOpNode) != 0) {
      return std::unexpected(SessionValidationError::CorruptOpsNodes);
    }
    if (!manifest.opsSha256.empty() &&
        computeSha256Hex(opsBytes) != manifest.opsSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }
    pkg.setOpsNodesBytes(opsBytes);
  } else {
    if (!entries.contains("primedia.slice")) {
      return std::unexpected(SessionValidationError::MissingPrimedia);
    }
    if (!entries.contains("ops.nodes")) {
      return std::unexpected(SessionValidationError::MissingOps);
    }

    const auto &primediaBytes = entries.at("primedia.slice");
    if (primediaBytes.size() != manifest.primediaLength) {
      return std::unexpected(SessionValidationError::PrimediaLengthMismatch);
    }
    if (!manifest.primediaSha256.empty() &&
        computeSha256Hex(primediaBytes) != manifest.primediaSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }
    pkg.primediaSlice = primediaBytes;

    const auto &opsBytes = entries.at("ops.nodes");
    if (opsBytes.size() % sizeof(CompactOpNode) != 0) {
      return std::unexpected(SessionValidationError::CorruptOpsNodes);
    }
    if (!manifest.opsSha256.empty() &&
        computeSha256Hex(opsBytes) != manifest.opsSha256) {
      return std::unexpected(SessionValidationError::PayloadHashMismatch);
    }
    pkg.setOpsNodesBytes(opsBytes);
  }

  return pkg;
}

std::expected<SessionPackage, SessionValidationError>
importPackage(const std::filesystem::path &filePath,
              std::optional<std::string_view> expectedMasterFingerprint,
              bool verifySignature,
              std::optional<std::span<const std::uint8_t, 32>> masterPubKey,
              std::string_view passphrase) {
  std::error_code ec;
  const auto fileSize = std::filesystem::file_size(filePath, ec);
  if (ec) {
    return std::unexpected(SessionValidationError::IoError);
  }
  static constexpr std::uintmax_t kMaxPackageFileSize = 256ULL * 1024 * 1024;
  if (fileSize > kMaxPackageFileSize) {
    return std::unexpected(SessionValidationError::CorruptArchive);
  }
  std::ifstream ifs(filePath, std::ios::binary);
  if (!ifs) {
    return std::unexpected(SessionValidationError::IoError);
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(fileSize));
  ifs.read(reinterpret_cast<char *>(buffer.data()),
           static_cast<std::streamsize>(fileSize));
  if (!ifs.good()) {
    return std::unexpected(SessionValidationError::IoError);
  }
  return importPackage(buffer, expectedMasterFingerprint, verifySignature,
                       masterPubKey, passphrase);
}

std::expected<std::string, SessionValidationError>
createX509DelegationCertificate(std::span<const std::uint8_t, 32> devicePubKey,
                                std::span<const std::uint8_t, 32> masterPrivKey,
                                std::string_view deviceId,
                                std::uint64_t validSeconds) {
  if (!isValidDeviceId(deviceId)) {
    return std::unexpected(SessionValidationError::InvalidFieldFormat);
  }

  if (isSmallOrderPoint(devicePubKey)) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  EvpPkeyPtr devKey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                devicePubKey.data(), 32),
                    &EVP_PKEY_free);
  if (!devKey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  EvpPkeyPtr caKey(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                masterPrivKey.data(), 32),
                   &EVP_PKEY_free);
  if (!caKey) {
    return std::unexpected(SessionValidationError::InvalidDeviceKey);
  }

  X509Ptr x509(X509_new(), &X509_free);
  if (!x509) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  if (X509_set_version(x509.get(), 2) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }
  ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1);
  X509_gmtime_adj(X509_get_notBefore(x509.get()), 0);
  const long adjSec =
      static_cast<long>(std::min<std::uint64_t>(validSeconds, 315360000ULL));
  X509_gmtime_adj(X509_get_notAfter(x509.get()), adjSec);
  if (X509_set_pubkey(x509.get(), devKey.get()) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  X509_NAME *name = X509_get_subject_name(x509.get());
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>(deviceId.data()),
      static_cast<int>(deviceId.size()), -1, 0);

  X509NamePtr issuer(X509_NAME_new(), &X509_NAME_free);
  if (!issuer) {
    return std::unexpected(SessionValidationError::SerializationError);
  }
  X509_NAME_add_entry_by_txt(
      issuer.get(), "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char *>("Xanadu Master Identity"), -1, -1,
      0);
  if (X509_set_issuer_name(x509.get(), issuer.get()) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  X509V3_CTX v3ctx;
  X509V3_set_ctx_nodb(&v3ctx);
  X509V3_set_ctx(&v3ctx, x509.get(), x509.get(), nullptr, nullptr, 0);

  X509_EXTENSION *extBc = X509V3_EXT_conf_nid(
      nullptr, &v3ctx, NID_basic_constraints, "critical,CA:FALSE");
  if (extBc) {
    X509_add_ext(x509.get(), extBc, -1);
    X509_EXTENSION_free(extBc);
  }

  X509_EXTENSION *extKu =
      X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_key_usage,
                          "critical,digitalSignature,nonRepudiation");
  if (extKu) {
    X509_add_ext(x509.get(), extKu, -1);
    X509_EXTENSION_free(extKu);
  }

  if (X509_sign(x509.get(), caKey.get(), nullptr) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  const int len = i2d_X509(x509.get(), nullptr);
  if (len <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  std::vector<std::uint8_t> der(len);
  unsigned char *p = der.data();
  if (i2d_X509(x509.get(), &p) <= 0) {
    return std::unexpected(SessionValidationError::SerializationError);
  }

  return base64Encode(der);
}

} // namespace xanadu
