/**
 * @file agent_executor_clock.cpp
 * @brief The time split of an observation (P19 I3-3b1, design 4.3): which
 *        refusals a device's own signed bytes decide, which the host's clock
 *        mapping or the path decide, and the host diagnostics that read the
 *        difference.
 */
#include "common/xanadu/vagent/runtime/agent_executor_internal.hpp"

#include <cmath>
#include <gleditor/logging.hpp>

namespace xanadu::vagent::runtime {

namespace {

constexpr const char *kIngressLog = "vagent.ingress";

// The signed checks run in 128-bit integers: a device chooses every operand,
// and a wrapped sum would turn a refusal into a pass.
__extension__ using Wide = __int128;

constexpr double kNanosPerSecond = 1e9;
constexpr Wide kNanosPerMilli    = 1'000'000;
/// A mapping parameter or mapped time beyond this is a defect of the mapping,
/// not a clock: about 31,700 years, far past anything a clock reads, and low
/// enough that a double still resolves a microsecond near it.
constexpr double kMaxClockSeconds = 1e12;
/// The drift bound is in parts per million of elapsed time.
constexpr double kPartsPerMillion = 1e-6;

double seconds(std::int64_t nanoseconds) {
  return static_cast<double>(nanoseconds) / kNanosPerSecond;
}

bool inRange(double value) {
  return std::isfinite(value) && std::abs(value) <= kMaxClockSeconds;
}

/// u_M(x): the mapping's uncertainty at its anchor, widened by drift.
double mappingUncertainty(const ClockMapping &m, double x) {
  return m.uncertainty + kPartsPerMillion * m.driftPpm * std::abs(x - m.anchor);
}

bool validMapping(const ClockMapping &m) {
  return inRange(m.offset) && inRange(m.anchor) &&
         std::isfinite(m.uncertainty) && m.uncertainty >= 0.0 &&
         std::isfinite(m.driftPpm) && m.driftPpm >= 0.0;
}

} // namespace

TimeVerdict classifySignedTimes(const CheckedObservation &o,
                                const ArbitrationPolicy &policy) {
  TimeVerdict v;
  // One shape for every site, so `tools/check-counted-sites.py` can pin them.
  const auto refuse = [&](EpistemicRefusal code, bool shared = false,
                          TimeVerdict::Feed feed = TimeVerdict::Feed::None) {
    v.refusal      = code;
    v.sharedSource = shared;
    v.feed         = feed;
    return v;
  };
  const auto &d = o.device;
  if (d.acquired < 0 || d.sent < 0 || d.acquiredUncertainty < 0 ||
      d.clockUncertainty < 0)
    return refuse(EpistemicRefusal::InvalidAcquisition);
  const Wide a = d.acquired, s = d.sent, ua = d.acquiredUncertainty,
             ud = d.clockUncertainty;
  if (a > s + ua)
    return refuse(EpistemicRefusal::ObservationTimeMismatch, true);
  if (ua + ud > Wide{policy.maxUncertaintyMs} * kNanosPerMilli)
    return refuse(EpistemicRefusal::UncertainAcquisition, true);
  if (s - a > Wide{policy.maxAgeMs} * kNanosPerMilli + ua)
    return refuse(EpistemicRefusal::StaleObservation, false,
                  TimeVerdict::Feed::Refused);
  return v;
}

TimeVerdict classifyMappedTimes(const CheckedObservation &o,
                                const ArbitrationPolicy &policy,
                                double receipt) {
  TimeVerdict v;
  const auto refuse = [&](EpistemicRefusal code,
                          TimeVerdict::Feed feed = TimeVerdict::Feed::None) {
    v.refusal = code;
    v.feed    = feed;
    return v;
  };
  if (!o.mapping) return refuse(EpistemicRefusal::UnknownClockMapping);
  const auto &m = *o.mapping;
  if (o.clockBasis != clockName(policy.clock) || !validMapping(m))
    return refuse(EpistemicRefusal::InvalidClockMapping);
  const auto &d       = o.device;
  const double a      = seconds(d.acquired);
  const double sent   = seconds(d.sent);
  const double ua     = seconds(d.acquiredUncertainty);
  const double ud     = seconds(d.clockUncertainty);
  const double mapped = a + m.offset;
  const double sentAt = sent + m.offset;
  const double uMa    = mappingUncertainty(m, a);
  const double uMs    = mappingUncertainty(m, sent);
  if (!inRange(mapped) || !inRange(sentAt) || !std::isfinite(uMa) ||
      !std::isfinite(uMs))
    return refuse(EpistemicRefusal::InvalidClockMapping);
  const double u = ua + ud + uMa;
  // Row 3 already bounded ua + ud, so exceeding the bound here is the
  // mapping's doing and the device is not at fault.
  if (u > static_cast<double>(policy.maxUncertaintyMs) / 1000.0)
    return refuse(EpistemicRefusal::UncertainClockMapping);
  const double skew = static_cast<double>(policy.clockSkewMs) / 1000.0;
  const double age  = static_cast<double>(policy.maxAgeMs) / 1000.0;
  if (sentAt > receipt + ud + uMs + skew)
    return refuse(EpistemicRefusal::ClockMappingMismatch,
                  TimeVerdict::Feed::Refused);
  if (receipt - mapped > age + u)
    return refuse(EpistemicRefusal::LateDelivery, TimeVerdict::Feed::Refused);
  if (d.expires) {
    const double e            = seconds(*d.expires);
    const double mappedExpiry = e + m.offset;
    const double uMe          = mappingUncertainty(m, e);
    if (!inRange(mappedExpiry) || !std::isfinite(uMe))
      return refuse(EpistemicRefusal::InvalidClockMapping);
    if (mappedExpiry + uMe < receipt)
      return refuse(EpistemicRefusal::MessageExpired);
  }
  v.feed        = TimeVerdict::Feed::Passed;
  v.acquiredAt  = mapped;
  v.uncertainty = u;
  return v;
}

bool AgentExecutor::Ingress::feedTimeClass(const StreamKey &stream,
                                           const SourceKey &source,
                                           std::uint64_t sequence, bool refused,
                                           const EpistemicLimits &limits) {
  // A sequence the estimator has passed, or one this fraction has already
  // taken, is a replay and feeds nothing.
  if (!estimatorCounts(stream, sequence)) return false;
  auto &fed = timeFed[stream];
  if (std::ranges::find(fed, sequence) != fed.end()) return false;
  fed.push_back(sequence);
  while (fed.size() > static_cast<std::size_t>(limits.healthReplayRing))
    fed.pop_front();
  const double decay = healthDecay(limits.timeClassHalfLife);
  auto &t            = timeClass[source];
  t.refused          = decay * t.refused + (refused ? 1.0 : 0.0);
  t.weight           = decay * t.weight + 1.0;
  const bool above = t.weight >= static_cast<double>(limits.suspectMinEvents) &&
                     t.refused / t.weight >= limits.timeSuspectFraction;
  const bool crossed = above && !t.above;
  t.above            = above;
  if (!crossed) return false;
  const auto others = std::ranges::count_if(timeClass, [&](const auto &entry) {
    return entry.first.first == source.first && entry.first != source &&
           entry.second.above;
  });
  return others < limits.commonCauseSources;
}

bool AgentExecutor::Ingress::noteSharedTime(const SourceKey &source,
                                            double steadyNow,
                                            const EpistemicLimits &limits) {
  auto &shared = sharedTime[source.first];
  const double window =
      static_cast<double>(limits.commonCauseWindowMs) / 1000.0;
  shared.hits[source.second] = steadyNow;
  std::erase_if(shared.hits, [&](const auto &hit) {
    return steadyNow - hit.second > window;
  });
  if (static_cast<std::int64_t>(shared.hits.size()) < limits.commonCauseSources)
    return false;
  if (shared.warnedAt && steadyNow - *shared.warnedAt <= window) return false;
  shared.warnedAt = steadyNow;
  return true;
}

void AgentExecutor::noteTime(const identity::ActorPid &target,
                             const CheckedObservation &o,
                             const TimeVerdict &verdict, double steadyNow) {
  const auto pid = pidKey(target);
  const Ingress::SourceKey source{pid, o.principal};
  const Ingress::StreamKey stream{pid, o.principal, o.incarnation, o.predicate};
  const auto &limits = settings_.epistemic;
  bool suspect       = false;
  bool shared        = false;
  double fraction    = 0.0;
  {
    const std::lock_guard lock(ingress_->mutex);
    if (verdict.feed != TimeVerdict::Feed::None) {
      suspect = ingress_->feedTimeClass(
          stream, source, o.sequence,
          verdict.feed == TimeVerdict::Feed::Refused, limits);
      if (const auto found = ingress_->timeClass.find(source);
          found != ingress_->timeClass.end() && found->second.weight > 0.0)
        fraction = found->second.refused / found->second.weight;
    }
    if (verdict.sharedSource)
      shared = ingress_->noteSharedTime(source, steadyNow, limits);
  }
  // Principal tokens only: a diagnostic names the device, never what it said.
  if (suspect)
    GLEDITOR_LOG_WARN(kIngressLog,
                      "source {} time class fraction {:.2f} is at or above "
                      "{:.2f}: its clock or its path looks wrong",
                      o.principal, fraction, limits.timeSuspectFraction);
  if (shared)
    GLEDITOR_LOG_WARN(kIngressLog,
                      "shared device time source: {} or more sources of one "
                      "actor signed inconsistent or over-uncertain times "
                      "within {} ms",
                      limits.commonCauseSources, limits.commonCauseWindowMs);
}

} // namespace xanadu::vagent::runtime
