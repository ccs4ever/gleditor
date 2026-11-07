/**
 * @file epistemic_adapter_wire.hpp
 * @brief The signed ingress message: canonical bytes, header and payloads
 * (P19 I3-3a1, `i3-3-design.md` sections 3.1 to 3.4).
 *
 * A message is a bencode envelope `{b, c?, k, s}` around a canonical bencode
 * body `{h, p}`; the signature is Ed25519 over `kDomainVagentSigned || b`.
 * Decoding is the shape step of the verification order (3.8 step 2): every
 * key set is closed, every field has one encoding, and nothing here trusts a
 * byte, because nothing here has checked a signature yet.
 *
 * Why C++: this codec sits in front of the signature check, which is the
 * host's authorization boundary; a Vortex routine would run on bytes nobody
 * has authenticated.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_WIRE_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_WIRE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"

namespace xanadu::identity {
class StandardCryptoEngine;
}

namespace xanadu::vagent::runtime {

/// Every signed integer is below this in magnitude (S-8): about 146 years in
/// nanoseconds, with room for every sum the time checks form, which are
/// still taken in 128-bit integers.
inline constexpr std::int64_t kSignedIntBound = std::int64_t{1} << 62;
/// A Float travels as its IEEE-754 binary64 bit pattern, big-endian.
inline constexpr std::size_t kSignedFloatBytes = 8;
/// Issuer, audience and policy hash: SHA-256 as lowercase hex.
inline constexpr std::size_t kSignedHexIdBytes = 64;
inline constexpr std::size_t kSignedNonceBytes = 32;
/// A health report code: an identifier, short enough for an audit line.
inline constexpr std::size_t kMaxHealthCodeBytes = 64;

/// `vagent.epistemic_signed_message_bytes`: an observation is under 1 KiB, and
/// the ceiling is the bound `observe` already holds signed bytes to.
inline constexpr std::size_t kDefaultSignedMessageBytes = 4096;
inline constexpr std::size_t kMinSignedMessageBytes     = 1024;
inline constexpr std::size_t kMaxSignedMessageBytes =
    kMaxObservationSignedBytes;

enum class SignedKind : std::uint8_t {
  Observation,
  Heartbeat,
  HealthReport,
  ClockPair,
  /// Its header is fixed now so I3-4a changes no accepted format; its payload
  /// is I3-4a's, so a1 refuses it as `UnsupportedKind`.
  Verification,
};

enum class SignedClockBasis : std::uint8_t {
  /// The device clock tracks wall time.
  Synchronised,
  /// A local monotonic clock, mapped by pairing.
  Free,
};

enum class ReportAction : std::uint8_t { Raise, Clear, Fail };

[[nodiscard]] std::string_view toString(SignedKind kind) noexcept;
[[nodiscard]] std::string_view toString(SignedClockBasis basis) noexcept;
[[nodiscard]] std::string_view toString(ReportAction action) noexcept;

/// The header common to every kind (3.2). Times are nanoseconds on the
/// device clock named by `epoch`; negative values pass decode, because
/// whether a negative time is a fault is a question about the device, which
/// the time checks answer once the signature says whose bytes these are.
struct SignedHeader {
  SignedKind kind{SignedKind::Observation};
  /// `sha256hex(k)`, compared as text.
  std::string issuer;
  std::int64_t incarnation{1};
  /// A device rule, strictly increasing per (issuer, incarnation) across all
  /// kinds; the host enforces it per kind through three floors.
  std::int64_t sequence{};
  std::string audience;
  std::int64_t sent{};
  std::int64_t epoch{};
  SignedClockBasis basis{SignedClockBasis::Synchronised};
  std::int64_t clockUncertainty{};
  std::optional<std::int64_t> expires;
  std::optional<std::int64_t> lowWatermark;

  bool operator==(const SignedHeader &) const = default;
};

/// One element of an observation's tuple as signed: an Int (also a Bool, 0 or
/// 1) or a byte string (a Float's bit pattern or UTF-8 text). The declaration
/// says which, and the adapter types it (I3-3a2); see `signedFloat`,
/// `signedBool` and `isSignedText`.
using SignedTupleItem = std::variant<std::int64_t, std::string>;

struct ObservationPayload {
  std::string eventId;
  std::string predicate;
  /// The predicate's `policyHash`, 64 lowercase hex.
  std::string policy;
  std::vector<SignedTupleItem> tuple;
  std::int64_t acquired{};
  std::int64_t acquiredUncertainty{};
  std::string frame;
  std::string units;
  std::optional<double> variance;

  bool operator==(const ObservationPayload &) const = default;
};

struct HeartbeatPayload {
  bool operator==(const HeartbeatPayload &) const = default;
};

struct HealthReportPayload {
  ReportAction action{ReportAction::Raise};
  std::string code;

  bool operator==(const HealthReportPayload &) const = default;
};

struct ClockPairPayload {
  std::array<std::uint8_t, kSignedNonceBytes> nonce{};

  bool operator==(const ClockPairPayload &) const = default;
};

using SignedPayload = std::variant<ObservationPayload, HeartbeatPayload,
                                   HealthReportPayload, ClockPairPayload>;

/// The unsigned wrapper. Tampering with `key` breaks the issuer check, with
/// `certificate` the delegation, and with `signature` the signature, so it
/// needs no canonical form of its own.
struct SignedEnvelope {
  /// The signed bytes, exactly as received.
  std::string body;
  /// Certificate DER: present exactly on a delegated binding.
  std::optional<std::vector<std::uint8_t>> certificate;
  identity::PubKey32 key;
  identity::Signature64 signature;

  bool operator==(const SignedEnvelope &) const = default;
};

struct SignedMessage {
  SignedEnvelope envelope;
  SignedHeader header;
  SignedPayload payload;
};

/**
 * @brief Decode and shape-check one message (3.8 step 2).
 *
 * Refuses `OversizeMessage` past @p maxBytes, `MalformedMessage` for any
 * shape fault, `NonCanonicalMessage` for a body that is a second spelling of
 * its value, and `UnsupportedKind` for a kind outside the table or reserved.
 * Never throws.
 */
[[nodiscard]] std::expected<SignedMessage, EpistemicRefusal>
decodeSignedMessage(std::string_view bytes, std::size_t maxBytes);

/// The canonical body for @p header and @p payload. Writes what it is given,
/// valid or not, so tests can build every refusal; `decodeSignedMessage`
/// decides validity.
[[nodiscard]] std::string encodeSignedBody(const SignedHeader &header,
                                           const SignedPayload &payload);

[[nodiscard]] std::string encodeSignedEnvelope(const SignedEnvelope &envelope);

/// What is signed: the domain followed by the body, with no length between
/// them; the domains are prefix-free instead (3.4).
[[nodiscard]] std::string signedBytesOf(std::string_view body);

/// Sign @p body with @p device's key and wrap it: the encoder tests and
/// firmware share.
[[nodiscard]] std::expected<SignedEnvelope, identity::ValidationError>
signEnvelope(const identity::StandardCryptoEngine &device, std::string body,
             std::optional<std::vector<std::uint8_t>> certificate = {});

/// The issuer token of a key: SHA-256 as 64 lowercase hex.
[[nodiscard]] std::string issuerOf(const identity::PubKey32 &key);

/// The 8-byte big-endian bit pattern of @p value. Writes `-0` and non-finite
/// values as they are, so tests can build their refusals.
[[nodiscard]] std::string encodeSignedFloat(double value);
/// A finite, non-negative-zero double from exactly 8 bytes, or nothing.
[[nodiscard]] std::optional<double> signedFloat(std::string_view bytes);
/// 0 or 1, or nothing.
[[nodiscard]] std::optional<bool> signedBool(std::int64_t value);
/// Printable ASCII 0x21 to 0x7E, one to @p maxBytes long: no space, control,
/// newline, NUL or bidi byte can reach an outcome key, an audit entry or a
/// log line.
[[nodiscard]] bool isSignedIdentifier(std::string_view text,
                                      std::size_t maxBytes);
/// Strict UTF-8 (no overlong form, surrogate or code point past U+10FFFF),
/// at most @p maxBytes long: tuple text is a reading, not an identifier.
[[nodiscard]] bool isSignedText(std::string_view text, std::size_t maxBytes);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_WIRE_HPP
