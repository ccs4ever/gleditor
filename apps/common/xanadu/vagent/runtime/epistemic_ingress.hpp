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

/// Queued, never committed: the ingestion turn decides that later.
struct TransportReceipt {
  std::string eventId;
};

enum class EpistemicRefusal : std::uint8_t {
  UnknownAgent,
  NotArbitrated,
  InvalidObservation,
};

[[nodiscard]] std::string_view
epistemicRefusalName(EpistemicRefusal refusal) noexcept;

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_INGRESS_HPP
