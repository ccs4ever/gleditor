/**
 * @file epistemic_abi_clock.hpp
 * @brief Names the clock-mapping slice (P19 I3-3b1) shares between the
 *        executor, the emitted routines and the translator registry.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_CLOCK_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_CLOCK_HPP

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

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ABI_CLOCK_HPP
