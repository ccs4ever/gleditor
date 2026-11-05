#ifndef COMMON_XANADU_VAGENT_EXECUTOR_HPP
#define COMMON_XANADU_VAGENT_EXECUTOR_HPP

// The §3.2 executor callback (design/vagent-actor-executor.md): the
// adapter installs this on MeshActorRuntime::setExecutor, and each pump
// service becomes one actor turn — base choice point before m0, per-turn
// dewey reset, $turn bump, bounded d.seen journal, `on` handler dispatch
// through clauses_of/body_of/runFragment, then the commit or failure
// protocol with zero residuals. Statement-segmented bodies, beliefs,
// selection, intentions and the outbox drain are M2–M4; this skeleton
// runs the turn protocol and the event-dispatch seam.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../vortex/mesh_actor.hpp"
#include "../vortex/worker_context.hpp"

namespace xanadu::vagent {

/// One §3.7 outbox entry as the drain hands it to host bindings —
/// `id` is the deterministic idempotence key (sender-name hash + turn
/// + outbox position), which is what makes duplicate delivery
/// detectable rather than invisible.
struct OutboxEntry {
  std::string kind;
  std::string target;
  std::string payload;
  std::string id;
  // `in`'s delivery scope — empty when the emit named none.
  std::string scope;
  // The staged record's payload cell — the marshaled term with its
  // +d.step field children, which tell/untell deliveries re-assert
  // receiver-side. noCell when the field is absent.
  zigzag::CellRef payloadCell{zigzag::noCell};
  // The agent whose outbox the entry drained from — the provenance
  // peer a delivered fact cites.
  zigzag::CellRef sender{zigzag::noCell};
  // The parked parent intention behind a kind "achieve" record —
  // the receiver-side adoption points the child's delegator field
  // back at it. noCell for every other kind.
  zigzag::CellRef delegator{zigzag::noCell};
};

/// One staged delivery the adapter owes a receiver: the directory-
/// resolved agent cell and the percept name it should see. Routing
/// is the adapter's name-resolution half — the receiver's own turn
/// still owns journaling, dispatch and belief mutation.
struct Delivery {
  zigzag::CellRef agent{zigzag::noCell};
  std::string percept;
  // The outbox entry's idempotence id — the receiver's +d.delivered
  // journal keys on it so a re-drained record delivers once.
  std::string id;
  // The staged record's kind — "tell"/"untell" deliver as receiver
  // inbox records (belief effects), everything else as a percept.
  std::string kind;
  // The marshaled payload term — carried for tell/untell so the
  // receiver asserts the same fact cell the sender marshaled.
  zigzag::CellRef payloadCell{zigzag::noCell};
  // The answer's correlation token — for kind "ask" the request
  // entry's id, echoed onto the journal cell's `correlation` record.
  std::string correlation;
  // The sender's member cell — the belief's +d.source peer.
  zigzag::CellRef sender{zigzag::noCell};
  // The delegating intention an "achieve" delivery answers to —
  // the receiver adoption writes it as the child's delegator.
  zigzag::CellRef delegator{zigzag::noCell};
};

class VagentExecutor {
public:
  /// The same settings-layer read channel the runtime holds (§4.1) —
  /// `vagent.journal_size`/`vagent.compact_every` read through it; an
  /// absent reader falls back to the compiled-in defaults.
  explicit VagentExecutor(
      std::function<std::optional<std::int64_t>(std::string_view)>
          settingReader = {});

  /// One §3.2 turn. A turn fault returns ExecutionFailed — the pump maps
  /// it to ActorDown, which is what fail_step means mesh-side.
  [[nodiscard]] std::expected<std::string, zigzag::vortex::mesh::ActorError>
  operator()(const zigzag::vortex::mesh::ActorExecution &exec,
             zigzag::vortex::WorkerContext &ctx);

  /// The §3.7 drain: a pure `rankAfter`/`linked` read of the agent's
  /// `+d.outbox` rank. ConsumeWork-free by construction — the adapter
  /// calls it only post-commit, where no mark is open and nothing mints.
  /// Entries persist on the rank; repeated drains return the same
  /// ids, which is exactly what a host binding compares to detect a
  /// duplicate (`EffectFailed{cause:"duplicate"}` — detectable, not
  /// exactly-once, per §3.7's honest bound).
  [[nodiscard]] static std::vector<OutboxEntry>
  drainOutbox(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
              zigzag::CellRef agent);

  /// Resolve one outbox entry into the deliveries it owes. `target`
  /// names a directory agent; "0"/empty means every member
  /// (broadcast). Payload carries the percept's functor name — a
  /// term cell's text, which is all the receiver's dispatch needs.
  /// A non-empty `scope` restricts delivery to members carrying that
  /// `+d.space` name.
  [[nodiscard]] static std::vector<Delivery>
  route(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
        zigzag::CellRef programRoot, const OutboxEntry &entry);

  /// Land one routed percept on the receiver: a `+d.seen` journal
  /// entry (the evidence a parked await/ask matches) plus a
  /// `+d.pending` trigger the receiver's own settle drains and
  /// dispatches. Two cells, no receiver mutation inside the sender's
  /// turn. Idempotence stays the caller's job: drain `id`s are
  /// stable, and a host binding that delivers the same id twice
  /// re-queues the percept.
  void deliver(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
               const Delivery &delivery) const;

private:
  [[nodiscard]] std::int64_t setting(std::string_view key,
                                     std::int64_t fallback) const;

  std::function<std::optional<std::int64_t>(std::string_view)> settingReader_;
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_EXECUTOR_HPP
