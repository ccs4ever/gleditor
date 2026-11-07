/**
 * @file epistemic_limits.hpp
 * @brief Shared P19 resource defaults and checked host configuration.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP

#include "common/xanadu/zigzag/manifold.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

namespace xanadu::vagent::runtime {

inline constexpr std::int64_t kDefaultEpistemicMaxSources    = 128;
inline constexpr std::int64_t kDefaultEpistemicMaxCandidates = 1024;
// A query frame nests record, list and field levels; 32 leaves room for a
// candidate list of scored records inside a result without admitting the
// unbounded recursion a hostile descriptor could request.
inline constexpr std::int64_t kDefaultEpistemicScopeDepth = 32;
// A query may call a ranked sub-query inside its callback; eight levels bound
// the saved caller continuations an actor can hold at once.
inline constexpr std::int64_t kDefaultEpistemicNestedScopes = 8;
// Checked ingestion (I3-1 design section 9). A record costs about 80 cells,
// so these defaults hold about 18 fully active principals in the cell budget.
inline constexpr std::int64_t kDefaultEpistemicHistory               = 4;
inline constexpr std::int64_t kDefaultEpistemicDedup                 = 16;
inline constexpr std::int64_t kDefaultEpistemicKeysPerSource         = 4;
inline constexpr std::int64_t kDefaultEpistemicIngressQueue          = 64;
inline constexpr std::int64_t kDefaultEpistemicIngressQueuePerSource = 8;
inline constexpr std::int64_t kDefaultEpistemicOutcomeLog            = 64;
/// A checkpoint encodes about two operations per epistemic cell, so this
/// stays under vagent.value_operations (65536) with the rest of the agent:
/// measured, 24k cells checkpoint and 32k do not.
inline constexpr std::int64_t kDefaultEpistemicCells = 24576;
/// About 20 ms of ingestion per wake at the measured 4.3k cycles a turn, so
/// a flooded actor cannot hold a shared pump for long.
inline constexpr std::int64_t kDefaultEpistemicIngestPerWake = 4;
/// Arena slots past which ingestion refuses ArenaExhausted. Slots are never
/// reused (excision is logical), so this bounds an actor's lifetime between
/// checkpoints: at about 250 slots an ingestion, 2^30 slots is about 50 days
/// at 1 Hz in aggregate. Half the ref space, below the ephemeral bit.
inline constexpr std::int64_t kDefaultEpistemicArenaSlots = std::int64_t{1}
                                                            << 30;

// Source health (I3-2 design section 3.3). The half-life is in counted events,
// not seconds: a slow source keeps its fraction through silence.
inline constexpr std::int64_t kDefaultEpistemicHealthHalfLife = 32;
inline constexpr std::int64_t kMaxEpistemicHealthHalfLife     = 1'000'000;
inline constexpr double kDefaultEpistemicSuspectFraction      = 0.25;
// The weight N at which the fraction starts to decide: N is 7.43 after eight
// events at the default half-life, so the ninth reading is the first that can
// read SUSPECT.
inline constexpr std::int64_t kDefaultEpistemicSuspectMinEvents = 8;
// As many refusals as the per-source queue share can hold at once.
inline constexpr std::int64_t kDefaultEpistemicHealthReplayRing  = 8;
inline constexpr std::int64_t kMaxEpistemicHealthReplayRing      = 4096;
inline constexpr std::int64_t kDefaultEpistemicHealthSequenceGap = 1024;
inline constexpr std::int64_t kMaxEpistemicHealthSequenceGap =
    std::numeric_limits<std::int32_t>::max();
// Health actions and queue bounds (2c1).
inline constexpr std::int64_t kDefaultEpistemicHealthPending         = 16;
inline constexpr std::int64_t kMaxEpistemicHealthPending             = 4096;
inline constexpr std::int64_t kDefaultEpistemicHealthPendingPerGrant = 2;
inline constexpr std::int64_t kDefaultEpistemicControlPerWake        = 32;
inline constexpr std::int64_t kMaxEpistemicControlPerWake = 1'000'000;
// Reasons and audit entries a profile keeps (2c2). One reason slot is always
// left for evidence-driven reasons, so the floor is 2: an operator then holds
// at least one.
inline constexpr std::int64_t kDefaultEpistemicHealthReasons = 3;
inline constexpr std::int64_t kMinEpistemicHealthReasons     = 2;
inline constexpr std::int64_t kMaxEpistemicHealthReasons     = 16;
inline constexpr std::int64_t kDefaultEpistemicHealthActions = 4;

/// The default pause (P19 T1b2) outlasts an ordinary drain, and a pause that
/// is forgotten lapses by itself: 1 s to 10 min bounds both mistakes.
inline constexpr std::int64_t kDefaultAdmissionPauseMs = 30'000;
inline constexpr std::int64_t kMinAdmissionPauseMs     = 1'000;
inline constexpr std::int64_t kMaxAdmissionPauseMs     = 600'000;
/// `upgradeQuiescing` pauses for its timeout plus this, so the swap after a
/// drain that ends on the timeout's last tick still finds admission paused.
/// One second: the swap is one pump of the host thread, far shorter.
inline constexpr std::int64_t kQuiescingPauseMarginMs    = 1'000;
inline constexpr std::int64_t kMinEpistemicHealthActions = 2;
inline constexpr std::int64_t kMaxEpistemicHealthActions = 64;
// `validate()` caps min events at this share of the estimator's weight bound
// 1/(1-b): a larger value would disable SUSPECT without a trace.
inline constexpr double kSuspectMinEventsWeightShare = 0.9;

// Wire bounds on one checked observation, checked before anything is
// stored: an event ID is an identifier and a name is a name, not a payload.
inline constexpr std::size_t kMaxEventIdBytes           = 256;
inline constexpr std::size_t kMaxObservationNameBytes   = 256;
inline constexpr std::size_t kMaxObservationSignedBytes = 64 * 1024;

// Signed ingress (P19 I3-3a, `i3-3-design.md` 9.1). Here rather than beside
// the adapter so `defaultSettingSpecs()` can name them without the adapter.
/// `vagent.epistemic_signed_message_bytes`: an observation is under 1 KiB, and
/// the ceiling is the bound `observe` already holds signed bytes to.
inline constexpr std::size_t kDefaultSignedMessageBytes = 4096;
inline constexpr std::size_t kMinSignedMessageBytes     = 1024;
inline constexpr std::size_t kMaxSignedMessageBytes =
    kMaxObservationSignedBytes;
/// `vagent.epistemic_delegation_cache`: validated chains kept; two sources
/// per slot of the default `max_sources`.
inline constexpr std::size_t kDefaultDelegationCache = 256;
inline constexpr std::size_t kMinDelegationCache     = 16;
inline constexpr std::size_t kMaxDelegationCache     = 4096;
/// `vagent.epistemic_verify_per_peer`: verifications a second one transport
/// peer may cost. Measured at 133 us a verify (I3-3a1), 500 a second is
/// about 7% of one core; the design's 2,000 was priced on a 60 us estimate
/// and would let one peer take about 27%.
inline constexpr std::int64_t kDefaultVerifyPerPeer = 500;
inline constexpr std::int64_t kMinVerifyPerPeer     = 10;
inline constexpr std::int64_t kMaxVerifyPerPeer     = 100'000;
/// `vagent.epistemic_wall_rollback_ms`: larger than an NTP slew step, smaller
/// than any certificate window.
inline constexpr std::int64_t kDefaultWallRollbackMs = 1000;
inline constexpr std::int64_t kMinWallRollbackMs     = 10;
inline constexpr std::int64_t kMaxWallRollbackMs     = 3'600'000;
/// `vagent.epistemic_incarnation_step`: the largest incarnation advance a
/// `signed` binding takes from a message, a thousand unobserved reboots.
inline constexpr std::int64_t kDefaultIncarnationStep = 1024;
inline constexpr std::int64_t kMinIncarnationStep     = 1;
inline constexpr std::int64_t kMaxIncarnationStep     = std::int64_t{1} << 31;

// The time-class diagnostic and the shared-time-source watch (I3-3b1, design
// 4.3 and 9.1). Host memory only: none of these reaches a decision.
/// `vagent.epistemic_time_class_half_life`: readings over which a source's
/// time-class fraction halves its memory; the health half-life, so the two
/// read alike.
inline constexpr std::int64_t kDefaultTimeClassHalfLife = 32;
inline constexpr std::int64_t kMinTimeClassHalfLife     = 8;
inline constexpr std::int64_t kMaxTimeClassHalfLife     = 4096;
/// `vagent.epistemic_time_suspect_fraction`: the I3-2 suspect fraction.
inline constexpr double kDefaultTimeSuspectFraction = 0.25;
inline constexpr double kMinTimeSuspectFraction     = 0.01;
inline constexpr double kMaxTimeSuspectFraction     = 1.0;
/// `vagent.epistemic_common_cause_sources`: three agreeing sources point to a
/// cause the sources share rather than to one device.
inline constexpr std::int64_t kDefaultCommonCauseSources = 3;
inline constexpr std::int64_t kMinCommonCauseSources     = 1;
inline constexpr std::int64_t kMaxCommonCauseSources     = 128;
/// `vagent.epistemic_common_cause_window_ms`: several heartbeats at typical
/// rates.
inline constexpr std::int64_t kDefaultCommonCauseWindowMs = 10'000;
inline constexpr std::int64_t kMinCommonCauseWindowMs     = 100;
inline constexpr std::int64_t kMaxCommonCauseWindowMs     = 3'600'000;

enum class EpistemicLimitError : std::uint8_t {
  InvalidSources,
  InvalidCandidates,
  InvalidScopeDepth,
  InvalidNestedScopes,
  InvalidIngestionBound,
  InvalidHealthHalfLife,
  InvalidSuspectFraction,
  InvalidSuspectMinEvents,
  InvalidHealthReplayRing,
  InvalidHealthSequenceGap,
  InvalidHealthPending,
  InvalidHealthPendingPerGrant,
  InvalidControlPerWake,
  InvalidHealthReasons,
  InvalidHealthActions,
  InvalidTimeClassHalfLife,
  InvalidTimeSuspectFraction,
  InvalidCommonCauseSources,
  InvalidCommonCauseWindow,
};

[[nodiscard]] constexpr std::string_view
epistemicLimitErrorName(EpistemicLimitError error) noexcept {
  switch (error) {
  case EpistemicLimitError::InvalidSources:
    return "InvalidSources";
  case EpistemicLimitError::InvalidCandidates:
    return "InvalidCandidates";
  case EpistemicLimitError::InvalidScopeDepth:
    return "InvalidScopeDepth";
  case EpistemicLimitError::InvalidNestedScopes:
    return "InvalidNestedScopes";
  case EpistemicLimitError::InvalidIngestionBound:
    return "InvalidIngestionBound";
  case EpistemicLimitError::InvalidHealthHalfLife:
    return "InvalidHealthHalfLife";
  case EpistemicLimitError::InvalidSuspectFraction:
    return "InvalidSuspectFraction";
  case EpistemicLimitError::InvalidSuspectMinEvents:
    return "InvalidSuspectMinEvents";
  case EpistemicLimitError::InvalidHealthReplayRing:
    return "InvalidHealthReplayRing";
  case EpistemicLimitError::InvalidHealthSequenceGap:
    return "InvalidHealthSequenceGap";
  case EpistemicLimitError::InvalidHealthPending:
    return "InvalidHealthPending";
  case EpistemicLimitError::InvalidHealthPendingPerGrant:
    return "InvalidHealthPendingPerGrant";
  case EpistemicLimitError::InvalidControlPerWake:
    return "InvalidControlPerWake";
  case EpistemicLimitError::InvalidHealthReasons:
    return "InvalidHealthReasons";
  case EpistemicLimitError::InvalidHealthActions:
    return "InvalidHealthActions";
  case EpistemicLimitError::InvalidTimeClassHalfLife:
    return "InvalidTimeClassHalfLife";
  case EpistemicLimitError::InvalidTimeSuspectFraction:
    return "InvalidTimeSuspectFraction";
  case EpistemicLimitError::InvalidCommonCauseSources:
    return "InvalidCommonCauseSources";
  case EpistemicLimitError::InvalidCommonCauseWindow:
    return "InvalidCommonCauseWindow";
  }
  return "Unknown";
}

/// Terms of the `expm1` series of I4-D1 3.3: on `|y| <= 1/2` the first
/// omitted term, `y^19 / 19!`, is below `1.6e-23`.
inline constexpr std::size_t kExpm1SeriesOrder = 18;

/// `1/k!` for `k <= 18` as the nearest doubles: `k!` is exact in a double up
/// to `18!` (below `2^53`), so each quotient is one correctly rounded division,
/// the same bits the reference generator's `float(1/k!)` holds.
inline constexpr auto kExpm1SeriesCoefficients = [] {
  std::array<double, kExpm1SeriesOrder + 1> c{};
  double factorial = 1.0;
  for (std::size_t k = 0; k <= kExpm1SeriesOrder; ++k) {
    if (k > 0) factorial *= static_cast<double>(k);
    c.at(k) = 1.0 / factorial;
  }
  return c;
}();

/// `expm1(y)` for `|y| <= 1/2` by the Horner form of I4-D1 3.3,
/// `y (1 + y/2! + ... + y^17/18!)`. It calls no libm, so it is the same bits on
/// every host (T0); the emitted routine runs the same `Mul`, `Add` sequence.
/// The product and the sum are separate statements so that a compiler allowed
/// to contract only within an expression cannot fuse them into an FMA, which
/// would change the bits the IR (separate opcodes) produces.
[[nodiscard]] inline double expm1Series(double y) {
  double acc = kExpm1SeriesCoefficients[kExpm1SeriesOrder];
  for (std::size_t k = kExpm1SeriesOrder - 1; k >= 1; --k) {
    const double product = y * acc;
    acc                  = kExpm1SeriesCoefficients.at(k) + product;
  }
  return y * acc;
}

/// The per-event decay `b = 2^(-1/H)` and its complement `1 - b`.
struct HealthDecay {
  double b;
  double oneMinusB;
};

/// `b` and `1 - b` as the IR computes them (I4-D1 3.9): `x = ln2 / H <= 0.35`
/// for `H >= 2`, so `b = 1 + expm1(-x)` comes from the series and `1 - b` is
/// `-expm1(-x)`, never the subtraction of nearly equal numbers that moved the
/// `min_events` domain bound by one across hosts at large `H` (NUM m3).
/// `H = 1` is `b = 1/2` exactly. The host and the routine agree bit for bit.
[[nodiscard]] inline HealthDecay healthDecayPair(std::int64_t halfLife) {
  if (halfLife == 1) return {.b = 0.5, .oneMinusB = 0.5};
  const double em = expm1Series(
      -(std::numbers::ln2_v<double> / static_cast<double>(halfLife)));
  return {.b = 1.0 + em, .oneMinusB = -em};
}

/// The per-event decay `b = 2^(-1/H)`.
[[nodiscard]] inline double healthDecay(std::int64_t halfLife) {
  return healthDecayPair(halfLife).b;
}

/// The largest `N` the estimator can reach, `1 / (1 - b)`.
[[nodiscard]] inline double healthWeightCap(std::int64_t halfLife) {
  return 1.0 / healthDecayPair(halfLife).oneMinusB;
}

/// The settle reserve is one 64th of the slot allowance.
inline constexpr std::size_t kSlotReserveDivisor = 64;

/// Exempt entries (isolate, clear, reset, settle; never a degrade, even on a
/// tombstone) may take the cell ceiling this fraction past it: a repair must
/// land on an arena that ingestion has filled.
inline constexpr std::size_t kHeadroomDivisor = 16;

/// Cells a live profile costs before its records and its health reserve: the
/// 44 measured in I3-1 plus the two health weights.
inline constexpr std::size_t kProfileBaseCells = 48;

/// Logical admission bounds; actor cell, byte and fuel grants remain
/// independent.
struct EpistemicLimits {
  std::size_t maxSources{static_cast<std::size_t>(kDefaultEpistemicMaxSources)};
  std::size_t maxCandidates{
      static_cast<std::size_t>(kDefaultEpistemicMaxCandidates)};
  /// Schema and value nesting a scoped query may admit.
  std::size_t maxScopeDepth{
      static_cast<std::size_t>(kDefaultEpistemicScopeDepth)};
  /// Scoped evaluations one actor may have open at once, nesting included.
  std::size_t maxNestedScopes{
      static_cast<std::size_t>(kDefaultEpistemicNestedScopes)};
  /// `vagent.epistemic_history`: records retained per (principal, key) support;
  /// current and pinned records are never excised.
  std::size_t history{static_cast<std::size_t>(kDefaultEpistemicHistory)};
  /// `vagent.epistemic_dedup`: (incarnation, event ID, digest) entries per
  /// source profile; the replay horizon for exact retries.
  std::size_t dedup{static_cast<std::size_t>(kDefaultEpistemicDedup)};
  /// `vagent.epistemic_keys_per_source`: keys one source principal may support
  /// on one predicate.
  std::size_t keysPerSource{
      static_cast<std::size_t>(kDefaultEpistemicKeysPerSource)};
  /// `vagent.epistemic_ingress_queue`: checked observations queued for one
  /// actor.
  std::size_t ingressQueue{
      static_cast<std::size_t>(kDefaultEpistemicIngressQueue)};
  /// `vagent.epistemic_ingress_queue_per_source`: queued observations one
  /// source principal may hold.
  std::size_t ingressQueuePerSource{
      static_cast<std::size_t>(kDefaultEpistemicIngressQueuePerSource)};
  /// `vagent.epistemic_outcome_log`: ingestion outcomes the host log keeps per
  /// source of an actor, so no source's flood evicts another's.
  std::size_t outcomeLog{static_cast<std::size_t>(kDefaultEpistemicOutcomeLog)};
  /// `vagent.epistemic_cells`: maintained epistemic cell ceiling, kept below
  /// vagent.value_operations so a full budget still checkpoints.
  std::size_t cells{static_cast<std::size_t>(kDefaultEpistemicCells)};
  /// `vagent.epistemic_ingest_per_wake`: ingestion turns one wake runs before
  /// the pump moves on to other actors.
  std::size_t ingestPerWake{
      static_cast<std::size_t>(kDefaultEpistemicIngestPerWake)};
  /// `vagent.epistemic_arena_slots`: the actor's arena slot allowance.
  std::size_t arenaSlots{static_cast<std::size_t>(kDefaultEpistemicArenaSlots)};
  /// `vagent.epistemic_health_half_life`: counted events over which a source's
  /// refusal fraction halves its memory. Appended after `arenaSlots`:
  /// `checkedEpistemicLimits` initialises the leading fields positionally.
  std::int64_t healthHalfLife{kDefaultEpistemicHealthHalfLife};
  /// `vagent.epistemic_suspect_fraction`: the fraction of counted outcomes
  /// that were unusable at or above which a source reads SUSPECT.
  double suspectFraction{kDefaultEpistemicSuspectFraction};
  /// `vagent.epistemic_suspect_min_events`: the estimator weight below which
  /// the fraction does not decide.
  std::int64_t suspectMinEvents{kDefaultEpistemicSuspectMinEvents};
  /// `vagent.epistemic_health_replay_ring`: counted refusal sequences kept per
  /// stream above its high-water mark, so a replayed refusal counts once.
  std::int64_t healthReplayRing{kDefaultEpistemicHealthReplayRing};
  /// `vagent.epistemic_health_sequence_gap`: how far above its mark a dropped
  /// reading's sequence may sit and still be credited.
  std::int64_t healthSequenceGap{kDefaultEpistemicHealthSequenceGap};
  /// `vagent.epistemic_health_pending`: health actions queued per actor.
  std::int64_t healthPending{kDefaultEpistemicHealthPending};
  /// `vagent.epistemic_health_pending_per_grant`: health actions queued per
  /// grant on one actor.
  std::int64_t healthPendingPerGrant{kDefaultEpistemicHealthPendingPerGrant};
  /// `vagent.epistemic_control_per_wake`: control turns one wake may run before
  /// yielding to other actors.
  std::int64_t controlPerWake{kDefaultEpistemicControlPerWake};
  /// `vagent.epistemic_health_reasons`: live reasons a profile keeps.
  std::int64_t healthReasons{kDefaultEpistemicHealthReasons};
  /// `vagent.epistemic_health_actions`: audit entries a profile keeps.
  std::int64_t healthActions{kDefaultEpistemicHealthActions};
  /// `vagent.epistemic_time_class_half_life`: readings over which a source's
  /// time-class fraction halves its memory.
  std::int64_t timeClassHalfLife{kDefaultTimeClassHalfLife};
  /// `vagent.epistemic_time_suspect_fraction`: the time-class fraction at or
  /// above which the host logs its warning.
  double timeSuspectFraction{kDefaultTimeSuspectFraction};
  /// `vagent.epistemic_common_cause_sources`: sources that agreeing means a
  /// shared cause.
  std::int64_t commonCauseSources{kDefaultCommonCauseSources};
  /// `vagent.epistemic_common_cause_window_ms`: the window those sources
  /// must fall in.
  std::int64_t commonCauseWindowMs{kDefaultCommonCauseWindowMs};

  /// Slots past `arenaSlots` that only settle turns may use, so a batch of
  /// belief events still settles when the allowance is spent. Zero for a tiny
  /// allowance, and clamped to the room below the arena's ref space: a ref
  /// with a dense index past `ephemeralBit` aliases a lower slot silently, so
  /// a large setting must not let a turn start near that limit.
  [[nodiscard]] std::size_t slotReserve() const noexcept {
    constexpr std::size_t refSpace = zigzag::ephemeralBit;
    const std::size_t room = arenaSlots < refSpace ? refSpace - arenaSlots : 0;
    return std::min(arenaSlots / kSlotReserveDivisor, room);
  }

  /// True when `slotReserve()` is smaller than the divisor rule gives, so the
  /// host can say once that the allowance is too close to the ref space.
  [[nodiscard]] bool slotReserveClamped() const noexcept {
    return slotReserve() < arenaSlots / kSlotReserveDivisor;
  }

  [[nodiscard]] std::expected<void, EpistemicLimitError> validate() const {
    if (maxSources == 0)
      return std::unexpected(EpistemicLimitError::InvalidSources);
    if (maxCandidates == 0)
      return std::unexpected(EpistemicLimitError::InvalidCandidates);
    if (maxScopeDepth == 0)
      return std::unexpected(EpistemicLimitError::InvalidScopeDepth);
    if (maxNestedScopes == 0)
      return std::unexpected(EpistemicLimitError::InvalidNestedScopes);
    for (const auto bound :
         {history, dedup, keysPerSource, ingressQueue, ingressQueuePerSource,
          outcomeLog, cells, ingestPerWake, arenaSlots})
      if (bound == 0)
        return std::unexpected(EpistemicLimitError::InvalidIngestionBound);
    if (ingressQueuePerSource > ingressQueue)
      return std::unexpected(EpistemicLimitError::InvalidIngestionBound);
    if (healthHalfLife < 1 || healthHalfLife > kMaxEpistemicHealthHalfLife)
      return std::unexpected(EpistemicLimitError::InvalidHealthHalfLife);
    if (!std::isfinite(suspectFraction) || suspectFraction <= 0.0 ||
        suspectFraction > 1.0)
      return std::unexpected(EpistemicLimitError::InvalidSuspectFraction);
    // The relation H >= ingress_queue_per_source is deliberately not checked:
    // a legitimate configuration can break it, and it only sizes an order
    // error (design 3.2), so construction warns instead (R3-2).
    if (suspectMinEvents < 1 || static_cast<double>(suspectMinEvents) >
                                    std::floor(kSuspectMinEventsWeightShare *
                                               healthWeightCap(healthHalfLife)))
      return std::unexpected(EpistemicLimitError::InvalidSuspectMinEvents);
    if (healthReplayRing < 1 ||
        healthReplayRing > kMaxEpistemicHealthReplayRing)
      return std::unexpected(EpistemicLimitError::InvalidHealthReplayRing);
    if (healthSequenceGap < 1 ||
        healthSequenceGap > kMaxEpistemicHealthSequenceGap)
      return std::unexpected(EpistemicLimitError::InvalidHealthSequenceGap);
    if (healthPending < 1 || healthPending > kMaxEpistemicHealthPending)
      return std::unexpected(EpistemicLimitError::InvalidHealthPending);
    if (healthPendingPerGrant < 1 || healthPendingPerGrant > healthPending)
      return std::unexpected(EpistemicLimitError::InvalidHealthPendingPerGrant);
    if (controlPerWake < 1 || controlPerWake > kMaxEpistemicControlPerWake)
      return std::unexpected(EpistemicLimitError::InvalidControlPerWake);
    if (healthReasons < kMinEpistemicHealthReasons ||
        healthReasons > kMaxEpistemicHealthReasons)
      return std::unexpected(EpistemicLimitError::InvalidHealthReasons);
    if (healthActions < kMinEpistemicHealthActions ||
        healthActions > kMaxEpistemicHealthActions)
      return std::unexpected(EpistemicLimitError::InvalidHealthActions);
    if (timeClassHalfLife < kMinTimeClassHalfLife ||
        timeClassHalfLife > kMaxTimeClassHalfLife)
      return std::unexpected(EpistemicLimitError::InvalidTimeClassHalfLife);
    if (!std::isfinite(timeSuspectFraction) ||
        timeSuspectFraction < kMinTimeSuspectFraction ||
        timeSuspectFraction > kMaxTimeSuspectFraction)
      return std::unexpected(EpistemicLimitError::InvalidTimeSuspectFraction);
    if (commonCauseSources < kMinCommonCauseSources ||
        commonCauseSources > kMaxCommonCauseSources)
      return std::unexpected(EpistemicLimitError::InvalidCommonCauseSources);
    if (commonCauseWindowMs < kMinCommonCauseWindowMs ||
        commonCauseWindowMs > kMaxCommonCauseWindowMs)
      return std::unexpected(EpistemicLimitError::InvalidCommonCauseWindow);
    return {};
  }

  bool operator==(const EpistemicLimits &) const = default;
};

/// Validate signed system-setting values before converting them to allocation
/// bounds.
[[nodiscard]] inline std::expected<EpistemicLimits, EpistemicLimitError>
checkedEpistemicLimits(std::int64_t sources, std::int64_t candidates) {
  if (sources <= 0 || !std::in_range<std::size_t>(sources))
    return std::unexpected(EpistemicLimitError::InvalidSources);
  if (candidates <= 0 || !std::in_range<std::size_t>(candidates))
    return std::unexpected(EpistemicLimitError::InvalidCandidates);
  return EpistemicLimits{static_cast<std::size_t>(sources),
                         static_cast<std::size_t>(candidates)};
}

[[nodiscard]] inline std::expected<EpistemicLimits, EpistemicLimitError>
checkedEpistemicLimits(std::int64_t sources, std::int64_t candidates,
                       std::int64_t scopeDepth, std::int64_t nestedScopes) {
  auto limits = checkedEpistemicLimits(sources, candidates);
  if (!limits) return limits;
  if (scopeDepth <= 0 || !std::in_range<std::size_t>(scopeDepth))
    return std::unexpected(EpistemicLimitError::InvalidScopeDepth);
  if (nestedScopes <= 0 || !std::in_range<std::size_t>(nestedScopes))
    return std::unexpected(EpistemicLimitError::InvalidNestedScopes);
  limits->maxScopeDepth   = static_cast<std::size_t>(scopeDepth);
  limits->maxNestedScopes = static_cast<std::size_t>(nestedScopes);
  return limits;
}

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP
