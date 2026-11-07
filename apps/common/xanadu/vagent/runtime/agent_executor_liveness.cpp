/**
 * @file agent_executor_liveness.cpp
 * @brief The host side of liveness (P19 I3-3c1, `i3-3-design.md` 5.1 to 5.4):
 *        the deadline heap's arming, the timer wake that posts one liveness
 *        item per agent, what a fresh heartbeat writes, and the deadlines a
 *        resume re-derives from the rebased stamps.
 *
 * Why C++ and not Vortex (design 11): a deadline is a comparison of the host's
 * steady clock with a wake the embedder drives, and a heartbeat arrives
 * through the adapter's authorization boundary. What the flags mean in the
 * arena is IR (`epistemic_emit_liveness.cpp`); this only decides when to ask
 * for it.
 *
 * Nothing here moves `R` or `N`, adds a reason or changes the displayed
 * state: a flag is read beside the state (design 7).
 */
#include <gleditor/logging.hpp>

#include <algorithm>
#include <cmath>

#include "common/xanadu/vagent/runtime/agent_executor_internal.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_liveness.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

namespace {

constexpr const char *kLog = "vagent.ingress";

LivenessTime timeOf(const double seconds) {
  return LivenessTime(std::chrono::duration_cast<LivenessTime::duration>(
      std::chrono::duration<double>(seconds)));
}

double secondsOf(const LivenessTime t) {
  return std::chrono::duration<double>(t.time_since_epoch()).count();
}

std::string_view kindName(const LivenessDue::Kind kind) {
  switch (kind) {
  case LivenessDue::Kind::Silent:
    return kLivenessKindSilent;
  case LivenessDue::Kind::Stale:
    return kLivenessKindStale;
  case LivenessDue::Kind::Clear:
    return kLivenessKindClear;
  }
  return kLivenessKindSilent;
}

/// The arena's liveness view of one profile, read the way `audienceOf` reads
/// the audience: through the cells, with no turn.
struct ArenaLiveness {
  std::optional<double> receive;
  std::optional<std::string> instance;
  bool silent{false};
  bool latched{false};
  std::map<std::string, double> streams;
};

ArenaLiveness readArena(const zigzag::ArenaManifold &arena, const ValueDims &vd,
                        const zigzag::CellRef profile) {
  ArenaLiveness out;
  const auto real = [&](zigzag::CellRef owner, std::string_view name) {
    const auto cell = fieldValue(arena, vd, owner, name);
    return cell ? arena.asDouble(*cell) : std::nullopt;
  };
  const auto flag = [&](zigzag::CellRef owner, std::string_view name) {
    const auto cell = fieldValue(arena, vd, owner, name);
    return cell && arena.asBool(*cell).value_or(false);
  };
  out.receive = real(profile, kProfileLastReceive);
  if (const auto cell =
          fieldValue(arena, vd, profile, kProfileLivenessInstance))
    out.instance = arena.textOf(*cell);
  out.silent  = flag(profile, kProfileSilent);
  out.latched = flag(profile, kProfileFaulted);
  if (const auto streams = fieldValue(arena, vd, profile, kProfileStreams))
    for (const auto &[name, value] : fieldsOf(arena, vd, *streams))
      if (value)
        if (const auto at = real(*value, kStreamReceived))
          out.streams.emplace(arena.textOf(name), *at);
  return out;
}

std::optional<zigzag::CellRef> profileIn(const zigzag::ArenaManifold &arena,
                                         const zigzag::CellRef agent,
                                         const zigzag::DimRef trust,
                                         std::string_view principal) {
  for (const auto profile : zigzag::rankAfter(arena, agent, trust))
    if (arena.textOf(profile) == principal) return profile;
  return std::nullopt;
}

} // namespace

// -- Arming (ingress mutex held) ---------------------------------------------

std::optional<std::chrono::milliseconds>
AgentExecutor::heartbeatOfLocked(const std::string &pid,
                                 const std::string &principal) const {
  std::optional<std::chrono::milliseconds> least;
  const auto now = steadyNow();
  for (auto it =
           ingress_->bindings.lower_bound({pid, principal, std::string{}});
       it != ingress_->bindings.end() && std::get<0>(it->first) == pid &&
       std::get<1>(it->first) == principal;
       ++it) {
    if (now > it->second.expiresAt || !it->second.heartbeat) continue;
    least =
        least ? std::min(*least, *it->second.heartbeat) : *it->second.heartbeat;
  }
  return least;
}

void AgentExecutor::armProfileLocked(const std::string &pid,
                                     const std::string &principal,
                                     const LivenessTime from, bool keepLater) {
  const Ingress::SourceKey source{pid, principal};
  const LivenessKey key{pid, principal, std::nullopt};
  const auto beat = heartbeatOfLocked(pid, principal);
  // A latched source arms nothing (design 5.3): its flags stay as they were.
  if (!beat || ingress_->latched.contains(source) ||
      ingress_->live[source].latched) {
    ingress_->liveHeap.remove(key);
    return;
  }
  const auto &src = ingress_->live[source];
  ingress_->liveHeap.arm(
      key, from + std::chrono::duration_cast<LivenessTime::duration>(*beat),
      src.arenaStamp, keepLater);
}

void AgentExecutor::armStreamLocked(const std::string &pid,
                                    const std::string &principal,
                                    const std::string &predicate,
                                    const LivenessTime from,
                                    const double stamp) {
  const LivenessKey key{pid, principal, predicate};
  const auto binding = ingress_->bindings.find({pid, principal, predicate});
  if (binding == ingress_->bindings.end() || !binding->second.data ||
      steadyNow() > binding->second.expiresAt ||
      ingress_->latched.contains({pid, principal}) ||
      ingress_->live[{pid, principal}].latched) {
    ingress_->liveHeap.remove(key);
    return;
  }
  ingress_->liveHeap.arm(key,
                         from +
                             std::chrono::duration_cast<LivenessTime::duration>(
                                 *binding->second.data),
                         stamp, false);
}

void AgentExecutor::livenessCommittedLocked(const std::string &pid,
                                            const IngestKey &ingest) {
  const Ingress::SourceKey source{pid, ingest.principal};
  auto &src        = ingress_->live[source];
  const auto from  = timeOf(ingest.receivedSteady);
  src.arenaStamp   = ingest.receivedSteady;
  src.lastFresh    = std::max(src.lastFresh.value_or(from), from);
  src.silent       = false;
  src.clockPending = std::nullopt;
  armProfileLocked(pid, ingest.principal, from, true);
  armStreamLocked(pid, ingest.principal, ingest.predicate, from,
                  ingest.receivedSteady);
}

void AgentExecutor::livenessLatchedLocked(const std::string &pid,
                                          const std::string &principal) {
  ingress_->liveHeap.removeSource(pid, principal);
}

void AgentExecutor::livenessBoundLocked(const identity::ActorPid &target,
                                        const std::string &principal) {
  const auto pid = pidKey(target);
  const Ingress::SourceKey source{pid, principal};
  auto &src = ingress_->live[source];
  if (const auto found = states_.find(pid);
      found != states_.end() && found->second->prepared &&
      found->second->context && !found->second->inTurn &&
      live(*found->second)) {
    const State &state = *found->second;
    if (const auto profile =
            profileIn(state.context->arena, state.agent.agentCell,
                      state.dims.trust, principal)) {
      const auto held = readArena(state.context->arena, state.vd, *profile);
      src.silent      = held.silent;
      // A stamp of another instance cannot be placed until the rebase turn
      // has run; its deadline is armed then (`rearmFromArena`).
      if (held.instance == ingress_->clockInstance)
        src.arenaStamp = held.receive;
      src.latched = held.latched;
      if (held.instance == ingress_->clockInstance && !held.latched)
        for (const auto &[predicate, at] : held.streams)
          armStreamLocked(pid, principal, predicate, steadyNow(), at);
    }
  }
  if (src.silent) return;
  // The first deadline is the bind plus the expectation; a source already
  // being timed keeps its own.
  if (ingress_->liveHeap.find({pid, principal, std::nullopt}) == nullptr)
    armProfileLocked(pid, principal, steadyNow(), false);
}

void AgentExecutor::livenessResetLocked(const identity::ActorPid &target,
                                        const std::string &principal) {
  const auto pid = pidKey(target);
  const Ingress::SourceKey source{pid, principal};
  auto &src        = ingress_->live[source];
  src.silent       = false;
  src.lastFresh    = std::nullopt;
  src.arenaStamp   = std::nullopt;
  src.clockPending = std::nullopt;
  src.latched      = false;
  ingress_->liveHeap.removeSource(pid, principal);
  // UNHEARD again: the heartbeat deadline restarts from the reset, and each
  // stream's data deadline from the reset too, anchored on the `received`
  // the reset kept.
  const auto now = steadyNow();
  armProfileLocked(pid, principal, now, false);
  const auto found = states_.find(pid);
  if (found == states_.end() || !found->second->context ||
      !live(*found->second))
    return;
  const State &state = *found->second;
  if (const auto profile =
          profileIn(state.context->arena, state.agent.agentCell,
                    state.dims.trust, principal))
    for (const auto &[predicate, at] :
         readArena(state.context->arena, state.vd, *profile).streams)
      armStreamLocked(pid, principal, predicate, now, at);
}

void AgentExecutor::rearmFromArena(State &state) {
  const auto pid = pidKey(state.pid);
  const std::lock_guard lock(ingress_->mutex);
  std::set<std::string> principals;
  for (const auto &[key, binding] : ingress_->bindings)
    if (std::get<0>(key) == pid) principals.insert(std::get<1>(key));
  const auto now = steadyNow();
  for (const auto &principal : principals) {
    const auto profile = profileIn(state.context->arena, state.agent.agentCell,
                                   state.dims.trust, principal);
    if (!profile) continue;
    const Ingress::SourceKey source{pid, principal};
    const auto held = readArena(state.context->arena, state.vd, *profile);
    auto &src       = ingress_->live[source];
    src.silent      = held.silent;
    src.arenaStamp =
        held.instance == ingress_->clockInstance ? held.receive : std::nullopt;
    src.latched = held.latched;
    ingress_->liveHeap.removeSource(pid, principal);
    if (held.silent || held.latched) continue;
    // Never from the bind time when the arena holds a stamp: a stamp that is
    // old enough puts the deadline in the past, and the source goes silent at
    // the next wake, as it would have had the host not restarted.
    armProfileLocked(pid, principal,
                     src.arenaStamp ? timeOf(*src.arenaStamp) : now, false);
    if (held.instance == ingress_->clockInstance)
      for (const auto &[predicate, at] : held.streams)
        armStreamLocked(pid, principal, predicate, timeOf(at), at);
  }
}

// -- The timer wake -----------------------------------------------------------

void AgentExecutor::postLivenessLocked(const identity::ActorPid &target,
                                       std::vector<LivenessDue> due) {
  if (due.empty()) return;
  WireRecord items;
  std::size_t position = 0;
  for (const auto &d : due) {
    WireRecord item;
    item.set(std::string(kLivenessKind),
             WireValue::text(std::string(kindName(d.kind))));
    item.set(std::string(kLivenessSource), WireValue::text(d.principal));
    if (d.predicate)
      item.set(std::string(kLivenessPredicate), WireValue::text(*d.predicate));
    if (d.stamp)
      item.set(std::string(kLivenessStamp), WireValue::real(*d.stamp));
    if (d.kind == LivenessDue::Kind::Silent) {
      item.set(std::string(kLivenessCause), WireValue::text(d.cause));
      // The binding's incarnation, for a source with no profile yet.
      std::optional<std::uint64_t> bound;
      const auto pidText = pidKey(target);
      for (auto it = ingress_->bindings.lower_bound(
               {pidText, d.principal, std::string{}});
           it != ingress_->bindings.end() &&
           std::get<0>(it->first) == pidText &&
           std::get<1>(it->first) == d.principal;
           ++it)
        bound = std::max(bound.value_or(0), it->second.incarnation);
      if (bound)
        item.set(std::string(kLivenessIncarnation),
                 WireValue::integer(static_cast<std::int64_t>(*bound)));
    }
    items.set(std::to_string(position++), WireValue::record(std::move(item)));
  }
  WireRecord record;
  record.set(std::string(kLivenessNow),
             WireValue::real(secondsOf(steadyNow())));
  record.set(std::string(kLivenessItems), WireValue::record(std::move(items)));
  record.set(std::string(kLivenessMaxSources),
             WireValue::integer(
                 static_cast<std::int64_t>(settings_.epistemic.maxSources)));
  WireRecord fields;
  fields.set(std::string(kTurnLiveness), WireValue::record(std::move(record)));
  PostBox::Item item;
  item.pid       = target;
  item.clockTurn = ClockTurn{std::move(fields), std::nullopt, false};
  posts_->pushControl(std::move(item));
  ++ingress_->livenessItems;
}

void AgentExecutor::collectLiveness() {
  const std::lock_guard lock(ingress_->mutex);
  const auto now = steadyNow();
  auto due       = ingress_->liveHeap.popDue(now);
  if (due.empty()) return;
  const auto retry =
      std::chrono::milliseconds(settings_.epistemic.heartbeatFloorMs);
  std::map<std::string, std::vector<LivenessDue>> byPid;
  for (const auto &[key, entry] : due) {
    const Ingress::SourceKey source{key.pid, key.principal};
    auto &src = ingress_->live[source];
    if (ingress_->latched.contains(source) || src.latched) continue;
    LivenessDue d;
    d.principal = key.principal;
    d.stamp     = entry.stamp;
    if (key.predicate) {
      const auto binding =
          ingress_->bindings.find({key.pid, key.principal, *key.predicate});
      if (binding == ingress_->bindings.end() || !binding->second.data)
        continue;
      d.kind      = LivenessDue::Kind::Stale;
      d.predicate = key.predicate;
    } else {
      if (src.silent || !heartbeatOfLocked(key.pid, key.principal)) continue;
      d.kind  = LivenessDue::Kind::Silent;
      d.cause = std::string(src.clockPending ? kSilentCauseClock
                                             : kSilentCauseHeartbeat);
    }
    byPid[key.pid].push_back(std::move(d));
  }
  for (auto &[pidText, items] : byPid) {
    const auto target = pidFromText(pidText);
    // The door is closed for an upgrade's swap: the items would land between
    // its count of the post box and the swap. Try again when it can open.
    if (!target || admissionClosedLocked(*target)) {
      for (const auto &d : items) {
        if (d.kind == LivenessDue::Kind::Stale)
          ingress_->liveHeap.arm({pidText, d.principal, d.predicate},
                                 now + retry, d.stamp, false);
        else
          ingress_->liveHeap.arm({pidText, d.principal, std::nullopt},
                                 now + retry, d.stamp, false);
      }
      continue;
    }
    GLEDITOR_LOG_DEBUG(kLog, "liveness: {} deadline(s) due for one agent",
                       items.size());
    // One item per agent (S-13c), or more only when a turn's scratch could not
    // hold the entries: a source never heard makes a profile in the turn.
    std::vector<LivenessDue> chunk;
    std::size_t bytes = 0;
    for (auto &d : items) {
      const auto &src = ingress_->live[{pidText, d.principal}];
      const auto cost = d.kind == LivenessDue::Kind::Silent && !src.arenaStamp
                            ? kLivenessCreateBytes
                            : kLivenessEntryBytes;
      if (!chunk.empty() && bytes + cost > kLivenessItemBudgetBytes) {
        postLivenessLocked(*target, std::exchange(chunk, {}));
        bytes = 0;
      }
      bytes += cost;
      if (d.kind == LivenessDue::Kind::Silent)
        ingress_->live[{pidText, d.principal}].silent = true;
      chunk.push_back(std::move(d));
    }
    postLivenessLocked(*target, std::move(chunk));
  }
}

AgentExecutor *AgentExecutor::postLivenessItem(const identity::ActorPid &target,
                                               std::vector<LivenessDue> due) {
  const std::lock_guard lock(ingress_->mutex);
  postLivenessLocked(target, std::move(due));
  return this;
}

// -- What a message tells the host
// ---------------------------------------------

AgentExecutor *AgentExecutor::noteFresh(const identity::ActorPid &target,
                                        std::string_view principal,
                                        zigzag::vortex::mesh::ActorTime at) {
  const auto pid = pidKey(target);
  const std::string who(principal);
  const Ingress::SourceKey source{pid, who};
  const std::lock_guard lock(ingress_->mutex);
  if (!ingress_->bound(pid, who) || ingress_->latched.contains(source) ||
      ingress_->live[source].latched)
    return this;
  auto &src        = ingress_->live[source];
  src.lastFresh    = std::max(src.lastFresh.value_or(at), at);
  src.clockPending = std::nullopt;
  armProfileLocked(pid, who, at, true);
  // Heartbeats write the arena only on the transition from SILENT to LIVE. A
  // closed door keeps the mirror silent, so the next heartbeat tries again.
  if (!src.silent || admissionClosedLocked(target)) return this;
  src.silent = false;
  LivenessDue clear;
  clear.kind      = LivenessDue::Kind::Clear;
  clear.principal = who;
  postLivenessLocked(target, {std::move(clear)});
  return this;
}

AgentExecutor *AgentExecutor::noteClockPending(
    const identity::ActorPid &target, std::string_view principal,
    std::optional<zigzag::vortex::mesh::ActorTime> since) {
  const std::lock_guard lock(ingress_->mutex);
  ingress_->live[{pidKey(target), std::string(principal)}].clockPending = since;
  return this;
}

std::optional<std::chrono::milliseconds>
AgentExecutor::heartbeatExpectation(const identity::ActorPid &target,
                                    std::string_view principal) const {
  const std::lock_guard lock(ingress_->mutex);
  return heartbeatOfLocked(pidKey(target), std::string(principal));
}

std::size_t AgentExecutor::livenessEntries() const {
  const std::lock_guard lock(ingress_->mutex);
  return ingress_->liveHeap.size();
}

std::uint64_t AgentExecutor::livenessItemsPosted() const {
  const std::lock_guard lock(ingress_->mutex);
  return ingress_->livenessItems;
}

// -- What SourceHealth shows
// ---------------------------------------------------

void AgentExecutor::readLivenessHealth(const State &state,
                                       const zigzag::CellRef profile,
                                       SourceHealth &out) const {
  const auto &arena = state.context->arena;
  const auto &vd    = state.vd;
  if (const auto cell = fieldValue(arena, vd, profile, kProfileSilent))
    out.silent = arena.asBool(*cell).value_or(false);
  if (out.silent)
    if (const auto cell = fieldValue(arena, vd, profile, kProfileSilentCause))
      out.silentCause = arena.textOf(*cell);
  if (const auto streams = fieldValue(arena, vd, profile, kProfileStreams))
    for (const auto &[name, value] : fieldsOf(arena, vd, *streams))
      if (value)
        if (const auto stale = fieldValue(arena, vd, *value, kStreamStale);
            stale && arena.asBool(*stale).value_or(false))
          ++out.staleStreams;
}

} // namespace xanadu::vagent::runtime
