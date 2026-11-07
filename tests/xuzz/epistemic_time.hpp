/**
 * @file epistemic_time.hpp
 * @brief Builds the signed device times and host mapping of a
 *        `CheckedObservation` from "acquired at wall second t" (P19 I3-3b1).
 *        Free of gtest so the ingress fuzzer can include it.
 *
 * The default device signs at acquisition and its clock is the policy's wall
 * clock, so the identity mapping at version 1 reproduces what the tests meant
 * before the time split: only the mapped second and uncertainty matter.
 */
#ifndef TESTS_XUZZ_EPISTEMIC_TIME_HPP
#define TESTS_XUZZ_EPISTEMIC_TIME_HPP

#include <cmath>
#include <cstdint>

#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"

namespace xanadu::vagent::runtime {

/// Nanoseconds for @p seconds. A time that is not finite, or past the
/// Int64, becomes -1, the signed refusal of row 1, so a test that wants an
/// invalid acquisition can still say "NaN".
[[nodiscard]] inline std::int64_t testNanos(double seconds) {
  constexpr double kNanosPerSecond = 1e9;
  const double scaled              = seconds * kNanosPerSecond;
  if (!std::isfinite(scaled) || std::abs(scaled) >= 9.0e18) return -1;
  return static_cast<std::int64_t>(std::llround(scaled));
}

/// A reading acquired and signed at wall second @p at, with signed
/// uncertainty @p uncertainty, mapped by the identity at @p version.
inline void setTimes(CheckedObservation &o, double at, double uncertainty,
                     std::uint64_t version = 1) {
  o.device.acquired            = testNanos(at);
  o.device.sent                = o.device.acquired;
  o.device.acquiredUncertainty = testNanos(uncertainty);
  o.device.clockUncertainty    = 0;
  o.mapping                    = ClockMapping{.version = version};
}

/// Move the acquisition (and the send, which follows it) and keep the rest.
inline void setAcquiredAt(CheckedObservation &o, double at) {
  o.device.acquired = testNanos(at);
  o.device.sent     = o.device.acquired;
}

inline void setAcquiredUncertainty(CheckedObservation &o, double uncertainty) {
  o.device.acquiredUncertainty = testNanos(uncertainty);
}

inline void setMappingVersion(CheckedObservation &o, std::uint64_t version) {
  if (!o.mapping) o.mapping = ClockMapping{};
  o.mapping->version = version;
}

} // namespace xanadu::vagent::runtime

#endif // TESTS_XUZZ_EPISTEMIC_TIME_HPP
