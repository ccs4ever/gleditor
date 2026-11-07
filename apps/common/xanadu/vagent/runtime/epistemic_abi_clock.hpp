/**
 * @file epistemic_abi_clock.hpp
 * @brief Names the clock-mapping slice (P19 I3-3b1) shares between the
 *        executor, the emitted routines and the translator registry.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_CLOCK_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_CLOCK_HPP

#include <cstdint>
#include <string_view>

namespace xanadu::vagent::runtime {

// What the origin signed, in nanoseconds on its own clock, kept beside the
// mapped values so a later reader can tell a device's claim from the host's
// reading of it (design 4.1). `acquired_uncertainty` already holds u_a + u_d
// + u_M(a) in seconds, so `device_uncertainty` is u_d alone: the part of the
// uncertainty the device signed in the header and the record would otherwise
// lose. Written after the ordered fields: absent reads as absent.
inline constexpr std::string_view kObsAcquiredDevice    = "acquired_device";
inline constexpr std::string_view kObsSentDevice        = "sent_device";
inline constexpr std::string_view kObsDeviceUncertainty = "device_uncertainty";

/// A stream's `clock_instance`: the executor steady clock the stream's last
/// mapping version was issued under, so a restart that restarts the versions
/// is not read as a regression (A4).
inline constexpr std::string_view kStreamClockInstance = "clock_instance";

/// The support walk's second ordering site (A3): a mapped acquisition earlier
/// than the current record's beyond both uncertainties and the skew. Decided
/// on host-mapped times, so it is not counted.
inline constexpr std::string_view kTurnResultAcquisitionOrderMismatch =
    "AcquisitionOrderMismatch";

// The clock lineage (I3-3b2, design 4.4): one record on the agent's own
// `d.epistemic` rank holding the list `segments`, oldest first. A segment
// covers the steady times of its instance from `start_steady` to the next
// segment's start. `anchor_wall` and `anchor_sync` are absent while the host's
// wall clock was unsynchronised, never 0.
/// Parts per million as a factor, for the drift terms of the segment rule, the
/// rebase and the host's jump test, which all agree on it.
inline constexpr double kClockPartsPerMillion = 1e-6;

inline constexpr std::string_view kLineageRecordName  = "EpistemicClockLineage";
inline constexpr std::string_view kLineageSegments    = "segments";
inline constexpr std::string_view kLineageSegmentName = "ClockSegment";
inline constexpr std::string_view kSegInstance        = "instance";
inline constexpr std::string_view kSegStartSteady     = "start_steady";
inline constexpr std::string_view kSegAnchorSteady    = "anchor_steady";
inline constexpr std::string_view kSegAnchorWall      = "anchor_wall";
inline constexpr std::string_view kSegAnchorSync      = "anchor_sync";

/// `$turn.clock`: what the host read, in seconds, when a turn is to note the
/// segment rule: the executor's `instance`, `steady`, the `wall` reading and
/// its uncertainty `sync` (both absent while the wall clock is unsynchronised),
/// the steady `drift_ppm` and the lineage bound `max_segments`. Ingestion
/// carries one too, so its commit is a second detector of a jump.
inline constexpr std::string_view kTurnClock         = "clock";
inline constexpr std::string_view kClockTurnInstance = "instance";
inline constexpr std::string_view kClockTurnSteady   = "steady";
inline constexpr std::string_view kClockTurnWall     = "wall";
inline constexpr std::string_view kClockTurnSync     = "sync";
inline constexpr std::string_view kClockTurnDrift    = "drift_ppm";
inline constexpr std::string_view kClockTurnMax      = "max_segments";

/// `$turn.clock_rebase`: a TEST AND DIAGNOSTIC surface. Nothing calls
/// `rtClockRebase` in production until I4-6; this runs it in a turn that is
/// rolled back and hands its answer to the host. The request holds `instance`,
/// `t`, `now` and `drift_ppm`; the turn writes `status` (0 rebased, 1 the
/// segment left the lineage, 2 an anchor is missing), `delta` and
/// `uncertainty` (seconds) into the same record.
inline constexpr std::string_view kTurnClockRebase   = "clock_rebase";
inline constexpr std::string_view kRebaseInstance    = "instance";
inline constexpr std::string_view kRebaseT           = "t";
inline constexpr std::string_view kRebaseNow         = "now";
inline constexpr std::string_view kRebaseDrift       = "drift_ppm";
inline constexpr std::string_view kRebaseStatus      = "status";
inline constexpr std::string_view kRebaseDelta       = "delta";
inline constexpr std::string_view kRebaseUncertainty = "uncertainty";
inline constexpr std::int64_t kRebaseOk              = 0;
inline constexpr std::int64_t kRebaseGone            = 1;
inline constexpr std::int64_t kRebaseUnanchored      = 2;

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_CLOCK_HPP
