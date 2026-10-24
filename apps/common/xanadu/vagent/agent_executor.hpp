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
#ifndef COMMON_XANADU_VAGENT_AGENT_EXECUTOR_HPP
#define COMMON_XANADU_VAGENT_AGENT_EXECUTOR_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime_abi.hpp"
#include "common/xanadu/vagent/wire.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vortex/worker_context.hpp"

namespace xanadu::vagent {

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
  /// The validator wants a hashcash stamp on a spawn and the runtime does not
  /// check one, so a spawn the host itself issues carries this fixed text.
  std::string spawnStamp{"vagent-local-spawn"};
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
/// A `stop` from anyone but the parent, refused.
inline constexpr std::string_view kStatStopRefused = "stop_refused";

/// Why a delivery was abandoned (the `cause` of `DeliveryFailed`).
inline constexpr std::string_view kCauseRetries  = "retries";
inline constexpr std::string_view kCauseDeadline = "deadline";

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
         WireRecord args = {});

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

  /// Delivery upkeep (§7.3): redeliver what is overdue, abandon what has
  /// exhausted `sendRetries`, forget what completed.
  void tick();
  void tick(std::chrono::system_clock::time_point now);

  /// Abandon the delivery `requestId` at `deadline` even if its retries are not
  /// spent (§7.3); false if it is not outstanding.
  bool setDeliveryDeadline(std::string_view requestId,
                           std::chrono::system_clock::time_point deadline);

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

  [[nodiscard]] std::optional<SpawnInfo>
  readSpawn(const zigzag::vortex::mesh::ActorMessage &request) const;
  [[nodiscard]] static std::string pidKey(const identity::ActorPid &pid);

  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  execute(const zigzag::vortex::mesh::ActorExecution &execution,
          zigzag::vortex::WorkerContext &context);
  State &stateFor(const identity::ActorPid &pid);
  [[nodiscard]] bool prepare(State &state,
                             zigzag::vortex::WorkerContext &context,
                             const zigzag::vortex::mesh::ActorMessage &spawn);
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  handleMessage(State &state,
                const zigzag::vortex::mesh::ActorExecution &execution,
                zigzag::vortex::WorkerContext &context);
  std::expected<std::string, zigzag::vortex::mesh::ActorError>
  handleSend(State &state,
             const zigzag::vortex::mesh::ActorExecution &execution,
             zigzag::vortex::WorkerContext &context);
  TurnOutcome runTurn(State &state, zigzag::CellRef cursor,
                      const zigzag::vortex::mesh::ActorGrant &grant,
                      zigzag::vortex::WorkerContext &context,
                      const TurnSpec &spec);
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
                   std::string_view cause);
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
  void stageChildStopping(State &state, std::string_view cause);
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
  identity::Fingerprint owner_;
  std::string localNode_;
  std::uint64_t epoch_;
  AgentSettings settings_;
  AgentClock clock_;
  TurnObserver observer_;
  identity::ActorPid hostPid_;
  std::uint64_t launches_{0};
  std::uint64_t hostSends_{0};
  HostSink hostSink_;
  std::map<std::string, std::unique_ptr<State>> states_;
  std::map<std::string, Outstanding> outstanding_;
  std::vector<identity::ActorPid> needsFirstTurn_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_AGENT_EXECUTOR_HPP
