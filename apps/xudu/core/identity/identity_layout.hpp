/**
 * @file identity_layout.hpp
 * @brief Standard alignment structures and types for decentralized identity.
 *
 * Implements hardened, fixed-size byte containers with constant-time
 * comparison, strict bounds validation, and standard layout structs for ledger
 * entries, Oracle consensus, and BEP 10 wire messages.
 */
#ifndef XUDU_IDENTITY_LAYOUT_HPP
#define XUDU_IDENTITY_LAYOUT_HPP

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xudu::identity {

// Maximum allowed sizes to prevent memory exhaustion and DoS attacks
inline constexpr std::size_t kMaxEmailLength        = 254; // RFC 5321 limit
inline constexpr std::size_t kMaxIdentityNameLength = 128;
inline constexpr std::size_t kMaxArmoredKeyLength   = 16 * 1024; // 16 KB max
inline constexpr std::size_t kMaxSmtpChallengeToken = 64;
inline constexpr std::size_t kMaxPayloadBytes = 64 * 1024; // 64 KB per frame

// Constant-time memory comparison helper to resist timing side-channel attacks
template <std::size_t N>
[[nodiscard]] constexpr bool
constantTimeEquals(const std::array<std::uint8_t, N> &a,
                   const std::array<std::uint8_t, N> &b) noexcept {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < N; ++i) {
    diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

template <std::size_t N>
[[nodiscard]] constexpr bool
constantTimeEquals(const std::array<char, N> &a,
                   const std::array<char, N> &b) noexcept {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < N; ++i) {
    diff |= static_cast<std::uint8_t>(static_cast<std::uint8_t>(a[i]) ^
                                      static_cast<std::uint8_t>(b[i]));
  }
  return diff == 0;
}

/// 40-character normalized uppercase hexadecimal OpenPGP v4 fingerprint.
struct Fingerprint {
  std::array<char, 40> hex{};

  constexpr Fingerprint() noexcept { hex.fill('0'); }

  explicit constexpr Fingerprint(const std::array<char, 40> &chars) noexcept
      : hex(chars) {}

  [[nodiscard]] constexpr bool isValid() const noexcept {
    for (const char c : hex) {
      const bool isHex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
      if (!isHex) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string_view view() const noexcept {
    return {hex.data(), hex.size()};
  }

  [[nodiscard]] std::string toString() const {
    return std::string(hex.data(), hex.size());
  }

  [[nodiscard]] static std::optional<Fingerprint>
  fromString(std::string_view raw) noexcept {
    Fingerprint fp;
    std::size_t outIdx = 0;
    for (const char c : raw) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        continue;
      }
      if (outIdx >= 40) {
        return std::nullopt; // Exceeds 40 hex digits
      }
      const auto uc =
          static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (!((uc >= '0' && uc <= '9') || (uc >= 'A' && uc <= 'F'))) {
        return std::nullopt; // Invalid hex character
      }
      fp.hex[outIdx++] = uc;
    }
    if (outIdx != 40) {
      return std::nullopt; // Incomplete fingerprint
    }
    return fp;
  }

  [[nodiscard]] constexpr bool
  operator==(const Fingerprint &other) const noexcept {
    return constantTimeEquals(hex, other.hex);
  }

  [[nodiscard]] constexpr auto
  operator<=>(const Fingerprint &other) const noexcept = default;
};

/// 32-byte cryptographic hash (SHA-256 / Blake3 / Merkle Root).
struct Hash32 {
  std::array<std::uint8_t, 32> bytes{};

  constexpr Hash32() noexcept = default;
  explicit constexpr Hash32(const std::array<std::uint8_t, 32> &b) noexcept
      : bytes(b) {}

  [[nodiscard]] constexpr bool isZero() const noexcept {
    std::uint8_t acc = 0;
    for (const auto b : bytes) {
      acc |= b;
    }
    return acc == 0;
  }

  [[nodiscard]] std::string toHex() const {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const auto b : bytes) {
      out.push_back(kHexDigits[(b >> 4) & 0x0F]);
      out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
  }

  [[nodiscard]] static std::optional<Hash32>
  fromHex(std::string_view hex) noexcept {
    if (hex.size() != 64) {
      return std::nullopt;
    }
    Hash32 h;
    for (std::size_t i = 0; i < 32; ++i) {
      auto decodeNibble = [](char c) -> std::optional<std::uint8_t> {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
          return static_cast<std::uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
          return static_cast<std::uint8_t>(c - 'A' + 10);
        return std::nullopt;
      };
      const auto hi = decodeNibble(hex[2 * i]);
      const auto lo = decodeNibble(hex[2 * i + 1]);
      if (!hi || !lo) {
        return std::nullopt;
      }
      h.bytes[i] = static_cast<std::uint8_t>((*hi << 4) | *lo);
    }
    return h;
  }

  [[nodiscard]] constexpr bool operator==(const Hash32 &other) const noexcept {
    return constantTimeEquals(bytes, other.bytes);
  }

  [[nodiscard]] constexpr auto
  operator<=>(const Hash32 &other) const noexcept = default;
};

/// 32-byte Ed25519/Curve25519 Public Key.
struct PubKey32 {
  std::array<std::uint8_t, 32> bytes{};

  constexpr PubKey32() noexcept = default;
  explicit constexpr PubKey32(const std::array<std::uint8_t, 32> &b) noexcept
      : bytes(b) {}

  [[nodiscard]] constexpr bool isZero() const noexcept {
    std::uint8_t acc = 0;
    for (const auto b : bytes) {
      acc |= b;
    }
    return acc == 0;
  }

  [[nodiscard]] constexpr bool
  operator==(const PubKey32 &other) const noexcept {
    return constantTimeEquals(bytes, other.bytes);
  }

  [[nodiscard]] constexpr auto
  operator<=>(const PubKey32 &other) const noexcept = default;
};

/// 64-byte Ed25519 or cryptographic signature buffer.
struct Signature64 {
  std::array<std::uint8_t, 64> bytes{};

  constexpr Signature64() noexcept = default;
  explicit constexpr Signature64(const std::array<std::uint8_t, 64> &b) noexcept
      : bytes(b) {}

  [[nodiscard]] constexpr bool isZero() const noexcept {
    std::uint8_t acc = 0;
    for (const auto b : bytes) {
      acc |= b;
    }
    return acc == 0;
  }

  [[nodiscard]] constexpr bool
  operator==(const Signature64 &other) const noexcept {
    return constantTimeEquals(bytes, other.bytes);
  }

  [[nodiscard]] constexpr auto
  operator<=>(const Signature64 &other) const noexcept = default;
};

/// BEP 10 Extension Message Identifiers
enum class MessageType : std::uint8_t {
  // 0x01 - 0x0F: Identity Lookup & Peer Challenge (Extension
  // "xudu_identity_lookup")
  IdentityQuery     = 0x01,
  IdentityResponse  = 0x02,
  PeerAuthChallenge = 0x03,
  PeerAuthResponse  = 0x04,
  ConnectionDenial  = 0x05,

  // 0x10 - 0x1F: Oracle Consensus & Voting (Extension "xudu_oracle_vote")
  OracleVoteBroadcast     = 0x10,
  OracleConsensusQuery    = 0x11,
  OracleConsensusResponse = 0x12,

  // 0x20 - 0x2F: Oracle Email Verification (Extension "xudu_oracle_verify")
  EmailVerifyRequest      = 0x20,
  EmailVerifyChallengeAck = 0x21,
  EmailVerifyAttestation  = 0x22,

  // 0x30 - 0x3F: Transcopyright Micropayments & Key Delivery (Extension
  // "xudu_transcopyright")
  TcInvoiceQuery    = 0x30,
  TcInvoiceResponse = 0x31,
  TcSettleRequest   = 0x32,
  TcKeyDelivery     = 0x33
};

/// Serialization and Deserialization Errors
enum class SerializationError : std::uint8_t {
  None = 0,
  InvalidBencode,
  MissingField,
  InvalidFieldLength,
  TypeMismatch,
  PayloadOverflow,
  InvalidHexFormat,
  StringLengthExceeded,
  TrailingData
};

inline constexpr std::uint8_t kDefaultHashcashDifficulty = 20;

/// Ledger and Engine Validation Errors
enum class ValidationError : std::uint8_t {
  None = 0,
  InvalidMerkleRoot,
  ProofVerificationFailed,
  NonSequentialBlock,
  BlockIndexMismatch,
  TimestampInFuture,
  HistoryTruncationDetected,
  DuplicateEntry,
  VoterNotFound,
  VoterTooYoung, // Active identity age < 30 days
  InvalidSignature,
  AttestationExpired,
  OracleNotAuthorized,
  UnauthorizedAction,
  InsufficientProofOfWork,
  ProofOfWorkExpired,
  ProofOfWorkReplayDetected
};

/// A single verified identity record in the Merkle ledger.
struct IdentityEntry {
  Fingerprint fingerprint{};
  std::string email;
  std::string identityName;
  std::string publicKeyArmored;
  std::uint64_t timestamp{};
  std::uint64_t sequence{};
  bool revoked{false};
  Signature64 signature{};

  [[nodiscard]] bool isValid() const noexcept {
    return fingerprint.isValid() && !email.empty() &&
           email.size() <= kMaxEmailLength &&
           identityName.size() <= kMaxIdentityNameLength &&
           publicKeyArmored.size() <= kMaxArmoredKeyLength;
  }

  [[nodiscard]] bool operator==(const IdentityEntry &) const = default;
};

/// A weighted consensus vote endorsing an Oracle candidate.
struct VoteEntry {
  Fingerprint voterFingerprint{};
  Fingerprint candidateOracle{};
  std::uint64_t timestamp{};
  std::uint64_t sequence{};
  Signature64 signature{};

  [[nodiscard]] bool isValid() const noexcept {
    return voterFingerprint.isValid() && candidateOracle.isValid() &&
           !(voterFingerprint == candidateOracle); // Cannot vote for self
  }

  [[nodiscard]] bool operator==(const VoteEntry &) const = default;
};

/// Cryptographic header declared in every block of the ledger torrent.
struct BlockHeader {
  std::uint64_t blockIndex{};
  std::uint64_t timestamp{};
  Hash32 previousHash{};
  Hash32 merkleRoot{};
  std::uint32_t identityCount{};
  std::uint32_t voteCount{};

  [[nodiscard]] bool isValid() const noexcept {
    return !merkleRoot.isZero() && (blockIndex == 0 || !previousHash.isZero());
  }

  [[nodiscard]] bool operator==(const BlockHeader &) const = default;
};

/// Attestation token issued by a verified Oracle confirming an SMTP
/// verification.
struct OracleAttestation {
  Fingerprint oracleFingerprint{};
  Fingerprint targetFingerprint{};
  std::string verifiedEmail;
  std::uint64_t issuedTimestamp{};
  std::uint64_t expiresTimestamp{};
  Signature64 oracleSignature{};

  [[nodiscard]] bool isValid() const noexcept {
    return oracleFingerprint.isValid() && targetFingerprint.isValid() &&
           !verifiedEmail.empty() && verifiedEmail.size() <= kMaxEmailLength &&
           expiresTimestamp > issuedTimestamp;
  }

  [[nodiscard]] bool operator==(const OracleAttestation &) const = default;
};

/// Cryptographic authentication challenge sent across the wire.
struct PeerChallenge {
  Hash32 nonce{};
  std::uint64_t timestamp{};

  [[nodiscard]] bool isValid() const noexcept { return !nonce.isZero(); }
  [[nodiscard]] bool operator==(const PeerChallenge &) const = default;
};

/// Response to a PeerChallenge containing detached signature over the nonce.
struct PeerChallengeResponse {
  Hash32 nonce{};
  Fingerprint claimedIdentity{};
  /// The Ed25519 device key the signature is by. Carried so the verifier
  /// knows which key to check against -- but on its own it proves nothing,
  /// since a peer picks it freely. What decides the question is whether the
  /// claimed identity ever delegated to this key.
  std::array<std::uint8_t, 32> devicePublicKey{};
  Signature64 signature{};

  [[nodiscard]] bool isValid() const noexcept {
    return !nonce.isZero() && claimedIdentity.isValid() && !signature.isZero();
  }
  [[nodiscard]] bool operator==(const PeerChallengeResponse &) const = default;
};

/// BEP 10 Identity Query request payload.
struct IdentityQueryMsg {
  Fingerprint targetFingerprint{};
  std::string targetEmail;

  [[nodiscard]] bool isValid() const noexcept {
    return targetFingerprint.isValid() ||
           (!targetEmail.empty() && targetEmail.size() <= kMaxEmailLength);
  }
  [[nodiscard]] bool operator==(const IdentityQueryMsg &) const = default;
};

/// A verified Hashcash Proof-of-Work stamp.
struct HashcashStamp {
  std::string resource;
  std::uint64_t timestamp{};
  std::uint64_t nonce{};
  std::uint8_t difficultyBits{kDefaultHashcashDifficulty};

  [[nodiscard]] bool operator==(const HashcashStamp &) const = default;
};

/// BEP 10 Email Verification Request payload sent to an Oracle.
struct EmailVerifyRequestMsg {
  Fingerprint requesterFingerprint{};
  std::string targetEmail;
  std::uint64_t timestamp{};
  std::uint64_t powNonce{0};
  std::uint8_t difficultyBits{kDefaultHashcashDifficulty};
  Signature64 requesterSignature{};

  [[nodiscard]] bool isValid() const noexcept {
    return requesterFingerprint.isValid() && !targetEmail.empty() &&
           targetEmail.size() <= kMaxEmailLength &&
           !requesterSignature.isZero() && difficultyBits > 0;
  }
  [[nodiscard]] bool operator==(const EmailVerifyRequestMsg &) const = default;
};

/// BEP 10 Transcopyright Invoice Query message.
struct TcInvoiceQueryMsg {
  Hash32 keyId{};
  std::uint64_t requestedBytes{0};

  [[nodiscard]] bool isValid() const noexcept { return !keyId.isZero(); }
  [[nodiscard]] bool operator==(const TcInvoiceQueryMsg &) const = default;
};

/// BEP 10 Transcopyright Invoice Response message.
struct TcInvoiceResponseMsg {
  Hash32 keyId{};
  std::uint64_t priceAtomicUnits{0};
  bool flatFee{false};
  std::string currencySymbol{"XU"};
  Fingerprint authorWallet{};
  PubKey32 authorPubKey{};
  Hash32 paymentChallenge{};
  std::uint64_t expiresTimestamp{0};

  [[nodiscard]] bool isValid() const noexcept {
    return !keyId.isZero() && authorWallet.isValid() &&
           !authorPubKey.isZero() && !currencySymbol.empty();
  }
  [[nodiscard]] bool operator==(const TcInvoiceResponseMsg &) const = default;
};

/// BEP 10 Transcopyright Micropayment Settlement Request message.
struct TcSettleRequestMsg {
  Hash32 keyId{};
  Hash32 paymentChallenge{};
  std::uint64_t amountAtomicUnits{0};
  Fingerprint payerWallet{};
  PubKey32 payerPubKey{}; // X25519 public key for KEM CEK delivery
  Signature64 paymentProofSignature{};
  std::string micropaymentTicket{};

  [[nodiscard]] bool isValid() const noexcept {
    return !keyId.isZero() && !payerPubKey.isZero() && payerWallet.isValid();
  }
  [[nodiscard]] bool operator==(const TcSettleRequestMsg &) const = default;
};

/// BEP 10 Transcopyright CEK Key Delivery message.
struct TcKeyDeliveryMsg {
  Hash32 keyId{};
  std::vector<std::uint8_t> wrappedCek{}; // 104-byte KEM payload
  Signature64 authorSignature{};

  [[nodiscard]] bool isValid() const noexcept {
    return !keyId.isZero() && !wrappedCek.empty();
  }
  [[nodiscard]] bool operator==(const TcKeyDeliveryMsg &) const = default;
};

} // namespace xudu::identity

#endif // XUDU_IDENTITY_LAYOUT_HPP
