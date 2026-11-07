/**
 * @file epistemic_ingress.hpp
 * @brief Checked observation ingress for arbitrated predicates (P19 I3-1b).
 *
 * The host's ingress adapter has already verified the origin signature; the
 * executor checks what needs no arena, queues the observation and runs it in
 * its own ingestion turn. A receipt means queued, never committed.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_INGRESS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_INGRESS_HPP

#include <gleditor/cpp26_inplace_vector.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/runtime/wire.hpp"

namespace xanadu::vagent::runtime {

enum class EpistemicRefusal : std::uint8_t;

/// What the origin signed, in nanoseconds on its clock (design 4.1).
struct DeviceTimes {
  std::int64_t acquired{};
  std::int64_t sent{};
  std::int64_t acquiredUncertainty{};
  std::int64_t clockUncertainty{};
  std::optional<std::int64_t> expires;
};

/// The host's mapping of one device clock onto the policy's basis, in
/// seconds: M(x) = x + offset, u_M(x) = uncertainty + 1e-6 * driftPpm *
/// |x - anchor|.
struct ClockMapping {
  double offset{};
  double uncertainty{};
  double anchor{};
  double driftPpm{};
  std::uint64_t version{};
};

/// One observation as the host's verifying ingress adapter hands it over.
/// `principal` is the origin signer, never the transport peer.
struct CheckedObservation {
  std::string principal;
  std::uint64_t incarnation{};
  /// The transport principal, set by the host whenever it differs from the
  /// origin signer.
  std::optional<std::string> relayedBy;
  std::string eventId;
  std::uint64_t sequence{};
  std::string predicate;
  /// The predicate's arguments, in declaration order.
  WireList tuple;
  /// What the origin signed (the adapter decoded it; `observe` never reads
  /// the device's clock itself).
  DeviceTimes device;
  /// The host's mapping of the device clock onto `clockBasis`; absent when it
  /// has none for the epoch (row 5 of design 4.3).
  std::optional<ClockMapping> mapping;
  /// The basis the mapping maps onto: the policy's, or the reading refuses.
  std::string clockBasis;
  std::uint64_t clockEpoch{};
  std::string frame;
  std::string units;
  /// Full scalar isotropic variance in squared units, continuous only.
  std::optional<double> variance;
  /// The predicate's policy hash as the origin signed it. Set by the signed
  /// adapter; a hash that is not the binding's refuses `PolicyMismatch`, so a
  /// message signed for another declaration of the predicate never lands.
  std::optional<std::string> policy;
  /// A refusal the signed adapter decided on verified, signed fields after
  /// step 5 (`IncarnationStep`, `InvalidValue`), handed over so it is logged
  /// and charged by the same rules as `observe`'s own. Any other code is
  /// refused as `InvalidObservation`.
  std::optional<EpistemicRefusal> adapterRefusal;
  /// The issuer's highest verified sequence for the incarnation (design 7.1),
  /// set by the signed adapter. Sequences it passed are signed and verified,
  /// so a dropped reading's pass credit is bounded by it for any stride;
  /// without it the bound is the health sequence gap above the mark.
  std::optional<std::uint64_t> verifiedSequenceBound;
};

/// The host's authorization for one origin principal to report one arbitrated
/// predicate to one actor. The alias is for routing and display only; the
/// principal names the source everywhere it is recorded.
struct EpistemicSourceGrant {
  std::string alias;
  /// Lowercase hex fingerprint of the origin identity, 64 digits.
  std::string principal;
  std::uint64_t incarnation{};
  std::string predicate;
  /// From binding, on the executor's steady clock, so a wall-clock step can
  /// neither expire nor extend it.
  std::chrono::steady_clock::duration lifetime{};
  /// The device's declared heartbeat interval for the principal (design 5.1),
  /// and for this predicate its data interval. Each is at least
  /// `heartbeat_floor_ms` or `bindSource` refuses `InvalidBinding`.
  std::optional<std::chrono::milliseconds> heartbeat;
  std::optional<std::chrono::milliseconds> data;
};

/// Queued, never committed: the ingestion turn decides that later.
struct TransportReceipt {
  std::string eventId;
};

/// Refusals the executor gives before queueing (binding and observation
/// checks that need no arena). Each is also recorded in the outcome log.
enum class EpistemicRefusal : std::uint8_t {
  NotArbitrated,
  InvalidPrincipal,
  AliasInUse,
  DuplicateAlias,
  UnknownSource,
  /// Below the incarnation bound for the principal: a replay of a superseded
  /// boot, or a binding that would lower the floor.
  StaleIncarnation,
  /// Above the bound incarnation: only binding advances one.
  UnboundIncarnation,
  /// An incarnation that does not fit the arena's Int64.
  InvalidIncarnation,
  /// No actor the executor knows has that pid.
  UnknownActor,
  InvalidObservation,
  ClockUnsynchronised,
  /// Row 2: acquired after it was sent, on the device's own signed times.
  ObservationTimeMismatch,
  /// Row 3: a signed acquisition uncertainty above the policy's
  /// max_uncertainty: the reading cannot be placed in time closely enough.
  UncertainAcquisition,
  /// Row 4: the device held the reading longer than max_age before signing.
  StaleObservation,
  ObservationFrameMismatch,
  MissingVariance,
  InvalidVariance,
  IngressBackpressure,
  /// A tuple that is mistyped or carries a non-finite number: decided on the
  /// origin's signed fields, so it counts for source health even relayed.
  InvalidValue,
  /// A negative signed time or uncertainty (row 1 of design 4.3): decided on
  /// signed integers alone, so it counts even relayed.
  InvalidAcquisition,
  /// Health actions and grants (P19 I3-2c1).
  InvalidHealthGrant,
  UnauthorizedHealthAction,
  InvalidHealthCode,
  InvalidHealthRequest,
  UnknownGrant,
  SelfCertification,
  EntropyUnavailable,
  /// The host's negative cache says the source is isolated; the arena would
  /// refuse the reading the same way, so no turn is spent on it.
  SourceFaulted,
  /// The host has closed this actor's door for an upgrade or a pause. Decided
  /// in the scope that would have queued the post, so the device did nothing
  /// wrong: nothing is logged, charged or noted, and the same bytes can be
  /// resent once admission reopens.
  AdmissionClosed,
  // Signed ingress (P19 I3-3a1), steps 2, 3 and 5 of the verification order.
  // None is logged, charged or counted: until step 5 passes the bytes are
  // nobody's, so an unauthenticated flood cannot grow the outcome log.
  /// Larger than `vagent.epistemic_signed_message_bytes`.
  OversizeMessage,
  /// Not the envelope, header or payload shape: a missing, unknown, mistyped
  /// or out-of-bounds field, or a field the kind's table refuses.
  MalformedMessage,
  /// A body that parses but is a second spelling of its value.
  NonCanonicalMessage,
  /// A kind outside the table, or one reserved for a later slice.
  UnsupportedKind,
  /// A small-order key or a non-canonical point encoding.
  InvalidSignerKey,
  /// The header's issuer is not the SHA-256 of the envelope's key.
  IssuerKeyMismatch,
  /// A certificate on a pinned binding.
  UnexpectedCertificate,
  /// No certificate on a delegated binding.
  DelegationMissing,
  /// The revocation registry has not loaded; resend once it has.
  RevocationUnavailable,
  BadSignature,
  /// The certificate does not chain to the binding's master, or names
  /// another key, or the master is not in the ledger.
  DelegationInvalid,
  /// The certificate is outside its validity window at receipt.
  DelegationExpired,
  /// The device key, its certificate serial or its master is revoked.
  KeyRevoked,
  /// Step 4: a header a verified message already used, refused before the
  /// signature check. The names are the arena's turn results for the same
  /// conditions.
  StaleSequence,
  Duplicate,
  // The adapter (P19 I3-3a2), steps 3 and 7 and binding.
  /// The header's audience is not that of any agent the adapter binds.
  AudienceMismatch,
  /// A bound agent's audience has not committed yet; resend once it has.
  AudienceUnavailable,
  /// The payload's policy hash is not the bound predicate's.
  PolicyMismatch,
  /// A `signed` binding's incarnation advance larger than
  /// `vagent.epistemic_incarnation_step`.
  IncarnationStep,
  /// A source binding the adapter cannot hold: a bad principal or master, no
  /// predicate, a repeated predicate or a non-positive lifetime.
  InvalidBinding,
  /// No mapping from the device's clock to the policy's basis (row 5).
  UnknownClockMapping,
  // The time split (P19 I3-3b1, design 4.3 rows 5a to 9). Logged and charged,
  // never counted: each is decided on the host's mapping or on the path.
  /// The mapping holds a non-finite or out-of-range value, or maps onto
  /// another basis than the policy's.
  InvalidClockMapping,
  /// The signed uncertainty fits the policy's bound only before the mapping's
  /// own is added.
  UncertainClockMapping,
  /// The mapped send time is in the host's future beyond every bound.
  ClockMappingMismatch,
  /// The path made the reading older than the policy admits.
  LateDelivery,
  /// The message's signed `expires` has passed.
  MessageExpired,
  // Pairing (P19 I3-3b1 part B). Decided by the adapter before the executor
  // sees anything: never logged, charged or counted.
  /// A `clock_pair` reply with no live challenge for its issuer, a nonce or
  /// audience that is not the challenge's, or one that came too late.
  ClockPairUnsolicited,
};

/// What the host may do with a refused event.
enum class OutcomeClass : std::uint8_t {
  /// Never resend these bytes.
  Terminal,
  /// Resend unchanged; the condition clears by itself.
  Retryable,
  /// Clears only after the host acts (retire a source, bind an incarnation,
  /// change a setting, checkpoint and resume).
  NeedsHostAction,
};

/// What the settle loop did after a source change committed (P19 I3-2b).
struct SettleReport {
  /// Top-level belief events whose settle turn committed. A handler that
  /// failed inside its own choice point is contained and still counts here.
  std::uint32_t settled{};
  /// Top-level belief events whose settle turn failed and were dropped.
  std::uint32_t discarded{};
  /// The first four distinct event types discarded, in order; `discarded`
  /// counts all of them, so a fifth distinct type is counted but not named.
  gleditor::cpp26::inplace_vector<std::string, 4> names;

  /// Count one discarded event and name its type if there is room for it.
  SettleReport &discard(std::string_view type) {
    ++discarded;
    name(type);
    return *this;
  }
  /// Add @p other's counts and the names this report lacks, up to four.
  SettleReport &merge(const SettleReport &other) {
    settled += other.settled;
    discarded += other.discarded;
    for (const auto &type : other.names) name(type);
    return *this;
  }
  bool operator==(const SettleReport &) const = default;

private:
  void name(std::string_view type) {
    if (names.size() < names.capacity() &&
        std::ranges::find(names, type) == names.end())
      names.push_back(std::string(type));
  }
};

/// One event's ingestion outcome as the host's outcome log records it.
struct IngestOutcome {
  enum class Kind : std::uint8_t {
    Pending,
    Committed,
    Duplicate,
    Refused,
    NoChange
  };
  Kind kind{Kind::Pending};
  /// The refusal code, present exactly when `kind` is `Refused`.
  std::optional<std::string> code;
  std::optional<OutcomeClass> outcomeClass;
  /// A committed source change that queued belief events: how they settled.
  /// Absent when it queued none, so a change that only moved a floor reads
  /// as before.
  std::optional<SettleReport> settle;
  bool operator==(const IngestOutcome &) const = default;
};

[[nodiscard]] OutcomeClass outcomeClassOf(std::string_view code) noexcept;

[[nodiscard]] std::string_view
epistemicRefusalName(EpistemicRefusal refusal) noexcept;

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_INGRESS_HPP
