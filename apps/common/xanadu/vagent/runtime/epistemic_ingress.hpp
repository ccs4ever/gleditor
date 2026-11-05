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
  /// Mapped seconds on the policy's clock basis.
  double acquiredAt{};
  double acquiredUncertainty{};
  std::string clockBasis;
  std::uint64_t clockEpoch{};
  std::uint64_t clockMappingVersion{};
  std::string frame;
  std::string units;
  /// Full scalar isotropic variance in squared units, continuous only.
  std::optional<double> variance;
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
  ObservationTimeMismatch,
  /// A claimed acquisition uncertainty above the policy's max_uncertainty:
  /// the reading cannot be placed in time closely enough to be used.
  UncertainAcquisition,
  StaleObservation,
  ObservationFrameMismatch,
  MissingVariance,
  InvalidVariance,
  IngressBackpressure,
  /// A tuple that is mistyped or carries a non-finite number: decided on the
  /// origin's signed fields, so it counts for source health even relayed.
  InvalidValue,
  /// A non-finite `acquiredAt` or uncertainty, or a negative uncertainty:
  /// decided on host-mapped fields, so only a direct copy counts.
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
