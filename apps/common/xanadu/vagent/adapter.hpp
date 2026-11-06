#ifndef COMMON_XANADU_VAGENT_ADAPTER_HPP
#define COMMON_XANADU_VAGENT_ADAPTER_HPP

// The adapter the design doc calls greenfield: everything between
// MeshActorRuntime's callbacks and VagentExecutor's language-level
// entry points. Install `operator()` on setExecutor and `genesis` on
// setGenesisHook; the runtime's own machinery (monitors, spawn acks,
// resume) stays mesh-side — this layer translates its observations
// into the durable events the language sees.
//
// Two pid namespaces meet here. MeshActorRuntime assigns
// ActorPid{node, cursor=contentId, epoch, nextSpawn_++} — a global
// counter with no relation to a program's member list. The language
// resolves a member by its index in the home directory's +d.agents
// rank (VagentExecutor's agentCellOf convention: the declared agent
// is sequence 1). The durable half of the translation is a `pid`
// varsRecord on the member cell carrying the rendered mesh key —
// written by noteMemberPid at spawn-completion and rewritten by
// genesis on each reincarnation — so a crashed adapter loses no
// mapping and a migrated member's identity follows it.

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "executor.hpp"
#include "../identity/identity_layout.hpp"

namespace xanadu::vagent {

/// A native skill body's ABI: the drain hands the binding the calling
/// member and the marshaled call term (its +d.step children are the
/// args), and the body either mints the return cell in the arena or
/// fails with the cause an `EffectFailed` percept reports.
using NativeBody = std::function<std::expected<zigzag::CellRef, std::string>(
    zigzag::vortex::WorkerContext &, zigzag::CellRef agent,
    zigzag::CellRef payload)>;

/// The wire-send half a `spawn` entry needs: the integration layer
/// reads the member's records (+d.name, +d.space), builds the
/// ActorSpawn (programRef, capability, hashcash stamp) and hands it
/// to MeshActorRuntime::send. `requestId` is the staged entry's
/// durable id — the same token spawnOutcome resolves, so the
/// member's `spawnRequest` record is written before the callback
/// runs.
using SpawnSender =
    std::function<std::expected<void, zigzag::vortex::mesh::ActorError>(
        zigzag::vortex::WorkerContext &, zigzag::CellRef member,
        std::string_view requestId)>;

class VagentAdapter {
public:
  /// The executor does every turn's work; the adapter wraps it.
  explicit VagentAdapter(VagentExecutor executor = VagentExecutor{});

  /// Install the host body behind `capacity.method` — a `native skill`
  /// declaration's real implementation. The drain invokes it once per
  /// staged effect entry and replies `EffectDone` with the minted
  /// result or `EffectFailed{cause}` with the refusal.
  void bindNative(std::string_view capacity, std::string_view method,
                  NativeBody body);

  /// Install the transport the drain calls per staged `spawn` entry
  /// — absent, the entry waits undispatched (a member is a live
  /// language-side child either way; the wire request is mesh-side
  /// only). Setters chain.
  VagentAdapter *setSpawnSender(SpawnSender sender);

  /// MeshActorRuntime::Executor. Translates before delegating: the
  /// exec pid's mesh spawnSequence becomes the member's directory
  /// index (the language's pid namespace), and an ActorDown mailbox
  /// message becomes childDown on the parent — the message is
  /// consumed rather than dispatched, because the language-level
  /// percept is `ChildDown{child}`, never a generic "ActorDown"
  /// event. A committed turn ends with the §3.7 outbox drain.
  [[nodiscard]] std::expected<std::string,
                              zigzag::vortex::mesh::ActorError>
  operator()(const zigzag::vortex::mesh::ActorExecution &exec,
             zigzag::vortex::WorkerContext &ctx);

  /// MeshActorRuntime::GenesisHook. Every genesis (spawn or resume)
  /// rewrites the member's `pid` record to the incarnation the mesh
  /// just assigned — the durable identity a later ActorDown resolves
  /// through. A transported checkpoint additionally means the agent
  /// reincarnated: its effect parks fault with "migrated" per §5.4.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  genesis(const zigzag::vortex::mesh::ActorGenesis &genesis,
          zigzag::vortex::WorkerContext &ctx);

  /// §3.7's post-commit half: every staged entry routes through the
  /// directory and delivers locally — a shared-arena program's only
  /// reachable targets. A concrete target naming no member faults the
  /// sender through deliveryFailed; an empty-scoped or broadcast fan-
  /// out to nobody is not a failure.
  void drain(zigzag::vortex::WorkerContext &ctx,
             zigzag::CellRef member) const;

  /// Write the member's durable mesh identity — the `pid` record a
  /// later memberForPid resolves through. Idempotent; a reincarnation
  /// overwrites the prior incarnation's key.
  void noteMemberPid(zigzag::vortex::WorkerContext &ctx,
                     zigzag::CellRef member,
                     const zigzag::vortex::mesh::ActorPid &pid) const;

  /// The member whose `pid` record names @p pid's rendered key, if
  /// any — the only sound resolution for a mesh pid, whose
  /// spawnSequence is the runtime's global counter, never a member
  /// index.
  [[nodiscard]] std::optional<zigzag::CellRef>
  memberForPid(zigzag::vortex::WorkerContext &ctx,
               const zigzag::vortex::mesh::ActorPid &pid) const;

  /// The member whose `+d.name` is @p name — the spawn-refusal seam's
  /// resolution: a refused member carries a name but never received a
  /// pid.
  [[nodiscard]] std::optional<zigzag::CellRef>
  memberNamed(zigzag::vortex::WorkerContext &ctx,
              std::string_view name) const;

  /// @p member's index across the home directory's +d.agents ranks —
  /// the language-level spawnSequence the executor resolves. Empty
  /// when @p member is not in the directory.
  [[nodiscard]] std::optional<std::size_t>
  memberIndex(zigzag::vortex::WorkerContext &ctx,
              zigzag::CellRef member) const;

  /// §5.4's Moved receiver half — the mesh observed the peer at a new
  /// reference; @p member is the adapter's fresh reference cell for
  /// @p holder's stale binding.
  void memberRebound(zigzag::vortex::WorkerContext &ctx,
                     zigzag::CellRef holder, zigzag::CellRef member) const;

  /// §8.6's refused spawn — the runtime's ActorSpawn reply named no
  /// pid; @p parent is the member that staged the request, @p child
  /// the refused member cell (its `+d.name` is the spawn's name).
  void spawnRefused(zigzag::vortex::WorkerContext &ctx,
                    zigzag::CellRef parent, zigzag::CellRef child) const;

  /// §4.5's announcement for the causes only the mesh observes — a
  /// genesis refusal ("init"), a lease lapsed without renewal
  /// ("lease"), a host terminate ("terminate"). `stop` itself runs
  /// in-turn and never reaches here; the shared `childstopping:`
  /// dedupe keeps the report singular either way.
  void memberStopping(zigzag::vortex::WorkerContext &ctx,
                      zigzag::CellRef parent, zigzag::CellRef member,
                      std::string_view cause) const;

  /// §"timers and deadlines"' heap derivation. Every parked wait
  /// writes `awaitDeadline` as an absolute `$turn` ordinal; the
  /// minimum across the member's parked intentions is the turn the
  /// runtime owes a wake by — an idle actor's clock advances only
  /// when MeshActorRuntime::wake injects a P4 turn, so the heap is
  /// the difference between a deadline firing and it never arriving.
  /// Empty when nothing waits on a deadline. The host polls after
  /// each committed turn; a deadline that comes due while the member
  /// keeps working costs nothing (the settle's own rung consumes it).
  [[nodiscard]] std::optional<std::int64_t>
  nextDeadline(zigzag::vortex::WorkerContext &ctx,
               zigzag::CellRef member) const;

  /// Correlate an outbound ActorSpawn with the member it asked for —
  /// the `spawnRequest` record the ack resolves through. Written
  /// before runtime.send so a crash between them loses no request.
  void noteSpawnRequest(zigzag::vortex::WorkerContext &ctx,
                        zigzag::CellRef member,
                        std::string_view requestId) const;

  /// The SpawnResultHook's adapter half: find the member whose
  /// `spawnRequest` names the request's id — grant notes its real
  /// mesh pid and clears the pending record, refusal runs §8.6's
  /// SpawnFailed on the requesting agent. An id no member claims is
  /// a stale or duplicate ack and reports nothing.
  void spawnOutcome(
      zigzag::vortex::WorkerContext &ctx,
      const zigzag::vortex::mesh::ActorMessage &request,
      const std::expected<zigzag::vortex::mesh::ActorPid,
                          zigzag::vortex::mesh::ActorError> &result)
      const;

private:
  VagentExecutor executor_;
  std::map<std::string, NativeBody> natives_;
  SpawnSender spawnSender_;
};

} // namespace xanadu::vagent

#endif
