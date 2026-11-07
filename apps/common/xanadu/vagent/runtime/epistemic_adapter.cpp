#include "common/xanadu/vagent/runtime/epistemic_adapter.hpp"

#include <gleditor/logging.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/epistemic_clock_rows.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"

namespace xanadu::vagent::runtime {

namespace {

constexpr const char *kLog = "vagent.ingress";

using SteadyTime = std::chrono::steady_clock::time_point;

bool isToken(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

constexpr double kNanosPerSecond  = 1e9;
constexpr double kMillisPerSecond = 1e3;

double seconds(std::int64_t nanoseconds) {
  return static_cast<double>(nanoseconds) / kNanosPerSecond;
}

double milliseconds(std::int64_t ms) {
  return static_cast<double>(ms) / kMillisPerSecond;
}

std::int64_t nanosOf(std::int64_t ms) { return ms * 1'000'000; }

template <class Clock>
std::int64_t nanosSince(const std::chrono::time_point<Clock> &t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             t.time_since_epoch())
      .count();
}

HostInstant instantOf(const AgentExecutor::HostTime &now) {
  return {nanosSince(now.steady), nanosSince(now.wall), now.wallSynchronised};
}

double secondsOn(ObservationClock clock, const HostInstant &now) {
  return seconds(clock == ObservationClock::Wall ? now.wall : now.steady);
}

ClockTableLimits tableLimits(const AdapterLimits &l) {
  return {.wallSyncUncertainty = milliseconds(l.wallSyncUncertaintyMs),
          .pairTimeout         = nanosOf(l.clockPairTimeoutMs),
          .pairInterval        = nanosOf(l.clockPairIntervalMs),
          .maxEpochs           = kMaxMappingEpochsPerIssuer};
}

/// One declared argument from its signed form, or nothing when the item is
/// not of the declared kind: an Int or a Bool travels as an integer, a Float
/// as its 8-byte bit pattern, text as strict UTF-8.
std::optional<WireValue> typed(Type::Kind kind, const SignedTupleItem &item) {
  const auto *integer = std::get_if<std::int64_t>(&item);
  const auto *bytes   = std::get_if<std::string>(&item);
  switch (kind) {
  case Type::Kind::Int:
    if (integer) return WireValue::integer(*integer);
    return std::nullopt;
  case Type::Kind::Bool:
    if (integer)
      if (const auto b = signedBool(*integer)) return WireValue::boolean(*b);
    return std::nullopt;
  case Type::Kind::Float:
    if (bytes)
      if (const auto d = signedFloat(*bytes)) return WireValue::real(*d);
    return std::nullopt;
  case Type::Kind::Str:
  case Type::Kind::Agent:
    if (bytes && isSignedText(*bytes, kMaxObservationSignedBytes))
      return WireValue::text(*bytes);
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

} // namespace

std::string_view toString(const AdapterLimitError error) noexcept {
  switch (error) {
  case AdapterLimitError::InvalidSignedIngress:
    return "InvalidSignedIngress";
  case AdapterLimitError::InvalidIncarnationStep:
    return "InvalidIncarnationStep";
  case AdapterLimitError::InvalidClockDrift:
    return "InvalidClockDrift";
  case AdapterLimitError::InvalidClockRepairFraction:
    return "InvalidClockRepairFraction";
  case AdapterLimitError::InvalidClockPairInterval:
    return "InvalidClockPairInterval";
  case AdapterLimitError::InvalidClockPairTimeout:
    return "InvalidClockPairTimeout";
  case AdapterLimitError::InvalidWallSyncUncertainty:
    return "InvalidWallSyncUncertainty";
  case AdapterLimitError::InvalidMaxPathLateness:
    return "InvalidMaxPathLateness";
  }
  return "Unknown";
}

std::expected<void, AdapterLimitError> AdapterLimits::validate() const {
  if (!verify.validate())
    return std::unexpected(AdapterLimitError::InvalidSignedIngress);
  if (incarnationStep < kMinIncarnationStep ||
      incarnationStep > kMaxIncarnationStep)
    return std::unexpected(AdapterLimitError::InvalidIncarnationStep);
  const auto outside = [](std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return v < lo || v > hi;
  };
  if (outside(clockDriftPpm, kMinClockDriftPpm, kMaxClockDriftPpm))
    return std::unexpected(AdapterLimitError::InvalidClockDrift);
  // Written so a NaN fails it.
  if (!(clockRepairFraction >= kMinClockRepairFraction &&
        clockRepairFraction <= kMaxClockRepairFraction))
    return std::unexpected(AdapterLimitError::InvalidClockRepairFraction);
  if (outside(clockPairIntervalMs, kMinClockPairIntervalMs,
              kMaxClockPairIntervalMs))
    return std::unexpected(AdapterLimitError::InvalidClockPairInterval);
  if (outside(clockPairTimeoutMs, kMinClockPairTimeoutMs,
              kMaxClockPairTimeoutMs))
    return std::unexpected(AdapterLimitError::InvalidClockPairTimeout);
  if (outside(wallSyncUncertaintyMs, kMinWallSyncUncertaintyMs,
              kMaxWallSyncUncertaintyMs))
    return std::unexpected(AdapterLimitError::InvalidWallSyncUncertainty);
  if (outside(maxPathLatenessMs, kMinMaxPathLatenessMs, kMaxMaxPathLatenessMs))
    return std::unexpected(AdapterLimitError::InvalidMaxPathLateness);
  return {};
}

AdapterLimits AdapterLimits::fromModel(const SystemStoreModel &model) {
  AdapterLimits out;
  auto &v             = out.verify;
  const auto readSize = [&](std::string_view name, std::size_t &target) {
    const auto read = model.getInt64(name, static_cast<std::int64_t>(target));
    // Kept as read when it would not fit, so validate() names it.
    target = read > 0 ? static_cast<std::size_t>(read) : 0;
  };
  const auto readSigned = [&](std::string_view name, std::int64_t &target) {
    target = model.getInt64(name, target);
  };
  readSize(settings::kVagentEpistemicSignedMessageBytes, v.signedMessageBytes);
  readSize(settings::kVagentEpistemicDelegationCache, v.delegationCache);
  readSigned(settings::kVagentEpistemicVerifyPerPeer, v.verifyPerPeer);
  readSigned(settings::kVagentEpistemicWallRollbackMs, v.wallRollbackMs);
  readSigned(settings::kVagentEpistemicHealthReplayRing, v.replayRing);
  readSigned(settings::kVagentEpistemicIncarnationStep, out.incarnationStep);
  readSigned(settings::kVagentEpistemicClockDriftPpm, out.clockDriftPpm);
  out.clockRepairFraction = model.getDouble(
      settings::kVagentEpistemicClockRepairFraction, out.clockRepairFraction);
  readSigned(settings::kVagentEpistemicClockPairIntervalMs,
             out.clockPairIntervalMs);
  readSigned(settings::kVagentEpistemicClockPairTimeoutMs,
             out.clockPairTimeoutMs);
  readSigned(settings::kVagentEpistemicWallSyncUncertaintyMs,
             out.wallSyncUncertaintyMs);
  readSigned(settings::kVagentEpistemicMaxPathLatenessMs,
             out.maxPathLatenessMs);
  if (const auto checked = out.validate(); !checked) {
    if (const auto ingress = out.verify.validate(); !ingress)
      GLEDITOR_LOG_WARN(kLog,
                        "signed ingress settings refused ({}); keeping the "
                        "defaults for all",
                        toString(ingress.error()));
    else
      GLEDITOR_LOG_WARN(kLog,
                        "signed ingress settings refused ({}); keeping the "
                        "defaults for all",
                        toString(checked.error()));
    return AdapterLimits{};
  }
  return out;
}

struct EpistemicAdapter::State {
  State(AgentExecutor &e, AdapterLimits l,
        const identity::IRevocationRegistry &revocations,
        const MasterKeys &masters)
      : executor(e), limits(std::move(l)),
        verifier(limits.verify, revocations, masters),
        clocks(tableLimits(limits)) {}

  struct Row {
    identity::ActorPid pid;
    SourceBinding binding;
  };
  /// An observation queued with its header, waiting for a final outcome
  /// before step 4 may refuse its bytes.
  struct Pending {
    identity::ActorPid pid;
    std::string principal;
    std::uint64_t incarnation{};
    std::string eventId;
    SignedHeader header;
  };
  /// A run of `UncertainClockMapping` refusals for one source, from its
  /// first until an observation of it is accepted.
  struct Streak {
    std::int64_t since{};
    bool warned{false};
  };

  AgentExecutor &executor;
  AdapterLimits limits;
  SignedVerifier verifier;
  ClockMappingTable clocks;
  std::mutex mutex;
  /// By (pid text, principal).
  std::map<std::pair<std::string, std::string>, Row> rows;
  /// Audience to pid text, and the pids already read.
  std::map<std::string, std::string, std::less<>> audiences;
  std::set<std::string> mirrored;
  std::deque<Pending> pending;
  IngressAuditSink audit;
  ChallengeSink challengeSink;
  NonceSource nonces;
  /// Raised under the mutex and delivered after it is released, so a sink
  /// that calls back into the adapter cannot deadlock it.
  std::vector<IngressAudit> auditOutbox;
  std::vector<ClockChallenge> challengeOutbox;
  /// An issuer's highest verified sequence, any kind, for its newest
  /// incarnation (design 7.1): (incarnation, sequence).
  std::map<std::string, std::pair<std::int64_t, std::int64_t>, std::less<>>
      highest;
  std::map<std::pair<std::string, std::string>, Streak> streaks;
  /// The executor's jump generation this adapter last invalidated for.
  std::uint64_t clockGeneration{};
  /// By (pid text, principal): when a jump dropped the source's paired steady
  /// mapping, until its pairing completes.
  std::map<std::pair<std::string, std::string>, SteadyTime> clockPending;

  /// Test the host's clocks before anything is mapped. A jump invalidates the
  /// steady-basis mappings in the same critical section, so a message is
  /// either mapped before the jump is recorded or after the invalidation.
  void followClock() {
    const auto generation = executor.checkClock();
    if (generation == clockGeneration) return;
    clockGeneration = generation;
    const auto now  = executor.hostTime();
    const auto lost = clocks.invalidateSteady(instantOf(now));
    for (const auto &issuer : lost)
      for (const auto &[key, row] : rows) {
        if (row.binding.principal != issuer) continue;
        clockPending.insert_or_assign(key, now.steady);
        GLEDITOR_LOG_INFO(kLog,
                          "source {}: the host clock jumped, its paired "
                          "mapping is dropped until it pairs again",
                          issuer);
      }
  }

  void emit(const IngressAudit &event) {
    if (audit) auditOutbox.push_back(event);
  }

  void noteVerified(const SignedHeader &h) {
    auto [it, fresh] =
        highest.try_emplace(h.issuer, std::pair{h.incarnation, h.sequence});
    if (fresh) return;
    auto &[incarnation, sequence] = it->second;
    if (h.incarnation > incarnation) {
      incarnation = h.incarnation;
      sequence    = h.sequence;
    } else if (h.incarnation == incarnation) {
      sequence = std::max(sequence, h.sequence);
    }
  }

  /// The bound for a message's pass credit: its issuer's highest verified
  /// sequence in the message's own incarnation.
  [[nodiscard]] std::int64_t verifiedBound(const SignedHeader &h) const {
    const auto it = highest.find(h.issuer);
    return it != highest.end() && it->second.first == h.incarnation
               ? it->second.second
               : h.sequence;
  }

  /// The tightest skew allowance among the source's live predicates: a
  /// message with no policy of its own is held to the strictest one.
  [[nodiscard]] std::int64_t smallestSkewMs(const Row &row) const {
    std::optional<std::int64_t> least;
    for (const auto &predicate : row.binding.predicates)
      if (const auto bound = executor.boundPredicate(
              row.pid, row.binding.principal, predicate))
        least = std::min(least.value_or(bound->policy.clockSkewMs),
                         bound->policy.clockSkewMs);
    return least.value_or(0);
  }

  /// Whether a challenge for the message's incarnation could be answered
  /// usefully: the bound one, or one a `signed` binding may still advance to.
  [[nodiscard]] bool pairable(const Row &row, const SignedHeader &h) const {
    const auto bound = row.binding.incarnation;
    const auto sent  = static_cast<std::uint64_t>(h.incarnation);
    if (sent == bound) return true;
    return row.binding.incarnationPolicy == IncarnationPolicy::Signed &&
           sent > bound &&
           sent - bound <= static_cast<std::uint64_t>(limits.incarnationStep);
  }

  /// Start a pairing under the challenge rules of 4.2. Only a message that
  /// passed steps 1 to 5 reaches this, so a replay below the floor never
  /// does.
  void startPairing(const Row &row, const SignedHeader &h) {
    if (!challengeSink || !pairable(row, h)) return;
    // Read now, not at the message's receipt: the nonce does not exist before
    // this, and a later t1 only tightens the interval.
    const auto begunAt = instantOf(executor.hostTime());
    auto begun = clocks.begin(row.pid, h.audience, h.issuer, h.incarnation,
                              begunAt, nonces);
    if (!begun) {
      // No nonce, key or audience in the line.
      switch (begun.error()) {
      case PairingSkip::Pending:
        GLEDITOR_LOG_TRACE(kLog, "clock challenge for {} already pending",
                           h.issuer);
        break;
      case PairingSkip::RateLimited:
        GLEDITOR_LOG_TRACE(kLog, "clock challenge for {} rate limited",
                           h.issuer);
        break;
      case PairingSkip::EntropyUnavailable:
        GLEDITOR_LOG_DEBUG(
            kLog, "no clock challenge for {}: {}", h.issuer,
            epistemicRefusalName(EpistemicRefusal::EntropyUnavailable));
        break;
      }
      return;
    }
    GLEDITOR_LOG_DEBUG(kLog, "clock challenge started for {}", h.issuer);
    challengeOutbox.push_back(std::move(*begun));
  }

  /// The mappings a message's header selects: a `synchronised` clock's are
  /// minted on first sight, a `free` clock's exist only after a pairing.
  const MappingSet *mappingsFor(const SignedHeader &h, const HostInstant &at) {
    const ClockMappingTable::Key key{h.issuer, h.incarnation, h.epoch, h.basis};
    return h.basis == SignedClockBasis::Synchronised
               ? clocks.synchronised(key, at)
               : clocks.find(key);
  }

  /// Re-pair once any predicate's mapping has grown to the repair fraction
  /// of that predicate's uncertainty bound at the message's own time. Each
  /// predicate is judged on its own timeline, which is the smallest bound
  /// when they share one.
  void maybeRepair(const Row &row, const SignedHeader &h,
                   const MappingSet &set) {
    const double x = seconds(h.sent);
    for (const auto &predicate : row.binding.predicates) {
      const auto bound =
          executor.boundPredicate(row.pid, row.binding.principal, predicate);
      if (!bound) continue;
      const auto &m = set.on(bound->policy.clock);
      if (!m) continue;
      const double limit = limits.clockRepairFraction *
                           milliseconds(bound->policy.maxUncertaintyMs);
      if (mappingUncertainty(*m, x) >= limit) {
        startPairing(row, h);
        return;
      }
    }
  }

  static DeviceTimes deviceTimes(const SignedHeader &h,
                                 const ObservationPayload &o) {
    return {.acquired            = o.acquired,
            .sent                = h.sent,
            .acquiredUncertainty = o.acquiredUncertainty,
            .clockUncertainty    = h.clockUncertainty,
            .expires             = h.expires};
  }

  /// A `clock_pair` reply: it skips step 6 and is bounded by its challenge's
  /// timeout instead (design 3.8).
  std::expected<TransportReceipt, EpistemicRefusal>
  pairReply(const Row &row, const SignedMessage &message,
            const HostInstant &at) {
    const auto &h = message.header;
    const auto &p = std::get<ClockPairPayload>(message.payload);
    // A device clock reads from zero; a negative stamp is not one.
    if (h.sent < 0) return std::unexpected(EpistemicRefusal::MalformedMessage);
    const ClockPairReply reply{
        .issuer      = h.issuer,
        .incarnation = h.incarnation,
        .epoch       = h.epoch,
        .basis       = h.basis,
        .audience    = h.audience,
        .nonce       = p.nonce,
        .sent        = h.sent,
        .driftPpm    = static_cast<double>(
            row.binding.driftPpm.value_or(limits.clockDriftPpm))};
    const auto paired = clocks.accept(reply, at);
    if (!paired) return std::unexpected(paired.error());
    std::erase_if(clockPending, [&](const auto &entry) {
      const auto bound = rows.find(entry.first);
      return bound != rows.end() && bound->second.binding.principal == h.issuer;
    });
    GLEDITOR_LOG_DEBUG(kLog, "clock paired for {} epoch {} version {}",
                       h.issuer, h.epoch, (*paired)->version);
    return TransportReceipt{};
  }

  /// Step 6 for a heartbeat or a report: rows 5, 5a, 7 and 9 on the host's
  /// steady timeline, with `sent` standing for the acquisition, and the
  /// freshness bound of 5.1. Dispatch of a fresh one is I3-3c1's and d's.
  std::expected<TransportReceipt, EpistemicRefusal>
  liveness(const Row &row, const SignedMessage &message,
           const HostInstant &at) {
    const auto &h = message.header;
    if (h.sent < 0 || h.clockUncertainty < 0)
      return std::unexpected(EpistemicRefusal::MalformedMessage);
    const auto *set = mappingsFor(h, at);
    std::optional<ClockMapping> mapping;
    if (set) {
      mapping = set->steady;
      maybeRepair(row, h, *set);
    }
    if (!mapping) startPairing(row, h);
    const DeviceTimes device{.acquired         = h.sent,
                             .sent             = h.sent,
                             .clockUncertainty = h.clockUncertainty,
                             .expires          = h.expires};
    if (const auto refused = classifyMappedSent(
            device, mapping, secondsOn(ObservationClock::ActorMapped, at),
            milliseconds(smallestSkewMs(row)),
            milliseconds(limits.maxPathLatenessMs)))
      return std::unexpected(*refused);
    return std::unexpected(EpistemicRefusal::UnsupportedKind);
  }

  /// Log once per streak that a source's mapping is too uncertain for its
  /// predicate: the operator's remedy is a tighter clock or a looser bound.
  void trackUncertainty(
      const Row &row,
      const std::expected<TransportReceipt, EpistemicRefusal> &receipt,
      const HostInstant &at) {
    const std::pair key{pidToText(row.pid), row.binding.principal};
    if (receipt) {
      streaks.erase(key);
      return;
    }
    if (receipt.error() != EpistemicRefusal::UncertainClockMapping) return;
    auto &streak =
        streaks.try_emplace(key, Streak{.since = at.steady}).first->second;
    const auto window =
        nanosOf(executor.settings().epistemic.commonCauseWindowMs);
    if (streak.warned || at.steady - streak.since <= window) return;
    streak.warned = true;
    GLEDITOR_LOG_WARN(kLog,
                      "source {} ({}) has been refused UncertainClockMapping "
                      "for over {} ms: its clock mapping is wider than its "
                      "predicate's max_uncertainty",
                      row.binding.alias, row.binding.principal,
                      executor.settings().epistemic.commonCauseWindowMs);
  }

  /// The pid text the audience names, or why there is none.
  std::expected<std::string, EpistemicRefusal>
  resolve(std::string_view audience) {
    if (const auto found = audiences.find(audience); found != audiences.end())
      return found->second;
    bool uncommitted = false;
    std::set<std::string> asked;
    for (const auto &[key, row] : rows) {
      const auto &pidText = key.first;
      if (mirrored.contains(pidText) || !asked.insert(pidText).second) continue;
      const auto committed = executor.audience(row.pid);
      if (!committed) {
        uncommitted = true;
        continue;
      }
      audiences.insert_or_assign(*committed, pidText);
      mirrored.insert(pidText);
      if (*committed == audience) return pidText;
    }
    // A bound agent whose identifier has not committed may be the one the
    // device means; resending later is the only way to find out.
    return std::unexpected(uncommitted ? EpistemicRefusal::AudienceUnavailable
                                       : EpistemicRefusal::AudienceMismatch);
  }

  /// Note every waiting header whose observation has a final outcome. A
  /// retryable refusal is dropped unnoted, so the same bytes pass step 4.
  void settle() {
    std::erase_if(pending, [&](const Pending &p) {
      const auto outcome =
          executor.ingestOutcome(p.pid, p.principal, p.incarnation, p.eventId);
      if (!outcome) return true;
      if (outcome->kind == IngestOutcome::Kind::Pending) return false;
      if (outcome->outcomeClass != OutcomeClass::Retryable)
        verifier.noteSettled(p.header);
      return true;
    });
  }

  /// 3.10's timeliness condition: the same step-6 rows as an observation's,
  /// against the mapping of the message's own epoch. A reading the path made
  /// old, or one sent in the future, cannot advance anything, and with no
  /// mapping nothing is timely.
  static bool timely(const SignedHeader &h, const ObservationPayload &o,
                     const ArbitrationPolicy &policy,
                     const std::optional<ClockMapping> &mapping,
                     const HostInstant &at) {
    if (policy.clock == ObservationClock::Wall && !at.wallSynchronised)
      return false;
    CheckedObservation probe;
    probe.device     = deviceTimes(h, o);
    probe.mapping    = mapping;
    probe.clockBasis = std::string(clockName(policy.clock));
    if (classifySignedTimes(probe, policy).refusal) return false;
    return !classifyMappedTimes(probe, policy, secondsOn(policy.clock, at))
                .refusal;
  }

  /// Rebind every predicate of @p row at @p to: the first bind supersedes
  /// the principal's lower bindings on all of them at once.
  std::expected<void, EpistemicRefusal> advance(Row &row, std::uint64_t to) {
    const auto from = row.binding.incarnation;
    for (const auto &predicate : row.binding.predicates) {
      const EpistemicSourceGrant grant{row.binding.alias, row.binding.principal,
                                       to, predicate, row.binding.lifetime};
      if (auto bound = executor.bindSource(row.pid, grant); !bound)
        return std::unexpected(bound.error());
    }
    row.binding.incarnation = to;
    GLEDITOR_LOG_DEBUG(kLog, "incarnation {} -> {} for {}", from, to,
                       row.binding.principal);
    emit({.kind      = IngressAudit::Kind::IncarnationAdvanced,
          .principal = row.binding.principal,
          .pid       = row.pid,
          .from      = from,
          .to        = to});
    return {};
  }

  std::expected<TransportReceipt, EpistemicRefusal>
  observation(Row &row, const SignedMessage &message, const TransportPeer &peer,
              const HostInstant &at) {
    const auto &h = message.header;
    const auto &o = std::get<ObservationPayload>(message.payload);
    if (!std::ranges::contains(row.binding.predicates, o.predicate))
      return std::unexpected(EpistemicRefusal::UnknownSource);
    auto bound =
        executor.boundPredicate(row.pid, row.binding.principal, o.predicate);
    if (!bound) return std::unexpected(EpistemicRefusal::UnknownSource);
    const auto incarnation = static_cast<std::uint64_t>(h.incarnation);
    const auto *set        = mappingsFor(h, at);
    const std::optional<ClockMapping> mapping =
        set ? set->on(bound->policy.clock) : std::nullopt;
    if (set) maybeRepair(row, h, *set);
    if (!mapping) startPairing(row, h);
    std::optional<EpistemicRefusal> adapterRefusal;
    if (incarnation > bound->incarnation &&
        row.binding.incarnationPolicy == IncarnationPolicy::Signed) {
      if (incarnation - bound->incarnation >
          static_cast<std::uint64_t>(limits.incarnationStep)) {
        adapterRefusal = EpistemicRefusal::IncarnationStep;
      } else if (timely(h, o, bound->policy, mapping, at)) {
        if (auto advanced = advance(row, incarnation); !advanced)
          return std::unexpected(advanced.error());
        bound = executor.boundPredicate(row.pid, row.binding.principal,
                                        o.predicate);
        if (!bound) return std::unexpected(EpistemicRefusal::UnknownSource);
      }
      // Otherwise not an advance: `observe` refuses the unbound incarnation.
    }
    CheckedObservation c;
    c.principal   = row.binding.principal;
    c.incarnation = incarnation;
    // From the transport, never from the bytes (3.7).
    if (peer.principal != h.issuer) c.relayedBy = peer.principal;
    c.eventId   = o.eventId;
    c.sequence  = static_cast<std::uint64_t>(h.sequence);
    c.predicate = o.predicate;
    if (o.tuple.size() != bound->params.size()) {
      // The wrong arity is the request's shape, not a reading: `observe`
      // refuses it as InvalidObservation by its length alone.
      c.tuple.assign(o.tuple.size(), WireValue::integer(0));
    } else {
      for (std::size_t i = 0; i < o.tuple.size(); ++i) {
        auto value = typed(bound->params[i], o.tuple[i]);
        // Decided on signed bytes, so it counts like observe's own; the
        // placeholder keeps the arity, which observe checks first.
        if (!value && !adapterRefusal)
          adapterRefusal = EpistemicRefusal::InvalidValue;
        c.tuple.push_back(value ? std::move(*value) : WireValue::integer(0));
      }
    }
    c.device                = deviceTimes(h, o);
    c.mapping               = mapping;
    c.verifiedSequenceBound = static_cast<std::uint64_t>(verifiedBound(h));
    c.clockBasis            = std::string(clockName(bound->policy.clock));
    c.clockEpoch            = static_cast<std::uint64_t>(h.epoch);
    c.frame                 = o.frame;
    c.units                 = o.units;
    c.variance              = o.variance;
    c.policy                = o.policy;
    c.adapterRefusal        = adapterRefusal;

    auto receipt = executor.observe(row.pid, c);
    trackUncertainty(row, receipt, at);
    if (receipt) {
      pending.push_back({row.pid, c.principal, c.incarnation, c.eventId, h});
      if (pending.size() > kMaxPendingSettles) pending.pop_front();
    } else if (outcomeClassOf(epistemicRefusalName(receipt.error())) !=
               OutcomeClass::Retryable) {
      verifier.noteSettled(h);
    }
    return receipt;
  }
};

EpistemicAdapter::EpistemicAdapter(
    AgentExecutor &executor, AdapterLimits limits,
    const identity::IRevocationRegistry &revocations, const MasterKeys &masters)
    : state_(std::make_unique<State>(executor, std::move(limits), revocations,
                                     masters)) {
  state_->verifier.onFirstRevocation(
      [state = state_.get()](std::string_view principal) {
        state->emit({.kind      = IngressAudit::Kind::KeyRevoked,
                     .principal = std::string(principal)});
      });
}

EpistemicAdapter::~EpistemicAdapter() = default;

std::expected<EpistemicAdapter *, EpistemicRefusal>
EpistemicAdapter::bind(const identity::ActorPid &target,
                       SourceBinding binding) {
  const auto &b       = binding;
  const bool masterOk = std::holds_alternative<PinnedTrust>(b.trust) ||
                        isToken(std::get<DelegatedTrust>(b.trust).master);
  auto sorted = b.predicates;
  std::ranges::sort(sorted);
  if (b.alias.empty() || !isToken(b.principal) || !masterOk ||
      b.predicates.empty() ||
      std::ranges::adjacent_find(sorted) != sorted.end() ||
      b.lifetime <= std::chrono::steady_clock::duration::zero() ||
      b.incarnation == 0 || !std::in_range<std::int64_t>(b.incarnation) ||
      (b.driftPpm &&
       (*b.driftPpm < kMinClockDriftPpm || *b.driftPpm > kMaxClockDriftPpm)))
    return std::unexpected(EpistemicRefusal::InvalidBinding);

  const std::lock_guard lock(state_->mutex);
  auto &executor = state_->executor;
  const std::pair key{pidToText(target), b.principal};
  std::vector<std::string> done;
  for (const auto &predicate : b.predicates) {
    const EpistemicSourceGrant grant{b.alias, b.principal, b.incarnation,
                                     predicate, b.lifetime};
    if (auto bound = executor.bindSource(target, grant); !bound) {
      for (const auto &undo : done)
        executor.unbindSource(target, b.principal, undo);
      return std::unexpected(bound.error());
    }
    done.push_back(predicate);
    // Every wall mapping carries the host's own sync uncertainty, so a bound
    // at or below it could never accept a reading (m10).
    const auto bound = executor.boundPredicate(target, b.principal, predicate);
    if (bound && bound->policy.clock == ObservationClock::Wall &&
        bound->policy.maxUncertaintyMs <=
            state_->limits.wallSyncUncertaintyMs) {
      for (const auto &undo : done)
        executor.unbindSource(target, b.principal, undo);
      return std::unexpected(EpistemicRefusal::InvalidBinding);
    }
  }
  if (const auto held = state_->rows.find(key); held != state_->rows.end())
    for (const auto &old : held->second.binding.predicates)
      if (!std::ranges::contains(b.predicates, old))
        executor.unbindSource(target, b.principal, old);
  state_->rows.insert_or_assign(key, State::Row{target, std::move(binding)});
  return this;
}

EpistemicAdapter *EpistemicAdapter::unbind(const identity::ActorPid &target,
                                           std::string_view principal) {
  const std::lock_guard lock(state_->mutex);
  const auto found =
      state_->rows.find({pidToText(target), std::string(principal)});
  if (found == state_->rows.end()) return this;
  for (const auto &predicate : found->second.binding.predicates)
    state_->executor.unbindSource(target, principal, predicate);
  state_->rows.erase(found);
  state_->streaks.erase({pidToText(target), std::string(principal)});
  state_->clockPending.erase({pidToText(target), std::string(principal)});
  const bool stillBound = std::ranges::any_of(
      state_->rows, [&](const auto &r) { return r.first.second == principal; });
  if (!stillBound) {
    state_->verifier.forget(principal);
    state_->clocks.forget(principal);
    if (const auto top = state_->highest.find(principal);
        top != state_->highest.end())
      state_->highest.erase(top);
  }
  return this;
}

std::expected<TransportReceipt, EpistemicRefusal>
EpistemicAdapter::ingest(std::string_view bytes, const TransportPeer &peer) {
  std::expected<TransportReceipt, EpistemicRefusal> result;
  std::vector<IngressAudit> audits;
  std::vector<ClockChallenge> challenges;
  IngressAuditSink auditSink;
  ChallengeSink challengeSink;
  {
    const std::lock_guard lock(state_->mutex);
    result = ingestLocked(bytes, peer);
    audits.swap(state_->auditOutbox);
    challenges.swap(state_->challengeOutbox);
    auditSink     = state_->audit;
    challengeSink = state_->challengeSink;
  }
  // After the lock: a sink may call back into the adapter.
  if (auditSink)
    for (const auto &event : audits) auditSink(event);
  if (challengeSink)
    for (const auto &challenge : challenges) challengeSink(challenge);
  return result;
}

std::expected<TransportReceipt, EpistemicRefusal>
EpistemicAdapter::ingestLocked(std::string_view bytes,
                               const TransportPeer &peer) {
  auto &s = *state_;
  s.settle();
  s.followClock();
  const auto now = s.executor.hostTime();
  const auto at  = instantOf(now);
  std::optional<EpistemicRefusal> audienceRefusal;
  State::Row *row   = nullptr;
  const auto lookup = [&](const SignedHeader &h) -> std::optional<SignerTrust> {
    auto pid = s.resolve(h.audience);
    if (!pid) {
      audienceRefusal = pid.error();
      return std::nullopt;
    }
    const auto found = s.rows.find({*pid, h.issuer});
    if (found == s.rows.end()) return std::nullopt;
    row = &found->second;
    return SignerTrust{row->binding.principal, row->binding.trust};
  };
  auto message =
      s.verifier.verify(bytes, peer, lookup, {now.wall, now.wallSynchronised},
                        SteadyTime(now.steady));
  if (!message) {
    if (message.error() == EpistemicRefusal::UnknownSource && audienceRefusal)
      return std::unexpected(*audienceRefusal);
    return std::unexpected(message.error());
  }
  s.noteVerified(message->header);
  // Step 6 and dispatch by kind. A `clock_pair` reply skips step 6; a
  // fresh heartbeat or report is still refused until I3-3c1 and d dispatch it.
  switch (message->header.kind) {
  case SignedKind::Observation:
    return s.observation(*row, *message, peer, at);
  case SignedKind::ClockPair:
    return s.pairReply(*row, *message, at);
  case SignedKind::Heartbeat:
  case SignedKind::HealthReport:
    return s.liveness(*row, *message, at);
  case SignedKind::Verification:
    break;
  }
  return std::unexpected(EpistemicRefusal::UnsupportedKind);
}

EpistemicAdapter *EpistemicAdapter::setAuditSink(IngressAuditSink sink) {
  const std::lock_guard lock(state_->mutex);
  state_->audit = std::move(sink);
  return this;
}

EpistemicAdapter *EpistemicAdapter::setChallengeSink(ChallengeSink sink) {
  const std::lock_guard lock(state_->mutex);
  state_->challengeSink = std::move(sink);
  return this;
}

EpistemicAdapter *EpistemicAdapter::setNonceSource(NonceSource source) {
  const std::lock_guard lock(state_->mutex);
  state_->nonces = std::move(source);
  return this;
}

EpistemicAdapter *EpistemicAdapter::clearAudiences() {
  const std::lock_guard lock(state_->mutex);
  state_->audiences.clear();
  state_->mirrored.clear();
  return this;
}

std::optional<std::chrono::steady_clock::time_point>
EpistemicAdapter::clockPending(const identity::ActorPid &target,
                               std::string_view principal) const {
  const std::lock_guard lock(state_->mutex);
  const auto found =
      state_->clockPending.find({pidToText(target), std::string(principal)});
  return found == state_->clockPending.end()
             ? std::nullopt
             : std::optional<std::chrono::steady_clock::time_point>(
                   found->second);
}

const SignedVerifierStats &EpistemicAdapter::stats() const noexcept {
  return state_->verifier.stats();
}

const AdapterLimits &EpistemicAdapter::limits() const noexcept {
  return state_->limits;
}

} // namespace xanadu::vagent::runtime
