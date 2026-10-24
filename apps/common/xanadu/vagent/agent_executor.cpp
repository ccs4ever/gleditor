#include "common/xanadu/vagent/agent_executor.hpp"

#include <gleditor/logging.hpp>

#include "common/xanadu/vortex/mesh_checkpoint.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <ranges>
#include <unordered_map>

namespace xanadu::vagent {
namespace {

using identity::MessageType;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

constexpr const char *kLog = "vortex.agent";

/// The host has no agent name of its own; this keeps its request ids in a
/// prefix no agent can share (agent names always contain a type).
constexpr std::string_view kHostName   = "vagent-host";
constexpr std::string_view kHostCursor = "vagent-host";

/// `Space` as `$outer` records it: the hosting agent and the space's name.
constexpr std::string_view kSpaceRecordType = "Space";
constexpr std::string_view kSpaceHost       = "host";
constexpr std::string_view kSpaceName       = "name";

constexpr std::string_view kChildStopping  = "ChildStopping";
constexpr std::string_view kChildDown      = "ChildDown";
constexpr std::string_view kSpawnFailed    = "SpawnFailed";
constexpr std::string_view kDeliveryFailed = "DeliveryFailed";
constexpr std::string_view kChildField     = "child";
constexpr std::string_view kCauseField     = "cause";
constexpr std::string_view kToField        = "to";
constexpr std::string_view kKindField      = "kind";
constexpr std::string_view kCauseInit      = "init";
constexpr std::string_view kCauseStop      = "stop";
constexpr std::string_view kCauseTerminate = "terminate";
constexpr std::string_view kCauseLease     = "lease";

/// `Moved{name}` (§4.5): an agent that resumed elsewhere tells its peers.
constexpr std::string_view kEventMoved = "Moved";
constexpr std::string_view kMovedName  = "name";
/// The resume request's own name prefix, so its ids cannot meet a launch's.
constexpr std::string_view kResumeSuffix = "/resume";

/// The envelope type that carries a `stop` performative.
constexpr std::string_view kStopType = "Stop";

/// A host event that raises another keeps the actor busy; this many rounds of
/// them run in one executor call and the rest wait for the next tick, so a
/// handler that provokes its own event cannot hold the owner thread.
constexpr std::size_t kMaxHostEventRounds = 8;

/// A Duration literal is nanoseconds; timers and `$turn.time` are
/// milliseconds.
constexpr std::int64_t kNanosPerMilli = 1'000'000;
/// A periodic timer that fell far behind (a suspended host, a long manual
/// advance) delivers at most this many missed periods at once and skips the
/// rest, so a tick cannot be turned into an unbounded burst of turns.
constexpr std::int64_t kMaxTimerCatchUp = 64;

/// A sender's floor is tracked per sender prefix; a peer cannot be allowed to
/// grow the table without bound by inventing prefixes.
constexpr std::size_t kMaxTrackedFloors = 4096;

/// ActorMessage::isValid caps `value` here.
constexpr std::size_t kMaxMessageValue = 32768;

constexpr std::string_view kFailCycles     = "cycles";
constexpr std::string_view kFailFuel       = "fuel";
constexpr std::string_view kFailStatement  = "fail";
constexpr std::string_view kFailUnfinished = "unfinished";

constexpr std::string_view kVmMaxCycles = "Exceeded max cycles";

std::string text(const ArenaManifold &arena, CellRef cell) {
  return arena.textOf(cell);
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

/// Unlink the first cell of `anchor`'s rank along `dim` that `chosen` accepts,
/// closing the gap. The cell is left unreachable, as every undone write is.
template <class Pred>
bool removeFromRank(ArenaManifold &arena, CellRef anchor, zigzag::DimRef dim,
                    Pred chosen) {
  CellRef before = anchor;
  for (const auto cell : zigzag::rankAfter(arena, anchor, dim)) {
    if (chosen(cell)) {
      const auto after = arena.linked(cell, dim);
      zigzag::expectWritten(arena.unlink(cell, dim, DimVector::POS));
      zigzag::expectWritten(arena.link(before, dim, DimVector::POS, after));
      return true;
    }
    before = cell;
  }
  return false;
}

/// Content and typed value of @p from onto @p to; links are not touched.
void copyCellData(ArenaManifold &arena, CellRef from, CellRef to) {
  const auto spans = arena.contentOf(from);
  const std::vector<xanadu::PrimediaSpan> held(spans.begin(), spans.end());
  zigzag::expectWritten(arena.setContent(to, held));
  switch (arena.valueKindOf(from)) {
  case xanadu::ValueKind::Int64:
    zigzag::expectWritten(arena.setValueBits(
        to, xanadu::ValueKind::Int64,
        std::bit_cast<std::uint64_t>(arena.asInt64(from).value_or(0))));
    break;
  case xanadu::ValueKind::Double:
    zigzag::expectWritten(arena.setValueBits(
        to, xanadu::ValueKind::Double,
        xanadu::canonicalDoubleBits(arena.asDouble(from).value_or(0.0))));
    break;
  case xanadu::ValueKind::Bool:
    zigzag::expectWritten(arena.setValueBits(
        to, xanadu::ValueKind::Bool, arena.asBool(from).value_or(false)));
    break;
  default:
    break;
  }
}

bool hasLinks(const ArenaManifold &arena, CellRef cell,
              std::span<const zigzag::DimRef> dims) {
  return std::ranges::any_of(dims, [&](zigzag::DimRef d) {
    return arena.linked(cell, d).has_value();
  });
}

/// A copy of everything reachable from @p from by positive links along
/// @p dims, relinked along @p into (the same dimensions as the arena now
/// knows them). Iterative: a list or a clause chain can be long.
CellRef cloneGraph(ArenaManifold &arena, CellRef from,
                   std::span<const zigzag::DimRef> dims,
                   std::span<const zigzag::DimRef> into) {
  std::vector<CellRef> order{from};
  std::unordered_map<CellRef, CellRef> copy;
  const auto admit = [&](CellRef c) {
    const auto made = arena.makeCell();
    copyCellData(arena, c, made);
    copy.emplace(c, made);
    order.push_back(c);
  };
  copy.emplace(from, CellRef{});
  copy.at(from) = arena.makeCell();
  copyCellData(arena, from, copy.at(from));
  for (std::size_t i = 0; i < order.size(); ++i) {
    for (const auto d : dims) {
      if (const auto next = arena.linked(order[i], d);
          next && !copy.contains(*next)) {
        admit(*next);
      }
    }
  }
  for (const auto c : order) {
    for (std::size_t k = 0; k < dims.size(); ++k) {
      if (const auto next = arena.linked(c, dims[k])) {
        zigzag::expectWritten(
            arena.link(copy.at(c), into[k], DimVector::POS, copy.at(*next)));
      }
    }
  }
  return copy.at(from);
}

} // namespace

std::string_view toString(CheckpointRefusal refusal) noexcept {
  switch (refusal) {
  case CheckpointRefusal::UnknownAgent:
    return "unknown agent";
  case CheckpointRefusal::Stopped:
    return "stopped";
  case CheckpointRefusal::HostDeliveryPending:
    return "host delivery pending";
  case CheckpointRefusal::MailboxNotEmpty:
    return "mailbox not empty";
  case CheckpointRefusal::SendsIncomplete:
    return "sends incomplete";
  case CheckpointRefusal::NotQuiescent:
    return "not quiescent";
  case CheckpointRefusal::NoIdentity:
    return "no checkpoint identity";
  case CheckpointRefusal::PublishFailed:
    return "publish failed";
  }
  return "unknown";
}

AgentClock AgentClock::system() {
  return AgentClock{.wall   = [] { return std::chrono::system_clock::now(); },
                    .steady = [] { return std::chrono::steady_clock::now(); }};
}

struct AgentExecutor::State {
  identity::ActorPid pid;
  std::string name;
  std::string parentName;
  std::string agentType;
  std::string prefix;
  std::optional<identity::ActorPid> parentPid;
  CompiledAgent agent{};
  AgentDims dims{};
  ValueDims vd{};
  WorkerContext *context{};
  bool prepared{false};
  std::map<std::string, std::string> floors;
  std::map<std::string, std::uint64_t, std::less<>> pendingStats;
  std::size_t sinceCompact{0};
  /// Events the host raises for the agent, run as turns of their own.
  std::vector<AgentExecutor::HostEvent> hostEvents;
  /// True inside `execute`: the arena is the runtime's and mid-turn.
  bool inTurn{false};
  /// The next wake only refreshes the lease and runs no dispatch.
  bool quietWake{false};
  /// What the last `execute` was given, so the adapter can run a turn between
  /// executor calls (lease end) and knows when the lease runs out.
  std::optional<zigzag::vortex::mesh::ActorGrant> grant;
  CellRef cursor{};
  std::chrono::system_clock::time_point leaseSeen;
  /// Re-parented to the launcher by `Orphaned`: its `ChildStopping` goes to
  /// the host sink, since the launcher is no actor.
  bool adopted{false};
  /// `$turn.time` of the turn in progress, so the drain files a timer against
  /// the same instant the handler saw.
  std::int64_t turnMillis{0};
  /// Rebuilt by `resume`: the first executor call announces `Moved`.
  bool resumed{false};
};

struct AgentExecutor::TurnSpec {
  /// Nullopt: no routine runs (a relay the host does not also handle).
  std::optional<CellRef> entry;
  /// A received event to expose as `$turn.event`, and the space it came by.
  const Envelope *event{};
  std::string space;
  /// An emission this agent hosts a space for, to fan out after the turn.
  const Envelope *relayed{};
  bool markInitialized{false};
};

struct AgentExecutor::TurnOutcome {
  bool committed{false};
  std::string cause;
};

struct AgentExecutor::HostEvent {
  std::string type;
  WireRecord fields;
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
  std::int64_t delayNs{0};
  std::int64_t periodNs{0};
  // log
  std::string level;
  std::string message;
};

AgentExecutor::AgentExecutor(zigzag::vortex::mesh::MeshActorRuntime &runtime,
                             CapabilityIssuer &issuer, AgentProgram &program,
                             identity::Fingerprint owner, std::string localNode,
                             std::uint64_t epoch, AgentSettings settings,
                             AgentClock clock)
    : runtime_(runtime), issuer_(issuer), program_(program), owner_(owner),
      localNode_(std::move(localNode)), epoch_(epoch),
      settings_(std::move(settings)), clock_(std::move(clock)),
      hostPid_{localNode_, std::string(kHostCursor), epoch_, 1} {}

AgentExecutor::~AgentExecutor() = default;

zigzag::vortex::mesh::ActorOptions
AgentExecutor::withFloors(zigzag::vortex::mesh::ActorOptions options) {
  options.floorOf = [](const ActorMessage &message) {
    return floorMarkOf(message);
  };
  return options;
}

zigzag::vortex::mesh::ActorOptions
AgentExecutor::runtimeOptions(const AgentSettings &settings) {
  auto options                   = withFloors();
  options.valueLimits.operations = settings.valueOperations;
  return options;
}

std::string AgentExecutor::pidKey(const identity::ActorPid &pid) {
  return pidToText(pid);
}

zigzag::vortex::mesh::MeshActorRuntime::Executor AgentExecutor::executor() {
  return [this](const ActorExecution &execution, WorkerContext &context) {
    return execute(execution, context);
  };
}

zigzag::vortex::mesh::MeshActorRuntime::Sender AgentExecutor::loopbackSender(
    zigzag::vortex::mesh::MeshActorRuntime::Sender remote) {
  return [this, remote = std::move(remote)](std::string_view node,
                                            const ActorMessage &message) {
    if (node == localNode_) {
      return runtime_.receive(owner_, message, clock_.steady(), wallNow())
          .has_value();
    }
    return remote && remote(node, message);
  };
}

AgentExecutor *
AgentExecutor::install(zigzag::vortex::mesh::MeshActorRuntime::Sender remote) {
  runtime_.setExecutor(executor())
      ->setSender(loopbackSender(std::move(remote)))
      ->trustNode(localNode_, owner_);
  return this;
}

AgentExecutor::State &AgentExecutor::stateFor(const identity::ActorPid &pid) {
  auto &slot = states_[pidKey(pid)];
  if (!slot) {
    slot      = std::make_unique<State>();
    slot->pid = pid;
  }
  return *slot;
}

zigzag::vortex::WorkerContext *
AgentExecutor::context(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  return found == states_.end() ? nullptr : found->second->context;
}

std::optional<CellRef>
AgentExecutor::agentCell(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !found->second->prepared) return std::nullopt;
  return found->second->agent.agentCell;
}

void AgentExecutor::liftLimits(WorkerContext &context) const {
  // The grant's budget is per turn (§4.2 step 2). Between turns the adapter's
  // own writes must not draw on whatever a failed turn left of it.
  context.arena.setExecutionLimits(std::numeric_limits<std::uint32_t>::max(),
                                   std::numeric_limits<std::size_t>::max(),
                                   std::numeric_limits<std::size_t>::max());
}

// -- Reserved fields, counters and statistics
// ----------------------------------

std::uint64_t AgentExecutor::counter(const State &state,
                                     std::string_view field) const {
  const auto &arena = state.context->arena;
  const auto cell   = fieldValue(arena, state.vd, state.agent.agentCell, field);
  return cell ? static_cast<std::uint64_t>(arena.asInt64(*cell).value_or(0))
              : 0;
}

void AgentExecutor::setCounter(State &state, std::string_view field,
                               std::uint64_t value) {
  auto &arena = state.context->arena;
  if (const auto cell =
          fieldValue(arena, state.vd, state.agent.agentCell, field)) {
    zigzag::expectWritten(arena.setValueBits(*cell, ValueKind::Int64, value));
    return;
  }
  setField(arena, state.vd, state.agent.agentCell, field,
           arena.makeScalarCell(static_cast<std::int64_t>(value)));
}

bool AgentExecutor::initialized(const State &state) const {
  const auto &arena = state.context->arena;
  const auto cell =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldInitialized);
  return cell && arena.asBool(*cell).value_or(false);
}

void AgentExecutor::bumpStat(State &state, std::string_view name,
                             std::uint64_t by) {
  state.pendingStats[std::string(name)] += by;
  // Between turns nothing else writes them until the agent's next turn, which
  // for an idle agent may never come. The runtime lends the arena for the
  // whole life of the actor, so the adapter writes it at once.
  if (!state.inTurn && live(state)) {
    liftLimits(*state.context);
    flushStats(state);
  }
}

bool AgentExecutor::live(const State &state) const {
  return state.prepared && state.context &&
         runtime_.localCursor(state.pid).has_value();
}

void AgentExecutor::flushStats(State &state) {
  if (state.pendingStats.empty() || !state.context || !state.prepared) return;
  auto &arena = state.context->arena;
  const auto stats =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldStats);
  if (!stats) return;
  for (const auto &[name, count] : state.pendingStats) {
    if (const auto cell = fieldValue(arena, state.vd, *stats, name)) {
      zigzag::expectWritten(arena.setValueBits(
          *cell, ValueKind::Int64,
          static_cast<std::uint64_t>(arena.asInt64(*cell).value_or(0)) +
              count));
    } else {
      setField(arena, state.vd, *stats, name,
               arena.makeScalarCell(static_cast<std::int64_t>(count)));
    }
  }
  state.pendingStats.clear();
}

std::uint64_t AgentExecutor::stat(const identity::ActorPid &pid,
                                  std::string_view name) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end()) return 0;
  const State &state = *found->second;
  std::uint64_t total{};
  if (const auto pending = state.pendingStats.find(name);
      pending != state.pendingStats.end()) {
    total += pending->second;
  }
  if (state.context && state.prepared) {
    const auto &arena = state.context->arena;
    if (const auto stats =
            fieldValue(arena, state.vd, state.agent.agentCell, kFieldStats)) {
      if (const auto cell = fieldValue(arena, state.vd, *stats, name)) {
        total += static_cast<std::uint64_t>(arena.asInt64(*cell).value_or(0));
      }
    }
  }
  return total;
}

// -- Setting an agent up
// ------------------------------------------------------

std::optional<AgentExecutor::SpawnInfo> AgentExecutor::readSpawn(
    const zigzag::vortex::mesh::ActorMessage &request) const {
  const auto bytes = issuer_.parametersOf(request.capability);
  if (!bytes) return std::nullopt;
  const auto decoded = decodeValue(*bytes);
  if (!decoded) return std::nullopt;
  const auto *dictionary = std::get_if<WireRecord>(&decoded->data);
  if (!dictionary) return std::nullopt;
  const auto textField =
      [&](std::string_view key) -> std::optional<std::string> {
    if (const auto *v = dictionary->find(key)) {
      if (const auto *t = std::get_if<std::string>(&v->data)) return *t;
    }
    return std::nullopt;
  };
  const auto name = textField(kWireSpawnName);
  if (!name) return std::nullopt;
  SpawnInfo info;
  info.name       = *name;
  info.agentType  = textField(kWireSpawnAgent).value_or(std::string{});
  info.parentName = textField(kWireSpawnParent).value_or(std::string{});
  // A top-level agent's request comes from the host, which is no parent.
  if (!info.parentName.empty()) info.parent = request.source;
  if (const auto *args = dictionary->find(kWireSpawnArgs)) {
    if (const auto *record = std::get_if<WireRecord>(&args->data)) {
      info.args = *record;
    }
  }
  return info;
}

bool AgentExecutor::prepare(State &state, WorkerContext &context,
                            const ActorMessage &spawn,
                            std::optional<CellRef> savedAgent) {
  auto &arena     = context.arena;
  const auto info = readSpawn(spawn);
  state.context   = &context;
  state.agentType = info ? info->agentType : std::string{};
  // A pid the host did not launch through this adapter still gets a name,
  // so its request ids are well formed.
  state.name =
      info ? info->name : "agent#" + std::to_string(state.pid.spawnSequence);
  state.parentName = info ? info->parentName : std::string{};
  state.parentPid  = info ? info->parent : std::nullopt;

  try {
    auto compiled = program_.instantiate(context, state.agentType);
    if (!compiled) {
      GLEDITOR_LOG_DEBUG(kLog, "instantiate refused: {}", compiled.error());
      return false;
    }
    const auto sound = [&](CellRef c) { return arena.contains(c); };
    if (!sound(compiled->agentCell) || !sound(compiled->initEntry) ||
        !sound(compiled->dispatchEntry) || !sound(compiled->haltOp) ||
        !sound(compiled->failOp)) {
      GLEDITOR_LOG_DEBUG(kLog, "instantiate returned a cell the arena lacks");
      return false;
    }
    state.agent = *compiled;
    state.dims  = registerDimensions(arena);
    state.vd    = valueDims(context.core);
    // Neither the oracle nor a cut callback is ever set for an agent, so the
    // Dewey path would only be copied into every choice point (P12).
    context.vm.setDeweyTracking(false);

    const auto ac  = state.agent.agentCell;
    const auto &vd = state.vd;
    if (savedAgent) {
      restoreState(state, *savedAgent);
      state.resumed = true;
    }
    // A compiled agent already holds every reserved field, empty, so a fresh
    // one is told by its name never having been written.
    const auto nameValue = fieldValue(arena, vd, ac, kFieldName);
    const bool fresh     = !nameValue || arena.textOf(*nameValue).empty();
    // Compiled code reads a scalar or text field through its static value
    // cell, so a fresh agent's values are written into that cell; a list or a
    // record is reached through its name cell and may be replaced.
    const auto assign = [&](std::string_view field, const WireValue &value) {
      const auto existing = fieldValue(arena, vd, ac, field);
      if (!existing) {
        setField(arena, vd, ac, field, writeValue(arena, vd, value));
        return;
      }
      if (const auto *i = std::get_if<std::int64_t>(&value.data)) {
        zigzag::expectWritten(
            arena.setValueBits(*existing, xanadu::ValueKind::Int64,
                               std::bit_cast<std::uint64_t>(*i)));
      } else if (const auto *d = std::get_if<double>(&value.data)) {
        zigzag::expectWritten(
            arena.setValueBits(*existing, xanadu::ValueKind::Double,
                               xanadu::canonicalDoubleBits(*d)));
      } else if (const auto *b = std::get_if<bool>(&value.data)) {
        zigzag::expectWritten(
            arena.setValueBits(*existing, xanadu::ValueKind::Bool, *b ? 1 : 0));
      } else if (const auto *t = std::get_if<std::string>(&value.data)) {
        const std::array spans{arena.intern(*t)};
        zigzag::expectWritten(arena.setContent(*existing, spans));
      } else {
        setField(arena, vd, ac, field, writeValue(arena, vd, value));
      }
    };
    const auto ensure = [&](std::string_view field, const WireValue &value) {
      if (!fieldNameCell(arena, vd, ac, field) || fresh) assign(field, value);
    };
    WireRecord outer;
    outer.type = std::string(kSpaceRecordType);
    outer.set(std::string(kSpaceHost), WireValue::text(state.parentName));
    outer.set(std::string(kSpaceName),
              WireValue::text(std::string(kSpaceOuter)));
    if (!state.parentName.empty()) {
      outer.set(std::string(kSpaceName),
                WireValue::text(std::string(kSpaceInner)));
    }
    ensure(kFieldName, WireValue::text(state.name));
    ensure(kFieldParent, WireValue::text(state.parentName));
    ensure(kFieldOuter, WireValue::record(std::move(outer)));
    ensure(kFieldInitialized, WireValue::boolean(false));
    ensure(kFieldPeers, WireValue::record({}));
    ensure(kFieldNextId, WireValue::integer(1));
    ensure(kFieldSpawnCount, WireValue::integer(0));
    ensure(kFieldTurnCount, WireValue::integer(0));
    ensure(kFieldTimers, WireValue::list({}));
    ensure(kFieldTerminating, WireValue::boolean(false));
    ensure(kFieldCancelled, WireValue::list({}));
    ensure(kFieldStats, WireValue::record({}));
    ensure(kFieldOrdinal, WireValue::integer(0));
    ensure(kFieldConsumed, WireValue::boolean(false));
    ensure(kFieldKeepChildren, WireValue::boolean(false));
    if (!fieldNameCell(arena, vd, ac, kFieldTurn)) {
      setField(arena, vd, ac, kFieldTurn, std::nullopt);
    }
    if (!fresh) {
      // A resumed arena already knows who it is; the cells win.
      if (const auto n = fieldValue(arena, vd, ac, kFieldName)) {
        state.name = text(arena, *n);
      }
      if (const auto p = fieldValue(arena, vd, ac, kFieldParent)) {
        state.parentName = text(arena, *p);
      }
    }
    if (state.resumed) {
      // The spawn request's source is the host that resumed it, not the
      // parent; the parent's pid is in $peers, where Moved keeps it current.
      state.parentPid = state.parentName.empty()
                            ? std::nullopt
                            : peerPid(state, state.parentName);
      if (const auto hint = resumeAdopted_.find(pidKey(state.pid));
          hint != resumeAdopted_.end()) {
        state.adopted = hint->second;
        resumeAdopted_.erase(hint);
      }
    }
    state.prefix = senderPrefix(state.name);

    if (!hasInnerSpace(state)) {
      appendToRank(arena, ac, state.dims.space,
                   arena.makeCell(std::string_view(kSpaceInner)));
    }
    if (fresh) {
      if (const auto peers = fieldValue(arena, vd, ac, kFieldPeers);
          peers && state.parentPid) {
        setField(arena, vd, *peers, state.parentName,
                 arena.makeCell(std::string_view(pidToText(*state.parentPid))));
      }
      if (info) {
        for (std::size_t i = 0; i < info->args.names.size(); ++i) {
          assign(info->args.names[i], info->args.values[i]);
        }
      }
    }
  } catch (const std::exception &e) {
    GLEDITOR_LOG_DEBUG(kLog, "preparing an agent failed: {}", e.what());
    return false;
  }
  state.prepared = true;
  return true;
}

std::optional<CellRef> AgentExecutor::innerSpace(const State &state) const {
  const auto &arena = state.context->arena;
  for (const auto space :
       zigzag::rankAfter(arena, state.agent.agentCell, state.dims.space)) {
    if (text(arena, space) == kSpaceInner) return space;
  }
  return std::nullopt;
}

bool AgentExecutor::hasInnerSpace(const State &state) const {
  return innerSpace(state).has_value();
}

std::optional<identity::ActorPid>
AgentExecutor::peerPid(const State &state, std::string_view name) const {
  const auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers) return std::nullopt;
  const auto cell = fieldValue(arena, state.vd, *peers, name);
  if (!cell) return std::nullopt;
  return pidFromText(text(arena, *cell));
}

std::vector<std::string> AgentExecutor::innerMembers(const State &state) const {
  const auto &arena = state.context->arena;
  const auto space  = innerSpace(state);
  if (!space) return {};
  return zigzag::rankAfter(arena, *space, state.dims.member) |
         std::views::transform(
             [&](CellRef member) { return text(arena, member); }) |
         std::ranges::to<std::vector>();
}

// -- The executor
// ----------------------------------------------------------------

std::expected<std::string, ActorError>
AgentExecutor::execute(const ActorExecution &execution,
                       WorkerContext &context) {
  State &state  = stateFor(execution.pid);
  state.context = &context;
  struct InTurn {
    State &state;
    explicit InTurn(State &s) : state(s) { state.inTurn = true; }
    ~InTurn() { state.inTurn = false; }
  } inTurn(state);
  liftLimits(context);
  if (!state.prepared &&
      !prepare(state, context, execution.spawnRequest, execution.stateRoot)) {
    return fatal(state, kCauseInit);
  }
  if (state.resumed) {
    state.resumed = false;
    announceMoved(state);
    // The first wake of a resumed agent has nothing to dispatch.
    if (!execution.message) state.quietWake = true;
  }
  if (!state.grant || state.grant->expiresAt != execution.grant.expiresAt) {
    state.leaseSeen = wallNow();
  }
  state.grant  = execution.grant;
  state.cursor = execution.cursor;
  flushStats(state);

  bool justInitialised = false;
  if (!initialized(state)) {
    TurnSpec spec;
    spec.entry           = state.agent.initEntry;
    spec.markInitialized = true;
    if (!runTurn(state, execution.cursor, execution.grant, context, spec)
             .committed) {
      return fatal(state, kCauseInit);
    }
    justInitialised = true;
  }
  if (!execution.message) {
    // A wake that only refreshes a lease or carries host events must not also
    // run a dispatch with no event.
    const bool plain =
        !justInitialised && !state.quietWake && state.hostEvents.empty();
    state.quietWake = false;
    if (plain) {
      TurnSpec spec;
      spec.entry = state.agent.dispatchEntry;
      runTurn(state, execution.cursor, execution.grant, context, spec);
    }
    settle(state, execution.cursor, execution.grant, context);
    return std::string{};
  }
  auto result = handleMessage(state, execution, context);
  if (state.context) {
    settle(state, execution.cursor, execution.grant, context);
    flushStats(state);
  }
  return result;
}

std::expected<std::string, ActorError>
AgentExecutor::fatal(State &state, std::string_view cause) {
  GLEDITOR_LOG_DEBUG(kLog, "agent {} stops: {}", state.name, cause);
  stageChildStopping(state, cause);
  // stop() frees the slot and tells monitors; erasure waits for pumpOne.
  (void)runtime_.stop(state.pid);
  state.context = nullptr;
  return std::unexpected(ActorError::ExecutionFailed);
}

void AgentExecutor::stageChildStopping(State &state, std::string_view cause,
                                       std::uint64_t position) {
  if (!state.parentPid && !state.adopted) return;
  const std::uint64_t turn =
      state.prepared && state.context ? counter(state, kFieldTurnCount) : 0;
  Envelope envelope;
  envelope.kind = std::string(kKindEvent);
  envelope.type = std::string(kChildStopping);
  envelope.from = state.name;
  envelope.fields.set(std::string(kChildField), WireValue::text(state.name));
  envelope.fields.set(std::string(kCauseField),
                      WireValue::text(std::string(cause)));
  if (!state.parentPid) {
    // The launcher hosts the space; there is no actor to send to.
    if (hostSink_) hostSink_(envelope);
    return;
  }
  if (state.prefix.empty()) state.prefix = senderPrefix(state.name);
  sendEnvelope(state, turn, position, *state.parentPid, std::move(envelope));
}

std::expected<std::string, ActorError>
AgentExecutor::handleMessage(State &state, const ActorExecution &execution,
                             WorkerContext &context) {
  const auto &message = *execution.message;
  switch (message.type) {
  case MessageType::ActorSend:
    return handleSend(state, execution, context);
  case MessageType::ActorDeliverValue:
  case MessageType::ActorDeliverWriteSet:
    // The runtime already reaped the delivery; the reply runs no user code.
    outstanding_.erase(message.requestId);
    return std::string{};
  case MessageType::ActorDown:
    onDown(state, context, message);
    return std::string{};
  default:
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
}

void AgentExecutor::onDown(State &state, WorkerContext &,
                           const ActorMessage &message) {
  if (!message.source) return;
  const auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers) return;
  for (const auto &[nameCell, valueCell] : fieldsOf(arena, state.vd, *peers)) {
    if (!valueCell) continue;
    const auto pid = pidFromText(text(arena, *valueCell));
    if (!pid || *pid != *message.source) continue;
    // A child that announced its stop is already gone from $peers, which is
    // how a stop is reported once and not twice (section 4.5); reaching here
    // means it vanished without a word.
    const auto name = text(arena, nameCell);
    bumpStat(state, kStatChildDown);
    removeChild(state, name);
    WireRecord fields;
    fields.set(std::string(kChildField), WireValue::text(name));
    raise(state, kChildDown, std::move(fields));
    return;
  }
}

bool AgentExecutor::removeChild(State &state, std::string_view name) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto ac    = state.agent.agentCell;
  const auto peers = fieldValue(arena, vd, ac, kFieldPeers);
  const bool known = peers && fieldNameCell(arena, vd, *peers, name);
  const auto named = [&](CellRef cell) { return text(arena, cell) == name; };
  if (peers) removeFromRank(arena, *peers, vd.vars, named);
  removeFromRank(arena, ac, state.dims.holon, named);
  if (const auto space = innerSpace(state)) {
    removeFromRank(arena, *space, state.dims.member, named);
  }
  // A stopped sender will send no later envelope carrying a floor, so its
  // last remembered ids would stay for good without this (P14).
  (void)runtime_.forgetSender(state.pid, senderPrefix(name));
  GLEDITOR_LOG_DEBUG(kLog, "{} forgets child {}", state.name, name);
  return known;
}

void AgentExecutor::acknowledgeChildStop(State &state,
                                         const ActorMessage &message,
                                         const Envelope &envelope) {
  const auto *child = envelope.fields.find(kChildField);
  const auto *name  = child ? std::get_if<std::string>(&child->data) : nullptr;
  // Only the child itself can announce its own stop.
  if (!name || *name != envelope.from) return;
  const auto pid = peerPid(state, *name);
  if (!pid || message.source != pid) return;
  removeChild(state, *name);
}

std::expected<std::string, ActorError>
AgentExecutor::handleSend(State &state, const ActorExecution &execution,
                          WorkerContext &context) {
  const auto &message = *execution.message;
  auto decoded        = decodeEnvelope(message.value);
  if (!decoded || decoded->type.empty()) {
    GLEDITOR_LOG_DEBUG(kLog, "dropping a malformed envelope for {}",
                       state.name);
    bumpStat(state, kStatMalformed);
    return std::string{};
  }
  const Envelope &envelope = *decoded;
  const bool isTell =
      envelope.kind == kKindTell || envelope.kind == kKindUntell;
  if (envelope.kind != kKindEvent && envelope.kind != kKindStop && !isTell) {
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
  // An ordinary event may not impersonate the internal event a tell becomes.
  if (envelope.kind == kKindEvent && !envelope.type.empty() &&
      envelope.type.front() == kInternalEventMark) {
    bumpStat(state, kStatBadTell);
    return std::string{};
  }

  // A message can wait in the mailbox past a later one that advanced the
  // floor; the runtime erased its dedup entry then, so the check is repeated
  // here against what this agent has seen.
  const auto prefix = message.requestId.substr(0, kRequestIdNameDigits);
  if (isRequestId(message.requestId) && isRequestId(envelope.floor) &&
      envelope.floor.starts_with(prefix)) {
    auto known = state.floors.find(prefix);
    if (known == state.floors.end() &&
        state.floors.size() < kMaxTrackedFloors) {
      known = state.floors.emplace(prefix, envelope.floor).first;
    } else if (known != state.floors.end() && known->second < envelope.floor) {
      known->second = envelope.floor;
    }
    if (known != state.floors.end() && message.requestId < known->second) {
      bumpStat(state, kStatStale);
      return std::string{};
    }
  }

  if (envelope.kind == kKindStop) return handleStop(state, execution, context);

  if (isTell) {
    // The dispatcher runs compiled code on the rewritten type (see
    // `kTellEventPrefix`). `from` is the sender's name, which becomes the
    // source of the fact.
    Envelope internal = envelope;
    internal.kind     = std::string(kKindEvent);
    internal.type =
        std::string(envelope.kind == kKindTell ? kTellEventPrefix
                                               : kUntellEventPrefix) +
        envelope.type;
    TurnSpec tell;
    tell.entry = state.agent.dispatchEntry;
    tell.event = &internal;
    tell.space = std::string(kSpaceInner);
    runTurn(state, execution.cursor, execution.grant, context, tell);
    return std::string{};
  }

  // The parent left and kept this agent: it now belongs to the launcher. Done
  // before the turn so the handler for `Orphaned` already sees the new parent.
  if (!envelope.relaySpace && envelope.type == kEventOrphaned &&
      state.parentPid && message.source == state.parentPid) {
    adoptByLauncher(state);
  }

  if (!envelope.relaySpace && envelope.type == kEventMoved) {
    applyMoved(state, message, envelope);
  }

  const bool relayed = envelope.relaySpace.has_value();
  if (relayed && *envelope.relaySpace != kSpaceInner) {
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
  // The host handles a relayed emission itself unless a `for` list leaves it
  // out; the relay to its members is the host's duty either way.
  const bool handle = !relayed || envelope.relayFor.empty() ||
                      contains(envelope.relayFor, state.name);
  if (!relayed && envelope.type == kChildStopping) {
    acknowledgeChildStop(state, message, envelope);
  }
  TurnSpec spec;
  if (handle) spec.entry = state.agent.dispatchEntry;
  spec.event = &envelope;
  if (relayed) {
    spec.space   = *envelope.relaySpace;
    spec.relayed = &envelope;
  } else if ((state.parentPid && message.source == state.parentPid) ||
             message.source == hostPid_) {
    // The launcher hosts a top-level agent's outer space.
    spec.space = std::string(kSpaceOuter);
  } else {
    spec.space = std::string(kSpaceInner);
  }
  runTurn(state, execution.cursor, execution.grant, context, spec);
  return std::string{};
}

std::expected<std::string, ActorError>
AgentExecutor::handleStop(State &state, const ActorExecution &execution,
                          WorkerContext &context) {
  // Accepted from the parent only (section 8.6): a sibling or a stranger
  // cannot end an agent.
  if (!state.parentPid || execution.message->source != state.parentPid) {
    bumpStat(state, kStatStopRefused);
    return std::string{};
  }
  const auto flag = [](const Envelope &e, std::string_view name) {
    const auto *v = e.fields.find(name);
    const auto *b = v ? std::get_if<bool>(&v->data) : nullptr;
    return b != nullptr && *b;
  };
  bool keep     = false;
  bool announce = true;
  if (const auto decoded = decodeEnvelope(execution.message->value)) {
    keep     = flag(*decoded, kWireStopKeep);
    announce = !flag(*decoded, kWireStopParentStopping);
  }
  orderlyStop(state, execution.cursor, execution.grant, context, kCauseStop,
              keep, announce);
  return std::string{};
}

void AgentExecutor::adoptByLauncher(State &state) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto ac    = state.agent.agentCell;
  const auto named = [&](CellRef cell) {
    return text(arena, cell) == state.parentName;
  };
  if (const auto peers = fieldValue(arena, vd, ac, kFieldPeers)) {
    removeFromRank(arena, *peers, vd.vars, named);
  }
  (void)runtime_.forgetSender(state.pid, senderPrefix(state.parentName));
  state.parentName.clear();
  state.parentPid.reset();
  state.adopted = true;
  if (const auto parent = fieldValue(arena, vd, ac, kFieldParent)) {
    const std::array spans{arena.intern(std::string_view{})};
    zigzag::expectWritten(arena.setContent(*parent, spans));
  }
  WireRecord outer;
  outer.type = std::string(kSpaceRecordType);
  outer.set(std::string(kSpaceHost), WireValue::text(std::string{}));
  outer.set(std::string(kSpaceName), WireValue::text(std::string(kSpaceOuter)));
  setField(arena, vd, ac, kFieldOuter,
           writeValue(arena, vd, WireValue::record(std::move(outer))));
  GLEDITOR_LOG_DEBUG(kLog, "{} is adopted by the launcher", state.name);
}

void AgentExecutor::releaseChildren(State &state, std::uint64_t &position,
                                    bool keepChildren) {
  const auto &arena = state.context->arena;
  const auto children =
      zigzag::rankAfter(arena, state.agent.agentCell, state.dims.holon) |
      std::views::transform([&](CellRef c) { return text(arena, c); }) |
      std::ranges::to<std::vector>();
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  if (state.prefix.empty()) state.prefix = senderPrefix(state.name);
  for (const auto &child : children) {
    const auto pid = peerPid(state, child);
    if (!pid) {
      bumpStat(state, kStatUnroutable);
      continue;
    }
    Envelope envelope;
    envelope.from = state.name;
    if (keepChildren) {
      envelope.kind = std::string(kKindEvent);
      envelope.type = std::string(kEventOrphaned);
      envelope.fields.set(std::string(kOrphanedFormer),
                          WireValue::text(state.name));
    } else {
      envelope.kind = std::string(kKindStop);
      envelope.type = std::string(kStopType);
      envelope.fields.set(std::string(kWireStopKeep),
                          WireValue::boolean(false));
      envelope.fields.set(std::string(kWireStopParentStopping),
                          WireValue::boolean(true));
    }
    sendEnvelope(state, turn, position, *pid, std::move(envelope));
  }
}

// -- Host events and the orderly stop (sections 4.5, 5.5)
// ------------------------

void AgentExecutor::raise(State &state, std::string_view type,
                          WireRecord fields) {
  state.hostEvents.push_back(HostEvent{std::string(type), std::move(fields)});
}

AgentExecutor::TurnOutcome AgentExecutor::deliverHost(State &state,
                                                      CellRef cursor,
                                                      const ActorGrant &grant,
                                                      WorkerContext &context,
                                                      const HostEvent &event) {
  Envelope envelope;
  envelope.kind   = std::string(kKindEvent);
  envelope.type   = event.type;
  envelope.fields = event.fields;
  TurnSpec spec;
  spec.entry = state.agent.dispatchEntry;
  spec.event = &envelope;
  return runTurn(state, cursor, grant, context, spec);
}

void AgentExecutor::settle(State &state, CellRef cursor,
                           const ActorGrant &grant, WorkerContext &context) {
  for (std::size_t round = 0; round < kMaxHostEventRounds && state.context &&
                              !state.hostEvents.empty();
       ++round) {
    const auto batch = std::exchange(state.hostEvents, {});
    for (const auto &event : batch) {
      deliverHost(state, cursor, grant, context, event);
    }
  }
  if (!state.context) return;
  const auto &arena = context.arena;
  const auto flag =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldTerminating);
  if (flag && arena.asBool(*flag).value_or(false)) {
    const auto keep =
        fieldValue(arena, state.vd, state.agent.agentCell, kFieldKeepChildren);
    orderlyStop(state, cursor, grant, context, kCauseTerminate,
                keep && arena.asBool(*keep).value_or(false));
  }
}

void AgentExecutor::orderlyStop(State &state, CellRef cursor,
                                const ActorGrant &grant, WorkerContext &context,
                                std::string_view cause, bool keepChildren,
                                bool announce) {
  GLEDITOR_LOG_DEBUG(kLog, "agent {} stops: {}", state.name, cause);
  // Whatever was waiting is moot once the agent is on its way out.
  state.hostEvents.clear();
  deliverHost(state, cursor, grant, context,
              HostEvent{std::string(kEventDestroy), {}});
  state.hostEvents.clear();
  // Lease end and init failure carry no statement to choose, so they reap
  // (the default of `keepChildren`).
  std::uint64_t position = 0;
  if (state.context) releaseChildren(state, position, keepChildren);
  if (announce) stageChildStopping(state, cause, position);
  flushStats(state);
  // stop() frees the slot and tells monitors; erasure waits for the executor
  // to return and the deliveries to drain.
  (void)runtime_.stop(state.pid);
  state.context = nullptr;
}

// -- One turn (§4.2)
// -------------------------------------------------------------

AgentExecutor::TurnOutcome AgentExecutor::runTurn(State &state, CellRef cursor,
                                                  const ActorGrant &grant,
                                                  WorkerContext &context,
                                                  const TurnSpec &spec) {
  auto &arena              = context.arena;
  auto &vm                 = context.vm;
  const auto &agent        = state.agent;
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  TurnOutcome outcome{.committed = true, .cause = {}};

  if (spec.entry) {
    // 1. Mark.
    const auto m0 = arena.mark();
    // 2. Fuel, relative to where the turn starts.
    const auto cells =
        std::min<std::uint64_t>(std::uint64_t{m0.cellCount} + grant.maxCells,
                                std::numeric_limits<std::uint32_t>::max());
    const auto work = static_cast<std::size_t>(std::min<std::uint64_t>(
        grant.maxInstructions, std::numeric_limits<std::size_t>::max()));
    arena.setExecutionLimits(static_cast<std::uint32_t>(cells), work,
                             std::size_t{m0.scratchSize} +
                                 grant.maxOutputBytes);

    std::optional<std::string> failure;
    try {
      // 3. Turn cell.
      const auto millis = static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              wallNow().time_since_epoch())
              .count());
      state.turnMillis = millis;
      WireRecord turnRecord;
      turnRecord.set(std::string(kTurnTime), WireValue::integer(millis));
      const auto turnCell =
          writeValue(arena, state.vd, WireValue::record(turnRecord));
      if (spec.event) {
        const auto event = mintEvent(state, *spec.event, spec.space, millis);
        setField(arena, state.vd, turnCell, kTurnEvent, event);
        if (observer_) observer_(state.pid, context, event);
      }
      setField(arena, state.vd, agent.agentCell, kFieldTurn, turnCell);

      // 4. Base: reset the path, then a choice point whose alternative is the
      //    failure routine.
      vm.setCurrentDeweyPath({});
      vm.setCursorOpcode(cursor, *spec.entry);
      vm.pushChoicePoint(cursor, agent.failOp);

      // 5. Dispatch, run to the halt opcode.
      const auto startCycles = vm.totalCycles();
      for (;;) {
        const auto used = vm.totalCycles() - startCycles;
        if (used >= work) {
          failure = std::string(kFailCycles);
          break;
        }
        const auto result = vm.run(cursor, work - used);
        if (result.success) break;
        if (result.contractViolation !=
            zigzag::vortex::ContractViolationKind::None) {
          // A contract violation is an ordinary failure of the statement; the
          // base choice point guarantees there is somewhere to land.
          if (vm.backtrack(cursor)) continue;
        }
        failure = result.errorMessage == kVmMaxCycles
                      ? std::string(kFailCycles)
                      : std::string(kFailStatement);
        break;
      }
      if (!failure) {
        const auto at = vm.getCursorOpcode(cursor);
        if (at == agent.failOp) {
          failure = std::string(kFailStatement);
        } else if (at != agent.haltOp) {
          failure = std::string(kFailUnfinished);
        }
      }
      // 6. Settle: nothing raises internal events in this iteration.
      if (!failure && spec.markInitialized) {
        setField(arena, state.vd, agent.agentCell, kFieldInitialized,
                 writeValue(arena, state.vd, WireValue::boolean(true)));
      }
    } catch (const std::exception &e) {
      GLEDITOR_LOG_DEBUG(kLog, "turn of {} hit a limit: {}", state.name,
                         e.what());
      failure = std::string(kFailFuel);
    }

    // 7. Commit or undo. Either way no choice point survives the turn.
    vm.clearChoicePoints();
    if (failure) {
      arena.release(m0);
      vm.setCursorOpcode(cursor, *spec.entry);
      outcome = {.committed = false, .cause = *failure};
    } else {
      arena.discard(m0);
    }
    liftLimits(context);
    // 8. Scope: the activation frames of this turn only.
    zigzag::expectWritten(
        arena.unlink(cursor, context.core.dims().vars, DimVector::POS));
    zigzag::expectWritten(
        arena.unlink(cursor, context.core.dims().stack, DimVector::POS));
    if (const auto name =
            fieldNameCell(arena, state.vd, agent.agentCell, kFieldTurn)) {
      setField(arena, state.vd, agent.agentCell, kFieldTurn, std::nullopt);
    }
    if (failure) {
      // After the undo, so the count survives it.
      bumpStat(state, kStatDropped);
      bumpStat(state, std::string(kStatFailedPrefix) + *failure);
      GLEDITOR_LOG_DEBUG(kLog, "turn of {} undone: {}", state.name, *failure);
    }
  }

  // 9. Drain. After an undo the outbox is empty again, but a relay is the
  //    host's duty and does not depend on the user's handler succeeding.
  drain(state, context, turn, spec);

  // 10. Count the turn, and every compact_every turns drop dead link runs.
  setCounter(state, kFieldTurnCount, turn + 1);
  flushStats(state);
  if (++state.sinceCompact >= settings_.compactEvery &&
      arena.outstandingMarks() == 0) {
    state.sinceCompact = 0;
    zigzag::expectWritten(arena.compact());
  }
  return outcome;
}

CellRef AgentExecutor::mintEvent(State &state, const Envelope &envelope,
                                 std::string_view space, std::int64_t millis) {
  auto &arena    = state.context->arena;
  const auto &vd = state.vd;
  WireRecord declared;
  declared.type   = envelope.type;
  declared.names  = envelope.fields.names;
  declared.values = envelope.fields.values;
  const auto cell =
      writeValue(arena, vd, WireValue::record(std::move(declared)));
  // The compiler types `source` and `space` as Str, so an absent source is
  // the empty string rather than an empty Opt.
  setField(arena, vd, cell, kEventImplicitSource,
           arena.makeCell(envelope.from));
  setField(arena, vd, cell, kEventImplicitSpace, arena.makeCell(space));
  setField(arena, vd, cell, kEventImplicitTime, arena.makeScalarCell(millis));
  setField(arena, vd, cell, kEventImplicitOrdinal,
           arena.makeScalarCell(static_cast<std::int64_t>(envelope.ordinal)));
  return cell;
}

// -- The drain (§4.2 step 9)
// -----------------------------------------------------

std::vector<AgentExecutor::OutboxItem>
AgentExecutor::collectOutbox(State &state, WorkerContext &context) {
  auto &arena    = context.arena;
  const auto &vd = state.vd;
  const auto entries =
      zigzag::rankAfter(arena, state.agent.agentCell, state.dims.outbox) |
      std::ranges::to<std::vector>();
  std::vector<OutboxItem> items;
  const auto read = [&](CellRef entry,
                        std::string_view field) -> std::optional<WireValue> {
    const auto cell = fieldValue(arena, vd, entry, field);
    if (!cell) return std::nullopt;
    auto value = readValue(arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  };
  const auto textOf =
      [](const std::optional<WireValue> &value) -> std::optional<std::string> {
    if (!value) return std::nullopt;
    if (const auto *s = std::get_if<std::string>(&value->data)) return *s;
    return std::nullopt;
  };
  for (const auto entry : entries) {
    OutboxItem item;
    item.kind = text(arena, entry);
    if (item.kind == kKindEvent) {
      const auto event = read(entry, kOutboxEvent);
      const auto *record =
          event ? std::get_if<WireRecord>(&event->data) : nullptr;
      if (!record) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.envelope.kind = std::string(kKindEvent);
      item.envelope.type = record->type;
      for (std::size_t i = 0; i < record->names.size(); ++i) {
        const auto &name = record->names[i];
        // The adapter stamps these four; a compiler that wrote them is not
        // allowed to speak for the emitter.
        if (name == kEventImplicitSource || name == kEventImplicitSpace ||
            name == kEventImplicitTime || name == kEventImplicitOrdinal) {
          continue;
        }
        item.envelope.fields.set(name, record->values[i]);
      }
      item.space =
          textOf(read(entry, kOutboxSpace)).value_or(std::string(kSpaceOuter));
      if (const auto targets = read(entry, kOutboxFor)) {
        if (const auto *list = std::get_if<WireList>(&targets->data)) {
          for (const auto &element : *list) {
            if (const auto *name = std::get_if<std::string>(&element.data)) {
              item.forNames.push_back(*name);
            }
          }
        } else if (const auto *one = std::get_if<std::string>(&targets->data)) {
          item.forNames.push_back(*one);
        }
        item.hasFor = true;
      }
    } else if (item.kind == kKindSpawn) {
      const auto type = textOf(read(entry, kOutboxAgentType));
      const auto name = textOf(read(entry, kOutboxChildName));
      if (!type || !name) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.agentType = *type;
      item.childName = *name;
      if (const auto args = read(entry, kOutboxArgs)) {
        if (const auto *record = std::get_if<WireRecord>(&args->data)) {
          item.args = *record;
        }
      }
    } else if (item.kind == kKindStop) {
      const auto child = textOf(read(entry, kOutboxStopChild));
      if (!child) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.childName = *child;
      if (const auto keep = read(entry, kOutboxStopKeep)) {
        if (const auto *flag = std::get_if<bool>(&keep->data)) {
          item.keepChildren = *flag;
        }
      }
    } else if (item.kind == kKindTell || item.kind == kKindUntell) {
      const auto fact = read(entry, kOutboxFact);
      const auto *record =
          fact ? std::get_if<WireRecord>(&fact->data) : nullptr;
      const auto targets = read(entry, kOutboxFor);
      const auto *list =
          targets ? std::get_if<WireList>(&targets->data) : nullptr;
      if (!record || record->type.empty() || !list) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.envelope.kind   = item.kind;
      item.envelope.type   = record->type;
      item.envelope.fields = *record;
      item.envelope.fields.type.clear();
      for (const auto &element : *list) {
        if (const auto *name = std::get_if<std::string>(&element.data)) {
          item.forNames.push_back(*name);
        }
      }
    } else if (item.kind == kKindTimer || item.kind == kKindTimerCancel) {
      const auto integer = [&](std::string_view field) {
        const auto value = read(entry, field);
        const auto *i =
            value ? std::get_if<std::int64_t>(&value->data) : nullptr;
        return i == nullptr ? std::optional<std::int64_t>{}
                            : std::optional<std::int64_t>(*i);
      };
      const auto id = integer(kOutboxTimerId);
      if (!id) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.timerId = *id;
      if (item.kind == kKindTimer) {
        const auto event = read(entry, kOutboxEvent);
        const auto *record =
            event ? std::get_if<WireRecord>(&event->data) : nullptr;
        const auto delay = integer(kOutboxTimerDelay);
        if (record == nullptr || !delay) {
          bumpStat(state, kStatMalformed);
          continue;
        }
        item.args     = *record;
        item.delayNs  = *delay;
        item.periodNs = integer(kOutboxTimerPeriod).value_or(0);
      }
    } else if (item.kind == kKindLog) {
      const auto level   = textOf(read(entry, kOutboxLogLevel));
      const auto message = textOf(read(entry, kOutboxLogMessage));
      if (!level || !message) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.level   = *level;
      item.message = *message;
    } else {
      bumpStat(state, kStatUnsupported);
      continue;
    }
    items.push_back(std::move(item));
  }
  // Detach the whole rank: every entry becomes unreachable at once.
  zigzag::expectWritten(
      arena.unlink(state.agent.agentCell, state.dims.outbox, DimVector::POS));
  return items;
}

void AgentExecutor::drain(State &state, WorkerContext &context,
                          std::uint64_t turn, const TurnSpec &spec) {
  const auto items       = collectOutbox(state, context);
  std::uint64_t position = 0;
  // Local spawns first, so a message in the same turn can name the child.
  for (const auto &item : items) {
    if (item.kind == kKindSpawn) {
      spawnChild(state, context, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindEvent) {
      routeEvent(state, context, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindTell || item.kind == kKindUntell) {
      routeTell(state, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindStop) sendStop(state, turn, position, item);
  }
  if (spec.relayed) relay(state, context, turn, position, *spec.relayed);
  // Logging names the agent and carries the handler's own text, never an
  // event payload (§10).
  for (const auto &item : items) {
    if (item.kind != kKindLog) continue;
    if (item.level == kLogLevelInfo) {
      GLEDITOR_LOG_INFO(kLog, "{}: {}", state.name, item.message);
    } else {
      GLEDITOR_LOG_DEBUG(kLog, "{}: {}", state.name, item.message);
    }
  }
  // Timer changes last (§4.2 step 9), in the order the handler made them.
  for (const auto &item : items) {
    if (item.kind == kKindTimer || item.kind == kKindTimerCancel) {
      applyTimerChange(state, item);
    }
  }
}

// -- Timers (§5.6, §10)
// -----------------------------------------------------------

WireList AgentExecutor::readTimers(const State &state) const {
  const auto &arena = state.context->arena;
  const auto cell =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldTimers);
  if (!cell) return {};
  const auto value = readValue(arena, state.vd, *cell);
  const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
  return list == nullptr ? WireList{} : *list;
}

void AgentExecutor::writeTimers(State &state, WireList timers) {
  auto &arena = state.context->arena;
  setField(arena, state.vd, state.agent.agentCell, kFieldTimers,
           writeValue(arena, state.vd, WireValue::list(std::move(timers))));
}

namespace {

std::optional<std::int64_t> timerInt(const WireRecord &record,
                                     std::string_view field) {
  const auto *value = record.find(field);
  const auto *i     = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
  return i == nullptr ? std::optional<std::int64_t>{}
                      : std::optional<std::int64_t>(*i);
}

} // namespace

void AgentExecutor::applyTimerChange(State &state, const OutboxItem &item) {
  auto timers = readTimers(state);
  if (item.kind == kKindTimerCancel) {
    std::erase_if(timers, [&](const WireValue &t) {
      const auto *r = std::get_if<WireRecord>(&t.data);
      return r != nullptr && timerInt(*r, kTimerId) == item.timerId;
    });
  } else {
    WireRecord timer;
    timer.type = std::string(kTimerRecord);
    const std::int64_t period =
        item.periodNs > 0
            ? std::max<std::int64_t>(item.periodNs / kNanosPerMilli, 1)
            : 0;
    const std::int64_t delay =
        std::max<std::int64_t>(item.delayNs, 0) / kNanosPerMilli;
    timer.set(std::string(kTimerId), WireValue::integer(item.timerId));
    timer.set(std::string(kTimerDue),
              WireValue::integer(state.turnMillis + delay));
    timer.set(std::string(kTimerPeriod), WireValue::integer(period));
    timer.set(std::string(kTimerEvent), WireValue::record(item.args));
    timers.push_back(WireValue::record(std::move(timer)));
  }
  writeTimers(state, std::move(timers));
}

bool AgentExecutor::fireDueTimers(State &state, std::int64_t nowMillis) {
  auto timers  = readTimers(state);
  bool changed = false;
  bool fired   = false;
  for (auto it = timers.begin(); it != timers.end();) {
    auto *record   = std::get_if<WireRecord>(&it->data);
    const auto due = record ? timerInt(*record, kTimerDue) : std::nullopt;
    if (record == nullptr || !due) {
      it      = timers.erase(it);
      changed = true;
      continue;
    }
    if (*due > nowMillis) {
      ++it;
      continue;
    }
    const auto period = timerInt(*record, kTimerPeriod).value_or(0);
    const auto *event = record->find(kTimerEvent);
    const auto *body  = event ? std::get_if<WireRecord>(&event->data) : nullptr;
    const std::int64_t missed =
        period > 0 ? (nowMillis - *due) / period + 1 : 1;
    if (body != nullptr) {
      WireRecord fields;
      fields.names  = body->names;
      fields.values = body->values;
      for (std::int64_t n = 0; n < std::min(missed, kMaxTimerCatchUp); ++n) {
        raise(state, body->type, fields);
        fired = true;
      }
    }
    changed = true;
    if (period > 0 && body != nullptr) {
      for (std::size_t i = 0; i < record->names.size(); ++i) {
        if (record->names[i] == kTimerDue) {
          record->values[i] = WireValue::integer(*due + missed * period);
        }
      }
      ++it;
    } else {
      it = timers.erase(it);
    }
  }
  if (changed) writeTimers(state, std::move(timers));
  return fired;
}

void AgentExecutor::sendStop(State &state, std::uint64_t turn,
                             std::uint64_t &position, const OutboxItem &item) {
  const auto pid = peerPid(state, item.childName);
  if (!pid || !contains(innerMembers(state), item.childName)) {
    bumpStat(state, kStatUnroutable);
    return;
  }
  Envelope envelope;
  envelope.kind = std::string(kKindStop);
  envelope.type = std::string(kStopType);
  envelope.from = state.name;
  envelope.fields.set(std::string(kWireStopKeep),
                      WireValue::boolean(item.keepChildren));
  sendEnvelope(state, turn, position, *pid, std::move(envelope));
}

void AgentExecutor::routeTell(State &state, std::uint64_t turn,
                              std::uint64_t &position, const OutboxItem &item) {
  // Only peers are addressable: a name is a pid only where `$peers` says so
  // (the parent and the children), the same rule `emit ... for` follows.
  for (const auto &name : item.forNames) {
    const auto pid = peerPid(state, name);
    if (!pid) {
      bumpStat(state, kStatUnroutable);
      continue;
    }
    Envelope envelope = item.envelope;
    envelope.from     = state.name;
    sendEnvelope(state, turn, position, *pid, std::move(envelope));
  }
}

void AgentExecutor::spawnChild(State &state, WorkerContext &context,
                               std::uint64_t turn, std::uint64_t &position,
                               const OutboxItem &item) {
  auto &arena = context.arena;
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(item.childName));
  params.set(std::string(kWireSpawnParent), WireValue::text(state.name));
  params.set(std::string(kWireSpawnArgs), WireValue::record(item.args));
  params.set(std::string(kWireSpawnAgent), WireValue::text(item.agentType));
  auto parameters   = encodeValue(WireValue::record(std::move(params)));
  const auto failed = [&] {
    bumpStat(state, kStatSpawnFailed);
    WireRecord fields;
    fields.set(std::string(kChildField), WireValue::text(item.childName));
    raise(state, kSpawnFailed, std::move(fields));
  };
  if (!parameters || position > kMaxRequestPosition) {
    failed();
    return;
  }
  SpawnTicket ticket{.programRef = std::string(program_.programRef()),
                     .agentType  = item.agentType,
                     .childName  = item.childName,
                     .parentName = state.name,
                     .parent     = state.pid,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) {
    failed();
    return;
  }
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = requestId(state.prefix, turn, position++);
  request.source        = state.pid;
  request.programRef    = ticket.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  const auto pid        = runtime_.spawn(owner_, request, wallNow());
  if (!pid) {
    GLEDITOR_LOG_DEBUG(kLog, "spawn of {} refused", item.childName);
    failed();
    return;
  }
  needsFirstTurn_.push_back(*pid);
  (void)runtime_.monitor(state.pid, *pid, clock_.steady());

  // A child appears twice: on d.holon, and as a member of the inner space, so
  // leaving the space leaves the holon alone (§5.2).
  appendToRank(arena, state.agent.agentCell, state.dims.holon,
               arena.makeCell(std::string_view(item.childName)));
  if (const auto space = innerSpace(state)) {
    appendToRank(arena, *space, state.dims.member,
                 arena.makeCell(std::string_view(item.childName)));
  }
  if (const auto peers =
          fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers)) {
    setField(arena, state.vd, *peers, item.childName,
             arena.makeCell(std::string_view(pidToText(*pid))));
  }
}

void AgentExecutor::routeEvent(State &state, WorkerContext &,
                               std::uint64_t turn, std::uint64_t &position,
                               const OutboxItem &item) {
  Envelope envelope = item.envelope;
  envelope.from     = state.name;
  // One ordinal per emission, not per recipient: it is the emitter's counter.
  envelope.ordinal = counter(state, kFieldOrdinal) + 1;
  setCounter(state, kFieldOrdinal, envelope.ordinal);

  if (item.space == kSpaceInner) {
    const auto members = innerMembers(state);
    std::vector<std::string> targets;
    if (item.hasFor) {
      for (const auto &name : item.forNames) {
        if (contains(members, name)) {
          targets.push_back(name);
        } else {
          bumpStat(state, kStatUnroutable);
        }
      }
    } else {
      targets = members;
    }
    for (const auto &name : targets) {
      const auto pid = peerPid(state, name);
      if (!pid) {
        bumpStat(state, kStatUnroutable);
        continue;
      }
      sendEnvelope(state, turn, position, *pid, envelope);
    }
    return;
  }
  if (item.space == kSpaceOuter) {
    if (!state.parentPid) {
      if (hostSink_) {
        hostSink_(envelope);
      } else {
        bumpStat(state, kStatUnroutable);
      }
      return;
    }
    // The parent hosts the space; it fans the emission out to its members.
    envelope.relaySpace = std::string(kSpaceInner);
    envelope.relayFor   = item.forNames;
    sendEnvelope(state, turn, position, *state.parentPid, std::move(envelope));
    return;
  }
  bumpStat(state, kStatUnsupported);
}

void AgentExecutor::relay(State &state, WorkerContext &, std::uint64_t turn,
                          std::uint64_t &position, const Envelope &received) {
  // Done here where the design has it in `std:agent`: the relay is plain
  // fan-out and iteration 2 has no compiled prelude to carry it.
  for (const auto &name : innerMembers(state)) {
    if (name == received.from) continue;
    if (!received.relayFor.empty() && !contains(received.relayFor, name)) {
      continue;
    }
    const auto pid = peerPid(state, name);
    if (!pid) {
      bumpStat(state, kStatUnroutable);
      continue;
    }
    Envelope forwarded = received;
    forwarded.relaySpace.reset();
    forwarded.relayFor.clear();
    sendEnvelope(state, turn, position, *pid, std::move(forwarded));
  }
}

std::string AgentExecutor::floorFor(const State &state,
                                    std::uint64_t turn) const {
  auto floor       = requestId(state.prefix, turn, 0);
  const auto owner = pidKey(state.pid);
  for (const auto &[id, sent] : outstanding_) {
    if (sent.ownerKey == owner && id < floor) floor = id;
  }
  return floor;
}

bool AgentExecutor::sendEnvelope(State &state, std::uint64_t turn,
                                 std::uint64_t &position,
                                 const identity::ActorPid &to,
                                 Envelope envelope) {
  if (position > kMaxRequestPosition) {
    bumpStat(state, kStatSendFailed);
    return false;
  }
  envelope.floor   = floorFor(state, turn);
  const auto bytes = encodeEnvelope(envelope);
  if (!bytes || bytes->size() > kMaxMessageValue) {
    bumpStat(state, kStatSendFailed);
    return false;
  }
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = requestId(state.prefix, turn, position++);
  message.source    = state.pid;
  message.target    = to;
  message.value     = *bytes;
  // Registered before the send: a loopback peer may answer before it returns.
  outstanding_[message.requestId] =
      Outstanding{pidKey(state.pid), to.node, wallNow(),   0,
                  envelope.kind,     to,      std::nullopt};
  const auto sent = runtime_.send(to.node, message);
  if (!sent && sent.error() != ActorError::TransportUnavailable) {
    outstanding_.erase(message.requestId);
    bumpStat(state, kStatSendFailed);
    return false;
  }
  return true;
}

// -- Launching, upkeep and pumping
// ---------------------------------------------------

std::expected<identity::ActorPid, ActorError>
AgentExecutor::launch(std::string_view agentType, std::string_view name,
                      WireRecord args) {
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(std::string(name)));
  params.set(std::string(kWireSpawnParent), WireValue::text(std::string{}));
  params.set(std::string(kWireSpawnArgs), WireValue::record(std::move(args)));
  params.set(std::string(kWireSpawnAgent),
             WireValue::text(std::string(agentType)));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters) return std::unexpected(ActorError::InvalidMessage);
  SpawnTicket ticket{.programRef = std::string(program_.programRef()),
                     .agentType  = std::string(agentType),
                     .childName  = std::string(name),
                     .parentName = {},
                     .parent     = std::nullopt,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) return std::unexpected(issued.error());
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = requestId(senderPrefix(kHostName), launches_++, 0);
  request.source        = hostPid_;
  request.programRef    = ticket.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  const auto pid        = runtime_.spawn(owner_, request, wallNow());
  if (!pid) return std::unexpected(pid.error());
  needsFirstTurn_.push_back(*pid);
  return pid;
}

std::expected<void, ActorError>
AgentExecutor::deliver(const identity::ActorPid &target, Envelope envelope) {
  // The lowest possible floor: a host delivery declares nothing complete.
  // Per node: an agent resumed on another host remembers the ids the old
  // host's deliveries used, and a second host counting from zero would
  // collide with them.
  const auto prefix =
      senderPrefix(std::string(kHostName) + "/send@" + localNode_);
  envelope.kind    = std::string(kKindEvent);
  envelope.floor   = requestId(prefix, 0, 0);
  const auto bytes = encodeEnvelope(envelope);
  if (!bytes) return std::unexpected(ActorError::InvalidMessage);
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = requestId(prefix, hostSends_++, 0);
  message.source    = hostPid_;
  message.target    = target;
  message.value     = *bytes;
  return runtime_.receive(owner_, message, clock_.steady(), wallNow());
}

void AgentExecutor::tick() { tick(wallNow()); }

bool AgentExecutor::setDeliveryDeadline(
    std::string_view id, std::chrono::system_clock::time_point deadline) {
  const auto found = outstanding_.find(std::string(id));
  if (found == outstanding_.end()) return false;
  found->second.deadline = deadline;
  return true;
}

void AgentExecutor::tick(std::chrono::system_clock::time_point now) {
  upkeepDeliveries(now);
  upkeepLeases(now);
  // The timer heap is a function of cells (§5.6): read `$timers` afresh rather
  // than keep a second copy that `resume` would have to rebuild.
  const auto nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();
  for (auto &[key, state] : states_) {
    if (!live(*state) || state->inTurn) continue;
    liftLimits(*state->context);
    (void)fireDueTimers(*state, nowMillis);
  }
  // Events raised between turns need a turn to be heard in.
  for (auto &[key, state] : states_) {
    if (state->hostEvents.empty()) continue;
    if (!live(*state) || !runtime_.wake(state->pid, now)) {
      state->hostEvents.clear();
    }
  }
}

void AgentExecutor::upkeepDeliveries(
    std::chrono::system_clock::time_point now) {
  for (auto it = outstanding_.begin(); it != outstanding_.end();) {
    auto &sent      = it->second;
    const bool late = sent.deadline && now >= *sent.deadline;
    if (!late && now - sent.lastSent < settings_.sendRetryInterval) {
      ++it;
      continue;
    }
    if (late || sent.redeliveries >= settings_.sendRetries) {
      (void)runtime_.abandon(it->first);
      if (const auto owner = states_.find(sent.ownerKey);
          owner != states_.end() && live(*owner->second)) {
        State &state       = *owner->second;
        std::string target = pidKey(sent.to);
        for (const auto &[nameCell, valueCell] :
             fieldsOf(state.context->arena, state.vd,
                      fieldValue(state.context->arena, state.vd,
                                 state.agent.agentCell, kFieldPeers)
                          .value_or(state.agent.agentCell))) {
          if (valueCell &&
              pidFromText(text(state.context->arena, *valueCell)) ==
                  std::optional<identity::ActorPid>(sent.to)) {
            target = text(state.context->arena, nameCell);
            break;
          }
        }
        WireRecord fields;
        fields.set(std::string(kToField), WireValue::text(target));
        fields.set(std::string(kKindField), WireValue::text(sent.kind));
        fields.set(std::string(kCauseField),
                   WireValue::text(
                       std::string(late ? kCauseDeadline : kCauseRetries)));
        raise(state, kDeliveryFailed, std::move(fields));
        bumpStat(state, kStatDeliveryFailed);
      }
      GLEDITOR_LOG_DEBUG(kLog, "delivery {} abandoned", it->first);
      it = outstanding_.erase(it);
      continue;
    }
    // The runtime erases a delivery when its reply arrives, so a refusal here
    // means the message completed.
    const auto again = runtime_.redeliver(it->first);
    if (!again && again.error() == ActorError::InvalidMessage) {
      it = outstanding_.erase(it);
      continue;
    }
    ++sent.redeliveries;
    sent.lastSent = now;
    ++it;
  }
}

void AgentExecutor::upkeepLeases(std::chrono::system_clock::time_point now) {
  for (auto &[key, owned] : states_) {
    State &state = *owned;
    if (!live(state) || !state.grant) continue;
    const auto end = state.grant->expiresAt;
    if (now >= end) {
      // The runtime refuses to run an expired actor, so Destroy runs here on
      // the arena it lent us.
      liftLimits(*state.context);
      orderlyStop(state, state.cursor, *state.grant, *state.context,
                  kCauseLease);
      continue;
    }
    const auto renewAt =
        state.leaseSeen +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            (end - state.leaseSeen) * settings_.renewFraction);
    if (now >= renewAt) (void)renewLease(state, now);
  }
}

bool AgentExecutor::renewLease(State &state,
                               std::chrono::system_clock::time_point now) {
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(state.name));
  params.set(std::string(kWireSpawnParent), WireValue::text(state.parentName));
  params.set(std::string(kWireSpawnArgs), WireValue::record({}));
  params.set(std::string(kWireSpawnAgent), WireValue::text(state.agentType));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters) return false;
  SpawnTicket ticket{.programRef = std::string(program_.programRef()),
                     .agentType  = state.agentType,
                     .childName  = state.name,
                     .parentName = state.parentName,
                     .parent     = state.parentPid,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) return false;
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = requestId(senderPrefix(kHostName), launches_++, 0);
  request.source        = hostPid_;
  request.programRef    = ticket.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  if (!runtime_.renew(state.pid, owner_, request, now)) return false;
  // The runtime hands the new grant to the executor, not to us; a wake that
  // runs nothing is how the adapter learns the new expiry.
  state.quietWake = true;
  if (!runtime_.wake(state.pid, now)) state.quietWake = false;
  return true;
}

std::size_t AgentExecutor::pumpUntilIdle(std::size_t maxTurns) {
  std::size_t turns = 0;
  while (turns < maxTurns) {
    bool progressed = false;
    auto pending    = std::move(needsFirstTurn_);
    needsFirstTurn_.clear();
    for (const auto &pid : pending) {
      const auto known = states_.find(pidKey(pid));
      if (known != states_.end() && known->second->prepared &&
          known->second->context && initialized(*known->second)) {
        continue;
      }
      if (runtime_.wake(pid, wallNow())) {
        ++turns;
        progressed = true;
      }
    }
    while (turns < maxTurns && needsFirstTurn_.empty()) {
      const auto pumped = runtime_.pumpOne(wallNow());
      if (!pumped || !*pumped) break;
      ++turns;
      progressed = true;
    }
    for (auto &[key, state] : states_) {
      if (!state->hostEvents.empty() && live(*state) && turns < maxTurns) {
        if (runtime_.wake(state->pid, wallNow())) {
          ++turns;
          progressed = true;
        } else {
          state->hostEvents.clear();
        }
      }
    }
    if (!progressed && needsFirstTurn_.empty()) break;
  }
  return turns;
}

// -- Checkpoint, resume and migration (§4.2, §4.4, §4.5)
// -------------------------
//
// How compiled routines find restored state. A checkpoint is the graph
// reachable from the agent cell, and `resume` rebuilds it as a base Manifold
// whose refs are dense indices of the replayed history, not the refs the
// running arena used. The compiled routines name their registers, field value
// cells and the agent cell by ref, in operand slots that hold those refs as
// plain integers, which promotion does not remap. The restored cells are
// therefore never run: `prepare` instantiates the program afresh in the
// rebuilt arena (a new agent cell, new registers) and `restoreState` copies the
// saved values onto it. Nothing here needs handle operands (P16).

void AgentExecutor::restoreState(State &state, CellRef saved) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto ac    = state.agent.agentCell;
  const auto &core = state.context->core.dims();

  // The rebuilt base does not register its dimensions, so the core and
  // `registerDimensions` minted fresh cells for "d.vars" and the rest, while
  // the saved cells link along the old ones. Find those by name. Only the
  // base's cells are walked: the core hangs its standard library off the
  // base's home cell, which is the agent cell, and that part is the new
  // arena's own.
  const auto inBase = [](CellRef ref) {
    return ref != zigzag::noCell && (ref & zigzag::ephemeralBit) == 0;
  };
  std::unordered_map<std::string, zigzag::DimRef> savedDim;
  {
    std::unordered_map<CellRef, bool> seen{{saved, true}};
    std::vector<CellRef> frontier{saved};
    for (std::size_t i = 0; i < frontier.size(); ++i) {
      for (const auto &link : arena.dimensionsOf(frontier[i])) {
        if (!inBase(link.dim)) continue;
        savedDim.try_emplace(text(arena, link.dim), link.dim);
        for (const auto next : {link.pos, link.neg}) {
          if (inBase(next) && seen.emplace(next, true).second) {
            frontier.push_back(next);
          }
        }
      }
    }
  }
  const auto old = [&](std::string_view name) {
    const auto found = savedDim.find(std::string(name));
    return found == savedDim.end() ? zigzag::noCell : found->second;
  };
  const ValueDims oldVd{.vars   = old("d.vars"),
                        .values = old("d.values"),
                        .grab   = old("d.grab"),
                        .step   = old("d.step")};
  GLEDITOR_LOG_DEBUG(kLog, "restoring {} from '{}': {} dimensions known",
                     state.name, text(arena, saved), savedDim.size());

  const std::array valueFrom{oldVd.vars, oldVd.values, oldVd.grab, oldVd.step};
  const std::array valueInto{vd.vars, vd.values, vd.grab, vd.step};
  const std::array termFrom{oldVd.grab, oldVd.step, old("d.source")};
  const std::array termInto{vd.grab, vd.step, state.dims.source};
  const std::array newValueDims{vd.vars, vd.values, vd.grab, vd.step};

  // Fields. A scalar or text field is read by compiled code through its
  // static value cell, so it is overwritten in place; a field with structure
  // (a list, a record) is read through its name cell and is replaced.
  if (oldVd.vars != zigzag::noCell && oldVd.values != zigzag::noCell) {
    for (const auto &[nameCell, valueCell] : fieldsOf(arena, oldVd, saved)) {
      if (!valueCell) continue;
      const auto name = text(arena, nameCell);
      if (name.empty() || name == kFieldTurn) continue;
      const auto target = fieldValue(arena, vd, ac, name);
      const bool structured =
          hasLinks(arena, *valueCell, valueFrom) ||
          (target && hasLinks(arena, *target, newValueDims));
      if (target && !structured) {
        copyCellData(arena, *valueCell, *target);
      } else {
        setField(arena, vd, ac, name,
                 cloneGraph(arena, *valueCell, valueFrom, valueInto));
      }
    }
  }

  const auto ranked = [&](CellRef owner, zigzag::DimRef dim) {
    return dim == zigzag::noCell ? std::vector<CellRef>{}
                                 : zigzag::rankAfter(arena, owner, dim) |
                                       std::ranges::to<std::vector<CellRef>>();
  };

  // Beliefs: the predicates are the new program's cells; the clauses, with
  // their terms and sources, are copied under them.
  const auto freshPredicates = ranked(ac, state.dims.belief);
  const auto oldClause       = old("d.clause");
  for (const auto predicate : ranked(saved, old("d.belief"))) {
    const auto name = text(arena, predicate);
    const auto match =
        std::ranges::find_if(freshPredicates, [&](CellRef candidate) {
          return text(arena, candidate) == name;
        });
    if (match == freshPredicates.end()) continue;
    for (const auto clause : ranked(predicate, oldClause)) {
      appendToRank(arena, *match, core.clause,
                   cloneGraph(arena, clause, termFrom, termInto));
    }
  }

  // Hosted spaces and their members, then children.
  auto spaces = ranked(ac, state.dims.space);
  for (const auto space : ranked(saved, old("d.space"))) {
    const auto name  = text(arena, space);
    const auto match = std::ranges::find_if(
        spaces, [&](CellRef c) { return text(arena, c) == name; });
    CellRef fresh;
    if (match == spaces.end()) {
      fresh = arena.makeCell(std::string_view(name));
      appendToRank(arena, ac, state.dims.space, fresh);
      spaces.push_back(fresh);
    } else {
      fresh = *match;
    }
    for (const auto member : ranked(space, old("d.member"))) {
      appendToRank(arena, fresh, state.dims.member,
                   arena.makeCell(std::string_view(text(arena, member))));
    }
  }
  for (const auto child : ranked(saved, old("d.holon"))) {
    appendToRank(arena, ac, state.dims.holon,
                 arena.makeCell(std::string_view(text(arena, child))));
  }

  // §5.5: request ids from this name must exceed any the old incarnation
  // sent, whose turn counter the checkpoint may be behind.
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          wallNow().time_since_epoch())
          .count());
  if (counter(state, kFieldTurnCount) < millis) {
    setCounter(state, kFieldTurnCount, millis);
  }
}

void AgentExecutor::announceMoved(State &state) {
  const auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers) return;
  std::vector<identity::ActorPid> targets;
  for (const auto &[nameCell, valueCell] : fieldsOf(arena, state.vd, *peers)) {
    if (!valueCell) continue;
    if (const auto pid = pidFromText(text(arena, *valueCell))) {
      targets.push_back(*pid);
    }
  }
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  std::uint64_t position   = 0;
  for (const auto &pid : targets) {
    Envelope envelope;
    envelope.kind = std::string(kKindEvent);
    envelope.type = std::string(kEventMoved);
    envelope.from = state.name;
    envelope.fields.set(std::string(kMovedName), WireValue::text(state.name));
    sendEnvelope(state, turn, position, pid, std::move(envelope));
  }
  // The ids above are spent; no later turn may mint them again.
  setCounter(state, kFieldTurnCount, turn + 1);
}

void AgentExecutor::applyMoved(State &state, const ActorMessage &message,
                               const Envelope &envelope) {
  const auto *named = envelope.fields.find(kMovedName);
  const auto *name  = named ? std::get_if<std::string>(&named->data) : nullptr;
  // Only an agent can announce its own move, and only a peer we know of.
  if (!name || *name != envelope.from || !message.source) return;
  auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers || !fieldNameCell(arena, state.vd, *peers, *name)) return;
  setField(arena, state.vd, *peers, *name,
           arena.makeCell(std::string_view(pidToText(*message.source))));
  if (*name == state.parentName) state.parentPid = message.source;
  GLEDITOR_LOG_DEBUG(kLog, "{} learns that {} moved", state.name, *name);
}

AgentExecutor *AgentExecutor::setCheckpointIdentity(CheckpointIdentity id) {
  checkpointIdentity_ = std::move(id);
  return this;
}

std::expected<AgentCheckpoint, CheckpointRefusal>
AgentExecutor::checkpoint(const identity::ActorPid &pid) {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !found->second->prepared) {
    return std::unexpected(CheckpointRefusal::UnknownAgent);
  }
  State &state = *found->second;
  if (!live(state)) return std::unexpected(CheckpointRefusal::Stopped);
  if (!state.hostEvents.empty()) {
    return std::unexpected(CheckpointRefusal::HostDeliveryPending);
  }
  if (runtime_.pending(pid) != 0) {
    return std::unexpected(CheckpointRefusal::MailboxNotEmpty);
  }
  const auto owner = pidKey(pid);
  if (std::ranges::any_of(outstanding_, [&](const auto &entry) {
        return entry.second.ownerKey == owner;
      })) {
    return std::unexpected(CheckpointRefusal::SendsIncomplete);
  }
  const auto view = runtime_.checkpointView(pid, state.agent.agentCell);
  if (!view) return std::unexpected(CheckpointRefusal::NotQuiescent);
  if (!checkpointIdentity_) {
    return std::unexpected(CheckpointRefusal::NoIdentity);
  }
  const auto continuation =
      zigzag::vortex::mesh::encodeContinuation(view->continuation);
  if (!continuation) {
    return std::unexpected(CheckpointRefusal::PublishFailed);
  }
  const auto &id = *checkpointIdentity_;
  zigzag::vortex::mesh::ValueLimits limits;
  limits.operations = settings_.valueOperations;
  xanadu::Store store;
  auto seed = zigzag::vortex::mesh::publishValue(
      store, {}, view->arena, view->root, id.keys, id.origin, id.provenance,
      id.seedRoot, limits, {}, *continuation);
  if (!seed) {
    GLEDITOR_LOG_DEBUG(kLog, "checkpoint of {} failed: {}", state.name,
                       seed.error());
    return std::unexpected(CheckpointRefusal::PublishFailed);
  }
  AgentCheckpoint out;
  out.pid        = pid;
  out.agentType  = state.agentType;
  out.name       = state.name;
  out.parentName = state.parentName;
  out.programRef = std::string(program_.programRef());
  out.adopted    = state.adopted;
  out.source     = std::make_shared<xanadu::DirectoryContentSource>();
  for (const auto &payload : seed->payloads) {
    out.source->add(payload.torrent.file, payload.path.string());
  }
  out.source->add(seed->history.torrentFile,
                  (id.seedRoot / seed->history.hash.hex()).string());
  out.source->add(seed->carrier.file, seed->carrierPath.string());
  out.contentHash =
      xanadu::sha256Hex(zigzag::vortex::mesh::encodeValue(seed->value));
  out.seed = std::move(*seed);
  return out;
}

std::expected<identity::ActorPid, ActorError>
AgentExecutor::resume(const AgentCheckpoint &checkpoint) {
  zigzag::vortex::mesh::ValueLimits limits;
  limits.operations = settings_.valueOperations;
  auto received     = zigzag::vortex::mesh::fetchValue(
      checkpoint.seed.carrier.hash, *checkpoint.source,
      std::make_shared<xanadu::UserPermascroll>(), limits);
  if (!received) {
    GLEDITOR_LOG_DEBUG(kLog, "resume cannot read the checkpoint: {}",
                       received.error());
    return std::unexpected(ActorError::InvalidMessage);
  }
  // This host's owner signed the checkpoint, so the node that spawned the
  // agent is the owner's too; the runtime checks the spawner's node is known.
  if (const auto continuation = zigzag::vortex::mesh::decodeContinuation(
          received->value.continuation);
      continuation && continuation->spawnRequest.source) {
    runtime_.trustNode(continuation->spawnRequest.source->node, owner_);
  }
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(checkpoint.name));
  params.set(std::string(kWireSpawnParent),
             WireValue::text(checkpoint.parentName));
  params.set(std::string(kWireSpawnArgs), WireValue::record({}));
  params.set(std::string(kWireSpawnAgent),
             WireValue::text(checkpoint.agentType));
  params.set(std::string(kWireSpawnBase),
             WireValue::text(checkpoint.contentHash));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters) return std::unexpected(ActorError::InvalidMessage);
  SpawnTicket ticket{.programRef = checkpoint.programRef,
                     .agentType  = checkpoint.agentType,
                     .childName  = checkpoint.name,
                     .parentName = {},
                     .parent     = std::nullopt,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) return std::unexpected(issued.error());
  ActorMessage request;
  request.type      = MessageType::ActorSpawn;
  request.requestId = requestId(
      senderPrefix(std::string(kHostName) + std::string(kResumeSuffix)),
      resumes_++, 0);
  request.source        = hostPid_;
  request.programRef    = checkpoint.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  const auto pid        = runtime_.resume(owner_, request, std::move(*received),
                                          checkpoint.source, wallNow());
  if (!pid) return std::unexpected(pid.error());
  resumeAdopted_[pidKey(*pid)] = checkpoint.adopted;
  needsFirstTurn_.push_back(*pid);
  return pid;
}

bool AgentExecutor::retire(const identity::ActorPid &pid) {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !live(*found->second)) return false;
  if (!runtime_.stop(pid)) return false;
  found->second->context = nullptr;
  return true;
}

} // namespace xanadu::vagent
