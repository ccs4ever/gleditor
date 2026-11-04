/**
 * @file epistemic_abi_health.hpp
 * @brief Names the source-health estimator shares between the executor and
 *        the emitted routines (P19 I3-2 slice 2a). Each is pinned by the ABI
 *        test, so a rename is a test failure and not a silent drift.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_HEALTH_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_HEALTH_HPP

#include <string_view>

namespace xanadu::vagent::runtime {

// Profile fields: the exponentially weighted refusal weight `R` and the total
// weight `N`, both Float, absent until first written and read as 0 then.
inline constexpr std::string_view kProfileHealthRefusedWeight =
    "health_refused_weight";
inline constexpr std::string_view kProfileHealthWeight = "health_weight";

// Derived states besides NOMINAL and FAULTED (runtime_abi.hpp). The state is
// derived, never stored on the profile; a record's `health` field is a stamp.
inline constexpr std::string_view kObsHealthSuspect  = "SUSPECT";
inline constexpr std::string_view kObsHealthDegraded = "DEGRADED";
/// The refusal fraction `f` after the ingestion's own update, one position
/// after `health` in the record.
inline constexpr std::string_view kObsHealthFraction = "health_fraction";

// The codes that decide a derived state. A latched profile reads `faulted`
// until slice 2c2 gives the latch a code of its own.
inline constexpr std::string_view kHealthCodeFaulted  = "faulted";
inline constexpr std::string_view kHealthCodeFraction = "refusal_fraction";

// `$turn.refusal` note fields besides `content`, `other` and `code`: the
// counted health refusals, the counted pass credits, and the half-life the
// update runs under.
inline constexpr std::string_view kRefusalHealth   = "health";
inline constexpr std::string_view kRefusalPassed   = "passed";
inline constexpr std::string_view kRefusalHalfLife = "half_life";

// `$turn.ingest` fields besides the four limits the routine already reads.
inline constexpr std::string_view kIngestHalfLife        = "half_life";
inline constexpr std::string_view kIngestSuspectFraction = "suspect_fraction";
inline constexpr std::string_view kIngestMinEvents       = "min_events";
/// Whether the estimator counts this reading (the host decided at `observe`).
inline constexpr std::string_view kIngestCounts = "counts";

// Turn results the epoch and mapping regressions now end with, in place of
// ObservationTimeMismatch.
inline constexpr std::string_view kTurnResultClockEpochRegression =
    "ClockEpochRegression";
inline constexpr std::string_view kTurnResultClockMappingRegression =
    "ClockMappingRegression";

// `$turn.health` (P19 I3-2c1) fields, action kinds, turn results and stats.
inline constexpr std::string_view kTurnHealth            = "health";
inline constexpr std::string_view kHealthTurnSource      = "source";
inline constexpr std::string_view kHealthTurnKind        = "kind";
inline constexpr std::string_view kHealthTurnCode        = "code";
inline constexpr std::string_view kHealthTurnGrant       = "grant";
inline constexpr std::string_view kHealthTurnIssuer      = "issuer";
inline constexpr std::string_view kHealthTurnWall        = "wall";
inline constexpr std::string_view kHealthTurnIncarnation = "incarnation";
inline constexpr std::string_view kHealthTurnRetained    = "retained";
inline constexpr std::string_view kHealthTurnMaxSources  = "max_sources";
inline constexpr std::string_view kHealthTurnReasons     = "reasons";
inline constexpr std::string_view kHealthTurnActions     = "actions";
inline constexpr std::string_view kHealthTurnSequenceGap = "sequence_gap";
inline constexpr std::string_view kHealthTurnExpectResetEpoch =
    "expect_reset_epoch";
inline constexpr std::string_view kHealthTurnFloors         = "floors";
inline constexpr std::string_view kHealthTurnRemoved        = "removed";
inline constexpr std::string_view kHealthTurnDroppedContent = "dropped_content";
inline constexpr std::string_view kHealthTurnDroppedOther   = "dropped_other";

inline constexpr std::string_view kHealthKindIsolate = "isolate";
inline constexpr std::string_view kHealthKindDegrade = "degrade";
inline constexpr std::string_view kHealthKindClear   = "clear";
inline constexpr std::string_view kHealthKindReset   = "reset";

inline constexpr std::string_view kTurnResultHealthUnavailable =
    "HealthUnavailable";
inline constexpr std::string_view kTurnResultSupersededByReset =
    "SupersededByReset";
inline constexpr std::string_view kTurnResultUnauthorizedHealthAction =
    "UnauthorizedHealthAction";

inline constexpr std::string_view kStatAuditCalls  = "health.audit.calls";
inline constexpr std::string_view kStatAuditThrows = "health.audit.throws";

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_HEALTH_HPP
