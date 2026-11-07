/**
 * @file agent_executor_clock.cpp
 * @brief The time split of an observation (P19 I3-3b1, design 4.3): which
 *        refusals a device's own signed bytes decide, which the host's clock
 *        mapping or the path decide, and the host diagnostics that read the
 *        difference.
 */
#include "common/xanadu/vagent/runtime/agent_executor_internal.hpp"

#include <algorithm>
#include <cmath>
#include <gleditor/logging.hpp>

#include "common/xanadu/vagent/runtime/epistemic_abi_clock.hpp"

namespace xanadu::vagent::runtime {

namespace {

constexpr const char *kIngressLog = "vagent.ingress";
/// Answers the rebase test surface keeps; older ones are dropped.
constexpr std::size_t kMaxRebaseAnswers = 64;
constexpr double kMillisPerSecond       = 1e3;

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

} // namespace

bool inClockRange(const double value) {
  return std::isfinite(value) && std::abs(value) <= kMaxClockSeconds;
}

double mappingUncertainty(const ClockMapping &m, const double x) {
  return m.uncertainty + kPartsPerMillion * m.driftPpm * std::abs(x - m.anchor);
}

bool validMapping(const ClockMapping &m) {
  return inClockRange(m.offset) && inClockRange(m.anchor) &&
         std::isfinite(m.uncertainty) && m.uncertainty >= 0.0 &&
         std::isfinite(m.driftPpm) && m.driftPpm >= 0.0;
}

bool sentInFuture(const double mappedSent, const double clockUncertainty,
                  const double mappedUncertainty, const double receipt,
                  const double skew) {
  return mappedSent > receipt + clockUncertainty + mappedUncertainty + skew;
}

ExpiryVerdict classifyExpiry(const ClockMapping &m, const std::int64_t expires,
                             const double receipt) {
  const double e            = seconds(expires);
  const double mappedExpiry = e + m.offset;
  const double uMe          = mappingUncertainty(m, e);
  if (!inClockRange(mappedExpiry) || !std::isfinite(uMe))
    return ExpiryVerdict::Invalid;
  return mappedExpiry + uMe < receipt ? ExpiryVerdict::Expired
                                      : ExpiryVerdict::Live;
}

std::optional<EpistemicRefusal> classifyMappedSent(
    const DeviceTimes &d, const std::optional<ClockMapping> &mapping,
    const double receipt, const double skew, const double maxLateness) {
  if (!mapping) return EpistemicRefusal::UnknownClockMapping;
  const auto &m = *mapping;
  if (!validMapping(m)) return EpistemicRefusal::InvalidClockMapping;
  const double sent   = seconds(d.sent);
  const double ud     = seconds(d.clockUncertainty);
  const double sentAt = sent + m.offset;
  const double uMs    = mappingUncertainty(m, sent);
  if (!inClockRange(sentAt) || !std::isfinite(uMs))
    return EpistemicRefusal::InvalidClockMapping;
  if (sentInFuture(sentAt, ud, uMs, receipt, skew))
    return EpistemicRefusal::ClockMappingMismatch;
  if (receipt - sentAt > maxLateness + std::min(ud + uMs, maxLateness))
    return EpistemicRefusal::LateDelivery;
  if (d.expires) {
    switch (classifyExpiry(m, *d.expires, receipt)) {
    case ExpiryVerdict::Invalid:
      return EpistemicRefusal::InvalidClockMapping;
    case ExpiryVerdict::Expired:
      return EpistemicRefusal::MessageExpired;
    case ExpiryVerdict::Live:
      break;
    }
  }
  return std::nullopt;
}

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
  if (!inClockRange(mapped) || !inClockRange(sentAt) || !std::isfinite(uMa) ||
      !std::isfinite(uMs))
    return refuse(EpistemicRefusal::InvalidClockMapping);
  const double u = ua + ud + uMa;
  // Row 3 already bounded ua + ud, so exceeding the bound here is the
  // mapping's doing and the device is not at fault.
  if (u > static_cast<double>(policy.maxUncertaintyMs) / 1000.0)
    return refuse(EpistemicRefusal::UncertainClockMapping);
  const double skew = static_cast<double>(policy.clockSkewMs) / 1000.0;
  const double age  = static_cast<double>(policy.maxAgeMs) / 1000.0;
  if (sentInFuture(sentAt, ud, uMs, receipt, skew))
    return refuse(EpistemicRefusal::ClockMappingMismatch,
                  TimeVerdict::Feed::Refused);
  if (receipt - mapped > age + u)
    return refuse(EpistemicRefusal::LateDelivery, TimeVerdict::Feed::Refused);
  if (d.expires) {
    const auto expiry = classifyExpiry(m, *d.expires, receipt);
    if (expiry == ExpiryVerdict::Invalid)
      return refuse(EpistemicRefusal::InvalidClockMapping);
    if (expiry == ExpiryVerdict::Expired)
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

// -- The clock lineage and jump detection (I3-3b2, design 4.4 and 4.5)
// ---------------------------------------------------------------------

ClockReading AgentExecutor::readClockLocked() const {
  ClockReading reading;
  reading.steady = toSeconds(clock_.steady().time_since_epoch());
  reading.sigma =
      static_cast<double>(settings_.epistemic.wallSyncUncertaintyMs) /
      kMillisPerSecond;
  if (ingress_->wallSynchronised)
    reading.wall = toSeconds(wallNow().time_since_epoch());
  return reading;
}

WireRecord AgentExecutor::clockRecord(const ClockReading &reading) const {
  WireRecord clock;
  clock.set(std::string(kClockTurnInstance),
            WireValue::text(ingress_->clockInstance));
  clock.set(std::string(kClockTurnSteady), WireValue::real(reading.steady));
  if (reading.wall) {
    clock.set(std::string(kClockTurnWall), WireValue::real(*reading.wall));
    clock.set(std::string(kClockTurnSync), WireValue::real(reading.sigma));
  }
  clock.set(
      std::string(kClockTurnDrift),
      WireValue::real(static_cast<double>(settings_.epistemic.steadyDriftPpm)));
  clock.set(std::string(kClockTurnMax),
            WireValue::integer(settings_.epistemic.clockLineage));
  return clock;
}

namespace {
/// The `$turn` fields of a control turn that notes the segment rule.
WireRecord noteFields(WireRecord clock) {
  WireRecord fields;
  fields.set(std::string(kTurnClock), WireValue::record(std::move(clock)));
  return fields;
}
} // namespace

void AgentExecutor::queueClockNoteLocked(const identity::ActorPid &target,
                                         WireRecord fields) {
  if (admissionClosedLocked(target)) {
    ingress_->lineageOwed[pidKey(target)].push_back(std::move(fields));
    return;
  }
  PostBox::Item item;
  item.pid       = target;
  item.clockTurn = ClockTurn{std::move(fields), std::nullopt};
  posts_->pushControl(std::move(item));
}

void AgentExecutor::flushLineageOwedLocked() {
  for (auto it = ingress_->lineageOwed.begin();
       it != ingress_->lineageOwed.end();) {
    const auto actor = ingress_->lineageActors.find(it->first);
    if (actor == ingress_->lineageActors.end()) {
      it = ingress_->lineageOwed.erase(it);
      continue;
    }
    if (admissionClosedLocked(actor->second)) {
      ++it;
      continue;
    }
    auto notes = std::move(it->second);
    it         = ingress_->lineageOwed.erase(it);
    for (auto &fields : notes)
      queueClockNoteLocked(actor->second, std::move(fields));
  }
}

std::uint64_t AgentExecutor::checkClockLocked(const ClockReading &reading) {
  flushLineageOwedLocked();
  // Without a wall reference no offset exists to jump: the residual of 4.4.
  if (!reading.wall) return ingress_->clockGeneration;
  const Ingress::ClockAnchor here{reading.steady, *reading.wall, reading.sigma};
  if (const auto &anchor = ingress_->clockAnchor) {
    const double moved =
        std::abs((here.wall - here.steady) - (anchor->wall - anchor->steady));
    const double allowed =
        anchor->sigma + here.sigma +
        kClockPartsPerMillion *
            static_cast<double>(settings_.epistemic.steadyDriftPpm) *
            std::abs(here.steady - anchor->steady);
    // Written so that a NaN counts as a jump: refusing is the safe error.
    if (!(moved <= allowed)) {
      ++ingress_->clockGeneration;
      for (const auto &[pid, actor] : ingress_->lineageActors)
        queueClockNoteLocked(actor, noteFields(clockRecord(reading)));
      GLEDITOR_LOG_INFO(kIngressLog,
                        "the wall-minus-steady offset moved by {:.3f} s: "
                        "clock generation {}",
                        moved, ingress_->clockGeneration);
    }
  }
  // Rolled on every reading, as the arena's anchor is on every commit.
  ingress_->clockAnchor = here;
  return ingress_->clockGeneration;
}

std::uint64_t AgentExecutor::checkClock() {
  const std::lock_guard lock(ingress_->mutex);
  return checkClockLocked(readClockLocked());
}

std::string AgentExecutor::clockInstance() const {
  const std::lock_guard lock(ingress_->mutex);
  return ingress_->clockInstance;
}

void AgentExecutor::startLineageLocked(const identity::ActorPid &target) {
  // Before the actor joins the set, so the note below is its only one.
  const auto reading = readClockLocked();
  checkClockLocked(reading);
  if (!ingress_->lineageActors.emplace(pidKey(target), target).second) return;
  queueClockNoteLocked(target, noteFields(clockRecord(reading)));
}

std::optional<std::uint64_t>
AgentExecutor::requestClockRebase(const identity::ActorPid &target,
                                  std::string_view instance, double t) {
  const std::lock_guard lock(ingress_->mutex);
  if (!ingress_->lineageActors.contains(pidKey(target)) ||
      admissionClosedLocked(target))
    return std::nullopt;
  const auto id = ingress_->nextRebase++;
  WireRecord request;
  request.set(std::string(kRebaseInstance),
              WireValue::text(std::string(instance)));
  request.set(std::string(kRebaseT), WireValue::real(t));
  request.set(std::string(kRebaseNow),
              WireValue::real(readClockLocked().steady));
  request.set(
      std::string(kRebaseDrift),
      WireValue::real(static_cast<double>(settings_.epistemic.steadyDriftPpm)));
  WireRecord fields;
  fields.set(std::string(kTurnClockRebase),
             WireValue::record(std::move(request)));
  ingress_->rebases.emplace(id, std::nullopt);
  while (ingress_->rebases.size() > kMaxRebaseAnswers)
    ingress_->rebases.erase(ingress_->rebases.begin());
  PostBox::Item item;
  item.pid       = target;
  item.clockTurn = ClockTurn{std::move(fields), id};
  posts_->pushControl(std::move(item));
  return id;
}

std::optional<AgentExecutor::ClockRebase>
AgentExecutor::clockRebaseResult(std::uint64_t request) const {
  const std::lock_guard lock(ingress_->mutex);
  const auto found = ingress_->rebases.find(request);
  return found == ingress_->rebases.end() ? std::nullopt : found->second;
}

void AgentExecutor::armRebase(TurnSpec &spec, std::uint64_t request) {
  // The answer is a diagnostic: the turn is rolled back, so it mints nothing.
  spec.readOnly = true;
  spec.inspect  = [this, request](const WireValue &turn) {
    const auto *record = std::get_if<WireRecord>(&turn.data);
    const auto *asked  = record ? record->find(kTurnClockRebase) : nullptr;
    const auto *fields =
        asked ? std::get_if<WireRecord>(&asked->data) : nullptr;
    if (!fields) return;
    const auto number = [&](std::string_view name) -> std::optional<double> {
      const auto *v = fields->find(name);
      if (!v) return std::nullopt;
      if (const auto *real = std::get_if<double>(&v->data)) return *real;
      if (const auto *integer = std::get_if<std::int64_t>(&v->data))
        return static_cast<double>(*integer);
      return std::nullopt;
    };
    const auto status = number(kRebaseStatus);
    if (!status) return;
    ClockRebase answer;
    switch (static_cast<std::int64_t>(*status)) {
    case kRebaseOk:
      answer.status      = ClockRebase::Status::Rebased;
      answer.delta       = number(kRebaseDelta).value_or(0.0);
      answer.uncertainty = number(kRebaseUncertainty).value_or(0.0);
      break;
    case kRebaseGone:
      answer.status = ClockRebase::Status::SegmentGone;
      break;
    default:
      answer.status = ClockRebase::Status::Unanchored;
      break;
    }
    const std::lock_guard lock(ingress_->mutex);
    if (const auto found = ingress_->rebases.find(request);
        found != ingress_->rebases.end())
      found->second = answer;
  };
}

} // namespace xanadu::vagent::runtime
