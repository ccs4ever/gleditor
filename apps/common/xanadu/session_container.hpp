/**
 * @file session_container.hpp
 * @brief Session package container (.xuzzpkg) single-file exchange format
 *        for cross-device offline synchronization and author reconciliation.
 */
#ifndef XANADU_SESSION_CONTAINER_HPP
#define XANADU_SESSION_CONTAINER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compact_op.hpp"
#include "ops.hpp"

namespace xanadu {

/// Validation and processing error codes for session container exchange.
enum class SessionValidationError : std::uint8_t {
  Ok = 0,
  MissingManifest,
  MissingPrimedia,
  MissingOps,
  MissingDeviceCert,
  MissingSignature,
  CorruptArchive,
  CorruptManifest,
  DuplicateKey,
  InvalidKeyFormat,
  CarriageReturnRejected,
  MissingRequiredField,
  InvalidFieldFormat,
  IdentityMismatch,
  InvalidDeviceKey,
  InvalidSignature,
  InvalidDeviceCert,
  CorruptOpsNodes,
  PrimediaLengthMismatch,
  IoError,
  SerializationError,
  MissingDecryptionKey,
  DecryptionFailed,
  PayloadHashMismatch
};

using ValidationError = SessionValidationError;

[[nodiscard]] constexpr std::string_view
validationErrorToString(SessionValidationError err) noexcept {
  switch (err) {
  case SessionValidationError::Ok:
    return "Ok";
  case SessionValidationError::MissingManifest:
    return "Archive missing MANIFEST.tsv";
  case SessionValidationError::MissingPrimedia:
    return "Archive missing primedia.slice";
  case SessionValidationError::MissingOps:
    return "Archive missing ops.nodes";
  case SessionValidationError::MissingDeviceCert:
    return "Archive missing device.crt";
  case SessionValidationError::MissingSignature:
    return "Archive missing signature.sig";
  case SessionValidationError::CorruptArchive:
    return "Archive corrupted or truncated";
  case SessionValidationError::CorruptManifest:
    return "Manifest corrupted or malformed";
  case SessionValidationError::DuplicateKey:
    return "Manifest contains duplicate key";
  case SessionValidationError::InvalidKeyFormat:
    return "Manifest key does not match strict regex ^[a-z_][a-z0-9_]*$";
  case SessionValidationError::CarriageReturnRejected:
    return "Manifest contains forbidden carriage return (\\r)";
  case SessionValidationError::MissingRequiredField:
    return "Manifest missing required field";
  case SessionValidationError::InvalidFieldFormat:
    return "Manifest field value format invalid";
  case SessionValidationError::IdentityMismatch:
    return "Author binding mismatch: master fingerprint does not match target "
           "identity";
  case SessionValidationError::InvalidDeviceKey:
    return "Invalid device public key";
  case SessionValidationError::InvalidSignature:
    return "Ed25519 signature verification failed";
  case SessionValidationError::InvalidDeviceCert:
    return "Invalid X.509 device delegation certificate";
  case SessionValidationError::CorruptOpsNodes:
    return "ops.nodes size is not a multiple of sizeof(CompactOpNode)";
  case SessionValidationError::PrimediaLengthMismatch:
    return "primedia.slice length does not match manifest primedia_length";
  case SessionValidationError::IoError:
    return "I/O error reading or writing package";
  case SessionValidationError::SerializationError:
    return "Serialization or parsing error";
  case SessionValidationError::MissingDecryptionKey:
    return "Encrypted package requires decryption passphrase";
  case SessionValidationError::DecryptionFailed:
    return "Decryption failed: incorrect passphrase or corrupted AEAD "
           "ciphertext";
  case SessionValidationError::PayloadHashMismatch:
    return "Payload cryptographic hash does not match manifest commitment";
  }
  return "Unknown error";
}

/// Parsed metadata representing canonical MANIFEST.tsv
struct SessionManifest {
  std::uint32_t manifestVersion{1};
  std::string masterFingerprint; // 64-hex SHA-256
  std::string deviceId;
  std::string deviceKey; // 64-hex Ed25519 public key
  std::string baseVersion;
  std::string headVersion;
  std::uint64_t primediaOffset{0};
  std::uint64_t primediaLength{0};
  std::uint64_t timestamp{0};

  // Encryption metadata
  bool encrypted{false};
  std::string encryptionCipher; // "chacha20-poly1305"
  std::string encryptionSalt;   // 32-hex (16 bytes)
  std::string encryptionNonce;  // 24-hex (12 bytes)
  std::string payloadSha256;    // 64-hex SHA-256 of payload.enc

  // Content verification hashes
  std::string primediaSha256;   // 64-hex
  std::string opsSha256;        // 64-hex
  std::string deviceCertSha256; // 64-hex

  std::vector<std::pair<std::string, std::string>> extraFields;

  [[nodiscard]] std::string serialize() const;
  [[nodiscard]] static std::expected<SessionManifest, SessionValidationError>
  parse(std::string_view tsvContent);
};

/// Structure representing a complete .xuzzpkg session exchange container.
struct SessionPackage {
  SessionPackage();
  ~SessionPackage();
  SessionPackage(SessionPackage &&) noexcept;
  SessionPackage &operator=(SessionPackage &&) noexcept;
  SessionPackage(const SessionPackage &);
  SessionPackage &operator=(const SessionPackage &);

  // Manifest fields
  std::uint32_t manifestVersion{1};
  std::string masterFingerprint; // 64-hex SHA-256
  std::string deviceId;
  std::string deviceKey; // 64-hex Ed25519 public key
  std::string baseVersion;
  std::string headVersion;
  std::uint64_t primediaOffset{0};
  std::uint64_t primediaLength{0};
  std::uint64_t timestamp{0};

  // Encryption metadata
  bool encrypted{false};
  std::string encryptionCipher;
  std::string encryptionSalt;
  std::string encryptionNonce;
  std::string payloadSha256;
  std::string passphrase;

  // Content verification hashes
  std::string primediaSha256;
  std::string opsSha256;
  std::string deviceCertSha256;

  std::vector<std::pair<std::string, std::string>> extraFields;

  // Content files
  std::vector<std::uint8_t> primediaSlice;
  std::vector<CompactOpNode> opsNodes;
  std::string deviceCert; // Base64 DER X.509 delegation cert
  std::array<std::uint8_t, 64> signature{};

  // Preserved raw manifest bytes when imported
  std::string rawManifest;

  // Fluent setters (chaining via pointer)
  SessionPackage *setManifestVersion(std::uint32_t v) noexcept {
    manifestVersion = v;
    return this;
  }

  SessionPackage *setMasterFingerprint(std::string fp) noexcept {
    masterFingerprint = std::move(fp);
    return this;
  }

  SessionPackage *setDeviceId(std::string id) noexcept {
    deviceId = std::move(id);
    return this;
  }

  SessionPackage *setDeviceKey(std::string key) noexcept {
    deviceKey = std::move(key);
    return this;
  }

  SessionPackage *setBaseVersion(std::string v) noexcept {
    baseVersion = std::move(v);
    return this;
  }

  SessionPackage *setHeadVersion(std::string v) noexcept {
    headVersion = std::move(v);
    return this;
  }

  SessionPackage *setPrimediaOffset(std::uint64_t offset) noexcept {
    primediaOffset = offset;
    return this;
  }

  SessionPackage *setPrimediaLength(std::uint64_t length) noexcept {
    primediaLength = length;
    return this;
  }

  SessionPackage *setTimestamp(std::uint64_t ts) noexcept {
    timestamp = ts;
    return this;
  }

  SessionPackage *setEncrypted(bool enc) noexcept {
    encrypted = enc;
    return this;
  }

  [[nodiscard]] bool isEncrypted() const noexcept { return encrypted; }

  SessionPackage *setEncryptionCipher(std::string cipher) noexcept {
    encryptionCipher = std::move(cipher);
    return this;
  }

  SessionPackage *setEncryptionSalt(std::string salt) noexcept {
    encryptionSalt = std::move(salt);
    return this;
  }

  SessionPackage *setEncryptionNonce(std::string nonce) noexcept {
    encryptionNonce = std::move(nonce);
    return this;
  }

  SessionPackage *setPayloadSha256(std::string hash) noexcept {
    payloadSha256 = std::move(hash);
    return this;
  }

  SessionPackage *setPassphrase(std::string pass) noexcept;

  [[nodiscard]] const std::string &getPassphrase() const noexcept {
    return passphrase;
  }

  SessionPackage *setPrimediaSha256(std::string hash) noexcept {
    primediaSha256 = std::move(hash);
    return this;
  }

  SessionPackage *setOpsSha256(std::string hash) noexcept {
    opsSha256 = std::move(hash);
    return this;
  }

  SessionPackage *setDeviceCertSha256(std::string hash) noexcept {
    deviceCertSha256 = std::move(hash);
    return this;
  }

  SessionPackage *setExtraField(std::string key, std::string value) {
    extraFields.emplace_back(std::move(key), std::move(value));
    return this;
  }

  SessionPackage *setPrimediaSlice(std::vector<std::uint8_t> slice) noexcept {
    primediaSlice  = std::move(slice);
    primediaLength = primediaSlice.size();
    return this;
  }

  SessionPackage *setPrimediaSlice(std::span<const std::uint8_t> slice) {
    primediaSlice.assign(slice.begin(), slice.end());
    primediaLength = primediaSlice.size();
    return this;
  }

  SessionPackage *setPrimediaSlice(std::string_view slice) {
    primediaSlice.assign(reinterpret_cast<const std::uint8_t *>(slice.data()),
                         reinterpret_cast<const std::uint8_t *>(slice.data()) +
                             slice.size());
    primediaLength = primediaSlice.size();
    return this;
  }

  SessionPackage *setOpsNodes(std::vector<CompactOpNode> ops) noexcept {
    opsNodes = std::move(ops);
    return this;
  }

  SessionPackage *setOpsNodes(std::span<const CompactOpNode> ops) {
    opsNodes.assign(ops.begin(), ops.end());
    return this;
  }

  SessionPackage *setOpsNodesBytes(std::span<const std::uint8_t> bytes) {
    if (bytes.size() % sizeof(CompactOpNode) != 0) {
      opsNodes.clear();
      return this;
    }
    const std::size_t count = bytes.size() / sizeof(CompactOpNode);
    opsNodes.resize(count);
    if (count > 0) {
      std::memcpy(opsNodes.data(), bytes.data(), bytes.size());
    }
    return this;
  }

  SessionPackage *setDeviceCert(std::string cert) noexcept {
    deviceCert = std::move(cert);
    return this;
  }

  SessionPackage *
  setSignature(const std::array<std::uint8_t, 64> &sig) noexcept {
    signature = sig;
    return this;
  }

  SessionPackage *setSignature(std::span<const std::uint8_t, 64> sig) noexcept {
    std::copy(sig.begin(), sig.end(), signature.begin());
    return this;
  }

  // Accessors & helpers
  [[nodiscard]] std::string serializeManifest() const;
  [[nodiscard]] std::span<const std::uint8_t> opsNodesBytes() const noexcept {
    return {reinterpret_cast<const std::uint8_t *>(opsNodes.data()),
            opsNodes.size() * sizeof(CompactOpNode)};
  }
  [[nodiscard]] std::string_view primediaSliceString() const noexcept {
    return {reinterpret_cast<const char *>(primediaSlice.data()),
            primediaSlice.size()};
  }

  // Cryptographic operations
  [[nodiscard]] std::expected<void, SessionValidationError>
  signWithDeviceKey(std::span<const std::uint8_t, 32> devicePrivateKey);

  [[nodiscard]] std::expected<void, SessionValidationError>
  verifySignature() const;

  [[nodiscard]] std::expected<void, SessionValidationError>
  validateAuthorBinding(std::string_view expectedMasterFingerprint) const;

  [[nodiscard]] std::expected<void, SessionValidationError> verifyDeviceCert(
      std::optional<std::span<const std::uint8_t, 32>> masterPubKey =
          std::nullopt) const;

  bool operator==(const SessionPackage &) const = default;
};

// Container packing and unpacking
[[nodiscard]] std::expected<std::vector<std::uint8_t>, SessionValidationError>
exportPackage(const SessionPackage &pkg);

[[nodiscard]] std::expected<void, SessionValidationError>
exportPackage(const SessionPackage &pkg, const std::filesystem::path &filePath);

[[nodiscard]] std::expected<SessionPackage, SessionValidationError>
importPackage(
    std::span<const std::uint8_t> archiveBytes,
    std::optional<std::string_view> expectedMasterFingerprint = std::nullopt,
    bool verifySignature                                      = true,
    std::optional<std::span<const std::uint8_t, 32>> masterPubKey =
        std::nullopt,
    std::string_view passphrase = {});

[[nodiscard]] std::expected<SessionPackage, SessionValidationError>
importPackage(
    const std::filesystem::path &filePath,
    std::optional<std::string_view> expectedMasterFingerprint = std::nullopt,
    bool verifySignature                                      = true,
    std::optional<std::span<const std::uint8_t, 32>> masterPubKey =
        std::nullopt,
    std::string_view passphrase = {});

// Utility helpers for keys and X.509 delegation certs
[[nodiscard]] std::expected<std::string, SessionValidationError>
createX509DelegationCertificate(std::span<const std::uint8_t, 32> devicePubKey,
                                std::span<const std::uint8_t, 32> masterPrivKey,
                                std::string_view deviceId,
                                std::uint64_t validSeconds = 365ULL * 86400ULL);

} // namespace xanadu

#endif // XANADU_SESSION_CONTAINER_HPP
