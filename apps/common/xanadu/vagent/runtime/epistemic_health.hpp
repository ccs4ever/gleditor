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
#include <vector>

namespace xanadu::vagent::runtime {

enum class HealthState : std::uint8_t { Nominal, Degraded, Suspect, Faulted };

/// One live reason as the arena holds it (2c2).
struct HealthReason {
  std::string kind;
  std::string code;
  std::string origin;
  bool operator==(const HealthReason &) const = default;
};

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
  /// (design 3.1).
  std::uint64_t uncountedSinceCommit{};
  /// Host memory, diagnostic only (design 4.3): the share of this source's
  /// time-classified readings the device held or the clocks disagreed on
  /// (rows 4, 7, 8). Never a reason and never part of `refusalFraction`; absent
  /// until the source has been classified once.
  std::optional<double> timeClassFraction;
  /// Host memory: readings the latch cache refused since the last reset.
  std::uint64_t refusedWhileFaulted{};
  /// Durable arena state: how many resets the profile has had.
  std::uint64_t resetEpoch{0};
  /// The live reasons, oldest first.
  std::vector<HealthReason> reasons;
  /// The `seq` of the isolate entry holding the latch; 0 when not latched.
  std::uint64_t faultSeq{0};
  /// What the isolate entry holding the latch recorded; empty when not
  /// latched. A reset's `removed` text quotes them.
  std::string faultCode;
  std::string faultIssuer;
  std::string faultGrant;
  std::uint64_t faultIncarnation{0};
  /// Whether `last_receive` or `last_contact` is present.
  bool livenessPresent{false};
  /// Host audit sink call and throw counts for the actor (R3-10).
  std::uint64_t auditCalls{0};
  std::uint64_t auditThrows{0};
};

/// Bumped when the set of codes below changes.
inline constexpr int kHealthRefusalCodesVersion = 2;

/// One refusal that moves the estimator, and whether it still counts when a
/// relay delivered the copy. A relay can alter only what the origin did not
/// sign, so a relayed copy counts only for refusals decided on signed fields.
struct HealthRefusalCode {
  std::string_view code;
  bool countsWhenRelayed;
};

///
/// Version 2 (design 4.3): every counted code is decided on signed bytes at
/// pinned sites (`tools/check-counted-sites.py`), so none depends on a relay's
/// honesty and the relayed attenuation of version 1 is gone.
inline constexpr std::array<HealthRefusalCode, 8> kHealthRefusalCodes{{
    {"ObservationFrameMismatch", true},
    {"MissingVariance", true},
    {"InvalidVariance", true},
    {"InvalidValue", true},
    {"InvalidAcquisition", true},
    {"ClockEpochRegression", true},
    {"ObservationTimeMismatch", true},
    {"UncertainAcquisition", true},
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
