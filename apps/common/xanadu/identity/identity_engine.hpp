/**
 * @file identity_engine.hpp
 * @brief Modular cryptographic abstractions for in-process Ed25519, standard
 * X.509 v3 delegation certificates, and revocation querying.
 */
#ifndef XANADU_IDENTITY_ENGINE_HPP
#define XANADU_IDENTITY_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "identity_layout.hpp"

namespace xanadu::identity {

/// Domain separation prefixes to prevent cross-protocol signature replay
inline constexpr std::string_view kDomainManifest =
    "xanadu-authorship-manifest-v2\n";
inline constexpr std::string_view kDomainDeviceCsr = "xanadu-device-csr-v2\n";
inline constexpr std::string_view kDomainDeviceRevocation =
    "xanadu-device-revocation-v2\n";
inline constexpr std::string_view kDomainPeerAuth = "xanadu-peer-auth-v2\n";
inline constexpr std::string_view kDomainOracleAttestation =
    "xanadu-oracle-attestation-v1\n";

[[nodiscard]] constexpr std::string_view
validationErrorToString(ValidationError err) noexcept {
  switch (err) {
  case ValidationError::Ok:
    return "Ok";
  case ValidationError::KeyRevoked:
    return "Key revoked";
  case ValidationError::CertificateExpired:
    return "Certificate expired";
  case ValidationError::CertificateNotYetValid:
    return "Certificate not yet valid";
  case ValidationError::DelegationSignatureInvalid:
    return "Delegation signature invalid";
  case ValidationError::CsrSignatureInvalid:
    return "CSR signature invalid";
  case ValidationError::DuplicateKey:
    return "Duplicate key found";
  case ValidationError::PathValidationFailed:
    return "X.509 path validation failed";
  case ValidationError::CorruptKey:
    return "Corrupt public/private key";
  case ValidationError::CorruptCertificate:
    return "Corrupt X.509 certificate";
  case ValidationError::CorruptSignature:
    return "Corrupt digital signature";
  case ValidationError::UnsupportedAlgorithm:
    return "Unsupported cryptographic algorithm";
  case ValidationError::IdentityMismatch:
    return "Identity fingerprint mismatch";
  case ValidationError::SerializationError:
    return "Serialization or parsing error";
  case ValidationError::InvalidMerkleRoot:
    return "Invalid Merkle root";
  case ValidationError::ProofVerificationFailed:
    return "Proof verification failed";
  case ValidationError::NonSequentialBlock:
    return "Non-sequential block";
  case ValidationError::BlockIndexMismatch:
    return "Block index mismatch";
  case ValidationError::TimestampInFuture:
    return "Timestamp in future";
  case ValidationError::HistoryTruncationDetected:
    return "History truncation detected";
  case ValidationError::DuplicateEntry:
    return "Duplicate entry";
  case ValidationError::VoterNotFound:
    return "Voter not found";
  case ValidationError::VoterTooYoung:
    return "Voter too young";
  case ValidationError::InvalidSignature:
    return "Invalid signature";
  case ValidationError::AttestationExpired:
    return "Attestation expired";
  case ValidationError::OracleNotAuthorized:
    return "Oracle not authorized";
  case ValidationError::UnauthorizedAction:
    return "Unauthorized action";
  case ValidationError::InsufficientProofOfWork:
    return "Insufficient proof of work";
  case ValidationError::ProofOfWorkExpired:
    return "Proof of work expired";
  case ValidationError::ProofOfWorkReplayDetected:
    return "Proof of work replay detected";
  }
  return "Unknown error";
}

/// Standard RFC 5280 CRL Reason Codes
enum class RevocationReason : std::uint8_t {
  Unspecified          = 0,
  KeyCompromise        = 1, // Device stolen or private key leaked
  CACompromise         = 2, // Master root identity compromised
  Superseded           = 4, // Device decommissioned or replaced
  CessationOfOperation = 5  // Temporary/testing device retired
};

/// Structure representing a standard X.509 v3 certificate
struct X509Certificate {
  std::vector<std::uint8_t> der;
  std::uint64_t serialNumber{0};
  std::uint64_t notBefore{0};
  std::uint64_t notAfter{0};
  PubKey32 subjectPublicKey;
  PubKey32 issuerPublicKey;
  std::string subjectDn;
  std::string issuerDn;

  [[nodiscard]] bool empty() const noexcept { return der.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return der.size(); }
  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
    return der;
  }

  /// Fluent setters for chaining support
  X509Certificate *setDer(std::vector<std::uint8_t> bytes) noexcept {
    der = std::move(bytes);
    return this;
  }

  X509Certificate *setSerial(std::uint64_t serial) noexcept {
    serialNumber = serial;
    return this;
  }

  X509Certificate *setValidity(std::uint64_t nb, std::uint64_t na) noexcept {
    notBefore = nb;
    notAfter  = na;
    return this;
  }

  X509Certificate *setSubjectPublicKey(const PubKey32 &pk) noexcept {
    subjectPublicKey = pk;
    return this;
  }

  X509Certificate *setIssuerPublicKey(const PubKey32 &pk) noexcept {
    issuerPublicKey = pk;
    return this;
  }

  X509Certificate *setSubjectDn(std::string dn) noexcept {
    subjectDn = std::move(dn);
    return this;
  }

  X509Certificate *setIssuerDn(std::string dn) noexcept {
    issuerDn = std::move(dn);
    return this;
  }

  bool operator==(const X509Certificate &other) const noexcept {
    if (!der.empty() || !other.der.empty()) {
      return der == other.der;
    }
    return serialNumber == other.serialNumber && notBefore == other.notBefore &&
           notAfter == other.notAfter &&
           subjectPublicKey == other.subjectPublicKey &&
           subjectDn == other.subjectDn && issuerDn == other.issuerDn;
  }
};

/// Authenticated device revocation record
struct DeviceRevocationRecord {
  std::uint64_t serialNumber{0};
  PubKey32 devicePublicKey;
  Fingerprint masterFingerprint;
  std::uint64_t revocationDate{0};
  RevocationReason reason{RevocationReason::KeyCompromise};
  Signature64 masterSignature;

  [[nodiscard]] std::string signingBuffer() const {
    const auto fieldU64 = [](std::string_view label, std::uint64_t val) {
      const auto s = std::to_string(val);
      return std::string(label) + ":" + std::to_string(s.size()) + ":" + s +
             "\n";
    };
    const auto fieldStr = [](std::string_view label, std::string_view val) {
      return std::string(label) + ":" + std::to_string(val.size()) + ":" +
             std::string(val) + "\n";
    };

    std::string out = std::string(kDomainDeviceRevocation);
    out += fieldU64("serial", serialNumber);
    out +=
        fieldStr("device", std::string_view(reinterpret_cast<const char *>(
                                                devicePublicKey.bytes.data()),
                                            devicePublicKey.bytes.size()));
    out += fieldStr("master", masterFingerprint.view());
    out += fieldU64("date", revocationDate);
    out += fieldU64("reason", static_cast<std::uint64_t>(reason));
    return out;
  }

  bool operator==(const DeviceRevocationRecord &) const = default;
};

/// Record for transitioning master identities
struct IdentitySupersededRecord {
  Fingerprint oldMasterFingerprint;
  Fingerprint newMasterFingerprint;
  std::uint64_t supersessionDate{0};
  Signature64 oldMasterSignature;
  Signature64 newMasterSignature;

  [[nodiscard]] std::string signingBuffer() const {
    const auto fieldStr = [](std::string_view label, std::string_view val) {
      return std::string(label) + ":" + std::to_string(val.size()) + ":" +
             std::string(val) + "\n";
    };
    std::string out = "xanadu-master-superseded-v1\n";
    out += fieldStr("old", oldMasterFingerprint.view());
    out += fieldStr("new", newMasterFingerprint.view());
    out += "date:" + std::to_string(std::to_string(supersessionDate).size()) +
           ":" + std::to_string(supersessionDate) + "\n";
    return out;
  }

  bool operator==(const IdentitySupersededRecord &) const = default;
};

/// Constraints for X.509 certificate issuance
struct CertificateConstraints {
  std::uint64_t serialNumber{1};
  std::uint64_t notBefore{0};
  std::uint64_t notAfter{0};
  std::string deviceId;
  std::string deviceName;
  std::string authorName;
  std::string authorEmail;
};

/// Abstract interface for digital signing
class ISigner {
public:
  virtual ~ISigner() = default;

  [[nodiscard]] virtual std::string algorithmName() const noexcept = 0;
  [[nodiscard]] virtual PubKey32 publicKey() const noexcept        = 0;
  [[nodiscard]] virtual std::expected<Signature64, ValidationError>
  sign(std::span<const std::uint8_t> message) const noexcept = 0;
  [[nodiscard]] virtual std::optional<std::span<const std::uint8_t, 32>>
  rawPrivateKey() const noexcept {
    return std::nullopt;
  }
};

/// Abstract interface for signature verification
class IVerifier {
public:
  virtual ~IVerifier() = default;

  [[nodiscard]] virtual std::string algorithmName() const noexcept = 0;
  [[nodiscard]] virtual std::expected<void, ValidationError>
  verify(const PubKey32 &pubKey, std::span<const std::uint8_t> message,
         const Signature64 &signature) const noexcept = 0;
};

/// Abstract interface for X.509 certificate authority and verification
class ICertificateEngine {
public:
  virtual ~ICertificateEngine() = default;

  /// Generate a PKCS#10 CSR on a workstation using the local device key
  [[nodiscard]] virtual std::expected<std::vector<std::uint8_t>,
                                      ValidationError>
  generateCsr(const ISigner &deviceSigner, const std::string &deviceId,
              const std::string &deviceName) const noexcept = 0;

  /// Authorize a CSR using the Master Key (can run offline / air-gapped)
  [[nodiscard]] virtual std::expected<X509Certificate, ValidationError>
  authorizeCsr(const ISigner &masterSigner,
               std::span<const std::uint8_t> csrDer,
               const CertificateConstraints &constraints) const noexcept = 0;

  /// Full X.509 path validation using OpenSSL X509_STORE_CTX, validating
  /// signatures, temporal validity, BasicConstraints (cA=FALSE), and KeyUsage
  [[nodiscard]] virtual std::expected<void, ValidationError>
  verifyDelegation(const X509Certificate &cert,
                   const PubKey32 &expectedMasterKey,
                   std::uint64_t currentTime) const noexcept = 0;
};

/// Abstract interface for revocation queries
class IRevocationRegistry {
public:
  virtual ~IRevocationRegistry() = default;

  [[nodiscard]] virtual bool
  isDeviceRevoked(const PubKey32 &deviceKey,
                  std::uint64_t timestamp) const noexcept = 0;

  [[nodiscard]] virtual bool
  isSerialRevoked(std::uint64_t serialNumber,
                  std::uint64_t timestamp) const noexcept = 0;

  [[nodiscard]] virtual bool
  isIdentityRevoked(const Fingerprint &masterFingerprint) const noexcept = 0;
};

} // namespace xanadu::identity

#endif // XANADU_IDENTITY_ENGINE_HPP
