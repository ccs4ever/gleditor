#pragma once

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vortex/worker_context.hpp"

#include <chrono>
#include <cstdint>
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
class SpanReader;
class Store;
} // namespace xanadu

namespace zigzag::vortex {
class P16Ceremony;
// Opaque-enum declaration: the fixed underlying type makes P16Flag a
// complete type for members and by-value signatures; the enumerators stay
// in vortex/p16_ceremony.hpp, which the adapter includes.
enum class P16Flag : std::uint8_t;
} // namespace zigzag::vortex

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
  /// A remote verdict the wire carries as free text — the spawn
  /// ack's `reason` — surfaced through SpawnResultHook. The
  /// requester learns only that the host declined, not which of
  /// its own refuse paths did.
  Refused,
};

/// The verifier checks these against the signed capability, authenticated
/// identity and immutable program/base references before returning the grant.
struct ActorGrant {
  std::string programRef;
  std::string baseRef;
  std::uint64_t maxInstructions{};
  std::uint32_t maxCells{};
  /// Live-cell ceiling measured at a turn boundary — the
  /// `vagent.max_live_cells` bound of actor-executor §4.6. Zero means
  /// unbounded (pre-M0.5 grants carried no field).
  std::uint32_t maxLiveCells{};
  std::size_t maxOutputBytes{};
  std::size_t maxSolutions{};
  std::size_t maxMailboxBytes{};
  std::chrono::system_clock::time_point expiresAt;
  bool immutable{false};
};

struct ActorOptions {
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
  /// Absent on a P4 local wake turn — the executor runs with no mailbox
  /// message and sends no reply (vagent-agent-language.md:1670).
  std::optional<ActorMessage> message;
  CellRef cursor;
  std::optional<CellRef> stateRoot;
  /// The §4.1 arbitration counters as this turn was picked — runtime
  /// state, not config: consecutive same-source services before this
  /// one. The replay record (§4.7) reads them verbatim.
  std::size_t wakeStreak{0};
  std::size_t msgStreak{0};
};

/// Phase-6 (i): the spawn-lineage pin. `base` is the Manifold folded over
/// the seeded AND filed lineage Store — `rebuildManifold`'s product, whose
/// `store()` attachment is what lets the fresh context's first-resolution
/// resolve filed dimension names instead of minting orphan twins. `reader`
/// is the lineage Store itself (`Store` is a `SpanReader`), bound as the
/// arena's default span reader so artifact-local span names resolve.
struct ActorLineage {
  const Manifold *base{nullptr};
  const xanadu::SpanReader *reader{nullptr};
};

/// What the genesis hook sees on each lineage: the actor's fresh pid and,
/// on resume only, the caller-transported Store the checkpoint arrived
/// with — the take-or-verify comparand the (a)-arm verifies against the
/// rebuilt fold's `arena.base()->store()` (phase-6 (ii)). Null on spawn.
struct ActorGenesis {
  ActorPid pid;
  const xanadu::Store *transported{nullptr};
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
  /// The adapter-installed (a)-(e) driver — invoked once per actor on the
  /// spawn and resume paths, after the fresh context is minted and before
  /// any turn runs. `context` is that actor's own WorkerContext: the hook
  /// binds dimensions, registers the program image on the fresh VM, and
  /// verifies (or adopts) `genesis.transported`. A refusal aborts the
  /// spawn/resume with the hook's error.
  using GenesisHook = std::function<std::expected<void, ActorError>(
      const ActorGenesis &, WorkerContext &)>;

  /// The spawn-request outcome a requesting adapter cannot see —
  /// ActorSpawnAck is acknowledged at the runtime layer and never
  /// enqueues to the requester's mailbox, so a refusal would leave
  /// its staged deliveries waiting forever. Fired from receive()'s
  /// ActorSpawnAck arm — the requesting node's side — with the
  /// original request and the ack's verdict: the granted pid the
  /// member's `pid` record notes, or Refused when the ack names
  /// none (the wire's free-text reason is not a typed error).
  using SpawnResultHook = std::function<void(
      const ActorMessage &request,
      const std::expected<ActorPid, ActorError> &result)>;

  MeshActorRuntime(std::string node, std::uint64_t epoch,
                   ActorOptions options = {});
  ~MeshActorRuntime();
  MeshActorRuntime(const MeshActorRuntime &)            = delete;
  MeshActorRuntime &operator=(const MeshActorRuntime &) = delete;

  MeshActorRuntime *setAuthorizer(Authorizer authorizer);
  MeshActorRuntime *setExecutor(Executor executor);
  MeshActorRuntime *setSender(Sender sender);
  MeshActorRuntime *setGenesisHook(GenesisHook hook);
  MeshActorRuntime *setSpawnResultHook(SpawnResultHook hook);
  /// The settings-layer read channel for `vagent.*` tunables
  /// (actor-executor §4.1); the adapter binds `system://settings` reads.
  /// Unset readers fall back to the compiled-in defaults.
  MeshActorRuntime *setSettingReader(
      std::function<std::optional<std::int64_t>(std::string_view)> reader);
  /// Install only after verifying the node's device delegation to this master.
  MeshActorRuntime *trustNode(std::string node,
                              xanadu::identity::Fingerprint identity);

  /// @param lineage the spawn-lineage pin (phase-6 (i)): when present, the
  ///        actor's WorkerContext is constructed over `lineage->base` with
  ///        `lineage->reader` as its default span reader — the ordering is
  ///        mint/file → fold → pin/inject → first-resolution, so the fold
  ///        must already carry the filed vocabulary. A `lineage->base`
  ///        with no `store()` attached is refused: first-resolution would
  ///        mint dimension twins the next filing can never file.
  [[nodiscard]] std::expected<ActorPid, ActorError>
  spawn(const xanadu::identity::Fingerprint &identity,
        const ActorMessage &request,
        std::chrono::system_clock::time_point wallNow,
        std::optional<ActorLineage> lineage = std::nullopt);

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

  /// Executes at most one turn — a mailbox message or a queued P4 wake —
  /// and emits a value or DOWN (message turns only). Limits on VM
  /// instructions/solutions must be applied by the configured program
  /// adapter; allocation, mailbox, output and retention limits are applied
  /// here.
  [[nodiscard]] std::expected<bool, ActorError>
  pumpOne(std::chrono::system_clock::time_point wallNow);

  /// P4's local wake (vagent-agent-language.md:1670): queues the executor
  /// to run once with `ActorExecution::message` absent. No mailbox slot,
  /// no remembered entry, no reply. The lease check in pumpOne still
  /// applies to the wake turn. Local-only — wakes are never wire messages
  /// and never survive a checkpoint.
  [[nodiscard]] std::expected<void, ActorError> wake(const ActorPid &pid);
  [[nodiscard]] std::size_t pendingWakes(const ActorPid &pid) const;

  /// P16 ceremony seam (vagent/p16.hpp): the sibling-owned ceremony object
  /// installs itself per actor — typically bound to that actor's arena
  /// from inside the Executor callback. The runtime calls its hooks at the
  /// contract windows around the flagged turn; it never constructs one.
  [[nodiscard]] std::expected<MeshActorRuntime *, ActorError>
  setP16Ceremony(const ActorPid &pid, P16Ceremony *ceremony);
  [[nodiscard]] P16Ceremony *p16Ceremony(const ActorPid &pid) const;
  /// Arms the relocate-before-checkpoint flag — the adapter's half of the
  /// request step: set when the next turn is the one whose end publishes.
  /// Re-arming while Checkpoint is pending is allowed; the pending
  /// restore still drains first at the next turn's head.
  [[nodiscard]] std::expected<void, ActorError>
  raiseP16Flag(const ActorPid &pid);
  [[nodiscard]] P16Flag p16Flag(const ActorPid &pid) const;

  [[nodiscard]] std::expected<void, ActorError>
  send(std::string destinationNode, ActorMessage message);
  [[nodiscard]] std::expected<void, ActorError>
  retry(std::string_view requestId);
  [[nodiscard]] bool deliveryAcknowledged(std::string_view requestId) const;

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
  /// mailboxes, queued wakes, outstanding replies and choice stacks.
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

  std::string node_;
  std::uint64_t epoch_;
  std::uint64_t nextSpawn_{1};
  ActorOptions options_;
  Authorizer authorizer_;
  Executor executor_;
  Sender sender_;
  GenesisHook genesisHook_;
  SpawnResultHook spawnResultHook_;
  /// The settings-layer read channel the adapter installs — the
  /// `vagent.*` tunables of actor-executor §4.1/§4.2. Pump-side policy
  /// (`ready_per_message`) reads through it rather than ActorOptions,
  /// which is mesh-level and carries no language policy.
  std::function<std::optional<std::int64_t>(std::string_view)> settingReader_;
  /// P8 rotating pump-scan cursor — the pid key served last, so each
  /// pumpOne resumes the busy-actor scan where the previous call
  /// stopped instead of restarting at actors_.begin().
  std::string scanCursorKey_;
  std::map<std::string, std::unique_ptr<Actor>> actors_;
  std::map<std::string, std::pair<std::string, ActorPid>> spawns_;
  std::map<std::string, Monitor> monitors_;
  std::map<std::string, Delivery> deliveries_;
  std::map<std::string, xanadu::identity::Fingerprint> trustedNodes_;
};
} // namespace zigzag::vortex::mesh
