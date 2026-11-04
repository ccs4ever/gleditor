/**
 * @file epistemic_health.hpp
 * @brief Source health as the host sees it (P19 I3-2 slice 2a): the derived
 *        state, the read model, and the published list of refusals that move
 *        the estimator.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_HEALTH_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_HEALTH_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xanadu::vagent::runtime {

enum class HealthState : std::uint8_t { Nominal, Degraded, Suspect, Faulted };

/// A source profile as the arena derives it. The state comes from the same
/// rule the emitted `rtHealth_` applies; a view that disagrees with the stamp
/// the IR wrote is a test failure, not a second authority.
struct SourceHealth {
  std::string principal;
  HealthState state{HealthState::Nominal};
  /// What decided the state; empty for Nominal.
  std::optional<std::string> reasonCode;
  /// `f = R / N` from the rescaled weights, 0 while `N` is 0.
  double refusalFraction{0.0};
  /// The rescaled `N`.
  double weight{0.0};
  bool faulted{false};
  bool tombstone{false};
  std::uint64_t incarnation{};
  std::int64_t contentRefusals{};
  std::int64_t refusedCount{};
  std::int64_t rejectCount{};
  std::int64_t rejectionStreak{};
  std::optional<std::string> lastRefusal;
  /// Host memory: chargeable refusals the estimator did not count since this
  /// source last committed. The sign of a failure the estimator cannot see
  /// (design 3.1) until I3-3 reinstates the host-time class.
  std::uint64_t uncountedSinceCommit{};
  /// Durable arena state (2c2); 0 until then.
  std::uint64_t resetEpoch{0};
  /// Host audit sink call and throw counts for the actor (R3-10).
  std::uint64_t auditCalls{0};
  std::uint64_t auditThrows{0};
};

/// Bumped when the set of codes below changes.
inline constexpr int kHealthRefusalCodesVersion = 1;

/// One refusal that moves the estimator, and whether it still counts when a
/// relay delivered the copy. A relay can alter only what the origin did not
/// sign, so a relayed copy counts only for refusals decided on signed fields.
struct HealthRefusalCode {
  std::string_view code;
  bool countsWhenRelayed;
};

inline constexpr std::array<HealthRefusalCode, 6> kHealthRefusalCodes{{
    {"ObservationFrameMismatch", true},
    {"MissingVariance", true},
    {"InvalidVariance", true},
    {"InvalidValue", true},
    {"InvalidAcquisition", false},
    {"ClockEpochRegression", false},
}};

[[nodiscard]] constexpr bool isHealthRefusalCode(std::string_view code,
                                                 bool relayed) noexcept {
  for (const auto &entry : kHealthRefusalCodes)
    if (entry.code == code) return !relayed || entry.countsWhenRelayed;
  return false;
}

[[nodiscard]] constexpr std::string_view
healthStateName(HealthState state) noexcept {
  switch (state) {
  case HealthState::Nominal:
    return "NOMINAL";
  case HealthState::Degraded:
    return "DEGRADED";
  case HealthState::Suspect:
    return "SUSPECT";
  case HealthState::Faulted:
    return "FAULTED";
  }
  return "NOMINAL";
}

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_HEALTH_HPP
