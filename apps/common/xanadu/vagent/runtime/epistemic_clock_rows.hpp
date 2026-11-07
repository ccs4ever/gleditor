/**
 * @file epistemic_clock_rows.hpp
 * @brief The time split of an observation (P19 I3-3b1, design 4.3) as pure
 *        functions, shared by `observe` and the signed-ingress adapter so the
 *        mapped rows have one implementation.
 *
 * Why C++ and not Vortex: these decide, in 128-bit and floating point
 * arithmetic, whether a device's signed times are admissible before any
 * turn runs; they sit in front of the arena.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_CLOCK_ROWS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_CLOCK_ROWS_HPP

#include <cstdint>
#include <optional>

#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"

namespace xanadu::vagent::runtime {

/// How the time split of design 4.3 classified one reading, and what the host
/// diagnostics should learn from it. Decided from the reading, the policy and
/// the receipt alone, so it is the same on every host.
struct TimeVerdict {
  std::optional<EpistemicRefusal> refusal;
  /// Rows 4, 7 and 8 feed the time-class fraction as a refusal; a reading
  /// that clears row 9 feeds it as a pass. Everything else feeds nothing.
  enum class Feed : std::uint8_t { None, Refused, Passed } feed{Feed::None};
  /// Rows 2 and 3, the signed refusals the shared-time-source watch reads.
  bool sharedSource{false};
  /// M(a) and U in seconds, valid once `refusal` is empty.
  double acquiredAt{};
  double uncertainty{};
};

/// Rows 1 to 4: only the origin's signed integers, in 128-bit arithmetic so no
/// overflow turns a refusal into a pass.
[[nodiscard]] TimeVerdict classifySignedTimes(const CheckedObservation &o,
                                              const ArbitrationPolicy &policy);
/// Rows 5, 5a and 6 to 9 against the receipt @p receipt (seconds on the
/// policy's basis).
[[nodiscard]] TimeVerdict classifyMappedTimes(const CheckedObservation &o,
                                              const ArbitrationPolicy &policy,
                                              double receipt);

/// u_M(x): the mapping's uncertainty at its anchor, widened by drift.
[[nodiscard]] double mappingUncertainty(const ClockMapping &mapping, double x);
/// Row 5a's test of the mapping's own numbers.
[[nodiscard]] bool validMapping(const ClockMapping &mapping);
/// Whether @p seconds is a time a clock could read (row 5a).
[[nodiscard]] bool inClockRange(double value);
/// Row 7: M(s) > r + u_d + u_M(s) + k, with @p mappedSent = M(s).
[[nodiscard]] bool sentInFuture(double mappedSent, double clockUncertainty,
                                double mappedUncertainty, double receipt,
                                double skew);

/// Row 9 for a signed `expires` (nanoseconds on the device clock).
enum class ExpiryVerdict : std::uint8_t { Live, Expired, Invalid };
[[nodiscard]] ExpiryVerdict classifyExpiry(const ClockMapping &mapping,
                                           std::int64_t expires,
                                           double receipt);

/// Rows 5, 5a, 7 and 9 for a message with no acquisition (a heartbeat, a
/// report, or the timeliness of an incarnation advance), plus the freshness
/// bound of 5.1: the path lateness r - M(s) may be at most @p maxLateness plus
/// min(u_d + u_M(s), @p maxLateness), so a device cannot widen its own window
/// with a large `clock_uncertainty`. @p mapping empty is row 5.
/// @return the refusal, or nothing for a fresh message.
[[nodiscard]] std::optional<EpistemicRefusal>
classifyMappedSent(const DeviceTimes &device,
                   const std::optional<ClockMapping> &mapping, double receipt,
                   double skew, double maxLateness);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_CLOCK_ROWS_HPP
