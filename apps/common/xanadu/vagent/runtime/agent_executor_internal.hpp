/**
 * @file agent_executor_internal.hpp
 * @brief Internal declarations of AgentExecutor::State, PostBox, Ingress and
 *        turn structures shared between agent_executor translation units.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_INTERNAL_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_INTERNAL_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/agent_executor_health.hpp"
#include "common/xanadu/vagent/runtime/checker.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/settle_discard.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {

template <class Duration>
[[nodiscard]] inline double toSeconds(Duration d) noexcept {
  return std::chrono::duration<double>(d).count();
}

struct IngestKey {
  std::string principal;
  std::uint64_t incarnation{};
  std::string eventId;
  std::string predicate;
  std::optional<std::string> relay;
  double receivedSteady{};
  std::uint64_t sequence{};
  bool counts{false};
  std::uint64_t generation{0};
};

struct IngressCharge {
  std::int64_t content{};
  std::int64_t other{};
  std::int64_t health{};
  std::int64_t passed{};
  std::string code;
  std::optional<double> contact;
};

struct SourceChange {
  std::string principal;
  std::optional<std::uint64_t> incarnation;
  bool retire{false};
};

struct AgentExecutor::Flight {
  std::uint64_t token{};
  std::string skill;
  std::string capacity;
  std::string method;
  std::optional<std::int64_t> intention;
  std::int64_t deadline{};
};

struct AgentExecutor::PendingRequest {
  bool ask{true};
  std::int64_t intention{};
  std::int64_t key{};
  std::string to;
};

struct AgentExecutor::TurnSpec {
  std::optional<zigzag::CellRef> entry;
  std::optional<std::int64_t> intention;
  std::string cause;
  const Envelope *event{};
  std::string space;
  const Envelope *relayed{};
  bool markInitialized{false};
  bool readOnly{false};
  bool deferDrain{false};
  WireRecord turnFields;
  std::function<std::optional<std::string>()> admit;
};

struct AgentExecutor::TurnOutcome {
  bool committed{false};
  std::string cause;
  std::optional<WireValue> answer;
  std::optional<std::string> result;
};

struct AgentExecutor::HostEvent {
  std::string type;
  WireRecord fields;
  std::optional<IngestKey> ingest;
  std::optional<std::string> refusalsOf;
  std::optional<IngressCharge> charge;
  std::optional<SourceChange> sourceChange;
  std::optional<QueuedHealth> health;
  std::optional<zigzag::CellRef> entry;
  WireRecord turnFields;
};

[[nodiscard]] inline bool
isControl(const AgentExecutor::HostEvent &event) noexcept {
  return event.sourceChange.has_value() || event.health.has_value();
}

[[nodiscard]] inline bool
owesAccounting(const AgentExecutor::HostEvent &event) noexcept {
  return event.ingest.has_value() || event.refusalsOf.has_value() ||
         event.sourceChange.has_value() || event.health.has_value();
}

struct AgentExecutor::State {
  identity::ActorPid pid;
  std::string name;
  std::string parentName;
  std::string agentType;
  std::string prefix;
  std::optional<identity::ActorPid> parentPid;
  std::shared_ptr<AgentProgram> program;
  CompiledAgent agent{};
  AgentDims dims{};
  ValueDims vd{};
  zigzag::vortex::WorkerContext *context{};
  bool prepared{false};
  std::map<std::string, std::string> floors;
  std::map<std::string, std::uint64_t, std::less<>> pendingStats;
  std::size_t sinceCompact{0};
  std::vector<AgentExecutor::HostEvent> hostEvents;
  bool inTurn{false};
  bool quietWake{false};
  std::optional<zigzag::vortex::mesh::ActorGrant> grant;
  zigzag::CellRef cursor{};
  std::chrono::system_clock::time_point leaseSeen;
  bool adopted{false};
  std::int64_t turnMillis{0};
  bool resumed{false};
  std::map<std::int64_t, AgentExecutor::Flight> flights;
  std::map<std::string, AgentExecutor::PendingRequest> requests;
  std::optional<std::int64_t> wakeIntention;
  std::size_t wakesSinceMessage{0};

  struct ReadyIntention {
    std::int64_t id{};
    std::int64_t since{};
  };
  enum class DeadlineKind : std::uint8_t { Waiting, Timer, Effect };
  struct Deadline {
    std::int64_t at{};
    DeadlineKind kind{};
    std::int64_t id{};
  };
  struct Schedule {
    std::vector<ReadyIntention> ready;
    std::vector<Deadline> deadlines;
  } schedule;

  std::uint64_t auditCalls{0};
  std::uint64_t auditThrows{0};
  std::uint64_t checkpointsProduced{0};
};

struct AgentExecutor::PostBox {
  struct Item {
    identity::ActorPid pid;
    bool percept{false};
    std::string type;
    WireRecord fields;
    std::optional<WireRecord> observation;
    std::optional<IngestKey> ingest;
    std::optional<std::string> refusalsOf;
    std::optional<IngressCharge> charge;
    std::optional<SourceChange> sourceChange;
    std::optional<QueuedHealth> health;
    std::optional<QueuedHealthBatch> healthBatch;
    std::int64_t effect{};
    EffectResult result;
  };
  mutable std::mutex mutex;
  std::deque<Item> items;
  std::optional<int> notifyFd;

  PostBox()                           = default;
  PostBox(const PostBox &)            = delete;
  PostBox &operator=(const PostBox &) = delete;
  ~PostBox() {
    if (notifyFd) ::close(*notifyFd);
  }

  void signalLocked() {
    if (!notifyFd) return;
    const std::uint64_t one = 1;
    (void)!::write(*notifyFd, &one, sizeof one);
  }
  void push(Item item) {
    const std::lock_guard lock(mutex);
    items.push_back(std::move(item));
    signalLocked();
  }
  [[nodiscard]] std::optional<int> descriptor() {
    const std::lock_guard lock(mutex);
    if (!notifyFd) {
#if defined(__linux__)
      const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (fd < 0) return std::nullopt;
      notifyFd = fd;
      if (!items.empty()) {
        const std::uint64_t one = 1;
        (void)!::write(fd, &one, sizeof one);
      }
#else
      return std::nullopt;
#endif
    }
    return notifyFd;
  }
  void drainDescriptor() {
    const std::lock_guard lock(mutex);
    if (!notifyFd) return;
    std::uint64_t count = 0;
    while (::read(*notifyFd, &count, sizeof count) > 0) {
    }
  }
  std::deque<Item> take() {
    const std::lock_guard lock(mutex);
    return std::exchange(items, {});
  }
  void pushControl(Item item);
  void putBack(std::vector<Item> kept);
  bool attachCharge(const identity::ActorPid &pid, const std::string &principal,
                    IngressCharge charge) {
    const std::lock_guard lock(mutex);
    for (auto &item : items)
      if (item.pid == pid && item.refusalsOf == principal && !item.charge) {
        item.charge = std::move(charge);
        return true;
      }
    return false;
  }
  bool pendingFor(const identity::ActorPid &pid) const {
    const std::lock_guard lock(mutex);
    return std::ranges::any_of(
        items, [&](const Item &item) { return item.pid == pid; });
  }
};

[[nodiscard]] inline bool
isControl(const AgentExecutor::PostBox::Item &item) noexcept {
  return item.sourceChange.has_value() || item.health.has_value() ||
         item.healthBatch.has_value();
}

[[nodiscard]] inline bool
owesAccounting(const AgentExecutor::PostBox::Item &item) noexcept {
  return item.ingest.has_value() || item.refusalsOf.has_value() ||
         item.sourceChange.has_value() || item.health.has_value() ||
         item.healthBatch.has_value();
}

inline void AgentExecutor::PostBox::pushControl(Item item) {
  const std::lock_guard lock(mutex);
  const auto after = std::ranges::find_if_not(
      items, [](const Item &i) { return isControl(i); });
  items.insert(after, std::move(item));
  signalLocked();
}

inline void AgentExecutor::PostBox::putBack(std::vector<Item> kept) {
  const std::lock_guard lock(mutex);
  std::ranges::stable_partition(kept,
                                [](const Item &i) { return isControl(i); });
  items.insert(items.begin(), std::make_move_iterator(kept.begin()),
               std::make_move_iterator(kept.end()));
  std::ranges::stable_partition(items,
                                [](const Item &i) { return isControl(i); });
}

struct AgentExecutor::Ingress {
  struct Binding {
    std::string alias;
    std::uint64_t incarnation{};
    zigzag::vortex::mesh::ActorTime expiresAt;
    ArbitrationPolicy policy;
    std::vector<Type::Kind> params;
    /// The predicate's name and arity with its policy hash, stamped on
    /// every record ingested under the binding.
    std::string domain;
  };
  /// One event's entry in its source's outcome log.
  struct Logged {
    std::uint64_t incarnation{};
    std::string eventId;
    IngestOutcome outcome;
    /// The refusal this event was last charged for, kept while the entry
    /// moves on, so a resend meeting it again is not charged twice.
    std::optional<std::string> charged;
    /// Whether a directly received copy was among those charged: a relayed
    /// copy charged first must not hide the device's own.
    bool chargedDirect{false};
  };
  /// The lowest incarnation a new binding may present, as far as the host
  /// knows: the arena's floor reaches the host only through the turns that
  /// set it, so the advance turn still has the last word.
  struct Floor {
    std::uint64_t incarnation{};
    /// A tombstone's or a refused advance's floor must be exceeded; a live
    /// profile's may be met again (a rebind of the same boot).
    bool strict{false};
  };
  using SourceKey = std::pair<std::string, std::string>; // pid, principal

  mutable std::mutex mutex;
  /// By pid, principal and predicate.
  std::map<std::tuple<std::string, std::string, std::string>, Binding> bindings;
  /// By pid and alias: the principal an alias routes to.
  std::map<SourceKey, std::string> aliases;
  /// Ingestions queued or running per actor, per origin principal and per
  /// relay, counted until their turn completes.
  std::map<std::string, std::size_t> queued;
  std::map<SourceKey, std::size_t> queuedBy;
  std::map<SourceKey, std::size_t> queuedVia;
  /// Per source, so no source's flood evicts another's outcomes.
  std::map<SourceKey, std::deque<Logged>> outcomes;
  std::map<SourceKey, Floor> floors;
  /// The newest advance or retire turn's outcome per source.
  std::map<SourceKey, IngestOutcome> changes;
  /// Health grants by pid (P19 I3-2c1), host memory lost with the host.
  std::map<std::string, std::vector<StoredHealthGrant>> healthGrants;
  /// The reset floor per source: the in-flight snapshot a reset decides on.
  std::map<SourceKey, ResetMark> resetMarks;
  /// Readings in flight per (pid, principal) and predicate, a bounded
  /// multiset the reset call snapshots.
  std::map<SourceKey, std::map<std::string, std::multiset<std::int64_t>>>
      inFlight;
  /// A reset call that would change something takes the next generation;
  /// readings queued after it carry that generation, those before carry less.
  std::uint64_t requestReset(const SourceKey &source) {
    return ++resetMarks[source].requested;
  }
  /// Raised only by a committed reset, so a failed, revoked, stubbed or
  /// `NoChange` reset supersedes nothing (F19).
  void commitReset(const SourceKey &source, std::uint64_t generation) {
    auto &mark     = resetMarks[source];
    mark.committed = std::max(mark.committed, generation);
  }
  /// Whether a reading queued at @p generation predates a committed reset.
  [[nodiscard]] bool supersededByReset(const SourceKey &source,
                                       std::uint64_t generation) const {
    const auto found = resetMarks.find(source);
    return found != resetMarks.end() && generation < found->second.committed;
  }
  using Charge = IngressCharge;
  std::map<SourceKey, Charge> charges;
  /// A stream's estimator state (design 3.2): the newest sequence a commit or
  /// a pass credit moved the estimator past, and the sequences of counted
  /// refusals above it. Host memory, lost with the host.
  struct EstimatorMark {
    /// Raised only by a commit or a pass credit, never by a refusal: a
    /// refusal's sequence is any Int64 the device chose, so a mark it raised
    /// could freeze the estimator for the life of the incarnation.
    std::optional<std::uint64_t> mark;
    /// Oldest first; at most `healthReplayRing`.
    std::deque<std::uint64_t> counted;
  };
  using StreamKey =
      std::tuple<std::string, std::string, std::uint64_t, std::string>;
  std::map<StreamKey, EstimatorMark> marks;
  /// Chargeable refusals the estimator did not count since the source last
  /// committed.
  std::map<SourceKey, std::uint64_t> uncounted;
  /// Unknown until the host's time service reports: a wall-basis reading
  /// taken before then cannot be placed in time.
  bool wallSynchronised{false};
  /// Names this executor's steady clock: received_steady and last_contact
  /// stamps are comparable only between records that carry the same one.
  std::string clockInstance = [] {
    std::random_device device;
    std::uint64_t bits = (std::uint64_t{device()} << 32) | device();
    char out[17];
    std::snprintf(out, sizeof out, "%016llx",
                  static_cast<unsigned long long>(bits));
    return std::string(out);
  }();

  /// One entry per event, updated in place as the event moves on. A
  /// committed event stays committed: a later resend, relay or refusal of
  /// the same key is a retry of an event the store already holds.
  void log(const SourceKey &source, std::size_t bound,
           std::uint64_t incarnation, std::string_view eventId,
           IngestOutcome outcome) {
    auto &q = outcomes[source];
    for (auto &entry : q)
      if (entry.incarnation == incarnation && entry.eventId == eventId) {
        if (entry.outcome.kind != IngestOutcome::Kind::Committed &&
            entry.outcome.kind != IngestOutcome::Kind::Duplicate)
          entry.outcome = std::move(outcome);
        return;
      }
    q.push_back({incarnation, std::string(eventId), std::move(outcome)});
    while (q.size() > bound) q.pop_front();
  }
  [[nodiscard]] const Logged *find(const SourceKey &source,
                                   std::uint64_t incarnation,
                                   std::string_view eventId) const {
    const auto q = outcomes.find(source);
    if (q == outcomes.end()) return nullptr;
    for (const auto &entry : q->second)
      if (entry.incarnation == incarnation && entry.eventId == eventId)
        return &entry;
    return nullptr;
  }
  /// Whether a refusal of this event is charged: once per code, and once
  /// more for the first direct copy after relayed ones. Call after logging
  /// the event, so its entry exists.
  bool firstCharge(const SourceKey &source, std::uint64_t incarnation,
                   std::string_view eventId, std::string_view code,
                   bool relayed) {
    const auto q = outcomes.find(source);
    if (q == outcomes.end()) return true;
    for (auto &entry : q->second) {
      if (entry.incarnation != incarnation || entry.eventId != eventId)
        continue;
      if (entry.charged == code && (entry.chargedDirect || relayed))
        return false;
      entry.charged = std::string(code);
      entry.chargedDirect |= !relayed;
      return true;
    }
    return true;
  }
  /// Whether the estimator would count a reading of @p stream at @p sequence:
  /// above the mark (or the stream has none) and not already counted.
  [[nodiscard]] bool estimatorCounts(const StreamKey &stream,
                                     std::uint64_t sequence) const {
    const auto found = marks.find(stream);
    if (found == marks.end()) return true;
    const auto &m = found->second;
    if (m.mark && sequence <= *m.mark) return false;
    return std::ranges::find(m.counted, sequence) == m.counted.end();
  }
  /// Remember a counted refusal's sequence, the oldest leaving at @p ring.
  void rememberCounted(const StreamKey &stream, std::uint64_t sequence,
                       std::size_t ring) {
    auto &counted = marks[stream].counted;
    counted.push_back(sequence);
    while (counted.size() > ring) counted.pop_front();
  }
  /// Raise the mark (a commit or a credit) and drop the refusals it passed.
  void raiseMark(const StreamKey &stream, std::uint64_t sequence) {
    auto &m = marks[stream];
    if (!m.mark || sequence > *m.mark) m.mark = sequence;
    std::erase_if(m.counted, [&](auto s) { return s <= *m.mark; });
  }
  /// The share of the actor queue one principal or relay may hold: the
  /// per-source bound, shrunk so every bound source keeps a slot.
  [[nodiscard]] std::size_t share(const std::string &pid, std::size_t queue,
                                  std::size_t perSource) const {
    std::set<std::string> sources;
    for (const auto &[key, binding] : bindings)
      if (std::get<0>(key) == pid) sources.insert(std::get<1>(key));
    const auto fair = queue / std::max<std::size_t>(sources.size(), 1);
    return std::clamp<std::size_t>(fair, 1, perSource);
  }
  /// Release one queued ingestion's slots.
  void release(const std::string &pid, const std::string &principal,
               const std::optional<std::string> &relay) {
    const auto down = [](auto &map, const auto &key) {
      if (const auto it = map.find(key); it != map.end() && --it->second == 0)
        map.erase(it);
    };
    down(queued, pid);
    down(queuedBy, SourceKey{pid, principal});
    if (relay) down(queuedVia, SourceKey{pid, *relay});
  }
  /// Unbind one (principal, predicate), keeping its alias while another
  /// binding of the same principal still routes by it.
  void unbind(const std::string &pid, const std::string &principal,
              const std::string &predicate) {
    const auto found = bindings.find({pid, principal, predicate});
    if (found == bindings.end()) return;
    const auto alias = found->second.alias;
    bindings.erase(found);
    const bool stillUsed = std::ranges::any_of(bindings, [&](const auto &b) {
      return std::get<0>(b.first) == pid && std::get<1>(b.first) == principal &&
             b.second.alias == alias;
    });
    if (!stillUsed) aliases.erase({pid, alias});
  }
  void unbindAll(const std::string &pid, const std::string &principal) {
    std::vector<std::string> predicates;
    for (const auto &[key, binding] : bindings)
      if (std::get<0>(key) == pid && std::get<1>(key) == principal)
        predicates.push_back(std::get<2>(key));
    for (const auto &predicate : predicates) unbind(pid, principal, predicate);
  }
  /// Drop everything held for a stopped actor: its bindings authorize
  /// nothing now, and nothing will query or drain its queues again.
  void forget(const std::string &pid) {
    std::erase_if(bindings,
                  [&](const auto &b) { return std::get<0>(b.first) == pid; });
    const auto mine = [&](const auto &entry) {
      return entry.first.first == pid;
    };
    std::erase_if(aliases, mine);
    queued.erase(pid);
    std::erase_if(queuedBy, mine);
    std::erase_if(queuedVia, mine);
    std::erase_if(outcomes, mine);
    std::erase_if(floors, mine);
    std::erase_if(changes, mine);
    std::erase_if(charges, mine);
    healthGrants.erase(pid);
    std::erase_if(resetMarks, mine);
    std::erase_if(inFlight, mine);
    std::erase_if(uncounted, mine);
    std::erase_if(marks, [&](const auto &entry) {
      return std::get<0>(entry.first) == pid;
    });
  }
  /// Record one refusal against @p principal; true when it is the first
  /// since the last bookkeeping turn, so the caller posts a note for it.
  /// @p lifetime is `firstCharge`'s answer and feeds the lifetime counters and
  /// the contact stamp; @p health is the estimator's own, already gated by the
  /// stream's mark (R3-3): the two never share a mechanism.
  bool charge(const std::string &pid, const std::string &principal,
              std::string_view code, bool relayed, double receivedSteady,
              bool lifetime, bool health) {
    if (!chargeable(code) || (!lifetime && !health)) return false;
    auto [it, fresh] = charges.try_emplace({pid, principal});
    auto &c          = it->second;
    if (lifetime) {
      if (!relayed && isContentRefusal(code)) {
        ++c.content;
        c.contact =
            std::max(c.contact.value_or(receivedSteady), receivedSteady);
      } else {
        ++c.other;
      }
    }
    if (health) ++c.health;
    c.code = std::string(code);
    return fresh;
  }
  /// Frame, unit, variance and time refusals: the device demonstrably sent
  /// what the policy cannot use. Charging is host bookkeeping about the
  /// transport (which counter a refusal lands in), not a judgement of the
  /// reading, so it stays with the ingress that classified the refusal.
  static bool isContentRefusal(std::string_view code) {
    return code == "ObservationFrameMismatch" || code == "MissingVariance" ||
           code == "InvalidVariance" || code == "ObservationTimeMismatch" ||
           code == "UncertainAcquisition" || code == "StaleObservation" ||
           code == "InvalidValue" || code == "InvalidAcquisition" ||
           code == kTurnResultClockEpochRegression ||
           code == kTurnResultClockMappingRegression;
  }
  /// Whether a refusal moves the estimator (design 3.1): decided on the
  /// device's own bytes or epoch. A relayed copy counts only when decided on
  /// signed fields, so a hostile relay cannot fabricate refusals.
  static bool isHealthRefusal(std::string_view code, bool relayed) {
    return isHealthRefusalCode(code, relayed);
  }
  /// The host's own conditions, its pending actions and runtime defects are
  /// never the source's.
  static bool chargeable(std::string_view code) {
    return code != "ClockUnsynchronised" && code != "IngressBackpressure" &&
           code != "UnknownSource" && code != kTurnResultIngestionFault &&
           code != kTurnResultTurnExhausted &&
           code != kTurnResultArenaExhausted &&
           code != kTurnResultEpistemicBudget &&
           code != kTurnResultUnboundIncarnation &&
           code != kTurnResultUndelivered && code != kSettleFailedLabel &&
           code != "SourceFaulted" && code != "SupersededByReset" &&
           code != "HealthUnavailable" && code != "UnauthorizedHealthAction";
  }
};

struct AgentExecutor::OutboxItem {
  std::string kind;
  // event
  Envelope envelope;
  std::string space;
  std::vector<std::string> forNames;
  bool hasFor{false};
  // spawn
  std::string agentType;
  std::string childName;
  WireRecord args;
  // stop
  bool keepChildren{false};
  // timer, timer_cancel
  std::int64_t timerId{0};
  std::int64_t delayMs{0};
  std::int64_t periodMs{0};
  // log
  std::string level;
  std::string message;
  // effect, effect_cancel
  std::int64_t effectId{0};
  std::string skill;
  std::string capacity;
  std::string method;
  WireList effectArgs;
  std::int64_t deadlineMs{0};
  std::optional<std::int64_t> intention;
  bool preempts{false};
  // ask, achieve, cancel, outcome, refuse
  std::int64_t reqId{0};
  std::string reqTo;
  std::string reqName;
  WireRecord reqArgs;
  std::int64_t reqDeadlineMs{0};
  std::string correlation;
  std::string status;
  std::string cause;
};

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_INTERNAL_HPP
