#include "common/xanadu/vagent/agent_executor.hpp"

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/cell_views.hpp"

#include <algorithm>
#include <limits>
#include <ranges>

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

/// The envelope type that carries a `stop` performative.
constexpr std::string_view kStopType = "Stop";

/// A host event that raises another keeps the actor busy; this many rounds of
/// them run in one executor call and the rest wait for the next tick, so a
/// handler that provokes its own event cannot hold the owner thread.
constexpr std::size_t kMaxHostEventRounds = 8;

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

} // namespace

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
                            const ActorMessage &spawn) {
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

    const auto ac     = state.agent.agentCell;
    const auto &vd    = state.vd;
    const bool fresh  = !fieldNameCell(arena, vd, ac, kFieldName);
    const auto ensure = [&](std::string_view field, const WireValue &value) {
      if (!fieldNameCell(arena, vd, ac, field)) {
        setField(arena, vd, ac, field, writeValue(arena, vd, value));
      }
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
          setField(arena, vd, ac, info->args.names[i],
                   writeValue(arena, vd, info->args.values[i]));
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
  if (!state.prepared && !prepare(state, context, execution.spawnRequest)) {
    return fatal(state, kCauseInit);
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

void AgentExecutor::stageChildStopping(State &state, std::string_view cause) {
  if (!state.parentPid) return;
  const std::uint64_t turn =
      state.prepared && state.context ? counter(state, kFieldTurnCount) : 0;
  Envelope envelope;
  envelope.kind = std::string(kKindEvent);
  envelope.type = std::string(kChildStopping);
  envelope.from = state.name;
  envelope.fields.set(std::string(kChildField), WireValue::text(state.name));
  envelope.fields.set(std::string(kCauseField),
                      WireValue::text(std::string(cause)));
  std::uint64_t position = 0;
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
  if (envelope.kind != kKindEvent && envelope.kind != kKindStop) {
    bumpStat(state, kStatUnsupported);
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
  } else if (state.parentPid && message.source == state.parentPid) {
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
  orderlyStop(state, execution.cursor, execution.grant, context, kCauseStop);
  return std::string{};
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
    orderlyStop(state, cursor, grant, context, kCauseTerminate);
  }
}

void AgentExecutor::orderlyStop(State &state, CellRef cursor,
                                const ActorGrant &grant, WorkerContext &context,
                                std::string_view cause) {
  GLEDITOR_LOG_DEBUG(kLog, "agent {} stops: {}", state.name, cause);
  // Whatever was waiting is moot once the agent is on its way out.
  state.hostEvents.clear();
  deliverHost(state, cursor, grant, context,
              HostEvent{std::string(kEventDestroy), {}});
  state.hostEvents.clear();
  stageChildStopping(state, cause);
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
  const auto optText = [&](std::string_view value) {
    return makeOpt(arena, vd,
                   value.empty()
                       ? std::nullopt
                       : std::optional<CellRef>(arena.makeCell(value)));
  };
  setField(arena, vd, cell, kEventImplicitSource, optText(envelope.from));
  setField(arena, vd, cell, kEventImplicitSpace, optText(space));
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
    if (item.kind == kKindStop) sendStop(state, turn, position, item);
  }
  if (spec.relayed) relay(state, context, turn, position, *spec.relayed);
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
  sendEnvelope(state, turn, position, *pid, std::move(envelope));
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
      bumpStat(state, kStatUnroutable);
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

} // namespace xanadu::vagent
