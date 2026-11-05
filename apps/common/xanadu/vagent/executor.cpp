#include "executor.hpp"

#include "../vortex/vortex_vm.hpp"
#include "../zigzag/arena_manifold.hpp"
#include "meta.hpp"
#include "records.hpp"

namespace xanadu::vagent {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorExecution;

/// The dims the turn walks — every one resolved *before* the base CP:
/// a mint under a live mark is the P17 desync the pre-mark block exists
/// to prevent (§3.2; `findOrMintDimension` files the `d.dims` rank, and
/// even a trailed mint rolls back on `release(m0)`, leaving a DimRef
/// the next turn resolves to a fresh cell).
struct ExecutorDims {
  DimRef dirs{noCell};
  DimRef agents{noCell};
  DimRef seen{noCell};
  DimRef belief{noCell};
  DimRef source{noCell};
  DimRef evidence{noCell};
  DimRef pending{noCell};
  DimRef verb{noCell};
  DimRef intention{noCell};
  DimRef subgoal{noCell};
  DimRef outbox{noCell};
};

/// `home +d.dirs → directory +d.agents → agent` — walked, never cached,
/// so the read survives re-genesis.
[[nodiscard]] std::optional<CellRef>
agentCellOf(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
            const ExecutorDims &dims) {
  for (const CellRef dir : zigzag::rankAfter(arena, core.home(), dims.dirs))
    for (const CellRef agent : zigzag::rankAfter(arena, dir, dims.agents))
      return agent;
  return std::nullopt;
}

/// The event symbol carrying @p name as its cell content — the
/// exportSymbol shape (name as content, never a +d.name edge) — across
/// every module's +d.vars rank.
[[nodiscard]] std::optional<CellRef>
eventSymbolNamed(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                 CellRef programRoot, std::string_view name) {
  for (const CellRef module :
       zigzag::rankAfter(arena, programRoot, core.dims().stdlib))
    for (const CellRef sym : zigzag::rankAfter(arena, module, core.dims().vars))
      if (arena.textOf(sym) == name) return sym;
  return std::nullopt;
}

/// Append @p entry to the agent's +d.seen journal and drop the oldest
/// heads while the rank exceeds @p bound — the ringrank's bounded form
/// (§3.1's percept journal; provenance +d.evidence edges are M2).
void journalAppend(zigzag::ArenaManifold &arena, CellRef agent, CellRef entry,
                   DimRef seen, std::size_t bound) {
  zigzag::expectWritten(arena.link(zigzag::rankTail(arena, agent, seen), seen,
                                   DimVector::POS, entry));
  const auto members =
      zigzag::rankAfter(arena, agent, seen) | std::ranges::to<std::vector>();
  const std::size_t excess = members.size() - std::min(bound, members.size());
  for (std::size_t i = 0; i < excess; ++i) unlinkRank(arena, members[i], seen);
}

} // namespace

VagentExecutor::VagentExecutor(
    std::function<std::optional<std::int64_t>(std::string_view)> settingReader)
    : settingReader_(std::move(settingReader)) {}

std::int64_t VagentExecutor::setting(std::string_view key,
                                     std::int64_t fallback) const {
  if (!settingReader_) return fallback;
  const auto value = settingReader_(key);
  return (value && *value >= 0) ? *value : fallback;
}

std::expected<std::string, ActorError>
VagentExecutor::operator()(const ActorExecution &exec, WorkerContext &ctx) {
  auto &arena = ctx.arena;
  auto &core  = ctx.core;
  auto &vm    = ctx.vm;

  // Everything that mints dims or resolves the bind set happens before
  // any mark: arenaDims_ is untrailed (meta.hpp's P17 hazard), and
  // mintMetaBindSet refuses while marks are outstanding.
  // The whole vocabulary the actor language walks resolves here —
  // the M2+ dims come pre-filed so no verb ever mints one under m0.
  ExecutorDims dims;
  dims.dirs      = core.findOrMintDimension("d.dirs");
  dims.agents    = core.findOrMintDimension("d.agents");
  dims.seen      = core.findOrMintDimension("d.seen");
  dims.belief    = core.findOrMintDimension("d.belief");
  dims.source    = core.findOrMintDimension("d.source");
  dims.evidence  = core.findOrMintDimension("d.evidence");
  dims.pending   = core.findOrMintDimension("d.pending");
  dims.verb      = core.findOrMintDimension("d.verb");
  dims.intention = core.findOrMintDimension("d.intention");
  dims.subgoal   = core.findOrMintDimension("d.subgoal");
  dims.outbox    = core.findOrMintDimension("d.outbox");

  MetaBindSet binds;
  const CellRef programRoot = core.home();
  if (const auto agent = agentCellOf(arena, core, dims); agent) {
    if (auto minted = mintMetaBindSet(arena, core, programRoot, *agent)) {
      binds = std::move(*minted);
      // The agent's own module: the first +d.stdlib member — the same
      // default graft/learn resolve bare names against.
      for (const CellRef module :
           zigzag::rankAfter(arena, programRoot, core.dims().stdlib)) {
        binds.ownModule = module;
        break;
      }
    }
  }

  const CellRef failOp = vm.mintOpcode(OpcodeKind::Fail, "fail_step");
  // §3.2 order (review r3): the base CP is pushed *before* m0, so its
  // mark sits below m0 and release(m0) is unconditionally well-nested —
  // no zero-write gap to rely on.
  vm.pushChoicePoint(exec.cursor, failOp);
  const auto m0 = arena.mark();
  vm.setCurrentDeweyPath({});

  auto failTurn = [&]() -> std::expected<std::string, ActorError> {
    // Failure: per-statement CPs drop above the base barrier, release(m0)
    // rolls the turn back, backtrack lands on the base CP's fail op.
    vm.cut(1);
    arena.release(m0);
    vm.backtrack(exec.cursor);
    return std::unexpected(ActorError::ExecutionFailed);
  };

  if (binds.agentCell == noCell) return failTurn();
  const CellRef agent = binds.agentCell;

  // $turn++ — mint-and-repoint the seeded record's +d.values scalar:
  // content restates only under ShadowCommit::Values, which nothing opts
  // into (meta.cpp's serial idiom), so an in-place write would rewind at
  // the next genesis.
  std::int64_t turn = 0;
  if (const auto rec = varsRecord(arena, core, agent, "$turn", false)) {
    if (const auto value =
            arena.linked(*rec, core.dims().values, DimVector::POS)) {
      if (const auto current = arena.asInt64(*value)) {
        turn = *current + 1;
        zigzag::expectWritten(
            arena.unlink(*rec, core.dims().values, DimVector::POS));
      }
    }
    zigzag::expectWritten(arena.link(*rec, core.dims().values, DimVector::POS,
                                     arena.makeScalarCell(turn)));
  }

  // Journal + dispatch. A wake turn (message absent) carries no percept:
  // it journals nothing and dispatches no `on` handler.
  MetaRuntime meta(core, vm, binds);
  const auto maxCycles = static_cast<std::size_t>(std::min<std::uint64_t>(
      exec.grant.maxInstructions == 0 ? 100'000 : exec.grant.maxInstructions,
      std::uint64_t{1'000'000}));

  // One dispatch for both turn kinds: every clause on the named event
  // symbol's +d.clause bank runs — M0 clauses through their +d.spin
  // head directly, learn-clause graphs lacking the descriptor through
  // body_of's session wrapper. False means a refusal — the turn faults.

  // §3.7's staging point — the {kind,target,payload,id} record minted
  // inside m0; the adapter's post-commit drain delivers it. id is the
  // deterministic idempotence triple so a duplicate drain is
  // detectable by key rather than by cell identity.
  const auto stageOutbox = [&](std::string_view kind, CellRef payload,
                               CellRef target, CellRef scope = noCell) {
    const auto position = std::ranges::to<std::vector>(
                              zigzag::rankAfter(arena, agent, dims.outbox))
                              .size();
    const auto entry    = arena.makeCell();
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, agent, dims.outbox), dims.outbox,
                   DimVector::POS, entry));
    const auto outField = [&](std::string_view name, CellRef value) {
      if (const auto rec = varsRecord(arena, core, entry, name, true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS, value));
    };
    std::string sender;
    if (const auto name = arena.linked(agent, core.dims().name, DimVector::POS))
      sender = std::string{arena.textOf(*name)};
    outField("kind", arena.makeCell(std::string{kind}));
    outField("payload", payload);
    outField("target", target);
    if (scope != noCell) outField("scope", scope);
    outField("id", arena.makeCell(std::format("{}:{}:{}",
                                              std::hash<std::string>{}(sender),
                                              turn, position)));
  };

  // §3.8 host step: +d.verb names the MetaRuntime entry, +d.vars holds
  // the marshaled args. Peer and turn are implicit — a belief an agent
  // asserts is asserted by that agent on that turn.
  const auto runHostStep = [&](CellRef step, CellRef intention) -> bool {
    const auto verbCell = arena.linked(step, dims.verb, DimVector::POS);
    if (!verbCell) return false;
    const auto verb  = arena.textOf(*verbCell);
    const auto field = [&](std::string_view name) -> CellRef {
      if (const auto rec = varsRecord(arena, core, step, name, false))
        if (const auto value =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          return *value;
      return noCell;
    };
    if (verb == "adopt") {
      const auto goalName = field("goal");
      if (goalName == noCell) return false;
      const auto goal =
          eventSymbolNamed(arena, core, programRoot, arena.textOf(goalName));
      if (!goal) return false;
      // Concurrent-intention bound — the verb sees no settings, so the
      // caller enforces the rank-size cap here.
      const auto live = zigzag::rankAfter(arena, agent, dims.intention) |
                        std::ranges::to<std::vector>();
      if (live.size() >=
          static_cast<std::size_t>(setting("vagent.max_intentions", 32)))
        return false;
      return meta
          .adoptGoal(*goal, turn,
                     static_cast<std::size_t>(
                         setting("vagent.condition_turn_fuel", 256)))
          .has_value();
    }
    if (verb == "emit") {
      const auto payload = field("payload");
      if (payload == noCell) return false;
      // in names the scope, for the recipients; an untargeted emit
      // keeps the schema's 0 — the adapter reads a text target as a
      // delivery address, 0 as broadcast/self.
      const auto target = field("target");
      const auto scope  = field("scope");
      stageOutbox("emit", payload,
                  target != noCell ? target
                                   : arena.makeScalarCell(std::int64_t{0}),
                  scope);
      return true;
    }
    if (verb == "tell" || verb == "untell") {
      // Same staging as emit: the peer name rides target, the fact
      // rides payload — the receiver-side assert is the adapter's
      // delivery work, not the turn's.
      const auto peer    = field("peer");
      const auto payload = field("payload");
      if (peer == noCell || payload == noCell) return false;
      stageOutbox(verb, payload, peer);
      return true;
    }
    if (verb == "stop") {
      // The stopped flag lives on the target agent's vars record;
      // the turn protocol checks it before dispatch and settles
      // nothing for a stopped agent. `self` resolves to the running
      // agent — the current spine still finishes (statement-boundary
      // semantics); every later turn is a no-op.
      const auto peer = field("peer");
      if (peer == noCell) return false;
      const auto want = arena.textOf(peer);
      CellRef target  = noCell;
      if (want == "self") {
        target = agent;
      } else {
        for (const CellRef dir :
             zigzag::rankAfter(arena, programRoot, dims.dirs))
          for (const CellRef peer_ : zigzag::rankAfter(arena, dir, dims.agents))
            if (const auto name =
                    arena.linked(peer_, core.dims().name, DimVector::POS);
                name && arena.textOf(*name) == want)
              target = peer_;
      }
      if (target == noCell) return false;
      if (const auto rec = varsRecord(arena, core, target, "stopped", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeScalarCell(std::int64_t{1})));
      return true;
    }
    if (verb == "ask") {
      // Question staging + suspension in one step: the request rides
      // the outbox for the adapter, and the intention parks waiting
      // on a journal percept under the question's name — the same
      // functor-text match await uses. The within bound rides
      // awaitDeadline; expiry takes the fault ladder.
      if (intention == noCell) return false;
      const auto peer     = field("peer");
      const auto payload  = field("payload");
      const auto deadline = field("deadline");
      if (peer == noCell || payload == noCell || deadline == noCell)
        return false;
      const auto turns = arena.asInt64(deadline);
      if (!turns) return false;
      stageOutbox("ask", payload, peer);
      // awaitEvent must not reuse the payload cell: -d.values pairs
      // one inbound neighbor per cell, so a second link here would
      // evict the outbox entry's payload edge (the +d.holder finding,
      // again). The wake matcher only needs the functor's text.
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitEvent", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeCell(arena.textOf(payload))));
      meta.setIntentionField(intention, "awaitDeadline", turn + *turns);
      meta.setIntentionField(intention, "waiting", 1);
      meta.setIntentionField(intention, "ready", 0);
      return true;
    }
    if (verb == "await") {
      // Suspension needs a record to park — await in an `on` handler's
      // dispatch spine has nothing to suspend, so it refuses there.
      if (intention == noCell) return false;
      const auto event    = field("event");
      const auto deadline = field("deadline");
      if (event == noCell || deadline == noCell) return false;
      const auto turns = arena.asInt64(deadline);
      if (!turns) return false;
      // §3.5's park: the spine index advances past the await member
      // (the settle walker's step++ below), waiting/ready gate the
      // resume, and the absolute deadline rides on the intention.
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitEvent", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS, event));
      meta.setIntentionField(intention, "awaitDeadline", turn + *turns);
      meta.setIntentionField(intention, "waiting", 1);
      meta.setIntentionField(intention, "ready", 0);
      return true;
    }
    if (verb == "achieve") {
      // §3.5's synchronous delegation: the parent adopts the child
      // goal and parks — the child rides +d.subgoal, and its retire
      // (or drop) writes the parent's ready flag back. The `by` form
      // is remote: the goal stages onto the outbox for the named
      // peer (delivery is adapter work) and the parent parks on the
      // grammar-required within bound — its expiry is the
      // delegated-deadline rung in the settle.
      if (intention == noCell) return false;
      const auto goalName = field("goal");
      if (goalName == noCell) return false;
      if (const auto peer = field("peer"); peer != noCell) {
        stageOutbox("achieve", goalName, peer);
        meta.setIntentionField(intention, "delegated", 1);
        meta.setIntentionField(intention, "ready", 0);
        const auto dl      = field("deadline");
        const auto dlTurns = dl != noCell ? arena.asInt64(dl) : std::nullopt;
        if (!dlTurns) return false; // `by` without `within` doesn't parse
        meta.setIntentionField(intention, "awaitDeadline", turn + *dlTurns);
        return true;
      }
      const auto goal =
          eventSymbolNamed(arena, core, programRoot, arena.textOf(goalName));
      if (!goal) return false;
      const auto live = zigzag::rankAfter(arena, agent, dims.intention) |
                        std::ranges::to<std::vector>();
      if (live.size() >=
          static_cast<std::size_t>(setting("vagent.max_intentions", 32)))
        return false;
      auto child = meta.adoptGoal(
          *goal, turn,
          static_cast<std::size_t>(setting("vagent.condition_turn_fuel", 256)));
      if (!child) return false;
      zigzag::expectWritten(
          arena.link(intention, dims.subgoal, DimVector::POS, *child));
      meta.setIntentionField(*child, "delegator",
                             static_cast<std::int64_t>(intention));
      meta.setIntentionField(intention, "delegated", 1);
      meta.setIntentionField(intention, "ready", 0);
      // within bounds the delegation in turns — the same scalar clock
      // await's deadline rides on; the settle's delegated-deadline
      // rung faults the parent and drops the unfinished child. A
      // deadline-free re-delegation clears the field so the previous
      // bound can't fire early on the new child.
      if (const auto dl = field("deadline"); dl != noCell)
        if (const auto turns = arena.asInt64(dl)) {
          meta.setIntentionField(intention, "awaitDeadline", turn + *turns);
          return true;
        }
      meta.setIntentionField(intention, "awaitDeadline", 0);
      return true;
    }
    const auto term = field("term");
    if (term == noCell) return false;
    const auto pred = arena.textOf(term);
    if (verb == "believe")
      return meta.believe(pred, term, agent, turn).has_value();
    if (verb == "forget") {
      // forget is infallible — zero matches is a legal no-op.
      (void)meta.forget(pred, term);
      return true;
    }
    if (verb == "revise") {
      // The language's revise takes one term: the predicate's bank
      // becomes exactly that fact — clear then assert.
      for (const CellRef clause : meta.beliefsOf(pred))
        unlinkRank(arena, clause, core.dims().clause);
      return meta.believe(pred, term, agent, turn).has_value();
    }
    return false;
  };

  // A clause carrying +d.step members runs its §3.5 segment spine:
  // fragment members through runFragment, host-step cells through the
  // verb table — in order, inside m0.
  const auto runSpine = [&](CellRef clause) -> bool {
    for (const CellRef member :
         zigzag::rankAfter(arena, clause, core.dims().step)) {
      if (arena.linked(member, dims.verb, DimVector::POS).has_value()) {
        if (!runHostStep(member, noCell)) return false;
      } else {
        const auto outcome =
            meta.runFragment(member, {}, exec.cursor, maxCycles);
        if (outcome.refusal != MetaRunOutcome::Refusal::None) return false;
      }
    }
    return true;
  };

  auto dispatchEvent = [&](std::string_view name) -> bool {
    const auto eventSym = eventSymbolNamed(arena, core, programRoot, name);
    if (!eventSym) return true;
    for (const CellRef clause : meta.clausesOf(*eventSym)) {
      if (!zigzag::rankAfter(arena, clause, core.dims().step).empty()) {
        if (!runSpine(clause)) return false;
        continue;
      }
      const auto spin = arena.linked(clause, core.dims().spin, DimVector::POS);
      if (!spin) continue;
      auto outcome = meta.runFragment(*spin, {}, exec.cursor, maxCycles);
      if (outcome.refusal == MetaRunOutcome::Refusal::NotAHead ||
          outcome.refusal == MetaRunOutcome::Refusal::NoDescriptor) {
        if (const auto head = meta.bodyOf(clause))
          outcome = meta.runFragment(*head, {}, exec.cursor, maxCycles);
      }
      // consumeWork exhaustion and VM faults surface as refusals — a
      // turn fault (§3.2's fail path). An unproductive result (an empty
      // or valueless body) is success, not a fault.
      if (outcome.refusal != MetaRunOutcome::Refusal::None) return false;
    }
    return true;
  };

  // A stopped agent's turn is a clean no-op — no journal, dispatch,
  // drain or settle; the record state persists untouched.
  const auto agentStopped = [&] {
    if (const auto rec = varsRecord(arena, core, agent, "stopped", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto flag = arena.asInt64(*v)) return *flag != 0;
    return false;
  }();

  if (!agentStopped && exec.message) {
    const std::string &eventName = exec.message->value;
    journalAppend(arena, agent, arena.makeCell(eventName), dims.seen,
                  static_cast<std::size_t>(setting("vagent.journal_size", 64)));
    if (!dispatchEvent(eventName)) return failTurn();
  }

  // §3.2 settle: drain +d.pending internal events — each enqueued cell
  // names an event, pops off the rank head and dispatches. The per-turn
  // bound (vagent.max_pending) keeps a self-requeueing loop from
  // starving the actor; the remainder carries to the next turn.
  const auto drainBound =
      static_cast<std::size_t>(setting("vagent.max_pending", 64));
  for (std::size_t drained = 0; !agentStopped && drained < drainBound;
       ++drained) {
    std::optional<CellRef> head;
    for (const CellRef cell : zigzag::rankAfter(arena, agent, dims.pending)) {
      head = cell;
      break;
    }
    if (!head) break;
    const auto name = std::string{arena.textOf(*head)};
    unlinkRank(arena, *head, dims.pending);
    if (!dispatchEvent(name)) return failTurn();
  }

  // The fault arm of §3.5's ladder: count the fault and park for the
  // backoff window; the walker's faulted block owns retry and the
  // drop-when reconsideration at the cap. ready=1 because the ladder
  // only examines ready intentions — a suspension-point fault
  // (await/delegation deadline) would otherwise park forever.
  const auto faultIntention = [&](CellRef i) {
    const auto faults = meta.intentionField(i, "faults").value_or(0);
    meta.setIntentionField(i, "faults", faults + 1);
    meta.setIntentionField(i, "faulted", 1);
    meta.setIntentionField(i, "ready", 1);
    meta.setIntentionField(i, "blocked_until",
                           turn + setting("vagent.intention_backoff", 2));
  };

  // The intention's goal symbol, or noCell when the record lacks one.
  const auto goalOf = [&](CellRef i) -> CellRef {
    if (const auto rec = varsRecord(arena, core, i, "goal", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        return *v;
    return noCell;
  };

  // A produced value's truthiness — the same contract meta.cpp's
  // file-local truthy gives selectClause: bools as-is, numbers by
  // zero, text by the "true" spelling.
  const auto evalBool = [&](CellRef head) -> bool {
    const auto outcome = meta.runFragment(head, {}, exec.cursor, maxCycles);
    if (outcome.refusal != MetaRunOutcome::Refusal::None || !outcome.produced ||
        !outcome.result)
      return false;
    const auto &v = *outcome.result;
    if (const auto *b = std::get_if<bool>(&v)) return *b;
    if (const auto *n = std::get_if<std::int64_t>(&v)) return *n != 0;
    if (const auto *r = std::get_if<double>(&v)) return *r != 0.0;
    return std::get<std::string>(v) == "true";
  };

  // §3.5: ready intentions advance one spine step per settle — record
  // stepping, never cursor state. body_replaced is checked against the
  // adoption-time +d.spin name before any member runs.
  for (const CellRef intention :
       agentStopped ? std::vector<CellRef>{}
                    : zigzag::rankAfter(arena, agent, dims.intention) |
                          std::ranges::to<std::vector>()) {
    const auto waiting = meta.intentionField(intention, "waiting").value_or(0);
    if (waiting != 0) {
      // §3.5/§3.6 resume: the journal holds the percept the awaited
      // event journaled as; a match links +d.evidence — the provenance
      // edge that owns the cell through promote — and readies the
      // intention (the spine index already points past the await
      // member). Missed + deadline → faulted, the failure ladder's rung
      // before drop when.
      const auto awaitEvent = [&]() -> CellRef {
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitEvent", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            return *v;
        return noCell;
      }();
      const auto deadline =
          meta.intentionField(intention, "awaitDeadline").value_or(0);
      bool matched = false;
      if (awaitEvent != noCell) {
        const auto wanted = arena.textOf(awaitEvent);
        for (const CellRef percept :
             zigzag::rankAfter(arena, agent, dims.seen)) {
          if (arena.textOf(percept) == wanted) {
            zigzag::expectWritten(
                arena.link(intention, dims.evidence, DimVector::POS, percept));
            matched = true;
            break;
          }
        }
      }
      if (matched) {
        meta.setIntentionField(intention, "waiting", 0);
        meta.setIntentionField(intention, "ready", 1);
      } else if (turn >= deadline) {
        meta.setIntentionField(intention, "waiting", 0);
        faultIntention(intention);
      }
      if (!matched) continue;
    }
    const auto delegated =
        meta.intentionField(intention, "delegated").value_or(0);
    if (delegated != 0) {
      // achieve within: the delegation's own deadline. Expiry drops
      // the unfinished +d.subgoal children — the delegator's
      // notification path must not fire on them afterwards — then
      // the parent rides the ordinary fault ladder.
      const auto deadline =
          meta.intentionField(intention, "awaitDeadline").value_or(0);
      if (deadline != 0 && turn >= deadline) {
        for (const CellRef child :
             zigzag::rankAfter(arena, intention, dims.subgoal) |
                 std::ranges::to<std::vector>())
          unlinkRank(arena, child, dims.intention);
        zigzag::expectWritten(
            arena.unlink(intention, dims.subgoal, DimVector::POS));
        meta.setIntentionField(intention, "delegated", 0);
        faultIntention(intention);
      }
      continue;
    }
    const auto ready   = meta.intentionField(intention, "ready").value_or(0);
    const auto faulted = meta.intentionField(intention, "faulted").value_or(0);
    const auto blockedTill =
        meta.intentionField(intention, "blocked_until").value_or(0);
    if (ready == 0) continue;
    if (faulted != 0) {
      // §3.5's ladder: a fault parks for vagent.intention_backoff
      // turns, then retries; at the vagent.max_intention_faults cap
      // the goal's dropTest is the reconsideration boundary — truthy
      // drops the record off the rank, falsy (or absent) leaves it
      // faulted for inspection.
      if (blockedTill > turn) continue;
      const auto faults = meta.intentionField(intention, "faults").value_or(0);
      if (faults >= setting("vagent.max_intention_faults", 3)) {
        const auto goal = goalOf(intention);
        bool dropped    = false;
        if (goal != noCell)
          if (const auto rec = varsRecord(arena, core, goal, "dropTest", false))
            if (const auto head =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              dropped = evalBool(*head);
        if (dropped) {
          // A dropped subgoal fails the parent's achieve — the fault
          // ladder decides retry vs. drop there too.
          const auto delegator =
              meta.intentionField(intention, "delegator").value_or(0);
          if (delegator != 0) {
            meta.setIntentionField(static_cast<CellRef>(delegator), "delegated",
                                   0);
            faultIntention(static_cast<CellRef>(delegator));
          }
          unlinkRank(arena, intention, dims.intention);
        }
        continue;
      }
      // Backoff expired below the cap — retry the same member.
      meta.setIntentionField(intention, "faulted", 0);
    }
    if (blockedTill > turn) continue;

    const auto planCell = [&]() -> CellRef {
      if (const auto rec = varsRecord(arena, core, intention, "plan", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          return *v;
      return noCell;
    }();
    const auto bodyName = [&]() -> std::string {
      if (const auto rec =
              varsRecord(arena, core, intention, "bodyName", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          return std::string{arena.textOf(*v)};
      return {};
    }();
    if (planCell == noCell) {
      faultIntention(intention);
      continue;
    }
    // body_replaced: the clause's +d.spin target re-pointed since
    // adoption — graft/prune — so resume refuses silently running the
    // wrong segment.
    std::string liveName;
    if (const auto head =
            arena.linked(planCell, core.dims().spin, DimVector::POS))
      if (const auto name =
              arena.linked(*head, core.dims().name, DimVector::POS))
        liveName = std::string{arena.textOf(*name)};
    if (liveName != bodyName) {
      faultIntention(intention);
      continue;
    }

    const auto spine   = zigzag::rankAfter(arena, planCell, core.dims().step) |
                         std::ranges::to<std::vector>();
    const auto stepIdx = static_cast<std::size_t>(
        meta.intentionField(intention, "step").value_or(0));
    bool ok = true;
    if (spine.empty()) {
      // A single-fragment clause: step 0 runs the spin head, then
      // the intention retires.
      if (stepIdx == 0) {
        const auto head =
            arena.linked(planCell, core.dims().spin, DimVector::POS);
        if (!head) {
          faultIntention(intention);
          continue;
        }
        const auto outcome =
            meta.runFragment(*head, {}, exec.cursor, maxCycles);
        ok = outcome.refusal == MetaRunOutcome::Refusal::None;
      }
    } else if (stepIdx < spine.size()) {
      const auto member = spine[stepIdx];
      if (arena.linked(member, dims.verb, DimVector::POS).has_value())
        ok = runHostStep(member, intention);
      else {
        const auto outcome =
            meta.runFragment(member, {}, exec.cursor, maxCycles);
        ok = outcome.refusal == MetaRunOutcome::Refusal::None;
      }
    }
    if (!ok) {
      faultIntention(intention);
      continue;
    }
    const auto next = stepIdx + 1;
    if (next >= std::max<std::size_t>(spine.size(), 1)) {
      // The body's last member ran. For an achieve goal that is not
      // success yet — the guide's contract is that a plan's end
      // without `succeed when` counts as a failed plan: eval the
      // goal's succeedTest, falsy goes back down the ladder.
      const auto goal = goalOf(intention);
      if (goal != noCell) {
        const auto kind = [&]() -> std::int64_t {
          if (const auto rec = varsRecord(arena, core, goal, "goalKind", false))
            if (const auto v =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              if (const auto n = arena.asInt64(*v)) return *n;
          return 0;
        }();
        if (kind == 1) {
          bool succeeded = true;
          if (const auto rec =
                  varsRecord(arena, core, goal, "succeedTest", false))
            if (const auto head =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              succeeded = evalBool(*head);
          if (!succeeded) {
            faultIntention(intention);
            continue;
          }
        }
      }
      // A spine ending on a delegated achieve can't retire — the
      // record stays parked until the child's retire (or the
      // deadline rung) resolves the delegation; the wake re-enters
      // here with step already past the end and retires then.
      if (meta.intentionField(intention, "delegated").value_or(0) != 0) {
        meta.setIntentionField(intention, "step",
                               static_cast<std::int64_t>(next));
        continue;
      }
      // The intention retires; a delegating parent's achieve resumes
      // on this completion.
      const auto delegator =
          meta.intentionField(intention, "delegator").value_or(0);
      if (delegator != 0) {
        meta.setIntentionField(static_cast<CellRef>(delegator), "delegated", 0);
        meta.setIntentionField(static_cast<CellRef>(delegator), "ready", 1);
      }
      unlinkRank(arena, intention, dims.intention);
    } else {
      meta.setIntentionField(intention, "step",
                             static_cast<std::int64_t>(next));
    }
  }

  // Success: cut(0) drops the base CP too, discard(m0) keeps the turn.
  vm.cut(0);
  arena.discard(m0);
  // The §3.2 residual assertion — marks and choice points must both be
  // empty before the post-commit section can run consumeWork-free.
  if (arena.outstandingMarks() != 0)
    return std::unexpected(ActorError::ExecutionFailed);

  // Post-commit work is consumeWork-free: reads and the bounded
  // compaction only — the §3.2 invariant costs nothing.
  const auto compactEvery = setting("vagent.compact_every", 0);
  if (compactEvery > 0 && (turn + 1) % compactEvery == 0) {
    (void)arena.compact();
  }
  return std::string{"ok"};
}

std::vector<OutboxEntry>
VagentExecutor::drainOutbox(zigzag::ArenaManifold &arena,
                            zigzag::vortex::VortexCore &core,
                            zigzag::CellRef agent) {
  // Reads only — no marks, no consumeWork. The §3.7 contract makes
  // this safe to call post-commit: entries stay on the rank and the
  // host binding deduplicates by `id`. d.outbox is in the pre-mark
  // turn vocabulary, so findOrMint resolves here, never mints.
  const auto outboxDim = core.findOrMintDimension("d.outbox");
  std::vector<OutboxEntry> entries;
  for (const CellRef entry : zigzag::rankAfter(arena, agent, outboxDim)) {
    auto read = [&](std::string_view field) -> std::string {
      const auto rec = varsRecord(arena, core, entry, field, false);
      if (!rec) return {};
      const auto value = arena.linked(*rec, core.dims().values, DimVector::POS);
      return value ? std::string{arena.textOf(*value)} : std::string{};
    };
    entries.push_back(OutboxEntry{.kind    = read("kind"),
                                  .target  = read("target"),
                                  .payload = read("payload"),
                                  .id      = read("id"),
                                  .scope   = read("scope")});
  }
  return entries;
}

} // namespace xanadu::vagent
