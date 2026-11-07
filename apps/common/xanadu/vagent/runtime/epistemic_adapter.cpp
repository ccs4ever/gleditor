#include "common/xanadu/vagent/runtime/epistemic_adapter.hpp"

#include <gleditor/logging.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
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

double seconds(std::int64_t nanoseconds) {
  return static_cast<double>(nanoseconds) / 1e9;
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
  }
  return "Unknown";
}

std::expected<void, AdapterLimitError> AdapterLimits::validate() const {
  if (!verify.validate())
    return std::unexpected(AdapterLimitError::InvalidSignedIngress);
  if (incarnationStep < kMinIncarnationStep ||
      incarnationStep > kMaxIncarnationStep)
    return std::unexpected(AdapterLimitError::InvalidIncarnationStep);
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
        verifier(limits.verify, revocations, masters) {}

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

  AgentExecutor &executor;
  AdapterLimits limits;
  SignedVerifier verifier;
  std::mutex mutex;
  /// By (pid text, principal).
  std::map<std::pair<std::string, std::string>, Row> rows;
  /// Audience to pid text, and the pids already read.
  std::map<std::string, std::string, std::less<>> audiences;
  std::set<std::string> mirrored;
  std::deque<Pending> pending;
  IngressAuditSink audit;

  void emit(const IngressAudit &event) const {
    if (audit) audit(event);
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

  /// 3.10's timeliness condition under a2's mapping: a reading the path made
  /// old, or one sent in the future, cannot advance anything.
  static bool timely(const SignedHeader &h, const ObservationPayload &o,
                     const ArbitrationPolicy &policy,
                     const AgentExecutor::HostTime &now) {
    if (!now.wallSynchronised) return false;
    const double receipt =
        std::chrono::duration<double>(now.wall.time_since_epoch()).count();
    const double u =
        seconds(o.acquiredUncertainty) + seconds(h.clockUncertainty);
    const double age  = static_cast<double>(policy.maxAgeMs) / 1000.0;
    const double skew = static_cast<double>(policy.clockSkewMs) / 1000.0;
    return receipt - seconds(o.acquired) <= age + u &&
           seconds(h.sent) <= receipt + seconds(h.clockUncertainty) + skew;
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
              const AgentExecutor::HostTime &now) {
    const auto &h = message.header;
    const auto &o = std::get<ObservationPayload>(message.payload);
    if (!std::ranges::contains(row.binding.predicates, o.predicate))
      return std::unexpected(EpistemicRefusal::UnknownSource);
    auto bound =
        executor.boundPredicate(row.pid, row.binding.principal, o.predicate);
    if (!bound) return std::unexpected(EpistemicRefusal::UnknownSource);
    const auto incarnation = static_cast<std::uint64_t>(h.incarnation);
    std::optional<EpistemicRefusal> adapterRefusal;
    if (incarnation > bound->incarnation &&
        row.binding.incarnationPolicy == IncarnationPolicy::Signed) {
      if (incarnation - bound->incarnation >
          static_cast<std::uint64_t>(limits.incarnationStep)) {
        adapterRefusal = EpistemicRefusal::IncarnationStep;
      } else if (timely(h, o, bound->policy, now)) {
        if (auto advanced = advance(row, incarnation); !advanced)
          return std::unexpected(advanced.error());
        bound = executor.boundPredicate(row.pid, row.binding.principal,
                                        o.predicate);
        if (!bound) return std::unexpected(EpistemicRefusal::UnknownSource);
      }
      // Otherwise not an advance: `observe` refuses the unbound incarnation.
    }
    // Step 6 before I3-3b1: a synchronised device clock on a wall policy maps
    // with an offset of zero; nothing else has a mapping yet.
    if (bound->policy.clock != ObservationClock::Wall ||
        h.basis != SignedClockBasis::Synchronised)
      return std::unexpected(EpistemicRefusal::UnknownClockMapping);

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
    c.acquiredAt          = seconds(o.acquired);
    c.acquiredUncertainty = seconds(o.acquiredUncertainty + h.clockUncertainty);
    c.clockBasis          = std::string(clockName(ObservationClock::Wall));
    c.clockEpoch          = static_cast<std::uint64_t>(h.epoch);
    c.clockMappingVersion = 1;
    c.frame               = o.frame;
    c.units               = o.units;
    c.variance            = o.variance;
    c.policy              = o.policy;
    c.adapterRefusal      = adapterRefusal;

    auto receipt = executor.observe(row.pid, c);
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
      b.incarnation == 0 || !std::in_range<std::int64_t>(b.incarnation))
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
  const bool stillBound = std::ranges::any_of(
      state_->rows, [&](const auto &r) { return r.first.second == principal; });
  if (!stillBound) state_->verifier.forget(principal);
  return this;
}

std::expected<TransportReceipt, EpistemicRefusal>
EpistemicAdapter::ingest(std::string_view bytes, const TransportPeer &peer) {
  const std::lock_guard lock(state_->mutex);
  auto &s = *state_;
  s.settle();
  const auto now = s.executor.hostTime();
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
  // Heartbeats, reports and clock pairs are dispatched by I3-3b1, c1 and d;
  // until then a verified one is refused and its header is not noted.
  if (message->header.kind != SignedKind::Observation)
    return std::unexpected(EpistemicRefusal::UnsupportedKind);
  return s.observation(*row, *message, peer, now);
}

EpistemicAdapter *EpistemicAdapter::setAuditSink(IngressAuditSink sink) {
  const std::lock_guard lock(state_->mutex);
  state_->audit = std::move(sink);
  return this;
}

EpistemicAdapter *EpistemicAdapter::clearAudiences() {
  const std::lock_guard lock(state_->mutex);
  state_->audiences.clear();
  state_->mirrored.clear();
  return this;
}

const SignedVerifierStats &EpistemicAdapter::stats() const noexcept {
  return state_->verifier.stats();
}

const AdapterLimits &EpistemicAdapter::limits() const noexcept {
  return state_->limits;
}

} // namespace xanadu::vagent::runtime
