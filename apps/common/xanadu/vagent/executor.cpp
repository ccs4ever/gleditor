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
/// so the read survives re-genesis. `spawnSequence` selects the member:
/// the declared agent is sequence 1, spawned members append in order —
/// the directory rank is the spawn ledger. 0/1 both mean the head.
[[nodiscard]] std::optional<CellRef>
agentCellOf(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
            const ExecutorDims &dims, std::uint64_t spawnSequence) {
  const auto wanted = spawnSequence < 2 ? std::size_t{0} : spawnSequence - 1;
  for (const CellRef dir : zigzag::rankAfter(arena, core.home(), dims.dirs)) {
    const auto members = zigzag::rankAfter(arena, dir, dims.agents) |
                         std::ranges::to<std::vector>();
    if (wanted < members.size()) return members[wanted];
  }
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
  if (const auto agent = agentCellOf(arena, core, dims, exec.pid.spawnSequence);
      agent) {
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

  // §4.1's picked arbitration counters, journaled verbatim on $stats:
  // the replay record (§4.7) reads them from agent state, not the
  // transport. wakes/msgs accumulate the service mix across turns.
  if (const auto rec = varsRecord(arena, core, agent, "$stats", false)) {
    // The seeded value is a scalar placeholder; the first turn swaps
    // it for a stats record cell that per-field vars hang off.
    CellRef stats = noCell;
    if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
      if (!arena.asInt64(*v)) stats = *v;
    if (stats == noCell) {
      stats = arena.makeCell();
      if (arena.linked(*rec, core.dims().values, DimVector::POS))
        zigzag::expectWritten(
            arena.unlink(*rec, core.dims().values, DimVector::POS));
      zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                       DimVector::POS, stats));
    }
    const auto bump = [&](std::string_view name, std::int64_t value) {
      if (const auto r = varsRecord(arena, core, stats, name, true)) {
        if (arena.linked(*r, core.dims().values, DimVector::POS))
          zigzag::expectWritten(
              arena.unlink(*r, core.dims().values, DimVector::POS));
        zigzag::expectWritten(arena.link(*r, core.dims().values,
                                         DimVector::POS,
                                         arena.makeScalarCell(value)));
      }
    };
    const auto read = [&](std::string_view name) -> std::int64_t {
      if (const auto r = varsRecord(arena, core, stats, name, false))
        if (const auto v = arena.linked(*r, core.dims().values, DimVector::POS))
          if (const auto n = arena.asInt64(*v)) return *n;
      return 0;
    };
    bump("wakeStreak", static_cast<std::int64_t>(exec.wakeStreak));
    bump("msgStreak", static_cast<std::int64_t>(exec.msgStreak));
    bump("msgs", read("msgs") + (exec.message ? 1 : 0));
    bump("wakes", read("wakes") + (exec.message ? 0 : 1));
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
                               std::string_view target,
                               std::string_view scope = {},
                               CellRef delegator      = noCell) -> std::string {
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
    // The payload is a program-image term cell: its -d.values slot
    // already names it on the marshaled step, and -d.* admits one
    // inbound edge per dim — an edge here would evict the step's own
    // binding (and N entries naming one payload would evict each
    // other). Cross-record references are scalars, like delegator;
    // drainOutbox resolves the ref back to the cell.
    if (payload != noCell)
      outField("payload",
               arena.makeScalarCell(static_cast<std::int64_t>(payload)));
    outField("target", arena.makeCell(std::string{target}));
    if (!scope.empty()) outField("scope", arena.makeCell(std::string{scope}));
    if (delegator != noCell)
      outField("delegator",
               arena.makeScalarCell(static_cast<std::int64_t>(delegator)));
    const auto id = std::format("{}:{}:{}", std::hash<std::string>{}(sender),
                                turn, position);
    outField("id", arena.makeCell(id));
    return id;
  };

  // A marshaled peer/target name cell → the directory member name the
  // outbox should carry: `self` resolves to the running agent's
  // member, a bound vars name resolves through the handle the hoist
  // wrote, anything else stays literal. Text only — the record never
  // edge-links a name cell it didn't mint.
  const auto resolvePeer = [&](CellRef nameCell) -> std::string {
    if (nameCell == noCell) return {};
    const auto want = arena.textOf(nameCell);
    CellRef holder  = noCell;
    if (want == "self") {
      holder = agent;
    } else if (const auto rec = varsRecord(arena, core, agent, want, false)) {
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto ref = arena.asInt64(*v))
          holder = static_cast<CellRef>(*ref);
    }
    if (holder == noCell) return std::string{want};
    const auto n = arena.linked(holder, core.dims().name, DimVector::POS);
    return n ? std::string{arena.textOf(*n)} : std::string{want};
  };

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
    // The step's `else` head marshals as a scalar ref; the park
    // re-mints it — an edge to the marshaled cell is the same
    // single-inbound-slot hazard awaitEvent's payload dodge covers.
    const auto parkElse = [&] {
      if (const auto e = field("else"); e != noCell)
        if (const auto n = arena.asInt64(e))
          if (const auto rec =
                  varsRecord(arena, core, intention, "awaitElse", true))
            zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                             DimVector::POS,
                                             arena.makeScalarCell(*n)));
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
      auto child = meta.adoptGoal(
          *goal, turn,
          static_cast<std::size_t>(setting("vagent.condition_turn_fuel", 256)));
      if (!child) return false;
      // The hoist's result binding: `task := adopt G()` lands the
      // child's record ref on the agent's vars under the bound name —
      // the Intention-typed handle drop reads back.
      if (const auto bound = field("result"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, agent, arena.textOf(bound), true))
          zigzag::expectWritten(arena.link(
              *rec, core.dims().values, DimVector::POS,
              arena.makeScalarCell(static_cast<std::int64_t>(*child))));
      return true;
    }
    if (verb == "emit") {
      const auto payload = field("payload");
      if (payload == noCell) return false;
      // in names the scope, for the recipients; an untargeted emit
      // keeps the schema's 0 — the adapter reads a text target as a
      // delivery address, 0 as broadcast/self. resolvePeer turns
      // `for <bound var>`/`self` into the member name the record
      // must carry — a sender-local binding has no meaning off-agent.
      const auto target = field("target");
      const auto scope  = field("scope");
      stageOutbox("emit", payload,
                  target != noCell ? resolvePeer(target) : std::string{"0"},
                  scope != noCell ? arena.textOf(scope) : std::string_view{});
      return true;
    }
    if (verb == "tell" || verb == "untell") {
      // Same staging as emit: the peer name rides target, the fact
      // rides payload — the receiver-side assert is the adapter's
      // delivery work, not the turn's.
      const auto peer    = field("peer");
      const auto payload = field("payload");
      if (peer == noCell || payload == noCell) return false;
      stageOutbox(verb, payload, resolvePeer(peer));
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
    if (verb == "drop") {
      // `drop task`: the handle names the agent vars record an
      // expression-position adopt wrote its result into. The value is
      // the intention's record ref — unlink it off the rank and fault
      // any delegator, the same notification a dropped subgoal sends.
      const auto handle = field("handle");
      if (handle == noCell) return false;
      const auto rec =
          varsRecord(arena, core, agent, arena.textOf(handle), false);
      const auto ref = [&]() -> CellRef {
        if (!rec) return noCell;
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto n = arena.asInt64(*v)) return static_cast<CellRef>(*n);
        return noCell;
      }();
      if (ref == noCell) return true; // none-bound handle: vacuous drop
      const auto delegator = meta.intentionField(ref, "delegator").value_or(0);
      if (delegator != 0) {
        meta.setIntentionField(static_cast<CellRef>(delegator), "delegated", 0);
        faultIntention(static_cast<CellRef>(delegator));
      }
      unlinkRank(arena, ref, dims.intention);
      return true;
    }
    if (verb == "spawn") {
      // The child's durable half of the society: a directory member
      // with its own $-seeded records. The program image it binds is
      // the adapter's spawn-completion work — the cell and name exist
      // now, which is what `child`'s binding needs.
      const auto nameField = field("name");
      if (nameField == noCell) return false;
      const auto child = arena.makeCell();
      zigzag::expectWritten(
          arena.link(child, core.dims().name, DimVector::POS,
                     arena.makeCell(arena.textOf(nameField))));
      // `in <space>` rides +d.space on the member — the flat
      // directory's only space relation; route filters scoped emits
      // by it. The declared agent carries none, so `in inner` never
      // delivers back to the program's own agent.
      if (const auto scopeField = field("scope"); scopeField != noCell)
        zigzag::expectWritten(arena.link(
            child, core.findOrMintDimension("d.space"), DimVector::POS,
            arena.makeCell(arena.textOf(scopeField))));
      CellRef dir = noCell;
      for (const CellRef d : zigzag::rankAfter(arena, programRoot, dims.dirs)) {
        dir = d;
        break;
      }
      if (dir == noCell) return false;
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, dir, dims.agents), dims.agents,
                     DimVector::POS, child));
      for (const auto seed : {"$turn", "$step", "$stats"})
        if (const auto rec = varsRecord(arena, core, child, seed, true))
          zigzag::expectWritten(
              arena.link(*rec, core.dims().values, DimVector::POS,
                         arena.makeScalarCell(std::int64_t{0})));
      if (const auto bound = field("result"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, agent, arena.textOf(bound), true))
          zigzag::expectWritten(arena.link(
              *rec, core.dims().values, DimVector::POS,
              arena.makeScalarCell(static_cast<std::int64_t>(child))));
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
      // The staged entry's id is the request's correlation token: the
      // adapter echoes it on the answer's journal cell, and the wake
      // match requires it — two outstanding `stock` asks can no
      // longer cross-resolve.
      const auto requestId = stageOutbox("ask", payload, resolvePeer(peer));
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitCorr", true))
        zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                         DimVector::POS,
                                         arena.makeCell(requestId)));
      // awaitEvent must not reuse the payload cell: -d.values pairs
      // one inbound neighbor per cell, so a second link here would
      // evict the outbox entry's payload edge (the +d.holder finding,
      // again). The wake matcher only needs the functor's text.
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitEvent", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeCell(arena.textOf(payload))));
      // `r := ask ...`: the hoist's result name survives the park on
      // the intention — the wake match writes the answer percept's
      // handle under it.
      if (const auto bound = field("result"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitBind", true))
          zigzag::expectWritten(
              arena.link(*rec, core.dims().values, DimVector::POS,
                         arena.makeCell(arena.textOf(bound))));
      parkElse();
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
      // The marshaled event cell is shared step state — a second run
      // of this spine would find its binding evicted if the record
      // edge-linked it. The name goes as a text copy; a term with
      // +d.step children (AwaitTerm) also parks its ref so the wake
      // match can compare fields structurally.
      const auto structured =
          !zigzag::rankAfter(arena, event, core.dims().step).empty();
      // `await ready(x)` on a predicate is the belief form — it tests
      // what the bank already holds, and a satisfied fact means no
      // suspension at all (the guide's "tests what is already
      // present"). symKind on the symbol is the only predicate-vs-
      // event marker the grammar leaves behind.
      bool beliefForm = false;
      if (const auto sym =
              eventSymbolNamed(arena, core, programRoot, arena.textOf(event)))
        if (const auto rec = varsRecord(arena, core, *sym, "symKind", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            beliefForm = arena.textOf(*v) == "predicate";
      if (beliefForm && meta.believes(arena.textOf(event), event)) return true;
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitEvent", true))
        zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                         DimVector::POS,
                                         arena.makeCell(arena.textOf(event))));
      if (structured || beliefForm)
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitTerm", true))
          zigzag::expectWritten(arena.link(
              *rec, core.dims().values, DimVector::POS,
              arena.makeScalarCell(static_cast<std::int64_t>(event))));
      if (beliefForm)
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitForm", true))
          zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                           DimVector::POS,
                                           arena.makeCell("belief")));
      // `await X as g`: the bound name survives the park the same way
      // ask's result does — the wake match writes the fired event's
      // handle under it.
      if (const auto bound = field("binding"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitBind", true))
          zigzag::expectWritten(
              arena.link(*rec, core.dims().values, DimVector::POS,
                         arena.makeCell(arena.textOf(bound))));
      parkElse();
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
        // The parent intention's ref rides the record so the
        // receiver-side adoption can point the child's delegator
        // back — cross-agent delegation is the same cell-link
        // mechanism the local +d.subgoal path uses.
        stageOutbox("achieve", goalName, resolvePeer(peer), {}, intention);
        meta.setIntentionField(intention, "delegated", 1);
        meta.setIntentionField(intention, "ready", 0);
        const auto dl      = field("deadline");
        const auto dlTurns = dl != noCell ? arena.asInt64(dl) : std::nullopt;
        if (!dlTurns) return false; // `by` without `within` doesn't parse
        parkElse();
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
      parkElse();
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

  // The receiver half of tell/untell: each +d.inbox record is a
  // cross-agent assertion the adapter staged — the belief mutation
  // happens here, in the receiver's own turn, with the sender's
  // member cell as the +d.source peer. Same bounded drain shape as
  // pending.
  {
    const auto inboxDim = core.findOrMintDimension("d.inbox");
    for (std::size_t drained = 0; !agentStopped && drained < drainBound;
         ++drained) {
      std::optional<CellRef> head;
      for (const CellRef cell : zigzag::rankAfter(arena, agent, inboxDim)) {
        head = cell;
        break;
      }
      if (!head) break;
      const auto kind = arena.linked(*head, dims.verb, DimVector::POS);
      const auto ref  = [&](std::string_view name) -> CellRef {
        const auto rec = varsRecord(arena, core, *head, name, false);
        if (!rec) return noCell;
        const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
        if (!v) return noCell;
        const auto r = arena.asInt64(*v);
        return r ? static_cast<CellRef>(*r) : noCell;
      };
      const auto term = ref("grab");
      unlinkRank(arena, *head, inboxDim);
      if (!kind || term == noCell) continue;
      CellRef sender = agent;
      if (const auto s = ref("sender"); s != noCell) sender = s;
      const auto pred = arena.textOf(term);
      const auto k    = arena.textOf(*kind);
      if (k == "achieve") {
        // Remote delegation: the named goal must exist in the
        // receiver's own program; a miss is an honest no-adopt and
        // the delegator's deadline faults it rather than a success
        // nobody earned. The child points back at the sender's
        // parked intention — retire and drop write it the same as
        // local delegation, and +d.subgoal on the parent lets the
        // deadline rung drop the remote child.
        const auto delegator = ref("delegator");
        const auto goal      = eventSymbolNamed(arena, core, programRoot, pred);
        if (!goal || delegator == noCell) continue;
        auto child = meta.adoptGoal(*goal, turn,
                                    static_cast<std::size_t>(setting(
                                        "vagent.condition_turn_fuel", 256)));
        if (!child) continue;
        meta.setIntentionField(*child, "delegator",
                               static_cast<std::int64_t>(delegator));
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, delegator, dims.subgoal),
                       dims.subgoal, DimVector::POS, *child));
        continue;
      }
      if (k == "untell") {
        (void)meta.forget(pred, term);
      } else if (!meta.believe(pred, term, sender, turn)) {
        return failTurn();
      }
    }
  }

  // The intention's goal symbol, or noCell when the record lacks one.
  const auto goalOf = [&](CellRef i) -> CellRef {
    if (const auto rec = varsRecord(arena, core, i, "goal", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto n = arena.asInt64(*v)) return static_cast<CellRef>(*n);
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

  // §3.5's deadline recovery: a parked `else` head evaluates with
  // `cause` bound — production binds the value to the suspension's
  // `as r`/`r :=` destination and completes the wait rather than
  // faulting; refusal or non-production reports false so the caller
  // takes the ordinary fault ladder.
  const auto runElse = [&](CellRef intention) -> bool {
    CellRef head = noCell;
    if (const auto rec =
            varsRecord(arena, core, intention, "awaitElse", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto n = arena.asInt64(*v)) head = static_cast<CellRef>(*n);
    if (head == noCell) return false;
    // Params resolve by name: `cause` gets the expiry reason, the
    // rest read the intention's vars records — a param with no
    // readable value aborts the recovery to the fault path. The
    // descriptor's +d.grab/+d.step chain is the parameter signature
    // meta.cpp's parameterCells reads — mirrored here because the
    // reader is private.
    std::vector<std::string> paramNames;
    if (const auto descriptor =
            arena.linked(head, core.dims().values, DimVector::POS))
      for (auto p = arena.linked(*descriptor, core.dims().grab,
                                 DimVector::POS);
           p && arena.contains(*p);
           p = arena.linked(*p, core.dims().step, DimVector::POS))
        if (const auto n = arena.linked(*p, core.dims().name, DimVector::POS))
          paramNames.emplace_back(arena.textOf(*n));
    std::vector<CellValue> args;
    for (const auto &pname : paramNames) {
      if (pname == "cause") {
        args.emplace_back(std::string{"deadline"});
        continue;
      }
      const auto rec = varsRecord(arena, core, intention, pname, false);
      if (!rec) return false;
      const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
      if (!v) return false;
      if (const auto n = arena.asInt64(*v))
        args.emplace_back(*n);
      else if (const auto b = arena.asBool(*v))
        args.emplace_back(*b);
      else if (const auto d = arena.asDouble(*v))
        args.emplace_back(*d);
      else
        args.emplace_back(std::string{arena.textOf(*v)});
    }
    const auto outcome =
        meta.runFragment(head, args, std::nullopt, maxCycles);
    if (outcome.refusal != MetaRunOutcome::Refusal::None ||
        !outcome.produced || !outcome.result)
      return false;
    const auto value = std::visit(
        [&](const auto &x) -> CellRef {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, std::string>)
            return arena.makeCell(x);
          else
            return arena.makeScalarCell(x);
        },
        *outcome.result);
    if (const auto rec =
            varsRecord(arena, core, intention, "awaitBind", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto dst =
                varsRecord(arena, core, agent, arena.textOf(*v), true)) {
          if (arena.linked(*dst, core.dims().values, DimVector::POS))
            zigzag::expectWritten(
                arena.unlink(*dst, core.dims().values, DimVector::POS));
          zigzag::expectWritten(arena.link(*dst, core.dims().values,
                                           DimVector::POS, value));
        }
    meta.setIntentionField(intention, "waiting", 0);
    meta.setIntentionField(intention, "delegated", 0);
    meta.setIntentionField(intention, "ready", 1);
    return true;
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
      bool matched      = false;
      CellRef matchCell = noCell;
      if (awaitEvent != noCell) {
        const auto wanted = arena.textOf(awaitEvent);
        // awaitCorr narrows the match to the answer carrying this
        // request's id — the adapter echoes the outbox entry's id on
        // the journal cell's `correlation` record.
        std::string corr;
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitCorr", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            corr = std::string{arena.textOf(*v)};
        // A structural AwaitTerm parks its marshaled term as a scalar
        // ref; the percept's `grab` ref carries the delivered
        // envelope. A percept with no envelope compares as its own
        // 0-child term, so `await Foo()` still matches name-only.
        const auto awaitTerm = [&]() -> CellRef {
          if (const auto rec =
                  varsRecord(arena, core, intention, "awaitTerm", false))
            if (const auto v =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              if (const auto r = arena.asInt64(*v))
                return static_cast<CellRef>(*r);
          return noCell;
        }();
        // The belief form's satisfaction set is the bank, not the
        // journal: a matching fact clause is the wake's evidence.
        bool beliefForm = false;
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitForm", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            beliefForm = arena.textOf(*v) == "belief";
        if (beliefForm && awaitTerm != noCell) {
          for (const CellRef clause : meta.beliefsOf(arena.textOf(awaitTerm)))
            if (const auto head =
                    arena.linked(clause, core.dims().grab, DimVector::POS);
                head && meta.termEquals(awaitTerm, *head)) {
              zigzag::expectWritten(
                  arena.link(intention, dims.evidence, DimVector::POS, clause));
              matched   = true;
              matchCell = clause;
              break;
            }
        }
        if (!beliefForm)
          for (const CellRef percept :
               zigzag::rankAfter(arena, agent, dims.seen)) {
            if (arena.textOf(percept) != wanted) continue;
            if (awaitTerm != noCell) {
              CellRef delivered = percept;
              if (const auto rec =
                      varsRecord(arena, core, percept, "grab", false))
                if (const auto v =
                        arena.linked(*rec, core.dims().values, DimVector::POS))
                  if (const auto r = arena.asInt64(*v))
                    delivered = static_cast<CellRef>(*r);
              if (!meta.termEquals(awaitTerm, delivered)) continue;
            }
            if (!corr.empty()) {
              bool same = false;
              if (const auto prec =
                      varsRecord(arena, core, percept, "correlation", false))
                if (const auto v =
                        arena.linked(*prec, core.dims().values, DimVector::POS))
                  same = arena.textOf(*v) == corr;
              if (!same) continue;
            }
            zigzag::expectWritten(
                arena.link(intention, dims.evidence, DimVector::POS, percept));
            matched   = true;
            matchCell = percept;
            break;
          }
      }
      if (matched) {
        // awaitBind carries the `as g`/`r := ask` destination across
        // the park — the matched percept's handle lands agent-side,
        // the same Intention/Agent-ref shape adopt/spawn bind.
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitBind", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            if (const auto dst =
                    varsRecord(arena, core, agent, arena.textOf(*v), true)) {
              if (arena.linked(*dst, core.dims().values, DimVector::POS))
                zigzag::expectWritten(
                    arena.unlink(*dst, core.dims().values, DimVector::POS));
              zigzag::expectWritten(arena.link(
                  *dst, core.dims().values, DimVector::POS,
                  arena.makeScalarCell(static_cast<std::int64_t>(matchCell))));
            }
        meta.setIntentionField(intention, "waiting", 0);
        meta.setIntentionField(intention, "ready", 1);
      } else if (turn >= deadline) {
        meta.setIntentionField(intention, "waiting", 0);
        if (!runElse(intention)) faultIntention(intention);
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
        if (!runElse(intention)) faultIntention(intention);
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

    // A maintain intention is a monitor, never a spine-runner:
    // delegator==0 marks it — each settle re-evals holds and a lapse
    // fires a restorer child through the ordinary delegation
    // machinery (+d.subgoal, delegated park, retire write-back). The
    // child is an ordinary intention and runs the plan; its retire
    // readies the monitor for the re-check. maintain_backoff bounds
    // the refire rate after an unsuccessful pass.
    const auto goalCell = goalOf(intention);
    const auto goalKind = [&]() -> std::int64_t {
      if (goalCell == noCell) return 0;
      if (const auto rec =
              varsRecord(arena, core, goalCell, "goalKind", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto n = arena.asInt64(*v)) return *n;
      return 0;
    }();
    if (goalKind == 2 &&
        meta.intentionField(intention, "delegator").value_or(0) == 0) {
      bool holds = true;
      if (goalCell != noCell) {
        // holdsBelief is a container whose +d.grab rank carries one
        // unification pattern per `believes` conjunct — every pattern
        // must match some bank fact; holdsTest is the peeled scalar
        // remainder evalBool'd. The two AND: belief existence AND
        // scalar truth.
        if (const auto rec =
                varsRecord(arena, core, goalCell, "holdsBelief", false)) {
          if (const auto container =
                  arena.linked(*rec, core.dims().values, DimVector::POS)) {
            // +d.grab membership, not +d.step: a pattern's own
            // +d.step args ride the same chain, so +d.step cannot
            // tell sibling patterns from a pattern's arguments.
            for (const auto pattern :
                 zigzag::rankAfter(arena, *container, core.dims().grab)) {
              const auto pred = arena.textOf(pattern);
              bool       any  = false;
              for (const auto clause : meta.beliefsOf(pred))
                if (const auto fact = arena.linked(clause, core.dims().grab,
                                                   DimVector::POS);
                    fact && meta.termUnifies(pattern, *fact)) {
                  any = true;
                  break;
                }
              if (!any) {
                holds = false;
                break;
              }
            }
          }
        }
        if (holds)
          if (const auto rec = varsRecord(arena, core, goalCell, "holdsTest",
                                          false))
            if (const auto head =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              holds = evalBool(*head);
      }
      if (!holds) {
        auto child = meta.adoptGoal(
            goalCell, turn,
            static_cast<std::size_t>(
                setting("vagent.condition_turn_fuel", 256)));
        if (!child) {
          faultIntention(intention);
          continue;
        }
        meta.setIntentionField(*child, "delegator",
                               static_cast<std::int64_t>(intention));
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, intention, dims.subgoal),
                       dims.subgoal, DimVector::POS, *child));
        meta.setIntentionField(intention, "delegated", 1);
        meta.setIntentionField(intention, "ready", 0);
        meta.setIntentionField(
            intention, "blocked_until",
            turn + setting("vagent.maintain_backoff", 1));
      }
      continue;
    }

    const auto planCell = [&]() -> CellRef {
      if (const auto rec = varsRecord(arena, core, intention, "plan", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto n = arena.asInt64(*v))
            return static_cast<CellRef>(*n);
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
      // A spine ending on a suspended verb can't retire — the record
      // stays parked until the await match (or the deadline rung)
      // resolves the suspension; the wake re-enters here with step
      // already past the end and retires then. delegated and waiting
      // are the two suspension flags the host arms set.
      if (meta.intentionField(intention, "delegated").value_or(0) != 0 ||
          meta.intentionField(intention, "waiting").value_or(0) != 0) {
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
    const auto cellField = [&](std::string_view name) -> CellRef {
      const auto rec = varsRecord(arena, core, entry, name, false);
      if (!rec) return noCell;
      const auto value = arena.linked(*rec, core.dims().values, DimVector::POS);
      return value ? *value : noCell;
    };
    const auto refField = [&](std::string_view name) -> CellRef {
      const auto value = cellField(name);
      if (value == noCell) return noCell;
      const auto ref = arena.asInt64(value);
      return ref ? static_cast<CellRef>(*ref) : noCell;
    };
    const auto payloadCell = refField("payload");
    entries.push_back(
        OutboxEntry{.kind        = read("kind"),
                    .target      = read("target"),
                    .payload     = payloadCell != noCell
                                       ? std::string{arena.textOf(payloadCell)}
                                       : std::string{},
                    .id          = read("id"),
                    .scope       = read("scope"),
                    .payloadCell = payloadCell,
                    .sender      = agent,
                    .delegator   = refField("delegator")});
  }
  return entries;
}

std::vector<Delivery> VagentExecutor::route(zigzag::ArenaManifold &arena,
                                            zigzag::vortex::VortexCore &core,
                                            zigzag::CellRef programRoot,
                                            const OutboxEntry &entry) {
  // Name resolution only — the receiver's own turn owns everything
  // after the percept name. "0"/empty is the broadcast fan-out the
  // emit schema reserves; `self`/`parent` and other implicit handles
  // resolve as directory names only, which is why spawn binds a cell.
  const auto dirs   = core.findOrMintDimension("d.dirs");
  const auto agents = core.findOrMintDimension("d.agents");
  const auto space  = core.findOrMintDimension("d.space");
  std::vector<Delivery> deliveries;
  for (const CellRef dir : zigzag::rankAfter(arena, programRoot, dirs))
    for (const CellRef member : zigzag::rankAfter(arena, dir, agents)) {
      // `in <scope>` narrows the member set: only members spawned
      // into that space match. A member with no +d.space edge (the
      // declared agent) is outside every scope.
      if (!entry.scope.empty()) {
        const auto s = arena.linked(member, space, DimVector::POS);
        if (!s || arena.textOf(*s) != entry.scope) continue;
      }
      std::string name;
      if (const auto n = arena.linked(member, core.dims().name, DimVector::POS))
        name = std::string{arena.textOf(*n)};
      if (entry.target == "0" || entry.target.empty() || name == entry.target)
        deliveries.push_back(Delivery{.agent       = member,
                                      .percept     = entry.payload,
                                      .id          = entry.id,
                                      .kind        = entry.kind,
                                      .payloadCell = entry.payloadCell,
                                      .sender      = entry.sender,
                                      .delegator   = entry.delegator});
    }
  return deliveries;
}

void VagentExecutor::deliver(zigzag::ArenaManifold &arena,
                             zigzag::vortex::VortexCore &core,
                             const Delivery &delivery) const {
  if (delivery.agent == noCell) return;

  // §3.7's idempotence key, receiver-side: the entry's stable id
  // journals onto +d.delivered, so a re-drained record or a double
  // drain stages no second inbox/percept. The rank is deliberately
  // unbounded — evicting an id is how a duplicate becomes a fresh
  // delivery. An empty id isn't tracked.
  if (!delivery.id.empty()) {
    const auto delivered = core.findOrMintDimension("d.delivered");
    for (const CellRef idCell :
         zigzag::rankAfter(arena, delivery.agent, delivered))
      if (arena.textOf(idCell) == delivery.id) return;
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, delivery.agent, delivered),
                   delivered, DimVector::POS, arena.makeCell(delivery.id)));
  }

  if (delivery.kind == "tell" || delivery.kind == "untell" ||
      delivery.kind == "achieve") {
    // A cross-agent effect isn't a percept — it lands as an inbox
    // record the receiver's own settle processes (assert/retract for
    // tell/untell, subgoal adoption for achieve). Every cell
    // reference rides a scalar — sender-side fanout means N records
    // can name one program-image term, and -d.* holds a single
    // inbound edge per dim.
    const auto inbox  = core.findOrMintDimension("d.inbox");
    const auto record = arena.makeCell();
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, delivery.agent, inbox), inbox,
                   DimVector::POS, record));
    zigzag::expectWritten(arena.link(record, core.findOrMintDimension("d.verb"),
                                     DimVector::POS,
                                     arena.makeCell(delivery.kind)));
    const auto refField = [&](std::string_view name, CellRef ref) {
      if (ref == noCell) return;
      if (const auto rec = varsRecord(arena, core, record, name, true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeScalarCell(static_cast<std::int64_t>(ref))));
    };
    refField("grab", delivery.payloadCell);
    refField("sender", delivery.sender);
    refField("delegator", delivery.delegator);
    return;
  }

  if (delivery.percept.empty()) return;
  // The message path's two effects, split for adapter staging: the
  // journal entry is what a parked await/ask matches against, the
  // pending entry is what the receiver's settle dispatches. Pending
  // stays journal-free for internal events — delivery journals
  // explicitly instead.
  const auto seen = arena.makeCell(delivery.percept);
  if (!delivery.correlation.empty())
    if (const auto rec = varsRecord(arena, core, seen, "correlation", true))
      zigzag::expectWritten(arena.link(*rec, core.dims().values, DimVector::POS,
                                       arena.makeCell(delivery.correlation)));
  // The term envelope rides as a scalar ref — the durable record of
  // what the percept carried. AwaitTerm is belief-form (the checker's
  // term() requires a predicate), so nothing consumes it today; the
  // schema is what a future field-matching event await reads.
  if (delivery.payloadCell != noCell)
    if (const auto rec = varsRecord(arena, core, seen, "grab", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeScalarCell(
                         static_cast<std::int64_t>(delivery.payloadCell))));
  journalAppend(arena, delivery.agent, seen, core.findOrMintDimension("d.seen"),
                static_cast<std::size_t>(setting("vagent.journal_size", 64)));
  const auto pending = core.findOrMintDimension("d.pending");
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, delivery.agent, pending), pending,
                 DimVector::POS, arena.makeCell(delivery.percept)));
}

} // namespace xanadu::vagent
