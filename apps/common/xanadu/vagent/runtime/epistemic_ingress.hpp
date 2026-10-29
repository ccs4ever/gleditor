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
  InvalidObservation,
  ClockUnsynchronised,
  ObservationTimeMismatch,
  StaleObservation,
  ObservationFrameMismatch,
  MissingVariance,
  InvalidVariance,
  IngressBackpressure,
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

/// One event's ingestion outcome as the host's outcome log records it.
struct IngestOutcome {
  enum class Kind : std::uint8_t { Pending, Committed, Duplicate, Refused };
  Kind kind{Kind::Pending};
  /// The refusal code, empty unless `kind` is `Refused`.
  std::string code;
  std::optional<OutcomeClass> outcomeClass;
  bool operator==(const IngestOutcome &) const = default;
};

[[nodiscard]] OutcomeClass outcomeClassOf(std::string_view code) noexcept;

[[nodiscard]] std::string_view
epistemicRefusalName(EpistemicRefusal refusal) noexcept;

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_INGRESS_HPP
