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

constexpr std::string_view kChildStopping = "ChildStopping";
constexpr std::string_view kChildField    = "child";
constexpr std::string_view kCauseField    = "cause";
constexpr std::string_view kCauseInit     = "init";

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

bool AgentExecutor::prepare(State &state, WorkerContext &context) {
  auto &arena = context.arena;
  std::optional<SpawnInfo> info;
  const auto key = pidKey(state.pid);
  if (const auto found = spawnInfo_.find(key); found != spawnInfo_.end()) {
    info = std::move(found->second);
    spawnInfo_.erase(found);
  }
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
  liftLimits(context);
  if (!state.prepared && !prepare(state, context)) {
    return fatal(state, kCauseInit);
  }
  flushStats(state);

  bool justInitialised = false;
  if (!initialized(state)) {
    TurnSpec spec;
    spec.entry           = state.agent.initEntry;
    spec.markInitialized = true;
    if (!runTurn(state, execution, context, spec).committed) {
      return fatal(state, kCauseInit);
    }
    justInitialised = true;
  }
  if (!execution.message) {
    if (!justInitialised) {
      TurnSpec spec;
      spec.entry = state.agent.dispatchEntry;
      runTurn(state, execution, context, spec);
    }
    return std::string{};
  }
  auto result = handleMessage(state, execution, context);
  flushStats(state);
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
    const auto name = text(arena, nameCell);
    bumpStat(state, kStatChildDown);
    // A stopped sender will send no later envelope carrying a floor, so its
    // last remembered ids would stay for good without this (P14).
    (void)runtime_.forgetSender(state.pid, senderPrefix(name));
    return;
  }
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
  if (envelope.kind != kKindEvent) {
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

  const bool relayed = envelope.relaySpace.has_value();
  if (relayed && *envelope.relaySpace != kSpaceInner) {
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
  // The host handles a relayed emission itself unless a `for` list leaves it
  // out; the relay to its members is the host's duty either way.
  const bool handle = !relayed || envelope.relayFor.empty() ||
                      contains(envelope.relayFor, state.name);
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
  runTurn(state, execution, context, spec);
  return std::string{};
}

// -- One turn (§4.2)
// -------------------------------------------------------------

AgentExecutor::TurnOutcome
AgentExecutor::runTurn(State &state, const ActorExecution &execution,
                       WorkerContext &context, const TurnSpec &spec) {
  auto &arena              = context.arena;
  auto &vm                 = context.vm;
  const auto cursor        = execution.cursor;
  const auto &agent        = state.agent;
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  TurnOutcome outcome{.committed = true, .cause = {}};

  if (spec.entry) {
    // 1. Mark.
    const auto m0 = arena.mark();
    // 2. Fuel, relative to where the turn starts.
    const auto &grant = execution.grant;
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
  if (spec.relayed) relay(state, context, turn, position, *spec.relayed);
}

void AgentExecutor::spawnChild(State &state, WorkerContext &context,
                               std::uint64_t turn, std::uint64_t &position,
                               const OutboxItem &item) {
  auto &arena = context.arena;
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(item.childName));
  params.set(std::string(kWireSpawnParent), WireValue::text(state.name));
  params.set(std::string(kWireSpawnArgs), WireValue::record(item.args));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters || position > kMaxRequestPosition) {
    bumpStat(state, kStatSpawnFailed);
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
    bumpStat(state, kStatSpawnFailed);
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
    bumpStat(state, kStatSpawnFailed);
    return;
  }
  spawnInfo_[pidKey(*pid)] = SpawnInfo{item.agentType, item.childName,
                                       state.name, state.pid, item.args};
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
      Outstanding{pidKey(state.pid), to.node, wallNow(), 0};
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
  params.set(std::string(kWireSpawnArgs), WireValue::record(args));
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
  spawnInfo_[pidKey(*pid)] = SpawnInfo{std::string(agentType),
                                       std::string(name),
                                       {},
                                       std::nullopt,
                                       std::move(args)};
  needsFirstTurn_.push_back(*pid);
  return pid;
}

void AgentExecutor::tick() { tick(wallNow()); }

void AgentExecutor::tick(std::chrono::system_clock::time_point now) {
  for (auto it = outstanding_.begin(); it != outstanding_.end();) {
    auto &sent = it->second;
    if (now - sent.lastSent < settings_.sendRetryInterval) {
      ++it;
      continue;
    }
    if (sent.redeliveries >= settings_.sendRetries) {
      (void)runtime_.abandon(it->first);
      // Raising DeliveryFailed waits for iteration 3; the count is kept now.
      if (const auto owner = states_.find(sent.ownerKey);
          owner != states_.end()) {
        bumpStat(*owner->second, kStatDeliveryFailed);
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
    if (!progressed && needsFirstTurn_.empty()) break;
  }
  return turns;
}

} // namespace xanadu::vagent
