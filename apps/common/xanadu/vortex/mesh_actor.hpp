#pragma once

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"
#include "common/xanadu/vortex/worker_context.hpp"

#include <chrono>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xanadu {
class ContentSource;
}

namespace zigzag::vortex::mesh {

struct ReceivedValue;

using ActorPid     = xanadu::identity::ActorPid;
using ActorMessage = xanadu::identity::ActorMessage;
using ActorTime    = std::chrono::steady_clock::time_point;

enum class ActorError {
  NotAuthorized,
  InvalidMessage,
  UnknownProcess,
  WrongEpoch,
  MailboxFull,
  ResourceLimit,
  DuplicateMismatch,
  NotImmutable,
  ExecutionFailed,
  TransportUnavailable,
  NotQuiescent,
  /// The id lies below a floor the sender has already advanced past.
  StaleRequest,
};

/// The verifier checks these against the signed capability, authenticated
/// identity and immutable program/base references before returning the grant.
struct ActorGrant {
  std::string programRef;
  std::string baseRef;
  std::uint64_t maxInstructions{};
  std::uint32_t maxCells{};
  std::size_t maxOutputBytes{};
  std::size_t maxSolutions{};
  std::size_t maxMailboxBytes{};
  std::chrono::system_clock::time_point expiresAt;
  bool immutable{false};
};

/// A sender's promise that it will never again use an id in
/// [prefix, floorId); the receiver may forget those ids and must refuse them.
struct FloorMark {
  std::string prefix;
  std::string floorId;
};

struct ActorOptions {
  /// Interprets a message that carries a floor; the runtime knows no wire
  /// grammar for it, so the layer that defines request ids supplies this.
  std::function<std::optional<FloorMark>(const ActorMessage &)> floorOf;
  /// Bounds on rehydrating a checkpoint in resume(); clamped to the grant.
  ValueLimits valueLimits{};
  std::size_t maxActors{128};
  std::size_t maxMailboxMessages{128};
  std::size_t maxRememberedMessages{4096};
  std::size_t maxOutstandingDeliveries{4096};
  std::size_t maxMonitors{4096};
  std::chrono::milliseconds initialHeartbeatInterval{1000};
  std::chrono::milliseconds leaseDuration{60000};
  double suspectPhi{8.0};
};

enum class ActorLiveness { Alive, Suspect, Gone };

/// Evidence must come from a fresh topic re-query AND an authenticated ledger
/// lookup. Missing evidence is never interpreted as absence.
struct ActorDiscoveryEvidence {
  ActorTime observedAt;
  bool noSeeders{false};
  bool ledgerChecked{false};
  bool noNewerEpoch{false};
};

struct ActorExecution {
  ActorPid pid;
  ActorGrant grant;
  /// The `ActorSpawn` this actor was created or resumed from, so an executor
  /// recovers spawn parameters without a side table. A reference into the
  /// actor record, which the runtime keeps alive until the executor returns.
  const ActorMessage &spawnRequest;
  /// Empty when the executor is woken by wake() rather than by a message.
  std::optional<ActorMessage> message;
  CellRef cursor;
  std::optional<CellRef> stateRoot;
};

struct ActorCheckpointRecord {
  std::string requestId;
  std::string contentHash;
  std::optional<ActorMessage> response;
};

/// Only a message-boundary continuation is portable. Application state lives
/// in the signed PublishedValue root; VM PCs and native callbacks stay local.
struct ActorContinuation {
  ActorPid pid;
  std::string programRef;
  std::string baseRef;
  ActorMessage spawnRequest;
  std::vector<ActorCheckpointRecord> completed;
};

struct ActorCheckpointView {
  ActorContinuation continuation;
  const ArenaManifold &arena;
  CellRef root;
};

/// An owner-thread runtime. Actors share no mutable contexts; the executor
/// loads verified immutable programs and runs the VM with grant limits. No
/// native callback is received from the wire.
class MeshActorRuntime {
public:
  using Authorizer = std::function<std::expected<ActorGrant, ActorError>(
      const xanadu::identity::Fingerprint &, const ActorMessage &)>;
  using Executor   = std::function<std::expected<std::string, ActorError>(
      const ActorExecution &, WorkerContext &)>;
  using Sender = std::function<bool(std::string_view, const ActorMessage &)>;

  MeshActorRuntime(std::string node, std::uint64_t epoch,
                   ActorOptions options = {});
  ~MeshActorRuntime();
  MeshActorRuntime(const MeshActorRuntime &)            = delete;
  MeshActorRuntime &operator=(const MeshActorRuntime &) = delete;

  MeshActorRuntime *setAuthorizer(Authorizer authorizer);
  MeshActorRuntime *setExecutor(Executor executor);
  MeshActorRuntime *setSender(Sender sender);
  /// Install only after verifying the node's device delegation to this master.
  MeshActorRuntime *trustNode(std::string node,
                              xanadu::identity::Fingerprint identity);

  [[nodiscard]] std::expected<ActorPid, ActorError>
  spawn(const xanadu::identity::Fingerprint &identity,
        const ActorMessage &request,
        std::chrono::system_clock::time_point wallNow);

  /// Re-admit a signed message-boundary checkpoint under a fresh capability.
  /// The new grant must name the exact checkpoint content hash as its base.
  /// The owned source lease pins lazy verified span reads for the actor's life.
  [[nodiscard]] std::expected<ActorPid, ActorError>
  resume(const xanadu::identity::Fingerprint &identity,
         const ActorMessage &request, ReceivedValue checkpoint,
         std::shared_ptr<const xanadu::ContentSource> sourceLease,
         std::chrono::system_clock::time_point wallNow);

  [[nodiscard]] std::expected<MeshActorRuntime *, ActorError>
  setStateRoot(const ActorPid &pid, CellRef root);
  [[nodiscard]] std::optional<CellRef> stateRoot(const ActorPid &pid) const;

  /// Identity is supplied only by an authenticated transport. This boundary
  /// rechecks the peer owning each PID, independent of wire shape validation.
  [[nodiscard]] std::expected<void, ActorError>
  receive(const xanadu::identity::Fingerprint &identity,
          const ActorMessage &message, ActorTime now,
          std::chrono::system_clock::time_point wallNow);

  /// Executes at most one message and emits a value or DOWN. Limits on VM
  /// instructions/solutions must be applied by the configured program adapter;
  /// allocation, mailbox, output and retention limits are applied here.
  [[nodiscard]] std::expected<bool, ActorError>
  pumpOne(std::chrono::system_clock::time_point wallNow);

  /// Runs the executor for one actor with no message: no dedup entry, no
  /// reply. Same lease check as pumpOne; an executor error stops the actor.
  [[nodiscard]] std::expected<void, ActorError>
  wake(const ActorPid &pid, std::chrono::system_clock::time_point wallNow);

  /// Replaces the grant of a live (or lease-expired) actor under a fresh
  /// capability for the same program; pid, arena and mailbox are kept.
  [[nodiscard]] std::expected<void, ActorError>
  renew(const ActorPid &pid, const xanadu::identity::Fingerprint &identity,
        const ActorMessage &request,
        std::chrono::system_clock::time_point wallNow);

  /// Marks the actor stopped, tells its monitors, and erases it once its
  /// outstanding deliveries have drained. Safe from inside its own executor.
  [[nodiscard]] std::expected<void, ActorError> stop(const ActorPid &pid);

  /// Erases the actor's remembered ids that start with prefix; returns how
  /// many went immediately (the id being executed is erased afterwards).
  [[nodiscard]] std::expected<std::size_t, ActorError>
  forgetSender(const ActorPid &pid, std::string_view prefix);

  [[nodiscard]] std::expected<void, ActorError>
  send(std::string destinationNode, ActorMessage message);
  [[nodiscard]] std::expected<void, ActorError>
  retry(std::string_view requestId);
  /// Emits the stored message again even if it was acknowledged.
  [[nodiscard]] std::expected<void, ActorError>
  redeliver(std::string_view requestId);
  /// Forgets an incomplete delivery; a late reply to it is then ignored.
  [[nodiscard]] std::expected<void, ActorError>
  abandon(std::string_view requestId);
  [[nodiscard]] bool deliveryAcknowledged(std::string_view requestId) const;
  [[nodiscard]] std::size_t deliveryCount() const noexcept;

  [[nodiscard]] std::expected<void, ActorError>
  monitor(const ActorPid &watcher, const ActorPid &target, ActorTime now);
  /// Link semantics deliberately reduce to a monitor; no kill propagation.
  [[nodiscard]] std::expected<void, ActorError>
  link(const ActorPid &watcher, const ActorPid &target, ActorTime now);
  void heartbeat(const ActorPid &target, ActorTime now);
  void observe(const ActorPid &target, ActorDiscoveryEvidence evidence);
  void tick(ActorTime now);
  [[nodiscard]] std::optional<ActorLiveness>
  liveness(const ActorPid &target) const;

  /// Redispatches the exact immutable spawn input. Stateful restarts and
  /// one-for-all supervision are not supported by this mesh.
  [[nodiscard]] std::expected<void, ActorError>
  redispatch(const ActorPid &pid, std::string destinationNode);

  [[nodiscard]] std::size_t actorCount() const noexcept;
  [[nodiscard]] std::size_t pending(const ActorPid &pid) const;
  [[nodiscard]] std::optional<CellRef> localCursor(const ActorPid &pid) const;
  /// Owner-thread borrow: publish before resuming execution. Refuses pending
  /// mailboxes, outstanding replies and choice stacks.
  [[nodiscard]] std::expected<ActorCheckpointView, ActorError>
  checkpointView(const ActorPid &pid, CellRef stateRoot) const;

private:
  struct Actor;
  struct Monitor;
  struct Delivery;
  static std::string pidKey(const ActorPid &pid);
  Actor *find(const ActorPid &pid);
  const Actor *find(const ActorPid &pid) const;
  std::expected<void, ActorError> enqueue(Actor &actor,
                                          const ActorMessage &message);
  bool emit(std::string_view node, const ActorMessage &message);
  std::expected<std::string, ActorError>
  execute(Actor &actor, const std::optional<ActorMessage> &message);
  bool drained(const ActorPid &pid) const;
  void reapStopped();

  std::string node_;
  std::uint64_t epoch_;
  std::uint64_t nextSpawn_{1};
  ActorOptions options_;
  Authorizer authorizer_;
  Executor executor_;
  Sender sender_;
  std::map<std::string, std::unique_ptr<Actor>> actors_;
  std::map<std::string, std::pair<std::string, ActorPid>> spawns_;
  std::map<std::string, Monitor> monitors_;
  std::map<std::string, Delivery> deliveries_;
  std::map<std::string, xanadu::identity::Fingerprint> trustedNodes_;
  std::string lastServed_;
};
} // namespace zigzag::vortex::mesh
