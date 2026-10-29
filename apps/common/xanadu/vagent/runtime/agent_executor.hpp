/**
 * @file agent_executor.hpp
 * @brief The mesh executor adapter: the `MeshActorRuntime::Executor` that turns
 *        a compiled vagent agent into one actor (design §4.1, §4.2, §4.5,
 *        §5.4, §5.5, §7.2, §7.3, §11.1).
 *
 * The adapter is C++ because it reaches what Vortex code cannot: the mesh
 * runtime's tables, the clock, the capability issuer and the VM's marks. It
 * writes cells in the shapes of `runtime_abi.hpp` and runs the compiled
 * routines of a `CompiledAgent`; it knows nothing of the language those
 * routines came from, so a program is anything that implements `AgentProgram`.
 *
 * A turn (message or wake) is one `execute` call's worth of steps 1-10 of
 * §4.2. An agent's first turn of any kind first runs `initEntry` as a turn of
 * its own, committed before anything is dispatched, so a failing message can
 * never undo a successful initialisation and re-run it.
 *
 * Iteration 2 covers `event` and `spawn` outbox entries. The relay of an
 * emission into a space the emitter does not host is done here in C++, where
 * the design puts it in `std:agent`; see `relay` in agent_executor.cpp.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/resolver.hpp"
#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/host_bindings.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"
#include "common/xanadu/vortex/worker_context.hpp"

namespace xanadu::vagent::runtime {

/// The §10 settings the adapter reads, with their defaults. P9 will fill this
/// from `system://settings`; until then the host constructs it.
struct AgentSettings {
  std::size_t beliefEventDepth{16};
  std::size_t conditionFuel{1000};
  std::chrono::milliseconds maintainBackoff{30000};
  std::size_t sendRetries{5};
  std::chrono::milliseconds sendRetryInterval{1000};
  double renewFraction{0.8};
  std::size_t readyPerMessage{4};
  std::size_t compactEvery{64};
  /// `vagent.value_operations`: operations one checkpoint may mint, and the
  /// most `resume` will rebuild (P5). An agent that grows past it cannot be
  /// moved or have its dead cells reclaimed.
  std::uint32_t valueOperations{65536};
  /// The validator wants a hashcash stamp on a spawn and the runtime does not
  /// check one, so a spawn the host itself issues carries this fixed text.
  std::string spawnStamp{"vagent-local-spawn"};
  EpistemicLimits epistemic{};
};

/// What the issuer signs for one child. `parameters` is the bencoded
/// dictionary of `kWireSpawn*` (name, parent, args); the issuer embeds it in
/// the capability it returns and debits the parent's budget (§4.1).
struct SpawnTicket {
  std::string programRef;
  std::string agentType;
  std::string childName;
  /// Empty for a top-level agent.
  std::string parentName;
  std::optional<identity::ActorPid> parent;
  std::string parameters;
};

struct IssuedCapability {
  std::string capability;
  std::string hashcashStamp;
};

/// The host's delegated device key, as the adapter sees it. A running agent
/// cannot sign anything itself.
class CapabilityIssuer {
public:
  virtual ~CapabilityIssuer() = default;
  [[nodiscard]] virtual std::expected<IssuedCapability,
                                      zigzag::vortex::mesh::ActorError>
  issue(const SpawnTicket &ticket) = 0;
  /// The `SpawnTicket::parameters` dictionary inside a capability this issuer
  /// (or a peer's) signed; nullopt if the capability is not one it can read.
  /// The adapter cannot parse a signed capability itself.
  [[nodiscard]] virtual std::optional<std::string>
  parametersOf(std::string_view capability) const = 0;
};

/// A compiled program, or a stand-in for one. The adapter calls `instantiate`
/// when it sets up an agent's arena on that agent's first turn, and again for
/// an arena rebuilt by `resume`; an implementation that finds its agent cell
/// already present returns the same one.
class AgentProgram {
public:
  virtual ~AgentProgram() = default;
  /// What a spawn's `programRef` names; checked by the authorizer.
  [[nodiscard]] virtual std::string_view programRef() const = 0;
  [[nodiscard]] virtual std::expected<CompiledAgent, std::string>
  instantiate(zigzag::vortex::WorkerContext &context,
              std::string_view agentType) = 0;
};

/// The two clocks the adapter reads. Injected so tests control delivery
/// upkeep and the turn time (`$turn.time`).
struct AgentClock {
  std::function<std::chrono::system_clock::time_point()> wall;
  std::function<zigzag::vortex::mesh::ActorTime()> steady;
  [[nodiscard]] static AgentClock system();
};

/// Names of the counters the adapter keeps in an agent's `$stats` record.
inline constexpr std::string_view kStatDropped        = "dropped";
inline constexpr std::string_view kStatFailedPrefix   = "failed.";
inline constexpr std::string_view kStatMalformed      = "malformed";
inline constexpr std::string_view kStatStale          = "stale";
inline constexpr std::string_view kStatUnsupported    = "unsupported";
inline constexpr std::string_view kStatSpawnFailed    = "spawn_failed";
inline constexpr std::string_view kStatSendFailed     = "send_failed";
inline constexpr std::string_view kStatUnroutable     = "unroutable";
inline constexpr std::string_view kStatDeliveryFailed = "delivery_failed";
inline constexpr std::string_view kStatChildDown      = "child_down";
/// A `tell` or `untell` for a predicate the receiver did not declare, or a
/// stray internal-event envelope, is counted here and dropped.
inline constexpr std::string_view kStatBadTell = "bad_tell";
/// A `stop` from anyone but the parent, refused.
inline constexpr std::string_view kStatStopRefused = "stop_refused";
/// An effect's result that no intention or flight was waiting for: it was
/// cancelled, timed out, or the intention ended (§6.6).
inline constexpr std::string_view kStatEffectStale = "effect_stale";
/// An intention's turn failed hard and the recovery turn could not record it.
inline constexpr std::string_view kStatIntentionFailed = "intention_failed";

/// Why a delivery was abandoned (the `cause` of `DeliveryFailed`).
inline constexpr std::string_view kCauseRetries  = "retries";
inline constexpr std::string_view kCauseDeadline = "deadline";

/// Why `AgentExecutor::checkpoint` declined (§4.2, "quiescent"). Each names
/// one condition, in the order the adapter tests them.
enum class CheckpointRefusal : std::uint8_t {
  UnknownAgent,        ///< no such pid, or it has not had its first turn
  Stopped,             ///< stopped, or on its way out
  HostDeliveryPending, ///< the adapter holds an event it has yet to run
  MailboxNotEmpty,
  SendsIncomplete, ///< a message this agent sent has no reply yet
  EffectInFlight,  ///< a native skill is still running an effect it started
  NotQuiescent,    ///< the runtime refused: a choice point, mark or run
  NoIdentity,      ///< `setCheckpointIdentity` was never called
  PublishFailed,
};

[[nodiscard]] std::string_view toString(CheckpointRefusal refusal) noexcept;

/// The scoped-evaluation grant one turn may use. It never exceeds the actor's
/// grant: work and instructions are the turn's fuel, bytes its output bytes,
/// value nodes its cells; nesting comes from the validated settings. The VM
/// additionally clamps every entry to what the turn has left. A grant with a
/// zero budget yields none, so ScopedEvaluate refuses rather than runs free.
[[nodiscard]] std::optional<zigzag::vortex::ScopedEvaluationLimits>
scopedEvaluationGrant(const zigzag::vortex::mesh::ActorGrant &grant,
                      const EpistemicLimits &limits) noexcept;

/// What signs and names a checkpoint. `origin.str()` must equal the node name
/// the executor was built with, because `resume` compares them.
struct CheckpointIdentity {
  xanadu::MutableKeys keys;
  zigzag::vortex::mesh::NodeId origin;
  /// Where sealed checkpoint payloads are written; the host owns the
  /// directory and keeps it for as long as a checkpoint may be resumed.
  std::filesystem::path seedRoot;
  /// A running agent holds no signing key (§4.1), so the host supplies the
  /// provenance a checkpoint carries, as it supplies the issuer.
  xanadu::SignedProvenance provenance;
};

/// A published checkpoint and what `resume` needs to name the agent again.
/// The sealed state is in `seed` and `source`; the rest is what the adapter
/// keeps outside the arena.
struct AgentCheckpoint {
  identity::ActorPid pid;
  std::string agentType;
  std::string name;
  std::string parentName;
  std::string programRef;
  bool adopted{false};
  /// The hash a fresh grant must name as its base (`ActorGrant::baseRef`).
  std::string contentHash;
  std::shared_ptr<xanadu::DirectoryContentSource> source;
  zigzag::vortex::mesh::ValueSeed seed;
};

class CompiledProgram;

class AgentExecutor {
public:
  AgentExecutor(zigzag::vortex::mesh::MeshActorRuntime &runtime,
                CapabilityIssuer &issuer, AgentProgram &program,
                identity::Fingerprint owner, std::string localNode,
                std::uint64_t epoch, AgentSettings settings = {},
                AgentClock clock = AgentClock::system());
  ~AgentExecutor();
  AgentExecutor(const AgentExecutor &)            = delete;
  AgentExecutor &operator=(const AgentExecutor &) = delete;

  /// `options` with `floorOf` set to this layer's id grammar (P3). The runtime
  /// reads options at construction, so call this before building it.
  [[nodiscard]] static zigzag::vortex::mesh::ActorOptions
  withFloors(zigzag::vortex::mesh::ActorOptions options = {});

  /// The runtime's executor. The adapter must outlive every turn.
  [[nodiscard]] zigzag::vortex::mesh::MeshActorRuntime::Executor executor();
  /// A `Sender` that delivers to this node by `receive` with the owner's
  /// identity, and everything else to `remote` (false when there is none).
  [[nodiscard]] zigzag::vortex::mesh::MeshActorRuntime::Sender
  loopbackSender(zigzag::vortex::mesh::MeshActorRuntime::Sender remote = {});
  /// Sets the executor and loopback sender and trusts the local node.
  AgentExecutor *
  install(zigzag::vortex::mesh::MeshActorRuntime::Sender remote = {});

  /// Spawn a top-level agent with a host-minted source pid and schedule its
  /// first turn.
  [[nodiscard]] std::expected<identity::ActorPid,
                              zigzag::vortex::mesh::ActorError>
  launch(std::string_view agentType, std::string_view name,
         WireRecord args = {}, std::string_view programRef = {});

  /// Retain an immutable compiled version for actors, children and resumes.
  /// A retained ref is never rebound to a different program object.
  [[nodiscard]] bool retainProgram(std::shared_ptr<AgentProgram> program);
  [[nodiscard]] std::shared_ptr<AgentProgram>
  resolveProgram(std::string_view ref) const;

  /// Upgrade one prepared live actor under a replacement grant. The host must
  /// retain and authorize the new program before calling this owner-thread API.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  upgrade(const identity::ActorPid &pid,
          std::shared_ptr<CompiledProgram> program,
          const zigzag::vortex::mesh::ActorGrant &grant);

  /// A trusted typed translator may extend the owned graph with future ABI
  /// records. It runs privately before hooks, shares the relocation map and
  /// remaining work budget, and must stage no external effects. Without one,
  /// populated extension dimensions refuse the upgrade.
  using OwnedStateTranslator =
      std::function<std::expected<void, zigzag::vortex::mesh::ActorError>(
          const CompiledProgram &, const CompiledProgram &,
          const zigzag::ArenaManifold &, zigzag::CellRef,
          zigzag::vortex::WorkerContext &, zigzag::CellRef,
          std::map<zigzag::CellRef, zigzag::CellRef> &, std::uint64_t &)>;
  AgentExecutor *setOwnedStateTranslator(OwnedStateTranslator translator) {
    ownedStateTranslator_ = std::move(translator);
    return this;
  }

  /// Where a launched agent's `emit ... in outer` goes: the launcher hosts
  /// that space, so there is no parent actor to relay it. Without a sink such
  /// an emission is counted unroutable.
  using HostSink = std::function<void(const Envelope &)>;
  AgentExecutor *setHostSink(HostSink sink) {
    hostSink_ = std::move(sink);
    return this;
  }
  /// Deliver an event to a launched agent as the host: an `ActorSend` from
  /// the host pid, received like any peer's. Run `pumpUntilIdle` to process it.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  deliver(const identity::ActorPid &target, Envelope envelope);

  /// Where native-skill effects go (§5.3). Without a registry every effect
  /// fails at once with cause "unbound". Not owned; it must outlive the
  /// executor.
  AgentExecutor *setBindings(HostBindings *bindings) {
    bindings_ = bindings;
    return this;
  }
  /// Post an event of type @p type to @p target as a percept (§7.5): a host
  /// delivery with no source, run in a wake turn of its own, in arrival order
  /// with the other host deliveries. It takes no mailbox slot. Callable from
  /// any thread; it is queued and the agent hears it at the next
  /// `pumpUntilIdle` or `tick`, and always before a checkpoint is accepted.
  /// An event type the program does not declare is counted unhandled by the
  /// dispatcher, as any event is.
  /// Queue one checked observation for @p target's arbitrated predicate
  /// (P19 I3-1b). Callable from any thread, like `percept`; the observation
  /// runs in its own ingestion turn at the next pump. The receipt means
  /// queued, never committed.
  [[nodiscard]] std::expected<TransportReceipt, EpistemicRefusal>
  observe(const identity::ActorPid &target,
          const CheckedObservation &observation);

  void percept(const identity::ActorPid &target, std::string type,
               WireRecord fields = {});

  /// Called at the start of every wake turn that continues an intention,
  /// after `$turn` (with `intention`) is written, so a host or test can watch
  /// which intention the adapter chose.
  using IntentionObserver = std::function<void(
      const identity::ActorPid &, const zigzag::vortex::WorkerContext &,
      std::int64_t intention)>;
  AgentExecutor *setIntentionObserver(IntentionObserver observer) {
    intentionObserver_ = std::move(observer);
    return this;
  }
  /// The ids of the agent's ready intentions in the order the adapter will
  /// run them (§5.6): derived from cells now, not remembered.
  [[nodiscard]] std::vector<std::int64_t>
  readyIntentions(const identity::ActorPid &pid) const;
  /// The earliest absolute time, in milliseconds, at which a waiting
  /// intention's deadline or a timer falls due; nullopt when none is pending.
  [[nodiscard]] std::optional<std::int64_t>
  nextDeadline(const identity::ActorPid &pid) const;
  /// Effects the agent started that the host has not yet reported.
  [[nodiscard]] std::size_t
  effectsInFlight(const identity::ActorPid &pid) const;

  /// Delivery upkeep (§7.3): redeliver what is overdue, abandon what has
  /// exhausted `sendRetries`, forget what completed.
  void tick();
  void tick(std::chrono::system_clock::time_point now);

  /// Abandon the delivery `requestId` at `deadline` even if its retries are not
  /// spent (§7.3); false if it is not outstanding.
  bool setDeliveryDeadline(std::string_view requestId,
                           std::chrono::system_clock::time_point deadline);

  /// Runtime options with `floorOf` set and the checkpoint bound taken from
  /// @p settings (`vagent.value_operations`, P5).
  [[nodiscard]] static zigzag::vortex::mesh::ActorOptions
  runtimeOptions(const AgentSettings &settings);

  /// Required before `checkpoint`.
  AgentExecutor *setCheckpointIdentity(CheckpointIdentity identity);

  /// Publish the agent's state at a message boundary (§4.2, §4.4). Refuses
  /// unless the agent is quiescent: not stopped, nothing in the mailbox, no
  /// message it sent still unanswered, no event or result the adapter has yet
  /// to run, no effect still running, and nothing the runtime objects to.
  /// Timers, beliefs, ready intentions and pending deadlines travel in the
  /// state and do not prevent it.
  [[nodiscard]] std::expected<AgentCheckpoint, CheckpointRefusal>
  checkpoint(const identity::ActorPid &pid);

  /// Re-admit a checkpoint under a new pid on this executor's runtime
  /// (§4.4, §4.5). The program is instantiated afresh in the rebuilt arena and
  /// the saved state is copied onto it, so `Initialize` does not run again;
  /// the agent tells its peers where it went (`Moved`) on its first turn. The
  /// old actor is not stopped: `retire` it once the peers have been told.
  [[nodiscard]] std::expected<identity::ActorPid,
                              zigzag::vortex::mesh::ActorError>
  resume(const AgentCheckpoint &checkpoint);

  /// Stop an actor that has been resumed elsewhere, without the orderly
  /// shutdown (`Destroy`, `ChildStopping`). False if it is not a live actor
  /// of this executor.
  bool retire(const identity::ActorPid &pid);

  /// Called at the start of every turn that dispatches an event, with the
  /// event cell already minted, so a host can watch what agents are told
  /// (the cell is undone with the turn if the turn fails).
  using TurnObserver = std::function<void(const identity::ActorPid &,
                                          const zigzag::vortex::WorkerContext &,
                                          zigzag::CellRef event)>;
  AgentExecutor *setTurnObserver(TurnObserver observer) {
    observer_ = std::move(observer);
    return this;
  }

  /// Wake agents that have not had their first turn, then pump messages until
  /// nothing is left to do or `maxTurns` is reached; returns the turns run.
  std::size_t pumpUntilIdle(std::size_t maxTurns = 100000);

  /// The agent's arena bundle, once it has had a turn and while it lives.
  [[nodiscard]] zigzag::vortex::WorkerContext *
  context(const identity::ActorPid &pid) const;
  /// The agent cell of a prepared agent.
  [[nodiscard]] std::optional<zigzag::CellRef>
  agentCell(const identity::ActorPid &pid) const;
  /// One `$stats` counter, including those queued for the next turn.
  [[nodiscard]] std::uint64_t stat(const identity::ActorPid &pid,
                                   std::string_view name) const;
  /// Ids sent and not yet completed or abandoned.
  [[nodiscard]] std::size_t outstanding() const noexcept {
    return outstanding_.size();
  }

private:
  struct State;
  struct Outstanding {
    std::string ownerKey;
    std::string node;
    std::chrono::system_clock::time_point lastSent;
    std::size_t redeliveries{0};
    std::string kind;
    identity::ActorPid to;
    std::optional<std::chrono::system_clock::time_point> deadline;
  };
  /// What a spawn request's capability carries (§4.1).
  struct SpawnInfo {
    std::string agentType;
    std::string name;
    std::string parentName;
    std::optional<identity::ActorPid> parent;
    WireRecord args;
  };
  struct TurnSpec;
  struct TurnOutcome;
  struct OutboxItem;
  struct HostEvent;
  struct PostBox;
  struct Flight;
  struct PendingRequest;

  [[nodiscard]] std::optional<SpawnInfo>
  readSpawn(const zigzag::vortex::mesh::ActorMessage &request) const;
  [[nodiscard]] static std::string pidKey(const identity::ActorPid &pid);

  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  execute(const zigzag::vortex::mesh::ActorExecution &execution,
          zigzag::vortex::WorkerContext &context);
  State &stateFor(const identity::ActorPid &pid);
  [[nodiscard]] bool prepare(State &state,
                             zigzag::vortex::WorkerContext &context,
                             const zigzag::vortex::mesh::ActorMessage &spawn,
                             std::optional<zigzag::CellRef> savedAgent);
  /// Copy the saved agent cell's state onto the freshly instantiated one:
  /// fields, beliefs, hosted spaces and children. Compiled routines address
  /// their own cells by ref, so the restored cells cannot be used in place.
  void restoreState(State &state, zigzag::CellRef saved);
  void announceMoved(State &state);
  void applyMoved(State &state,
                  const zigzag::vortex::mesh::ActorMessage &message,
                  const Envelope &envelope);
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  handleMessage(State &state,
                const zigzag::vortex::mesh::ActorExecution &execution,
                zigzag::vortex::WorkerContext &context);
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  handleSend(State &state,
             const zigzag::vortex::mesh::ActorExecution &execution,
             zigzag::vortex::WorkerContext &context);
  /// One turn, and for a turn that continued an intention and failed hard,
  /// the `fail_step` recovery turn after it (§4.2).
  TurnOutcome runTurn(State &state, zigzag::CellRef cursor,
                      const zigzag::vortex::mesh::ActorGrant &grant,
                      zigzag::vortex::WorkerContext &context,
                      const TurnSpec &spec);
  TurnOutcome runTurnOnce(State &state, zigzag::CellRef cursor,
                          const zigzag::vortex::mesh::ActorGrant &grant,
                          zigzag::vortex::WorkerContext &context,
                          const TurnSpec &spec);
  /// The wake turn that continues intention @p id (§6.5).
  void continueIntention(State &state, std::int64_t id,
                         const zigzag::vortex::mesh::ActorGrant &grant,
                         zigzag::vortex::WorkerContext &context);
  /// Runs the ready intentions of @p state that the interleaving allows,
  /// `ready_per_message` at most while its mailbox has messages waiting.
  bool wakeReady(State &state, std::size_t &turns, std::size_t maxTurns);
  /// Rebuild the ready list and the deadline heap from cells (§5.6).
  void deriveSchedule(State &state) const;
  /// Move percepts and effect results the host posted onto their agents.
  void drainPosted();
  void startEffect(State &state, const OutboxItem &item);
  void cancelEffect(State &state, std::int64_t id);
  void cancelFlights(State &state);
  /// An effect ended (or was found past its deadline): wake its intention, or
  /// raise `EffectFailed` for a handler's call.
  void finishEffect(State &state, std::int64_t id, EffectResult result,
                    bool timedOut);
  /// Mark intentions whose `waiting.deadline` and effects whose deadline have
  /// elapsed. True if anything changed.
  bool expireDeadlines(State &state, std::int64_t nowMillis);
  /// §4.5: an intention waiting on an effect cannot follow the device.
  void failMigrated(State &state);
  /// A host delivery: an event with no source, dispatched in a turn of its
  /// own (§5.5).
  TurnOutcome deliverHost(State &state, zigzag::CellRef cursor,
                          const zigzag::vortex::mesh::ActorGrant &grant,
                          zigzag::vortex::WorkerContext &context,
                          const HostEvent &event);
  void raise(State &state, std::string_view type, WireRecord fields);
  /// Runs the host events raised so far, then an orderly stop if user code set
  /// `$terminating`.
  void settle(State &state, zigzag::CellRef cursor,
              const zigzag::vortex::mesh::ActorGrant &grant,
              zigzag::vortex::WorkerContext &context);
  /// `on Destroy`, `ChildStopping{cause}` to the parent, then the runtime's
  /// stop (§4.5). The state's context is gone afterwards.
  void orderlyStop(State &state, zigzag::CellRef cursor,
                   const zigzag::vortex::mesh::ActorGrant &grant,
                   zigzag::vortex::WorkerContext &context,
                   std::string_view cause, bool keepChildren = false,
                   bool announce = true);
  /// After `on Destroy`: stage `stop` to every child on `d.holon` (reap), or
  /// `Orphaned{former_parent}` (keep). Shares `position` with the
  /// `ChildStopping` that follows so request ids stay distinct.
  void releaseChildren(State &state, std::uint64_t &position,
                       bool keepChildren);
  /// A child told it was orphaned becomes the launcher's: no parent actor, the
  /// launcher's space as `$outer` (§4.5).
  void adoptByLauncher(State &state);
  void routeTell(State &state, std::uint64_t turn, std::uint64_t &position,
                 const OutboxItem &item);
  /// `ask`, `achieve`, `cancel`, `outcome` and `refuse` (design §7.1): see the
  /// questions block of `runtime_abi.hpp`.
  void sendRequest(State &state, std::uint64_t turn, std::uint64_t &position,
                   const OutboxItem &item);
  void sendResponse(State &state, std::uint64_t turn, std::uint64_t &position,
                    const OutboxItem &item);
  /// The receiving sides. Each returns the turn result.
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  answerAsk(State &state, const zigzag::vortex::mesh::ActorExecution &execution,
            zigzag::vortex::WorkerContext &context, const Envelope &envelope);
  std::expected<std::string, zigzag::vortex::mesh::ActorError> receiveAchieve(
      State &state, const zigzag::vortex::mesh::ActorExecution &execution,
      zigzag::vortex::WorkerContext &context, const Envelope &envelope);
  void receiveCancel(State &state, const Envelope &envelope);
  void receiveResponse(State &state, const Envelope &envelope);
  /// A reply to an `ask` arrived as a turn result.
  void receiveReply(State &state,
                    const zigzag::vortex::mesh::ActorMessage &message);
  /// End the `ask` or delegate wait of intention @p intention that is waiting
  /// on @p key with a failed match; false if it is no longer waiting there.
  bool failWait(State &state, std::int64_t intention, std::int64_t key,
                std::string_view cause, bool timedOut = false);
  /// Settle a pending request with @p outcome; false (and a stale count) when
  /// nothing waits on it.
  bool settleRequest(State &state, const std::string &requestId,
                     const EffectResult &outcome);
  /// Forget the requests of one wait (it ended some other way).
  void dropRequests(State &state, std::int64_t intention, std::int64_t key);
  /// Stage an outbox entry the adapter itself needs sent and drain it in a
  /// turn that runs no routine.
  void stageResponse(State &state,
                     const zigzag::vortex::mesh::ActorGrant &grant,
                     zigzag::vortex::WorkerContext &context,
                     std::string_view kind, std::string_view to,
                     std::string_view correlation, std::string_view status,
                     std::string_view cause);
  [[nodiscard]] WireList readCancelled(const State &state) const;
  void writeCancelled(State &state, WireList list);
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  handleStop(State &state,
             const zigzag::vortex::mesh::ActorExecution &execution,
             zigzag::vortex::WorkerContext &context);
  void sendStop(State &state, std::uint64_t turn, std::uint64_t &position,
                const OutboxItem &item);
  /// Drops a child from `d.holon`, the inner space and `$peers`; false if it
  /// was not there.
  bool removeChild(State &state, std::string_view name);
  void acknowledgeChildStop(State &state,
                            const zigzag::vortex::mesh::ActorMessage &message,
                            const Envelope &envelope);
  /// `$timers` as a list of `Timer` records (`kTimerRecord`).
  [[nodiscard]] WireList readTimers(const State &state) const;
  void writeTimers(State &state, WireList timers);
  void applyTimerChange(State &state, const OutboxItem &item);
  /// Raises the copied event of every timer due at `nowMillis` as a host
  /// event, then removes a one-shot or advances a periodic one. True if any
  /// event was raised.
  bool fireDueTimers(State &state, std::int64_t nowMillis);
  void upkeepDeliveries(std::chrono::system_clock::time_point now);
  void upkeepLeases(std::chrono::system_clock::time_point now);
  [[nodiscard]] bool renewLease(State &state,
                                std::chrono::system_clock::time_point now);
  [[nodiscard]] bool live(const State &state) const;
  void drain(State &state, zigzag::vortex::WorkerContext &context,
             std::uint64_t turn, const TurnSpec &spec);
  [[nodiscard]] std::vector<OutboxItem>
  collectOutbox(State &state, zigzag::vortex::WorkerContext &context);
  void spawnChild(State &state, zigzag::vortex::WorkerContext &context,
                  std::uint64_t turn, std::uint64_t &position,
                  const OutboxItem &item);
  void routeEvent(State &state, zigzag::vortex::WorkerContext &context,
                  std::uint64_t turn, std::uint64_t &position,
                  const OutboxItem &item);
  void relay(State &state, zigzag::vortex::WorkerContext &context,
             std::uint64_t turn, std::uint64_t &position,
             const Envelope &received);
  /// Stage one `ActorSend` to a peer; false when it was refused for good.
  bool sendEnvelope(State &state, std::uint64_t turn, std::uint64_t &position,
                    const identity::ActorPid &to, Envelope envelope);
  void onDown(State &state, zigzag::vortex::WorkerContext &context,
              const zigzag::vortex::mesh::ActorMessage &message);
  void stageChildStopping(State &state, std::string_view cause,
                          std::uint64_t position = 0);
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  fatal(State &state, std::string_view cause);

  [[nodiscard]] std::string floorFor(const State &state,
                                     std::uint64_t turn) const;
  [[nodiscard]] std::optional<zigzag::CellRef>
  innerSpace(const State &state) const;
  [[nodiscard]] bool hasInnerSpace(const State &state) const;
  zigzag::CellRef mintEvent(State &state, const Envelope &envelope,
                            std::string_view space, std::int64_t millis);
  [[nodiscard]] std::optional<identity::ActorPid>
  peerPid(const State &state, std::string_view name) const;
  [[nodiscard]] std::vector<std::string> innerMembers(const State &state) const;
  [[nodiscard]] std::uint64_t counter(const State &state,
                                      std::string_view field) const;
  void setCounter(State &state, std::string_view field, std::uint64_t value);
  void bumpStat(State &state, std::string_view name, std::uint64_t by = 1);
  void flushStats(State &state);
  [[nodiscard]] bool initialized(const State &state) const;
  void liftLimits(zigzag::vortex::WorkerContext &context) const;
  [[nodiscard]] std::chrono::system_clock::time_point wallNow() const {
    return clock_.wall();
  }

  zigzag::vortex::mesh::MeshActorRuntime &runtime_;
  CapabilityIssuer &issuer_;
  AgentProgram &program_;
  std::map<std::string, std::shared_ptr<AgentProgram>, std::less<>> programs_;
  identity::Fingerprint owner_;
  std::string localNode_;
  std::uint64_t epoch_;
  AgentSettings settings_;
  AgentClock clock_;
  TurnObserver observer_;
  IntentionObserver intentionObserver_;
  HostBindings *bindings_{};
  OwnedStateTranslator ownedStateTranslator_;
  std::shared_ptr<PostBox> posts_;
  identity::ActorPid hostPid_;
  std::uint64_t launches_{0};
  std::uint64_t hostSends_{0};
  HostSink hostSink_;
  std::map<std::string, std::unique_ptr<State>> states_;
  std::map<std::string, Outstanding> outstanding_;
  std::vector<identity::ActorPid> needsFirstTurn_;
  std::optional<CheckpointIdentity> checkpointIdentity_;
  std::uint64_t resumes_{0};
  /// Per resumed pid, what the arena does not hold: whether the launcher had
  /// adopted it. Consumed by `prepare`.
  std::map<std::string, bool> resumeAdopted_;
};

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_HPP
