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

// The code a latched profile reads. The fault's own code lives on its
// `isolate` audit entry, found by `fault_seq`.
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

// Profile fields of the health turn (P19 I3-2c2), all absent until first
// written and read as 0 or empty then.
inline constexpr std::string_view kProfileResetEpoch = "reset_epoch";
inline constexpr std::string_view kProfileActionSeq  = "action_seq";
/// The `seq` of the `isolate` entry that latched the profile; absent or 0 when
/// the profile is not latched.
inline constexpr std::string_view kProfileFaultSeq = "fault_seq";
inline constexpr std::string_view kProfileReasons  = "reasons";
/// The cells the health structure may occupy at its current population
/// (design 6.6): `kHealthFixedCells` plus a worst-case entry for each reason
/// and action. Worst case, not measured, so the admission reserve
/// `healthBound - health_cells` never undercounts what an entry can grow to.
inline constexpr std::string_view kProfileHealthCells = "health_cells";
inline constexpr std::string_view kProfileActions     = "actions";

// Cells of the health structure (design 6.1): scalars and list headers, and a
// worst-case reason and action entry. Pinned by a test that counts a full
// structure against them.
inline constexpr std::int64_t kHealthFixedCells = 14;
inline constexpr std::int64_t kReasonCells      = 16;
inline constexpr std::int64_t kActionCells      = 20;

/// The most cells a profile's health structure may reach under the limits.
[[nodiscard]] constexpr std::int64_t
healthBound(std::int64_t reasons, std::int64_t actions) noexcept {
  return kHealthFixedCells + kReasonCells * reasons + kActionCells * actions;
}

// Fields of one `reasons` entry and one `actions` entry. `wall` is present
// only when the executor's wall clock was synchronised; `removed` only on a
// `clear` or `reset` entry.
inline constexpr std::string_view kReasonKind           = "kind";
inline constexpr std::string_view kReasonCode           = "code";
inline constexpr std::string_view kReasonOrigin         = "origin";
inline constexpr std::string_view kReasonGrant          = "grant";
inline constexpr std::string_view kReasonIssuer         = "issuer";
inline constexpr std::string_view kReasonIncarnation    = "incarnation";
inline constexpr std::string_view kReasonWall           = "wall";
inline constexpr std::string_view kActionSeq            = "seq";
inline constexpr std::string_view kActionCount          = "count";
inline constexpr std::string_view kActionRemoved        = "removed";
inline constexpr std::string_view kReasonKindDegraded   = "degraded";
inline constexpr std::string_view kReasonKindSuspect    = "suspect";
inline constexpr std::string_view kReasonOriginOperator = "operator";
inline constexpr std::string_view kReasonOriginDevice   = "device";

/// Named field appended to a record after the positional ones, by whatever
/// retired it. Absent means superseded by a newer reading.
inline constexpr std::string_view kObsRetiredBy     = "retired_by";
inline constexpr std::string_view kRetiredByAdvance = "advance";
inline constexpr std::string_view kRetiredByRetire  = "retire";
inline constexpr std::string_view kRetiredByIsolate = "isolate";
inline constexpr std::string_view kRetiredByReset   = "reset";

inline constexpr std::string_view kTurnResultSourceFaulted = "SourceFaulted";
inline constexpr std::string_view kTurnResultStaleHealthState =
    "StaleHealthState";
inline constexpr std::string_view kTurnResultNotOperatorReason =
    "NotOperatorReason";
inline constexpr std::string_view kTurnResultHealthReasonsFull =
    "HealthReasonsFull";
inline constexpr std::string_view kTurnResultNoChange = "NoChange";

inline constexpr std::string_view kStatAuditCalls  = "health.audit.calls";
inline constexpr std::string_view kStatAuditThrows = "health.audit.throws";

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_HEALTH_HPP
