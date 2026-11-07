#include "executor.hpp"

#include "../scalar.hpp"
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
  DimRef skill{noCell};
  DimRef capacity{noCell};
  DimRef behavior{noCell};
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

/// The member's durable `pid` record — the rendered mesh pid text the
/// adapter's noteMemberPid wrote. Empty when the member never took a
/// wire identity (a shared-image child or an unnamed reference), which
/// is exactly what makes it a local-delivery target.
[[nodiscard]] std::optional<std::string>
memberPidKey(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
             CellRef member) {
  const auto rec = varsRecord(arena, core, member, "pid", false);
  if (!rec) return std::nullopt;
  const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
  if (!v) return std::nullopt;
  return std::string{arena.textOf(*v)};
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

/// The +d.step/+d.name field envelope the raise path mints —
/// `occurrence.<name>` on the handler reads these children back.
void perceptField(zigzag::ArenaManifold &arena,
                  zigzag::vortex::VortexCore &core, CellRef payload,
                  std::string_view name, CellRef value) {
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, payload, core.dims().step),
                 core.dims().step, DimVector::POS, value));
  zigzag::expectWritten(arena.link(value, core.dims().name,
                                 DimVector::POS, arena.makeCell(name)));
}

/// The journal+pending tail every adapter-side raise shares: the seen
/// cell is the dispatchable percept (its `grab` points at the payload
/// whose fields handlers read), the pending record is what the next
/// settle's drain turns into a dispatch.
void queueAdapterPercept(zigzag::ArenaManifold &arena,
                         zigzag::vortex::VortexCore &core, CellRef agent,
                         std::string_view name, CellRef payload,
                         std::size_t journalSize) {
  const auto seen = arena.makeCell(name);
  journalAppend(arena, agent, seen, core.findOrMintDimension("d.seen"),
                journalSize);
  if (const auto rec = varsRecord(arena, core, seen, "grab", true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, DimVector::POS,
                   refCell(arena, core, payload)));
  const auto pending = core.findOrMintDimension("d.pending");
  const auto pend    = arena.makeCell(name);
  if (const auto rec = varsRecord(arena, core, pend, "grab", true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, DimVector::POS,
                   refCell(arena, core, seen)));
  zigzag::expectWritten(arena.link(
      zigzag::rankTail(arena, agent, pending), pending, DimVector::POS, pend));
}

/// One-shot key on the agent's +d.delivered rank — the dedupe the
/// adapter's fault raises need so a redelivery (or the ActorDown
/// trailing an orderly stop) cannot re-raise.
[[nodiscard]] bool adapterOnce(zigzag::ArenaManifold &arena,
                               zigzag::vortex::VortexCore &core,
                               CellRef agent, std::string_view key) {
  const auto delivered = core.findOrMintDimension("d.delivered");
  for (const CellRef idCell : zigzag::rankAfter(arena, agent, delivered))
    if (arena.textOf(idCell) == key) return false;
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, agent, delivered), delivered,
                 DimVector::POS, arena.makeCell(key)));
  return true;
}

/// The int field the turn's `intentionField` reads — duplicated at
/// file scope because the adapter-side raises run outside a turn and
/// have no MetaRuntime bound.
[[nodiscard]] std::int64_t intentionFlag(zigzag::ArenaManifold &arena,
                                         zigzag::vortex::VortexCore &core,
                                         CellRef intention,
                                         std::string_view field) {
  const auto rec = varsRecord(arena, core, intention, field, false);
  if (!rec) return 0;
  const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
  return v ? arena.asInt64(*v).value_or(0) : 0;
}

/// §"child loss": an intention waiting on a member fails when that
/// member goes away — the settle's awaitFault rung turns the mark
/// into the same else/fault ladder a deadline runs. Every raise that
/// announces a member's end (ChildDown, ChildStopping) walks the
/// parked intentions naming it. awaitPeer compares member `+d.name`
/// text, so the mark survives a Moved's identity swap; only a parked
/// intention (waiting or delegated) is a wait on the member — a
/// stale awaitPeer on a running intention must not fire.
void markPeerWaits(zigzag::ArenaManifold &arena,
                   zigzag::vortex::VortexCore &core, CellRef agent,
                   CellRef member, std::string_view cause) {
  std::string memberName;
  if (const auto n = arena.linked(member, core.dims().name,
                                  DimVector::POS))
    memberName = std::string{arena.textOf(*n)};
  if (memberName.empty()) return;
  for (const CellRef intention : zigzag::rankAfter(
           arena, agent, core.findOrMintDimension("d.intention"))) {
    if (intentionFlag(arena, core, intention, "waiting") == 0 &&
        intentionFlag(arena, core, intention, "delegated") == 0)
      continue;
    const auto peer = varsRecord(arena, core, intention, "awaitPeer", false);
    if (!peer) continue;
    const auto v = arena.linked(*peer, core.dims().values, DimVector::POS);
    if (!v || arena.textOf(*v) != memberName) continue;
    if (const auto rec =
            varsRecord(arena, core, intention, "awaitFault", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeCell(cause)));
  }
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
  dims.skill     = core.findOrMintDimension("d.skill");
  dims.capacity  = core.findOrMintDimension("d.capacity");
  dims.behavior  = core.findOrMintDimension("d.behavior");

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
                               CellRef delegator      = noCell,
                               std::string_view correlation = {},
                               std::string_view percept     = {},
                               std::string_view fault       = {},
                               bool preempts = false,
                               std::string_view capacity = {},
                               CellRef targetMember        = noCell)
        -> std::string {
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
               refCell(arena, core, payload));
    outField("target", arena.makeCell(std::string{target}));
    if (!scope.empty()) outField("scope", arena.makeCell(std::string{scope}));
    // The ask-reply fields route reads: `percept` overrides the
    // payload cell's own text because the reply's payload is a bare
    // answer value whose text is the value, not the question name;
    // `correlation` echoes the request id the asker parked on.
    if (!correlation.empty())
      outField("correlation", arena.makeCell(std::string{correlation}));
    if (!percept.empty())
      outField("percept", arena.makeCell(std::string{percept}));
    if (!fault.empty())
      outField("fault", arena.makeCell(std::string{fault}));
    // V6's `effect preempts` — the entry carries the signature's
    // modifier so the binding's start-at-once rule survives the drain.
    if (preempts) outField("preempts", arena.makeScalarCell(std::int64_t{1}));
    // An effect's provider name — the host binding's dispatch key;
    // the payload term names only the method.
    if (!capacity.empty())
      outField("capacity", arena.makeCell(std::string{capacity}));
    if (delegator != noCell)
      outField("delegator",
               refCell(arena, core, delegator));
    // Member-addressed entries bypass the drain's name resolution —
    // a remote peer's name is not reliably known, its pid is.
    if (targetMember != noCell)
      outField("targetMember",
               refCell(arena, core, targetMember));
    const auto id = std::format("{}:{}:{}", std::hash<std::string>{}(sender),
                                turn, position);
    outField("id", arena.makeCell(id));
    // An achieve's own id is its durable correlation token — the
    // remote delegate's refuse/outcome replies echo it through
    // `__correlation`, and the parked intention's `correlation`
    // record matches them back. The wire requestId gets a transport
    // leg suffix the token deliberately doesn't carry.
    if (kind == "achieve" && correlation.empty())
      outField("correlation", arena.makeCell(id));
    return id;
  };

  // A record field's text — the string channel delegatorCorrelation,
  // correlation and awaitPeer ride on.
  const auto recordText = [&](CellRef holder, std::string_view name)
      -> std::string {
    const auto rec = varsRecord(arena, core, holder, name, false);
    if (!rec) return {};
    const auto v = arena.linked(*rec, core.dims().values, DimVector::POS);
    return v ? std::string{arena.textOf(*v)} : std::string{};
  };
  // A remote member is the delegator target a wire reply reaches —
  // distinguished from a local intention by its `pid` record
  // (genesis notes remote members; intentions never carry one).
  const auto remoteMemberRef = [&](CellRef cell) {
    return cell != noCell &&
           varsRecord(arena, core, cell, "pid", false).has_value();
  };
  // The +d.step/+d.name field envelope's read half — the wire
  // payload's `status`/`cause` children (perceptField's write shape).
  const auto fieldText = [&](CellRef holder, std::string_view name)
      -> std::string {
    if (holder == noCell) return {};
    for (const CellRef child :
         zigzag::rankAfter(arena, holder, core.dims().step))
      if (const auto n =
              arena.linked(child, core.dims().name, DimVector::POS))
        if (arena.textOf(*n) == name)
          return std::string{arena.textOf(child)};
    return {};
  };
  // The parked wait a remote reply answers: a `by peer` achieve
  // records its entry id as `correlation` and the adopted child
  // records it as `delegatorCorrelation`; refuse/outcome/cancel echo
  // the same token through `__correlation`.
  const auto intentionByCorrelation = [&](std::string_view field,
                                          std::string_view correlation)
      -> CellRef {
    if (correlation.empty()) return noCell;
    for (const CellRef i :
         zigzag::rankAfter(arena, agent, dims.intention))
      if (recordText(i, field) == correlation)
        return i;
    return noCell;
  };
  // A directory member by name — the cancel/outcome target for a
  // `by peer` delegation whose awaitPeer holds the peer's name.
  const auto memberNamedIn = [&](std::string_view name) -> CellRef {
    if (name.empty()) return noCell;
    for (const CellRef dir : zigzag::rankAfter(
             arena, core.home(), core.findOrMintDimension("d.dirs")))
      for (const CellRef m : zigzag::rankAfter(
               arena, dir, core.findOrMintDimension("d.agents")))
        if (const auto n = arena.linked(m, core.dims().name,
                                        DimVector::POS))
          if (arena.textOf(*n) == name) return m;
    return noCell;
  };
  // §955's {correlation,status,cause}: an adopted child's end stages
  // the reply the remote delegating achieve waits on. Local
  // delegators keep the intention-field write-back.
  const auto stageOutcome = [&](CellRef intention, std::string_view status,
                                std::string_view cause = {}) {
    const auto delegator = static_cast<CellRef>(
        meta.intentionField(intention, "delegator").value_or(0));
    if (!remoteMemberRef(delegator)) return;
    const auto payload = arena.makeCell("outcome");
    perceptField(arena, core, payload, "status",
                 arena.makeCell(std::string{status}));
    if (!cause.empty())
      perceptField(arena, core, payload, "cause",
                   arena.makeCell(std::string{cause}));
    std::string targetName;
    if (const auto n =
            arena.linked(delegator, core.dims().name, DimVector::POS))
      targetName = std::string{arena.textOf(*n)};
    stageOutbox("outcome", payload, targetName, {}, noCell,
                recordText(intention, "delegatorCorrelation"), "outcome",
                {}, false, {}, delegator);
  };
  // §954: a remote delegated wait that ends early owes the delegate a
  // cancel — best-effort and idempotent, keyed by the achieve's
  // correlation token. awaitPeer's name resolves to the member; an
  // unresolvable peer means there is nothing live to cancel.
  const auto stageCancel = [&](CellRef intention) {
    const auto correlation = recordText(intention, "correlation");
    const auto peer        = memberNamedIn(recordText(intention, "awaitPeer"));
    if (correlation.empty() || peer == noCell) return;
    stageOutbox("cancel", arena.makeCell("cancel"),
                recordText(intention, "awaitPeer"), {}, noCell, correlation,
                "cancel", {}, false, {}, peer);
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

  // The member's $parent record → the member cell that spawned it —
  // §7.1's accepts default and §4.5's parent-only stop both ask the
  // lineage question; the top-level agent's record is absent.
  const auto parentOf = [&](CellRef member) -> CellRef {
    if (const auto rec = varsRecord(arena, core, member, "$parent", false))
      if (const auto v = arena.linked(*rec, core.dims().values,
                                      DimVector::POS))
        if (const auto r = arena.asInt64(*v))
          return static_cast<CellRef>(*r);
    return noCell;
  };

  // `parent` resolves to the $parent member — the durable handle the
  // reply idiom needs: the genesis-minted record carries a pid but no
  // name, so a name-routed entry could never reach it. `self` and
  // bound vars keep the name route — `for <var>` fanning out to every
  // same-named member is the semantic the MissingTransport test pins.
  const auto resolvePeerMember = [&](CellRef nameCell) -> CellRef {
    if (nameCell == noCell || arena.textOf(nameCell) != "parent")
      return noCell;
    return parentOf(agent);
  };

  // A marshaled term's +d.step children → positional CellValues for
  // runFragment: scalar cells keep their literal type, a structured
  // child (one carrying +d.step of its own) passes as its ref — the
  // CellRef-as-int64 convention a fragment reads off the param's
  // declared type. Bare text children are strings.
  const auto argValuesOf = [&](CellRef term,
                               std::vector<CellRef> *refs = nullptr)
      -> std::vector<CellValue> {
    std::vector<CellValue> out;
    if (refs) refs->clear();
    if (term == noCell) return out;
    const auto reloc = arena.dimensionNamed("d.mesh.relocation");
    for (const CellRef arg :
         zigzag::rankAfter(arena, term, core.dims().step)) {
      if (const auto n = arena.asInt64(arg)) {
        out.emplace_back(*n);
        // A marked Int64's own value is the referent — the stored
        // args chain re-describes its refs this way.
        if (refs)
          refs->push_back(reloc && arena.linked(arg, *reloc,
                                                DimVector::POS)
                              ? static_cast<CellRef>(*n)
                              : noCell);
        continue;
      }
      if (const auto d = arena.asDouble(arg)) {
        out.emplace_back(*d);
        if (refs) refs->push_back(noCell);
        continue;
      }
      if (const auto b = arena.asBool(arg)) {
        out.emplace_back(*b);
        if (refs) refs->push_back(noCell);
        continue;
      }
      if (!zigzag::rankAfter(arena, arg, core.dims().step).empty()) {
        out.emplace_back(static_cast<std::int64_t>(arg));
        if (refs) refs->push_back(arg);
        continue;
      }
      out.emplace_back(std::string{arena.textOf(arg)});
      if (refs) refs->push_back(noCell);
    }
    return out;
  };
  // The intention's stored `args` chain back to CellValues — the same
  // positional contract adoptGoal wrote.
  const auto intentionArgs = [&](CellRef i,
                                 std::vector<CellRef> *refs = nullptr)
      -> std::vector<CellValue> {
    const auto rec = varsRecord(arena, core, i, "args", false);
    if (!rec) return {};
    const auto chain = arena.linked(*rec, core.dims().values, DimVector::POS);
    return chain ? argValuesOf(*chain, refs) : std::vector<CellValue>{};
  };
  // §5.8's trailing `$owner` slot: only *member* heads (the adopted
  // clause's guard/body/spine fragments, its `else` recovery) declare
  // it — the goal's own condition heads do not — so the owner rides
  // the intention's `owner` field and call sites append it where a
  // member head runs, never blanket into intentionArgs.
  const auto withOwner = [&](std::vector<CellValue> args,
                             CellRef i) -> std::vector<CellValue> {
    if (const auto owner = meta.intentionField(i, "owner");
        owner && *owner != 0)
      args.push_back(CellValue{static_cast<std::int64_t>(*owner)});
    return args;
  };

  // Store a var record's value: a declared `var`'s +d.values names one
  // stable "var.slot" holder — write through it so the cell fragment
  // bindings resolved stays the store (a repoint would orphan it).
  // Unmarked records keep the mint-and-repoint idiom — content writes
  // don't restate under the serial promote, fresh scalars do.
  const auto storeVar = [&](CellRef rec, const CellValue &val) {
    if (const auto holder =
            arena.linked(rec, core.dims().values, DimVector::POS)) {
      const auto mark =
          arena.linked(*holder, core.dims().name, DimVector::POS);
      if (mark && arena.textOf(*mark) == "var.slot") {
        core.writeValue(*holder, 0, std::nullopt, val);
        // A slot that once held a cell ref keeps the relocation mark
        // until cleared — a stale manifest would harvest the plain
        // integer as a position and corrupt it at the next fold. No
        // dim means no mark can exist — dimensionNamed, not mint.
        if (const auto reloc = arena.dimensionNamed("d.mesh.relocation"))
          if (const auto stale =
                  arena.linked(*holder, *reloc, DimVector::POS))
            zigzag::expectWritten(
                arena.unlink(*holder, *reloc, DimVector::POS));
        return;
      }
    }
    const auto value = std::visit(
        [&](const auto &v) -> CellRef {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, std::string>)
            return arena.makeCell(v);
          else
            return arena.makeScalarCell(v);
        },
        val);
    zigzag::expectWritten(
        arena.link(rec, core.dims().values, DimVector::POS, value));
  };

  // The ref-valued storeVar: `child := spawn A()` / `task := adopt G()`
  // bind a live cell, and the Int64 that carries its position must ride
  // the relocation manifest — verbatim through promote it dead-reads on
  // the rebuilt arena. On a var.slot the mark goes on the holder beside
  // the typed value writeValue writes; a plain record takes a fresh
  // refCell, the same shape outbox refs mint.
  const auto storeVarRef = [&](CellRef rec, CellRef referent) {
    // A bare `none` arrives as position 0 — noCell is never a live
    // referent, and marking it would harvest the Opt's empty state as
    // a pointer. Route it through the scalar store, which also clears
    // any mark a previous ref left on the slot.
    if (referent == noCell) {
      storeVar(rec, CellValue{std::int64_t{0}});
      return;
    }
    if (const auto holder =
            arena.linked(rec, core.dims().values, DimVector::POS)) {
      const auto mark =
          arena.linked(*holder, core.dims().name, DimVector::POS);
      if (mark && arena.textOf(*mark) == "var.slot") {
        core.writeValue(*holder, 0, std::nullopt,
                        CellValue{static_cast<std::int64_t>(referent)});
        const auto reloc = core.findOrMintDimension("d.mesh.relocation");
        if (!arena.linked(*holder, reloc, DimVector::POS))
          zigzag::expectWritten(arena.link(*holder, reloc, DimVector::POS,
                                         arena.makeCell("cell")));
        return;
      }
    }
    zigzag::expectWritten(
        arena.link(rec, core.dims().values, DimVector::POS,
                   refCell(arena, core, referent)));
  };

  // A produced Int64 is a position when the fragment's declared result
  // type carries refs — Cell/Intention/Agent handles, containers and
  // user terms all mint cells. Opt<X> inherits X's answer (`none` is
  // the bare 0 storeVarRef routes to the scalar path); the plain
  // scalars never mint. The CellValue alone cannot say which side an
  // Int64 sits on — `some(5)` and a ref are the same bits — so the
  // descriptor's `+d.name` spelling is the only honest channel.
  const auto refTypedResult = [&](CellRef head, const CellValue &val) {
    if (!std::holds_alternative<std::int64_t>(val)) return false;
    const auto type = meta.resultTypeOf(head);
    if (!type) return false;
    std::string_view inner{*type};
    // `<any>` is the lowering's stamp for a thunk whose real type the
    // checker did not thread through — unclassifiable Int64s take the
    // scalar path, the same default as a descriptor-less head.
    if (inner.empty() || inner == "<any>") return false;
    if (inner.starts_with("Opt<") && inner.ends_with('>'))
      inner = inner.substr(4, inner.size() - 5);
    return inner != "Int" && inner != "Float" && inner != "Str" &&
           inner != "Bool" && inner != "()" && inner != "Duration" &&
           inner != "Time";
  };

  // §9.2's lifecycle events: a raise journals the percept — it is
  // real §3.1 journal traffic — and enqueues a +d.pending internal
  // event, so `on GoalFailed` dispatches through the ordinary drain
  // rather than inside the settle walk that ended the intention. A
  // settle-phase raise waits for the next turn's drain; one raised
  // during percept dispatch drains right after the dispatch, still
  // clear of the intention walk.
  // Fields marshal as +d.step children labelled +d.name —
  // the event-payload shape emit and `term.field` already read.
  const auto raiseOn =
      [&](CellRef target, std::string_view name,
          std::initializer_list<std::pair<std::string_view, CellRef>>
              fields) {
        const auto payload = arena.makeCell(name);
        for (const auto &[key, value] : fields) {
          zigzag::expectWritten(
              arena.link(zigzag::rankTail(arena, payload,
                                          core.dims().step),
                         core.dims().step, DimVector::POS, value));
          zigzag::expectWritten(arena.link(value, core.dims().name,
                                         DimVector::POS,
                                         arena.makeCell(key)));
        }
        const auto seen = arena.makeCell(name);
        journalAppend(arena, target, seen, dims.seen,
                      static_cast<std::size_t>(
                          setting("vagent.journal_size", 64)));
        if (const auto rec = varsRecord(arena, core, seen, "grab", true))
          zigzag::expectWritten(arena.link(
              *rec, core.dims().values, DimVector::POS,
              refCell(arena, core, payload)));
        const auto pend = arena.makeCell(name);
        if (const auto rec = varsRecord(arena, core, pend, "grab", true))
          zigzag::expectWritten(arena.link(
              *rec, core.dims().values, DimVector::POS,
              refCell(arena, core, seen)));
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, target, dims.pending),
                       dims.pending, DimVector::POS, pend));
      };
  const auto raise =
      [&](std::string_view name,
          std::initializer_list<std::pair<std::string_view, CellRef>>
              fields) { raiseOn(agent, name, fields); };

  const auto goalNameOf = [&](CellRef i) {
    const auto goal = meta.intentionField(i, "goal").value_or(0);
    return goal == 0 ? std::string{}
                     : std::string{arena.textOf(
                           static_cast<CellRef>(goal))};
  };

  // The fault arm of §3.5's ladder: count the fault and park for the
  // backoff window; the walker's faulted block owns retry and the
  // drop-when reconsideration at the cap. ready=1 because the ladder
  // only examines ready intentions — a suspension-point fault
  // (await/delegation deadline) would otherwise park forever. Every
  // fault is the running plan's failure, so PlanFailed raises here.
  const auto faultIntention = [&](CellRef i, std::string_view cause) {
    const auto planCell = meta.intentionField(i, "plan").value_or(0);
    raise("PlanFailed",
          {{"intention", refCell(arena, core, i)},
           {"plan",
            arena.makeCell(planCell == 0
                               ? std::string_view{}
                               : arena.textOf(
                                     static_cast<CellRef>(planCell)))},
           {"cause", arena.makeCell(cause)}});
    const auto faults = meta.intentionField(i, "faults").value_or(0);
    meta.setIntentionField(i, "faults", faults + 1);
    meta.setIntentionField(i, "faulted", 1);
    meta.setIntentionField(i, "ready", 1);
    meta.setIntentionField(i, "blocked_until",
                           turn + setting("vagent.intention_backoff", 2));
  };

  // `consume`'s scratch flag: a handler-side host step sets it and
  // dispatchEvent's clause loop breaks after the current handler —
  // the event is spent for *later* handlers only. Turn-scoped, never
  // written to the record; dispatch resets it on entry so a consume
  // inside a plan body can't leak into the next event.
  bool eventConsumed = false;
  // runSpine sits behind std::function so a host step (install runs
  // `on install`, which is itself a clause spine) can reach it —
  // lambdas can't forward-reference, and the indirection also lets a
  // lifecycle body's own host verbs recurse through runHostStep.
  std::function<bool(CellRef, const std::vector<CellValue> &)> runSpine;
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
    // The peer a remote park is bound to — `ask`/`achieve for` write
    // it so a Down on that member can mark the wait; a peer-less park
    // clears it or a stale name outlives the wait it belonged to.
    const auto awaitPeer = [&](std::string_view peerName) {
      const auto rec =
          varsRecord(arena, core, intention, "awaitPeer", true);
      if (!rec) return;
      if (const auto v =
              arena.linked(*rec, core.dims().values, DimVector::POS))
        zigzag::expectWritten(
            arena.unlink(*rec, core.dims().values, DimVector::POS));
      if (peerName.empty()) return;
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeCell(peerName)));
    };
    if (verb == "consume") {
      eventConsumed = true;
      return true;
    }
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
      std::vector<CellRef> argRefs;
      auto child = meta.adoptGoal(
          *goal, turn,
          static_cast<std::size_t>(setting("vagent.condition_turn_fuel", 256)),
          argValuesOf(goalName, &argRefs), argRefs);
      if (!child) return false;
      // The hoist's result binding: `task := adopt G()` lands the
      // child's record ref on the agent's vars under the bound name —
      // the Intention-typed handle drop reads back.
      if (const auto bound = field("result"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, agent, arena.textOf(bound), true))
          storeVarRef(*rec, *child);
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
      // A resolved member rides targetMember — the drain's
      // member-addressed arm reaches a remote peer by pid whether or
      // not the record carries a name ($parent never does) — while a
      // plain name keeps the directory's name route.
      const auto member = resolvePeerMember(target);
      stageOutbox("emit", payload,
                  target != noCell ? resolvePeer(target) : std::string{"0"},
                  scope != noCell ? arena.textOf(scope) : std::string_view{},
                  noCell, {}, {}, {}, false, {}, member);
      return true;
    }
    if (verb == "tell" || verb == "untell") {
      // Same staging as emit: the peer name rides target, the fact
      // rides payload — the receiver-side assert is the adapter's
      // delivery work, not the turn's.
      const auto peer    = field("peer");
      const auto payload = field("payload");
      if (peer == noCell || payload == noCell) return false;
      stageOutbox(verb, payload, resolvePeer(peer),
                  std::string_view{}, noCell, {}, {}, {}, false, {},
                  resolvePeerMember(peer));
      return true;
    }
    if (verb == "stop") {
      // §4.5's orderly stop runs `on Destroy` before the stop lands:
      // `destroying` parks every later phase but the pending drain,
      // where the Destroy dispatch flips the record to `stopped`.
      // `self` resolves to the running agent — the current spine
      // still finishes (statement-boundary semantics).
      const auto peer = field("peer");
      if (peer == noCell) return false;
      const auto want = arena.textOf(peer);
      CellRef target  = noCell;
      if (want == "self") {
        target = agent;
      } else {
        // A bound member var names its cell directly — resolvePeer
        // runs the same var-then-name order for delivery targets.
        if (const auto rec = varsRecord(arena, core, agent, want, false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            if (const auto ref = arena.asInt64(*v))
              target = static_cast<CellRef>(*ref);
        if (target == noCell)
          for (const CellRef dir :
               zigzag::rankAfter(arena, programRoot, dims.dirs))
            for (const CellRef peer_ :
                 zigzag::rankAfter(arena, dir, dims.agents))
              if (const auto name =
                      arena.linked(peer_, core.dims().name, DimVector::POS);
                  name && arena.textOf(*name) == want)
                target = peer_;
      }
      if (target == noCell) return false;
      const auto flagged = [&](std::string_view which) {
        if (const auto rec = varsRecord(arena, core, target, which, false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            if (const auto flag = arena.asInt64(*v)) return *flag != 0;
        return false;
      };
      // §7.1's stop performative is "accepted from the parent only" —
      // self, and the members this agent spawned. A stop aimed
      // anywhere else is refused, not silently dropped.
      if (target != agent && parentOf(target) != agent) return false;
      // A member with a wire identity is a remote peer: its
      // destroying flag and Destroy queue belong to its own agent,
      // which the member-addressed send asks to run §4.5 — the
      // child's ChildStopping reports back over the same arm rather
      // than being raised here.
      if (target != agent)
        if (const auto peerKey = memberPidKey(arena, core, target);
            peerKey && peerKey != memberPidKey(arena, core, agent)) {
          stageOutbox("stop", noCell, {}, {}, noCell, {}, "stop", {},
                      false, {}, target);
          return true;
        }
      if (flagged("stopped") || flagged("destroying")) return true;
      if (const auto rec =
              varsRecord(arena, core, target, "destroying", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeScalarCell(std::int64_t{1})));
      raiseOn(target, "Destroy", {});
      // §4.5's announcement is at once — the parent learns the child
      // is stopping when the stop lands, not when the child's own
      // pump finishes `on Destroy`. The dedupe key is shared with
      // the adapter-side report so a terminate/lease/init already
      // observed suppresses this raise instead of doubling it.
      // `stop` is the only orderly path the executor has —
      // terminate, lease and init failures stage theirs adapter-side.
      if (const auto parent = parentOf(target); parent != noCell) {
        if (adapterOnce(arena, core, parent,
                        std::format("childstopping:{}", target)))
          raiseOn(parent, "ChildStopping",
                  {{"child",
                    refCell(arena, core, target)},
                   {"cause", arena.makeCell("stop")}});
        // "The parent reports one ChildStopping even when the
        // runtime's ActorDown follows it" — pre-marking the member's
        // down key makes that a fact of the delivered journal rather
        // than a race the adapter has to lose.
        (void)adapterOnce(arena, core, parent,
                          std::format("childdown:{}", target));
        // A self-stop fails the parent's waits on this member by
        // name; a child-stop fails this agent's own waits on it.
        markPeerWaits(arena, core, parent, target, "stop");
      }
      markPeerWaits(arena, core, agent, target, "stop");
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
      if (remoteMemberRef(static_cast<CellRef>(delegator))) {
        stageOutcome(ref, "dropped");
      } else if (delegator != 0) {
        meta.setIntentionField(static_cast<CellRef>(delegator), "delegated", 0);
        faultIntention(static_cast<CellRef>(delegator), "dropped");
      }
      stageCancel(ref);
      raise("GoalDropped",
            {{"intention", refCell(arena, core, ref)},
             {"goal", arena.makeCell(goalNameOf(ref))}});
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
      // The lineage record §7.1's accepts default and §4.5's
      // parent-only stop read — a scalar ref like every cross-record
      // handle, since -d.* pairs one inbound edge per dim.
      if (const auto rec = varsRecord(arena, core, child, "$parent", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       refCell(arena, core, agent)));
      // The wire half the adapter owes: the member exists, so the
      // entry's payload is the member cell itself — the drain turns
      // it into noteSpawnRequest + a spawn send, with the entry id
      // as the request's durable correlation.
      stageOutbox("spawn", child, {});
      if (const auto bound = field("result"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, agent, arena.textOf(bound), true))
          storeVarRef(*rec, child);
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
      const auto peerName  = resolvePeer(peer);
      const auto requestId =
          stageOutbox("ask", payload, peerName, {}, noCell, {}, {}, {},
                      false, {}, resolvePeerMember(peer));
      awaitPeer(peerName);
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
              refCell(arena, core, event)));
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
      // The `[guard]` head survives the park as a scalar ref — the
      // wake rung evaluates it per candidate percept before the
      // evidence link commits the match.
      if (const auto guard = field("guard"); guard != noCell)
        if (const auto g = arena.asInt64(guard))
          if (const auto rec =
                  varsRecord(arena, core, intention, "awaitGuard", true))
            zigzag::expectWritten(
                arena.link(*rec, core.dims().values, DimVector::POS,
                           arena.makeScalarCell(*g)));
      parkElse();
      awaitPeer({});
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
        const auto peerName = resolvePeer(peer);
        const auto requestId =
            stageOutbox("achieve", goalName, peerName, {}, intention,
                        {}, {}, {}, false, {}, resolvePeerMember(peer));
        // The durable correlation a remote refuse/outcome/cancel
        // echoes — the settle's delegated rung matches it back to
        // this park. The entry id self-correlates (stageOutbox), so
        // the same token lives on both sides of the wire.
        if (const auto rec =
                varsRecord(arena, core, intention, "correlation", true))
          zigzag::expectWritten(
              arena.link(*rec, core.dims().values, DimVector::POS,
                         arena.makeCell(requestId)));
        meta.setIntentionField(intention, "delegated", 1);
        meta.setIntentionField(intention, "ready", 0);
        const auto dl      = field("deadline");
        const auto dlTurns = dl != noCell ? arena.asInt64(dl) : std::nullopt;
        if (!dlTurns) return false; // `by` without `within` doesn't parse
        parkElse();
        awaitPeer(peerName);
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
      std::vector<CellRef> argRefs;
      auto child = meta.adoptGoal(
          *goal, turn,
          static_cast<std::size_t>(setting("vagent.condition_turn_fuel", 256)),
          argValuesOf(goalName, &argRefs), argRefs);
      if (!child) return false;
      zigzag::expectWritten(
          arena.link(intention, dims.subgoal, DimVector::POS, *child));
      meta.setIntentionRef(*child, "delegator", intention);
      meta.setIntentionField(intention, "delegated", 1);
      meta.setIntentionField(intention, "ready", 0);
      // within bounds the delegation in turns — the same scalar clock
      // await's deadline rides on; the settle's delegated-deadline
      // rung faults the parent and drops the unfinished child. A
      // deadline-free re-delegation clears the field so the previous
      // bound can't fire early on the new child.
      parkElse();
      // The local delegation's child is an intention, not a member —
      // a stale awaitPeer from an earlier remote park must not point
      // a Down at this wait.
      awaitPeer({});
      if (const auto dl = field("deadline"); dl != noCell)
        if (const auto turns = arena.asInt64(dl)) {
          meta.setIntentionField(intention, "awaitDeadline", turn + *turns);
          return true;
        }
      meta.setIntentionField(intention, "awaitDeadline", 0);
      return true;
    }
    if (verb == "believe" || verb == "forget" || verb == "revise") {
      // The belief family shares the `term` field — gating here, not
      // before the verb ladder, keeps a term-free verb (install,
      // capcall, …) from being refused on a field it never claimed.
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
      // The language's revise takes one term: the predicate's bank
      // becomes exactly that fact — clear then assert.
      for (const CellRef clause : meta.beliefsOf(pred))
        unlinkRank(arena, clause, core.dims().clause);
      return meta.believe(pred, term, agent, turn).has_value();
    }
    if (verb == "effectcall") {
      // V6: an effect never runs inside the turn — the staged entry is
      // what the host binding performs after commit. The plan form
      // parks the intention on the request's correlation the way ask
      // does; the handler form stages and returns, any failure
      // arriving later as an EffectFailed percept.
      const auto cap      = field("capacity");
      const auto method   = field("method");
      const auto payload  = field("payload");
      const auto deadline = field("deadline");
      if (cap == noCell || method == noCell || payload == noCell ||
          deadline == noCell)
        return false;
      const auto turns = arena.asInt64(deadline);
      if (!turns) return false;
      // The honest provider gate: the capacity names an installed
      // skill's instance — the checker keeps effect methods
      // native-only, so a match is the binding's skill.
      bool provided = false;
      for (const CellRef inst :
           zigzag::rankAfter(arena, agent, dims.skill))
        if (const auto cn =
                arena.linked(inst, dims.capacity, DimVector::POS);
            cn && arena.textOf(*cn) == arena.textOf(cap)) {
          provided = true;
          break;
        }
      if (!provided) return false;
      const auto requestId =
          stageOutbox("effect", payload, {}, {}, noCell, {}, {}, {},
                      field("preempts") != noCell, arena.textOf(cap));
      if (intention == noCell) return true;
      // §6.6's suspension: EffectDone and EffectFailed share the
      // request's correlation — the awaitEffect marker lets the wake
      // rung complete on either name and route a failure's cause into
      // the else ladder. awaitCall keeps the marshaled call so an
      // elapsed deadline can stage cancel naming what to stop.
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitCorr", true))
        zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                       DimVector::POS,
                                       arena.makeCell(requestId)));
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitEvent", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeCell("EffectDone")));
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitCall", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       refCell(arena, core, payload)));
      if (const auto bound = field("result"); bound != noCell)
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitBind", true))
          zigzag::expectWritten(
              arena.link(*rec, core.dims().values, DimVector::POS,
                         arena.makeCell(arena.textOf(bound))));
      parkElse();
      // The wait binds a capacity instance, not a member — a stale
      // awaitPeer would point a Down at a wait the child never owned.
      awaitPeer({});
      meta.setIntentionField(intention, "awaitEffect", 1);
      meta.setIntentionField(intention, "awaitDeadline", turn + *turns);
      meta.setIntentionField(intention, "waiting", 1);
      meta.setIntentionField(intention, "ready", 0);
      return true;
    }
    if (verb == "install" || verb == "uninstall" || verb == "capcall" ||
        verb == "mount" || verb == "unmount") {
      // §5.3's frame+instance structure: `install` mints a clone
      // master and one instance per capacity the skill implements —
      // inherited ones count, so the descriptor's +d.capacity rank is
      // already closed — and a skill sharing any capacity goes whole
      // first. `uninstall` runs `on uninstall`, splices the instances
      // off both ranks, and orphans the frame. `capcall` walks the
      // agent's instances for the one whose +d.capacity name matches
      // the qualifier and runs its frame's qualified method. `mount`
      // mints a behavior instance onto +d.behavior and seeds its
      // fields the same way; `unmount` splices it off the rank —
      // §5.7's merge then skips its clauses for later triggers.
      const auto frameOf = [&](CellRef instance) -> CellRef {
        const auto master = arena.cloneMaster(instance, core.dims().clone);
        return master ? *master : noCell;
      };
      const auto skillRecord = [&](CellRef nameCell) -> CellRef {
        if (const auto sym = eventSymbolNamed(arena, core, programRoot,
                                              arena.textOf(nameCell)))
          if (const auto v =
                  arena.linked(*sym, core.dims().values, DimVector::POS))
            return *v;
        return noCell;
      };
      // A lifecycle clause runs like a handler body: the +d.step
      // spine when the body carried host verbs, the +d.spin head
      // otherwise. The owner ref rides the trailing `$owner` slot —
      // `on install { speed := … }` writes the frame's own field.
      const auto runLifecycle = [&](CellRef clause,
                                    std::vector<CellValue> args,
                                    CellRef owner) {
        args.push_back(CellValue{static_cast<std::int64_t>(owner)});
        if (!zigzag::rankAfter(arena, clause, core.dims().step).empty()) {
          (void)runSpine(clause, args);
          return;
        }
        if (const auto spin =
                arena.linked(clause, core.dims().spin, DimVector::POS))
          (void)meta.runFragment(*spin, args, exec.cursor, maxCycles);
      };
      const auto uninstallFrame = [&](CellRef frame) {
        // `on uninstall` replays the marshaled install args the frame
        // kept — lifecycle bodies take the skill's parameters.
        if (const auto rec = varsRecord(arena, core, frame, "skill", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            if (const auto ref = arena.asInt64(*v)) {
              const auto skillRec = static_cast<CellRef>(*ref);
              if (const auto h =
                      varsRecord(arena, core, skillRec, "uninstall", false))
                if (const auto clause =
                        arena.linked(*h, core.dims().values, DimVector::POS)) {
                  CellRef argCell = noCell;
                  if (const auto p =
                          varsRecord(arena, core, frame, "params", false))
                    if (const auto pv = arena.linked(*p, core.dims().values,
                                                     DimVector::POS))
                      argCell = *pv;
                  runLifecycle(*clause, argValuesOf(argCell), frame);
                }
            }
        for (const CellRef inst :
             zigzag::rankAfter(arena, frame, core.dims().clone) |
                 std::ranges::to<std::vector>()) {
          unlinkRank(arena, inst, dims.skill);
          if (arena.linked(inst, dims.capacity, DimVector::POS))
            zigzag::expectWritten(
                arena.unlink(inst, dims.capacity, DimVector::POS));
          unlinkRank(arena, inst, core.dims().clone);
        }
      };
      if (verb == "uninstall") {
        const auto name = field("name");
        if (name == noCell) return false;
        for (const CellRef inst : zigzag::rankAfter(arena, agent, dims.skill) |
                                      std::ranges::to<std::vector>()) {
          const auto frame = frameOf(inst);
          if (frame != noCell && arena.textOf(frame) == arena.textOf(name)) {
            uninstallFrame(frame);
            return true;
          }
        }
        // Removing a skill that isn't installed is an honest no.
        return false;
      }
      if (verb == "capcall") {
        const auto cap     = field("capacity");
        const auto method  = field("method");
        const auto payload = field("payload");
        if (cap == noCell || method == noCell || payload == noCell)
          return false;
        // §5.3: the call names the *capacity* — the agent's +d.skill
        // instances yield the one whose +d.capacity matches, and the
        // frame's skill name + method name pick the method head.
        for (const CellRef inst : zigzag::rankAfter(arena, agent, dims.skill)) {
          const auto cn = arena.linked(inst, dims.capacity, DimVector::POS);
          if (!cn || arena.textOf(*cn) != arena.textOf(cap)) continue;
          const auto frame = frameOf(inst);
          if (frame == noCell) return false;
          const auto sym = eventSymbolNamed(
              arena, core, programRoot,
              std::format("{}.{}", arena.textOf(frame), arena.textOf(method)));
          if (!sym) return false;
          const auto head =
              arena.linked(*sym, core.dims().values, DimVector::POS);
          if (!head) return false;
          // §5.8: the method head's trailing `$owner` binds the frame —
          // `speed` inside `Walker.goto` resolves on it, not the agent.
          auto callArgs = argValuesOf(payload);
          callArgs.push_back(
              CellValue{static_cast<std::int64_t>(frame)});
          const auto outcome = meta.runFragment(*head, callArgs,
                                                exec.cursor, maxCycles);
          if (outcome.refusal != MetaRunOutcome::Refusal::None ||
              !outcome.produced || !outcome.result)
            return false;
          // `x := Cap.m(...)` — the produced value lands on the
          // agent's vars under the bound name, the adopt convention.
          if (const auto bound = field("result"); bound != noCell)
            if (const auto rec =
                    varsRecord(arena, core, agent, arena.textOf(bound), true)) {
              if (refTypedResult(*head, *outcome.result))
                storeVarRef(*rec, static_cast<CellRef>(
                                      std::get<std::int64_t>(*outcome.result)));
              else
                storeVar(*rec, *outcome.result);
            }
          return true;
        }
        // Nothing installed provides the capacity — an honest refuse.
        return false;
      }
      // A descriptor's "fields" term: each slot's init head runs
      // against the mount/install arguments and lands on the owner's
      // vars — frames and behavior instances seed the same way.
      // The holder is the §5.8 shape: rec +d.values → holder, owner
      // +d.step → holder, holder +d.name → field name — so a member
      // body's #FIND/#SET hop reaches it through the owner ref, while
      // the vars record keeps the spec's inspectable layout. Init
      // heads take the trailing $owner too (a later field may read
      // an earlier one), so the args gain the owner ref.
      const auto seedFields = [&](CellRef owner, CellRef descriptor,
                                  std::vector<CellValue> args) {
        args.push_back(CellValue{static_cast<std::int64_t>(owner)});
        if (const auto rec =
                varsRecord(arena, core, descriptor, "fields", false))
          if (const auto list =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            for (const CellRef slot :
                 zigzag::rankAfter(arena, *list, core.dims().step)) {
              const std::string fieldName{arena.textOf(slot)};
              const auto frec =
                  varsRecord(arena, core, owner, fieldName, true);
              if (!frec) continue;
              const auto holder = arena.makeCell();
              zigzag::expectWritten(arena.link(
                  *frec, core.dims().values, DimVector::POS, holder));
              zigzag::expectWritten(arena.link(
                  zigzag::rankTail(arena, owner, core.dims().step),
                  core.dims().step, DimVector::POS, holder));
              zigzag::expectWritten(arena.link(
                  holder, core.dims().name, DimVector::POS,
                  arena.makeCell(fieldName)));
              const auto head =
                  arena.linked(slot, core.dims().values, DimVector::POS);
              if (!head) continue;
              const auto outcome =
                  meta.runFragment(*head, args, exec.cursor, maxCycles);
              // A refused or empty init leaves the slot standing —
              // the same honesty rule as an uninitialised agent var.
              if (outcome.refusal == MetaRunOutcome::Refusal::None &&
                  outcome.produced && outcome.result) {
                core.writeValue(holder, 0, std::nullopt, *outcome.result);
                // A ref-valued field carries the same mark a var.slot
                // does — the fresh holder starts unmarked, so this
                // only ever adds.
                if (refTypedResult(*head, *outcome.result)) {
                  const auto reloc =
                      core.findOrMintDimension("d.mesh.relocation");
                  zigzag::expectWritten(arena.link(
                      holder, reloc, DimVector::POS,
                      arena.makeCell("cell")));
                }
              }
            }
      };
      if (verb == "unmount") {
        const auto name = field("name");
        if (name == noCell) return false;
        for (const CellRef inst :
             zigzag::rankAfter(arena, agent, dims.behavior) |
                 std::ranges::to<std::vector>())
          if (arena.textOf(inst) == arena.textOf(name)) {
            unlinkRank(arena, inst, dims.behavior);
            return true;
          }
        // Removing a behavior that isn't mounted is an honest no.
        return false;
      }
      const auto payload = field("payload");
      if (payload == noCell) return false;
      if (verb == "mount") {
        const auto desc = skillRecord(payload);
        if (desc == noCell) return false;
        const auto inst = arena.makeCell(arena.textOf(payload));
        // The instance's "behavior" record points at its descriptor
        // — dispatch and unmount resolve it through that.
        if (const auto rec = varsRecord(arena, core, inst, "behavior", true))
          zigzag::expectWritten(
              arena.link(*rec, core.dims().values, DimVector::POS,
                         refCell(arena, core, desc)));
        seedFields(inst, desc, argValuesOf(payload));
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, agent, dims.behavior),
                       dims.behavior, DimVector::POS, inst));
        return true;
      }
      const auto skillRec = skillRecord(payload);
      if (skillRec == noCell) return false;
      const auto capNames = zigzag::rankAfter(arena, skillRec, dims.capacity) |
                            std::ranges::to<std::vector>();
      std::set<std::string> caps;
      for (const CellRef c : capNames) caps.emplace(arena.textOf(c));
      std::set<CellRef> evict;
      for (const CellRef inst : zigzag::rankAfter(arena, agent, dims.skill) |
                                    std::ranges::to<std::vector>()) {
        const auto frame = frameOf(inst);
        if (frame == noCell) continue;
        for (const CellRef peer :
             zigzag::rankAfter(arena, frame, core.dims().clone)) {
          const auto cn = arena.linked(peer, dims.capacity, DimVector::POS);
          if (cn && caps.contains(std::string{arena.textOf(*cn)})) {
            evict.insert(frame);
            break;
          }
        }
      }
      for (const CellRef f : evict) uninstallFrame(f);
      const auto frame = arena.makeCell(arena.textOf(payload));
      if (const auto rec = varsRecord(arena, core, frame, "skill", true))
        zigzag::expectWritten(arena.link(
            *rec, core.dims().values, DimVector::POS,
            refCell(arena, core, skillRec)));
      // The marshaled call term stays on the frame — `on uninstall`
      // replays it for its own parameter binding.
      if (const auto rec = varsRecord(arena, core, frame, "params", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS, payload));
      const auto args = argValuesOf(payload);
      seedFields(frame, skillRec, args);
      for (const CellRef cn : capNames) {
        const auto inst = arena.makeCell(arena.textOf(cn));
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, agent, dims.skill), dims.skill,
                       DimVector::POS, inst));
        zigzag::expectWritten(
            arena.link(zigzag::rankTail(arena, frame, core.dims().clone),
                       core.dims().clone, DimVector::POS, inst));
        zigzag::expectWritten(arena.link(inst, dims.capacity, DimVector::POS,
                                         arena.makeCell(arena.textOf(cn))));
      }
      if (const auto rec = varsRecord(arena, core, skillRec, "install", false))
        if (const auto clause =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          runLifecycle(*clause, args, frame);
      return true;
    }
    return false;
  };

  // A clause carrying +d.step members runs its §3.5 segment spine:
  // fragment members through runFragment, host-step cells through the
  // verb table — in order, inside m0. `args` are the host call's
  // bound values — empty for `on` dispatch, the intention's for plans.
  runSpine = [&](CellRef clause, const std::vector<CellValue> &args) -> bool {
    for (const CellRef member :
         zigzag::rankAfter(arena, clause, core.dims().step)) {
      if (arena.linked(member, dims.verb, DimVector::POS).has_value()) {
        if (!runHostStep(member, noCell)) return false;
      } else {
        const auto outcome =
            meta.runFragment(member, args, exec.cursor, maxCycles);
        if (outcome.refusal != MetaRunOutcome::Refusal::None) return false;
      }
    }
    return true;
  };

  // `seen` is the percept's journal record — its `grab` scalar ref
  // resolves to the delivered payload term. A name-only percept
  // supplies no payload; a param'd handler can't bind its declared
  // fields then, and its guard arity-refuses — which reads as a
  // failed `where`, not a turn fault.
  auto dispatchEvent = [&](std::string_view name, CellRef seen) -> bool {
    const auto eventSym = eventSymbolNamed(arena, core, programRoot, name);
    if (!eventSym) return true;
    eventConsumed = false;
    CellRef payload{noCell};
    if (seen != noCell)
      if (const auto rec = varsRecord(arena, core, seen, "grab", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto r = arena.asInt64(*v))
            payload = static_cast<CellRef>(*r);
    // A name-only percept is its own term: the seen cell's text is the
    // functor and its +d.step rank is empty — occurrence binds it,
    // field params still arity-refuse for want of children.
    if (payload == noCell) payload = seen;
    // A payload field's bound value is factValue's: typed bits first,
    // then the text-channel coercion marshalTerm's convention needs.
    const auto boundValue = [&](CellRef c) { return meta.factValue(c); };
    // Name-keyed binding: emit labels each payload field +d.name, and
    // the clause descriptor's param chain carries the declared order —
    // `emit P{b: 1, a: 2}` binds b to `a`'s slot positionally no
    // longer. A param no field matches leaves the vector short →
    // arity refusal → failed guard. `$owner` is the member-head
    // convention's trailing slot: it binds the mounted instance,
    // never a payload field.
    const auto eventArgs = [&](CellRef head,
                               CellRef owner) -> std::vector<CellValue> {
      std::vector<CellValue> args;
      if (payload == noCell) return args;
      const auto descriptor =
          arena.linked(head, core.dims().values, DimVector::POS);
      if (!descriptor) return args;
      for (auto p = arena.linked(*descriptor, core.dims().grab,
                               DimVector::POS);
           p && arena.contains(*p);
           p = arena.linked(*p, core.dims().step, DimVector::POS)) {
        const auto pname = arena.linked(*p, core.dims().name, DimVector::POS);
        if (!pname) break;
        if (arena.textOf(*pname) == "$owner") {
          if (owner == noCell) return args;
          args.push_back(CellValue{static_cast<std::int64_t>(owner)});
          continue;
        }
        CellRef field{noCell};
        for (const CellRef child :
             zigzag::rankAfter(arena, payload, core.dims().step))
          if (const auto cn =
                  arena.linked(child, core.dims().name, DimVector::POS);
              cn && arena.textOf(*cn) == arena.textOf(*pname)) {
            field = child;
            break;
          }
        if (field != noCell) {
          args.push_back(boundValue(field));
          continue;
        }
        // No payload field by that name: a Cell-typed param is the
        // occurrence/`as` binding — the whole term, not a field.
        const auto ptype =
            arena.linked(*p, core.dims().values, DimVector::POS);
        if (ptype && arena.textOf(*ptype) == "Cell") {
          args.push_back(
              CellValue{static_cast<std::int64_t>(payload)});
          continue;
        }
        return args;
      }
      return args;
    };
    // §3.5's merged order: priority first — a higher number runs
    // earlier — then source/declaration order, which the stable sort
    // leaves as clausesOf's rank walk for equal priorities.
    // §5.7's behavior merge: each mounted instance's descriptor
    // contributes its per-event clauses after the agent's own, in
    // +d.behavior mount order — the stable sort keeps this append
    // order for equal priorities. The instance rides each entry:
    // two mounts of one behavior contribute the same clause cell,
    // so a clause-keyed owner map would bind one instance's fields
    // to every copy's run.
    std::vector<std::pair<CellRef, CellRef>> clauses;
    for (const CellRef clause : meta.clausesOf(*eventSym))
      clauses.emplace_back(clause, noCell);
    for (const CellRef inst :
         zigzag::rankAfter(arena, agent, dims.behavior)) {
      const auto rec = varsRecord(arena, core, inst, "behavior", false);
      if (!rec) continue;
      const auto ref =
          arena.linked(*rec, core.dims().values, DimVector::POS);
      const auto desc = ref ? arena.asInt64(*ref) : std::nullopt;
      if (!desc) continue;
      const auto table = varsRecord(arena, core,
                                    static_cast<CellRef>(*desc), "handlers",
                                    false);
      if (!table) continue;
      const auto term =
          arena.linked(*table, core.dims().values, DimVector::POS);
      if (!term) continue;
      for (const CellRef child :
           zigzag::rankAfter(arena, *term, core.dims().step))
        if (arena.textOf(child) == name)
          for (const CellRef clause : meta.clausesOf(child))
            clauses.emplace_back(clause, inst);
    }
    const auto priorityOf = [&](CellRef clause) -> std::int64_t {
      if (const auto rec =
              varsRecord(arena, core, clause, "priority", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto n = arena.asInt64(*v)) return *n;
      return 0;
    };
    std::stable_sort(clauses.begin(), clauses.end(),
                     [&](const auto &a, const auto &b) {
                       return priorityOf(a.first) > priorityOf(b.first);
                     });
    for (const auto &[clause, owner] : clauses) {
      // The clause's +d.grab guard evaluates before the body — an
      // `on` handler's `[guard]` is a real filter, not decoration.
      // Refusals read as failures, same as selectClause.
      const auto guard =
          arena.linked(clause, core.dims().grab, DimVector::POS);
      const auto args = [&]() -> std::vector<CellValue> {
        if (guard) return eventArgs(*guard, owner);
        // No guard: bind off the body head's own param chain — the
        // spine's first member or the spin head — so a param'd
        // unguarded handler still fields the payload and `$owner`
        // always lands on member heads.
        auto members = zigzag::rankAfter(arena, clause, core.dims().step);
        if (!members.empty()) return eventArgs(*members.begin(), owner);
        if (const auto spin =
                arena.linked(clause, core.dims().spin, DimVector::POS))
          return eventArgs(*spin, owner);
        return {};
      }();
      if (guard) {
        const auto outcome =
            meta.runFragment(*guard, args, exec.cursor, maxCycles);
        // §8.2: production is the pass — `occurrence.goal == "Work"`
        // produces `"Work"`, and a produced `0`/`false` still counts.
        const bool pass =
            outcome.refusal == MetaRunOutcome::Refusal::None &&
            outcome.produced;
        if (!pass) continue;
      }
      if (!zigzag::rankAfter(arena, clause, core.dims().step).empty()) {
        if (!runSpine(clause, args)) return false;
      } else {
        const auto spin =
            arena.linked(clause, core.dims().spin, DimVector::POS);
        if (!spin) continue;
        auto outcome =
            meta.runFragment(*spin, args, exec.cursor, maxCycles);
        if (outcome.refusal == MetaRunOutcome::Refusal::NotAHead ||
            outcome.refusal == MetaRunOutcome::Refusal::NoDescriptor) {
          if (const auto head = meta.bodyOf(clause))
            outcome =
                meta.runFragment(*head, args, exec.cursor, maxCycles);
        }
        // consumeWork exhaustion and VM faults surface as refusals — a
        // turn fault (§3.2's fail path). An unproductive result (an
        // empty or valueless body) is success, not a fault.
        if (outcome.refusal != MetaRunOutcome::Refusal::None)
          return false;
      }
      // `consume` inside the just-run handler spends the event for
      // later clauses — the running handler's own body is unaffected.
      if (eventConsumed) break;
    }
    return true;
  };

  // A stopped agent's turn is a clean no-op — no journal, dispatch,
  // drain or settle; the record state persists untouched. A
  // destroying agent only keeps the pending drain: `on Destroy` is
  // the last work it ever dispatches.
  const auto flagOn = [&](std::string_view which) {
    if (const auto rec = varsRecord(arena, core, agent, which, false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto flag = arena.asInt64(*v)) return *flag != 0;
    return false;
  };
  const auto agentStopped    = flagOn("stopped");
  const auto agentDestroying = flagOn("destroying");

  // §8.5's creation order lands the member asserts at image time, so
  // the first exec turn is where `on Initialize` runs — once, ahead
  // of the turn's own percept. The $-namespaced flag record is
  // durable, so a checkpoint/resume or a later stop does not re-fire
  // initialization.
  if (!agentStopped && !agentDestroying)
    if (const auto rec =
            varsRecord(arena, core, agent, "$initialized", true);
        rec.has_value() &&
        !arena.linked(*rec, core.dims().values, DimVector::POS)
             .has_value()) {
      zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                     DimVector::POS,
                                     arena.makeScalarCell(std::int64_t{1})));
      // §8.5's creation order: `install`/`behavior` members run before
      // `on Initialize` — their steps sit on the $create clause the
      // lowerer collected in declaration order.
      if (const auto create =
              varsRecord(arena, core, agent, "$create", false))
        if (const auto clause = arena.linked(*create, core.dims().values,
                                             DimVector::POS))
          if (!runSpine(*clause, {})) return failTurn();
      const auto init = arena.makeCell("Initialize");
      journalAppend(arena, agent, init, dims.seen,
                    static_cast<std::size_t>(
                        setting("vagent.journal_size", 64)));
      if (!dispatchEvent("Initialize", init)) return failTurn();
    }

  if (!agentStopped && !agentDestroying && exec.message) {
    const std::string &eventName = exec.message->value;
    const auto seen            = arena.makeCell(eventName);
    journalAppend(arena, agent, seen, dims.seen,
                  static_cast<std::size_t>(setting("vagent.journal_size", 64)));
    if (!dispatchEvent(eventName, seen)) return failTurn();
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
    // The pending cell's `grab` record names its journal entry — read
    // it before the unlink drops the records with the cell.
    CellRef seen{noCell};
    if (const auto rec = varsRecord(arena, core, *head, "grab", false))
      if (const auto v =
              arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto r = arena.asInt64(*v))
          seen = static_cast<CellRef>(*r);
    unlinkRank(arena, *head, dims.pending);
    // A belief trigger stages its own shape — `beliefClause` names the
    // handler clause and +d.step the bound values, so it dispatches
    // through its own path rather than the event table.
    CellRef beliefClause = noCell;
    if (const auto rec =
            varsRecord(arena, core, *head, "beliefClause", false))
      if (const auto v =
              arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto r = arena.asInt64(*v))
          beliefClause = static_cast<CellRef>(*r);
    if (beliefClause != noCell) {
      std::vector<CellValue> args;
      for (const CellRef child :
           zigzag::rankAfter(arena, *head, core.dims().step))
        args.push_back(meta.factValue(child));
      if (const auto rec =
              varsRecord(arena, core, *head, "owner", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          if (const auto r = arena.asInt64(*v))
            args.push_back(CellValue{*r});
      if (const auto guard =
              arena.linked(beliefClause, core.dims().grab,
                           DimVector::POS)) {
        auto outcome =
            meta.runFragment(*guard, args, exec.cursor, maxCycles);
        if (outcome.refusal != MetaRunOutcome::Refusal::None ||
            !outcome.produced)
          continue;
      }
      bool fired = true;
      if (!zigzag::rankAfter(arena, beliefClause, core.dims().step)
               .empty()) {
        fired = runSpine(beliefClause, args);
      } else if (const auto spin =
                     arena.linked(beliefClause, core.dims().spin,
                                  DimVector::POS)) {
        const auto outcome =
            meta.runFragment(*spin, args, exec.cursor, maxCycles);
        fired = outcome.refusal == MetaRunOutcome::Refusal::None;
      }
      if (!fired) return failTurn();
      continue;
    }
    if (!dispatchEvent(name, seen)) return failTurn();
    // §4.5: `on Destroy` is the orderly stop's last act — nothing
    // behind it on the rank dispatches.
    if (name == "Destroy") break;
  }
  // Whether the drain just ran `on Destroy` or nothing was queued, a
  // destroying agent is done at this boundary.
  if (agentDestroying)
    if (const auto rec = varsRecord(arena, core, agent, "stopped", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeScalarCell(std::int64_t{1})));

  // The receiver half of tell/untell: each +d.inbox record is a
  // cross-agent assertion the adapter staged — the belief mutation
  // happens here, in the receiver's own turn, with the sender's
  // member cell as the +d.source peer. Same bounded drain shape as
  // pending.
  {
    const auto inboxDim = core.findOrMintDimension("d.inbox");
    for (std::size_t drained = 0;
         !agentStopped && !agentDestroying && drained < drainBound;
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
        const auto requestId = recordText(*head, "correlation");
        // §954: an achieve arriving inside the sender's retry
        // window after its cancel is consumed silently; past the
        // window (send_retries × send_retry_interval turns) the id
        // is forgotten and the achieve adopts fresh — the marker's
        // expired value link is what the window measures.
        if (!requestId.empty())
          if (const auto rec =
                  varsRecord(arena, core, agent,
                             std::string{"$cancelled:"} + requestId,
                             false)) {
            const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS);
            const auto expiry = v ? arena.asInt64(*v) : std::nullopt;
            if (expiry && turn <= *expiry)
              continue;
            if (v)
              zigzag::expectWritten(arena.unlink(
                  *rec, core.dims().values, DimVector::POS));
          }
        // §7.1's receiver default: accepts(sender, goal_name) admits
        // delegation from the parent and from children (and the
        // degenerate `by self`), refusing everything else — the
        // reply is a staged `refuse` the delegator hears through its
        // own inbox, carrying the parked intention's ref back.
        const auto accepted = sender == agent || parentOf(agent) == sender ||
                              parentOf(sender) == agent;
        if (!accepted && sender != noCell) {
          std::string senderName;
          if (const auto n =
                  arena.linked(sender, core.dims().name, DimVector::POS))
            senderName = std::string{arena.textOf(*n)};
          // A member delegator can't ride the delegator field — the
          // ref means nothing across the wire, and a same-node drain
          // would write `refused` onto the member itself. The
          // correlation token is the coherent handle on both legs,
          // and the wire needs nothing of the goal but its name —
          // refusing a positional-arg goal must not bounce off the
          // marshal gate.
          stageOutbox("refuse",
                      arena.makeCell(std::string{arena.textOf(term)}),
                      senderName, {},
                      remoteMemberRef(delegator) ? noCell : delegator,
                      requestId, {}, {}, false, {},
                      remoteMemberRef(sender) ? sender : noCell);
          continue;
        }
        if (!goal || delegator == noCell) continue;
        std::vector<CellRef> argRefs;
        auto child = meta.adoptGoal(*goal, turn,
                                    static_cast<std::size_t>(setting(
                                        "vagent.condition_turn_fuel", 256)),
                                    argValuesOf(term, &argRefs), argRefs);
        if (!child) continue;
        meta.setIntentionRef(*child, "delegator", delegator);
        if (remoteMemberRef(delegator)) {
          // A remote delegator's replies stage member-addressed and
          // correlation-keyed — +d.subgoal is intention machinery the
          // sender's own settle never walks here.
          if (!requestId.empty())
            if (const auto rec =
                    varsRecord(arena, core, *child, "delegatorCorrelation",
                               true))
              zigzag::expectWritten(
                  arena.link(*rec, core.dims().values, DimVector::POS,
                             arena.makeCell(requestId)));
        } else {
          zigzag::expectWritten(
              arena.link(zigzag::rankTail(arena, delegator, dims.subgoal),
                         dims.subgoal, DimVector::POS, *child));
        }
        continue;
      }
      if (k == "refuse") {
        // §7.1: the receiver's accepts said no. The parked delegator
        // hears it through the settle's delegated rung, which runs
        // the `else` ladder with cause `refused` — writing a flag
        // here keeps fragment runs out of the drain. A wire refuse
        // carries no intention ref; the achieve's correlation token
        // names the park instead.
        auto delegator = ref("delegator");
        if (delegator == noCell)
          delegator = intentionByCorrelation(
              "correlation", recordText(*head, "correlation"));
        if (delegator != noCell)
          if (const auto rec =
                  varsRecord(arena, core, delegator, "refused", true))
            zigzag::expectWritten(
                arena.link(*rec, core.dims().values, DimVector::POS,
                           arena.makeCell("refused")));
        continue;
      }
      if (k == "outcome") {
        // §955's {correlation, status, cause}: a remote delegate's
        // reply resumes the delegating achieve — "achieved" unblocks
        // it, anything else runs the else ladder with the cause.
        if (const auto parked = intentionByCorrelation(
                "correlation", recordText(*head, "correlation"));
            parked != noCell) {
          const auto status = fieldText(term, "status");
          if (status == "achieved") {
            meta.setIntentionField(parked, "delegated", 0);
            meta.setIntentionField(parked, "ready", 1);
          } else {
            const auto cause = fieldText(term, "cause");
            if (const auto rec = varsRecord(arena, core, parked, "refused",
                                            true))
              zigzag::expectWritten(arena.link(
                  *rec, core.dims().values, DimVector::POS,
                  arena.makeCell(cause.empty()
                                     ? std::string{status}
                                     : cause)));
          }
        }
        continue;
      }
      if (k == "cancel") {
        // §954: drop the intention adopted for that correlation id —
        // the adopted child recorded it as delegatorCorrelation. No
        // adoption yet means the achieve is still in flight:
        // $cancelled remembers the id for the retry window.
        const auto correlation = recordText(*head, "correlation");
        if (const auto child = intentionByCorrelation(
                "delegatorCorrelation", correlation);
            child != noCell) {
          raise("GoalDropped",
                {{"intention", refCell(arena, core, child)},
                 {"goal", arena.makeCell(goalNameOf(child))}});
          unlinkRank(arena, child, dims.intention);
        } else if (!correlation.empty()) {
          // The marker is the id's expiry turn — §954's retry
          // window, send_retries × send_retry_interval, in the
          // same scalar clock awaits and deadlines ride.
          if (const auto rec =
                  varsRecord(arena, core, agent,
                             std::string{"$cancelled:"} +
                                 std::string{correlation},
                             true))
            zigzag::expectWritten(
                arena.link(*rec, core.dims().values, DimVector::POS,
                           arena.makeScalarCell(
                               turn + setting("vagent.send_retries", 5) *
                                          setting("vagent.send_retry_interval",
                                                  1))));
        }
        continue;
      }
      if (k == "ask") {
        // §7.1's receiver half: run this agent's `answer q` head and
        // reply through the outbox. The reply's percept is the
        // question name (the asker's awaitEvent text), its
        // correlation the request id `deliver` journaled — and a
        // fault instead of a payload when there is no answer or the
        // head refuses, which the spec names `unanswered` and
        // `remote_fault`.
        std::string requestId;
        if (const auto rec =
                varsRecord(arena, core, *head, "correlation", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            requestId = std::string{arena.textOf(*v)};
        std::string senderName;
        if (const auto n =
                arena.linked(sender, core.dims().name, DimVector::POS))
          senderName = std::string{arena.textOf(*n)};
        CellRef answerHead = noCell;
        if (const auto sym =
                eventSymbolNamed(arena, core, programRoot, pred))
          if (const auto h =
                  arena.linked(*sym, core.dims().values, DimVector::POS))
            answerHead = *h;
        if (answerHead == noCell || senderName.empty()) {
          stageOutbox("answer", term, senderName, {}, noCell, requestId,
                      pred, "unanswered");
          continue;
        }
        const auto outcome =
            meta.runFragment(answerHead, argValuesOf(term), exec.cursor,
                             maxCycles);
        if (outcome.refusal != MetaRunOutcome::Refusal::None ||
            !outcome.produced || !outcome.result) {
          stageOutbox("answer", term, senderName, {}, noCell, requestId,
                      pred, "remote_fault");
          continue;
        }
        // §7.1: the answer must fit 32 KiB — the ActorSend value cap.
        // A structured answer already crosses as a ref handle, so the
        // bound only bites on an inline string body; over it the
        // reply is the same `remote_fault` a refusal earns.
        if (const auto *text = std::get_if<std::string>(&*outcome.result);
            text && text->size() > 32 * 1024) {
          stageOutbox("answer", term, senderName, {}, noCell, requestId,
                      pred, "remote_fault");
          continue;
        }
        // The produced value is marshaled as a bare cell — scalar or
        // text answers copy, a structured answer (a record term)
        // rides as the cell itself so the asker's `r.field` reads
        // through #FIND. A ref-typed Int64 answer mints refCell —
        // unmarked it dead-reads through the next fold.
        const auto value =
            refTypedResult(answerHead, *outcome.result)
                ? refCell(arena, core,
                          static_cast<CellRef>(
                              std::get<std::int64_t>(*outcome.result)))
                : std::visit(
                      [&](const auto &x) -> CellRef {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, std::string>)
                          return arena.makeCell(x);
                        else
                          return arena.makeScalarCell(x);
                      },
                      *outcome.result);
        stageOutbox("answer", value, senderName, {}, noCell, requestId,
                    pred);
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

  // A fragment head's declared param names, descriptor order — the
  // +d.values→(+d.grab/+d.step)+d.name chain meta.cpp's parameterCells
  // reads; mirrored because the reader is private.
  const auto paramNamesOf =
      [&](CellRef head) -> std::vector<std::string> {
    std::vector<std::string> names;
    if (const auto descriptor =
            arena.linked(head, core.dims().values, DimVector::POS))
      for (auto p = arena.linked(*descriptor, core.dims().grab,
                               DimVector::POS);
           p && arena.contains(*p);
           p = arena.linked(*p, core.dims().step, DimVector::POS))
        if (const auto n = arena.linked(*p, core.dims().name,
                                        DimVector::POS))
          names.emplace_back(arena.textOf(*n));
    return names;
  };
  // §8.2's production test: a condition succeeds by producing, whatever
  // the produced value — `a == b` yields `b` even when `b` is `0` or
  // `false`, which is why the checker bars bare Bool conditions from
  // these positions. Refusal and non-production are the only failures.
  const auto runCondition =
      [&](CellRef head, CellRef i,
          std::optional<std::vector<CellValue>> args =
              std::nullopt) -> MetaRunOutcome {
    return meta.runFragment(head, args.value_or(intentionArgs(i)),
                            exec.cursor, maxCycles);
  };
  const auto produces = [](const MetaRunOutcome &outcome) {
    return outcome.refusal == MetaRunOutcome::Refusal::None &&
           outcome.produced;
  };
  // §6.4's isolation rule: a refused condition is "unchanged", never a
  // drop, success or restorer — and raises ConditionFault exactly once,
  // flagged on the intention until the condition next evaluates
  // cleanly (production or clean failure both clear it).
  const auto condFault = [&](CellRef i, const MetaRunOutcome &outcome) {
    const auto field = "conditionFaulted";
    if (outcome.refusal == MetaRunOutcome::Refusal::None) {
      if (meta.intentionField(i, field).value_or(0) != 0)
        meta.setIntentionField(i, field, 0);
      return;
    }
    if (meta.intentionField(i, field).value_or(0) != 0) return;
    const auto cause = [&] {
      switch (outcome.refusal) {
      case MetaRunOutcome::Refusal::VmFault:
        return outcome.detail.empty() ? std::string{"fuel"}
                                      : outcome.detail;
      case MetaRunOutcome::Refusal::ArityMismatch:
        return std::string{"arity"};
      case MetaRunOutcome::Refusal::NotAHead:
      case MetaRunOutcome::Refusal::NoDescriptor:
        return std::string{"no_condition"};
      case MetaRunOutcome::Refusal::None:
        break;
      }
      return std::string{};
    }();
    raise("ConditionFault",
          {{"intention", refCell(arena, core, i)},
           {"cause", arena.makeCell(cause)}});
    meta.setIntentionField(i, field, 1);
  };

  // §3.5's deadline recovery: a parked `else` head evaluates with
  // `cause` bound — production binds the value to the suspension's
  // `as r`/`r :=` destination and completes the wait rather than
  // faulting; refusal or non-production reports false so the caller
  // takes the ordinary fault ladder.
  const auto runElse = [&](CellRef intention,
                           std::string_view cause = "deadline") -> bool {
    CellRef head = noCell;
    if (const auto rec =
            varsRecord(arena, core, intention, "awaitElse", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto n = arena.asInt64(*v)) head = static_cast<CellRef>(*n);
    if (head == noCell) return false;
    // Params bind positionally like every clause head — `cause` is
    // the one named exception the recovery's env declares; `$owner`
    // binds the intention's adopted-clause owner when a member body's
    // recovery runs.
    const auto paramNames = paramNamesOf(head);
    const auto stored     = intentionArgs(intention);
    std::vector<CellValue> args;
    std::size_t position = 0;
    for (const auto &pname : paramNames) {
      if (pname == "cause") {
        args.emplace_back(std::string{cause});
        continue;
      }
      if (pname == "$owner") {
        args.emplace_back(
            static_cast<std::int64_t>(
                meta.intentionField(intention, "owner").value_or(0)));
        continue;
      }
      if (position >= stored.size()) return false;
      args.push_back(stored[position++]);
    }
    const auto outcome =
        meta.runFragment(head, args, std::nullopt, maxCycles);
    if (outcome.refusal != MetaRunOutcome::Refusal::None ||
        !outcome.produced || !outcome.result)
      return false;
    // The recovery's produced value lands on the agent's bound var —
    // the same classified store capcall uses, so a ref-typed `else`
    // result rides the manifest and a var.slot keeps its holder.
    if (const auto rec =
            varsRecord(arena, core, intention, "awaitBind", false))
      if (const auto v = arena.linked(*rec, core.dims().values, DimVector::POS))
        if (const auto dst =
                varsRecord(arena, core, agent, arena.textOf(*v), true)) {
          if (refTypedResult(head, *outcome.result))
            storeVarRef(*dst, static_cast<CellRef>(
                                  std::get<std::int64_t>(*outcome.result)));
          else
            storeVar(*dst, *outcome.result);
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
       agentStopped || agentDestroying
           ? std::vector<CellRef>{}
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
      // V6's parked effect completes on either result name sharing the
      // request's correlation — done binds the payload, failed faults
      // into the else ladder through the percept's `fault` record.
      const auto awaitEffect =
          meta.intentionField(intention, "awaitEffect").value_or(0) != 0;
      // A mark the adapter wrote between turns — childDown's "down" or
      // resumed's "migrated" — is the freshest fact the intention can
      // hold: it postdates everything already journaled, so it wins
      // over a delivered reply that the dead member or the previous
      // incarnation sent before the mark was laid. One-shot like
      // `refused`: unlink before the else runs so a re-park can't
      // inherit the death.
      {
        std::string awaitFault;
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitFault", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            awaitFault = std::string{arena.textOf(*v)};
        if (!awaitFault.empty()) {
          if (const auto rec =
                  varsRecord(arena, core, intention, "awaitFault", false))
            zigzag::expectWritten(
                arena.unlink(*rec, core.dims().values, DimVector::POS));
          meta.setIntentionField(intention, "waiting", 0);
          if (!runElse(intention, awaitFault))
            faultIntention(intention, awaitFault);
          continue;
        }
      }
      bool matched      = false;
      CellRef matchCell = noCell;
      // The occurrence `as g` binds is the delivered payload term when
      // the percept carries an envelope — `r.field` reads it through
      // #FIND in the resumed body exactly as the guard saw it.
      CellRef matchDelivered = noCell;
      // awaitCorr's request id survives the block — an `r := ask`
      // bind tells answers from `as g` envelopes by its presence.
      std::string corr;
      if (awaitEvent != noCell) {
        const auto wanted = arena.textOf(awaitEvent);
        // awaitCorr narrows the match to the answer carrying this
        // request's id — the adapter echoes the outbox entry's id on
        // the journal cell's `correlation` record.
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
        // A parked guard head is a per-candidate wake filter — the
        // `as` binding's arg is the delivered payload term, and a
        // refusal or a failed verdict leaves the intention parked for
        // the next match rather than faulting the turn.
        const auto awaitGuard = [&]() -> CellRef {
          if (const auto rec =
                  varsRecord(arena, core, intention, "awaitGuard", false))
            if (const auto v =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              if (const auto r = arena.asInt64(*v))
                return static_cast<CellRef>(*r);
          return noCell;
        }();
        const auto awaitHasBinding =
            varsRecord(arena, core, intention, "awaitBind", false)
                .has_value();
        const auto guardPasses = [&](CellRef delivered) {
          if (awaitGuard == noCell) return true;
          auto gArgs = intentionArgs(intention);
          if (awaitHasBinding)
            gArgs.push_back(
                CellValue{static_cast<std::int64_t>(delivered)});
          const auto go = meta.runFragment(awaitGuard, gArgs,
                                           exec.cursor, maxCycles);
          condFault(intention, go);
          return produces(go);
        };
        if (beliefForm && awaitTerm != noCell) {
          for (const CellRef clause : meta.beliefsOf(arena.textOf(awaitTerm)))
            if (const auto head =
                    arena.linked(clause, core.dims().grab, DimVector::POS);
                head && meta.termEquals(awaitTerm, *head)) {
              if (!guardPasses(clause)) continue;
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
            const auto pname = arena.textOf(percept);
            if (pname != wanted &&
                !(awaitEffect && pname == "EffectFailed"))
              continue;
            // The delivered envelope: grab→payload when the percept
            // carries one, the percept itself otherwise — the term
            // an `as` binding's `r.field` reads through #FIND.
            CellRef delivered = percept;
            if (const auto rec =
                    varsRecord(arena, core, percept, "grab", false))
              if (const auto v =
                      arena.linked(*rec, core.dims().values, DimVector::POS))
                if (const auto r = arena.asInt64(*v))
                  delivered = static_cast<CellRef>(*r);
            if (awaitTerm != noCell &&
                !meta.termEquals(awaitTerm, delivered))
              continue;
            if (!corr.empty()) {
              bool same = false;
              if (const auto prec =
                      varsRecord(arena, core, percept, "correlation", false))
                if (const auto v =
                        arena.linked(*prec, core.dims().values, DimVector::POS))
                  same = arena.textOf(*v) == corr;
              if (!same) continue;
            }
            if (!guardPasses(delivered)) continue;
            zigzag::expectWritten(
                arena.link(intention, dims.evidence, DimVector::POS, percept));
            matched        = true;
            matchCell      = percept;
            matchDelivered = delivered;
            break;
          }
      }
      if (matched) {
        // An "answer" delivery's typed fault runs the `else` ladder
        // with the reply's cause instead of binding — `unanswered`
        // and `remote_fault` are the receiver-side names for it.
        std::string replyFault;
        if (const auto rec =
                varsRecord(arena, core, matchCell, "fault", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            replyFault = std::string{arena.textOf(*v)};
        if (!replyFault.empty()) {
          meta.setIntentionField(intention, "waiting", 0);
          if (!runElse(intention, replyFault))
            faultIntention(intention, replyFault);
          continue;
        }
        // awaitBind carries the `as g`/`r := ask` destination across
        // the park — the occurrence lands agent-side, the same
        // Intention/Agent-ref shape adopt/spawn bind. The journal
        // cell stays reachable through +d.evidence; what `as` names
        // is the event term so `r.field` reads the payload's data.
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitBind", false))
          if (const auto v =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            if (const auto dst =
                    varsRecord(arena, core, agent, arena.textOf(*v), true)) {
              const auto bound =
                  matchDelivered != noCell ? matchDelivered : matchCell;
              // An ask reply (`corr` set) delivers the bare answer
              // cell: a leaf answer binds its value so `r` reads 42,
              // not a handle — a structured answer stays a ref for
              // `r.field`/#FIND. An `as g` binding always takes the
              // ref: its value is the term to Find on. The store goes
              // through the classified helpers — a marked leaf (a
              // ref-typed answer) stays a ref, and a var.slot keeps
              // its holder.
              bool stored = false;
              if (!corr.empty() && bound != noCell) {
                const auto reloc =
                    arena.dimensionNamed("d.mesh.relocation");
                if (const auto n = arena.asInt64(bound);
                    reloc && arena.linked(bound, *reloc, DimVector::POS) &&
                    n) {
                  storeVarRef(*dst, static_cast<CellRef>(*n));
                  stored = true;
                } else if (n) {
                  storeVar(*dst, CellValue{*n});
                  stored = true;
                } else if (const auto d = arena.asDouble(bound)) {
                  storeVar(*dst, CellValue{*d});
                  stored = true;
                } else if (const auto b = arena.asBool(bound)) {
                  storeVar(*dst, CellValue{*b});
                  stored = true;
                } else if (zigzag::rankAfter(arena, bound,
                                             core.dims().step)
                               .empty()) {
                  storeVar(*dst, CellValue{std::string{arena.textOf(bound)}});
                  stored = true;
                }
              }
              if (!stored)
                storeVarRef(*dst, bound);
            }
        meta.setIntentionField(intention, "waiting", 0);
        meta.setIntentionField(intention, "ready", 1);
      } else if (turn >= deadline) {
        meta.setIntentionField(intention, "waiting", 0);
        // V6's elapsed deadline stages cancel to the binding — best
        // effort by the host contract — before the statement's own
        // ladder; the correlation names the in-flight request.
        if (awaitEffect) {
          CellRef call = noCell;
          if (const auto rec =
                  varsRecord(arena, core, intention, "awaitCall", false))
            if (const auto v =
                    arena.linked(*rec, core.dims().values, DimVector::POS))
              if (const auto r = arena.asInt64(*v))
                call = static_cast<CellRef>(*r);
          stageOutbox("cancel", call, {}, {}, noCell, corr);
        }
        if (!runElse(intention)) faultIntention(intention, "deadline");
      }
      if (!matched) continue;
    }
    const auto delegated =
        meta.intentionField(intention, "delegated").value_or(0);
    if (delegated != 0) {
      // A staged `refuse` reply marks the wait — the receiver's
      // accepts admitted no adoption, so there is no child to drop.
      // The refusal is one-shot: clearing it before the else runs
      // keeps a later re-delegation from inheriting the flag.
      std::string refused;
      if (const auto rec =
              varsRecord(arena, core, intention, "refused", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          refused = std::string{arena.textOf(*v)};
      if (!refused.empty()) {
        if (const auto rec =
                varsRecord(arena, core, intention, "refused", false))
          zigzag::expectWritten(
              arena.unlink(*rec, core.dims().values, DimVector::POS));
        meta.setIntentionField(intention, "delegated", 0);
        if (!runElse(intention, refused))
          faultIntention(intention, refused);
        continue;
      }
      // A remote delegation's Down mark — the member died between
      // turns, so the park faults now with the death cause instead of
      // riding its within bound out. A local delegation carries no
      // awaitPeer, so the mark can only be remote.
      std::string awaitFault;
      if (const auto rec =
              varsRecord(arena, core, intention, "awaitFault", false))
        if (const auto v =
                arena.linked(*rec, core.dims().values, DimVector::POS))
          awaitFault = std::string{arena.textOf(*v)};
      if (!awaitFault.empty()) {
        if (const auto rec =
                varsRecord(arena, core, intention, "awaitFault", false))
          zigzag::expectWritten(
              arena.unlink(*rec, core.dims().values, DimVector::POS));
        meta.setIntentionField(intention, "delegated", 0);
        if (!runElse(intention, awaitFault))
          faultIntention(intention, awaitFault);
        continue;
      }
      // achieve within: the delegation's own deadline. Expiry drops
      // the unfinished +d.subgoal children — the delegator's
      // notification path must not fire on them afterwards — then
      // the parent rides the ordinary fault ladder. A remote wait
      // ending owes the delegate §954's cancel first.
      const auto deadline =
          meta.intentionField(intention, "awaitDeadline").value_or(0);
      if (deadline != 0 && turn >= deadline) {
        stageCancel(intention);
        for (const CellRef child :
             zigzag::rankAfter(arena, intention, dims.subgoal) |
                 std::ranges::to<std::vector>()) {
          stageCancel(child);
          unlinkRank(arena, child, dims.intention);
        }
        zigzag::expectWritten(
            arena.unlink(intention, dims.subgoal, DimVector::POS));
        meta.setIntentionField(intention, "delegated", 0);
        if (!runElse(intention)) faultIntention(intention, "deadline");
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
      // the goal's dropTest is the reconsideration boundary —
      // producing drops the record off the rank, failing (or
      // absent) leaves it
      // faulted for inspection.
      if (blockedTill > turn) continue;
      const auto faults = meta.intentionField(intention, "faults").value_or(0);
      if (faults >= setting("vagent.max_intention_faults", 3)) {
        const auto goal = goalOf(intention);
        bool dropped    = false;
        if (goal != noCell)
          if (const auto rec = varsRecord(arena, core, goal, "dropTest", false))
            if (const auto head =
                    arena.linked(*rec, core.dims().values, DimVector::POS)) {
              const auto outcome = runCondition(*head, intention);
              condFault(intention, outcome);
              dropped = produces(outcome);
            }
        if (dropped) {
          // A dropped subgoal fails the parent's achieve — the fault
          // ladder decides retry vs. drop there too. A remote
          // delegator hears it as outcome{dropped,subgoal_dropped}.
          const auto delegator =
              meta.intentionField(intention, "delegator").value_or(0);
          if (remoteMemberRef(static_cast<CellRef>(delegator))) {
            stageOutcome(intention, "dropped", "subgoal_dropped");
          } else if (delegator != 0) {
            meta.setIntentionField(static_cast<CellRef>(delegator), "delegated",
                                   0);
            faultIntention(static_cast<CellRef>(delegator),
                           "subgoal_dropped");
          }
          raise("GoalDropped",
                {{"intention",
                  refCell(arena, core, intention)},
                 {"goal", arena.makeCell(goalNameOf(intention))}});
          unlinkRank(arena, intention, dims.intention);
        } else if (meta.intentionField(intention, "goalFailed")
                       .value_or(0) == 0) {
          // The ladder's cap is §6.3's "no applicable untried plan
          // remains": the goal has failed. V1 parks the record for
          // inspection rather than unlinking — a GoalFailed handler is
          // where re-adoption or an explicit drop happens — so the
          // raise runs once via the flag.
          stageOutcome(intention, "failed", "no_untried_plans");
          raise("GoalFailed",
                {{"intention",
                  refCell(arena, core, intention)},
                 {"goal", arena.makeCell(goalNameOf(intention))},
                 {"cause", arena.makeCell("no_untried_plans")}});
          meta.setIntentionField(intention, "goalFailed", 1);
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
        CellRef holdsHead = noCell;
        if (const auto rec =
                varsRecord(arena, core, goalCell, "holdsTest", false))
          if (const auto h =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            holdsHead = *h;
        const auto stored = intentionArgs(intention);
        const auto pnames = holdsHead != noCell
                                ? paramNamesOf(holdsHead)
                                : std::vector<std::string>{};
        // `?v` naming a goal param constrains the fact to the adopted
        // value; a fresh `?name` captures the fact child's. Seeding the
        // env with the bound args is what tells them apart.
        std::map<std::string, CellValue, std::less<>> seededEnv;
        for (std::size_t i = 0;
             i < stored.size() && i < pnames.size(); ++i)
          seededEnv.emplace(pnames[i], stored[i]);
        // holdsBelief is a container whose +d.grab rank carries one
        // unification pattern per `believes` conjunct (+d.grab
        // membership, not +d.step: a pattern's own +d.step args ride
        // the same chain). `holds` is existential over bindings: the
        // scalar half may reject a captured env, so the walk
        // backtracks each pattern through the bank until the whole
        // conjunction — every belief matched AND the peeled
        // remainder true — succeeds on one env. A captured `?name`
        // that never bound (reachable only if the head's tail out-
        // runs what the patterns captured) fails the leaf.
        std::vector<CellRef> patterns;
        if (const auto rec =
                varsRecord(arena, core, goalCell, "holdsBelief", false))
          if (const auto container =
                  arena.linked(*rec, core.dims().values, DimVector::POS))
            patterns = std::ranges::to<std::vector>(
                zigzag::rankAfter(arena, *container, core.dims().grab));
        const auto scalarHolds = [&](const auto &env) {
          if (holdsHead == noCell) return true;
          auto args = stored;
          for (std::size_t i = stored.size(); i < pnames.size(); ++i) {
            const auto it = env.find(pnames[i]);
            if (it == env.end()) return false;
            args.push_back(it->second);
          }
          const auto outcome = runCondition(holdsHead, intention, args);
          condFault(intention, outcome);
          // §6.4: a refused condition is "unchanged" — no restorer
          // fires on a lapse the runtime never actually proved.
          return outcome.refusal != MetaRunOutcome::Refusal::None ||
                 outcome.produced;
        };
        const std::function<bool(std::size_t, decltype(seededEnv))>
            solve = [&](std::size_t i,
                        const decltype(seededEnv) &env) -> bool {
          if (i == patterns.size()) return scalarHolds(env);
          const auto &pattern = patterns[i];
          const auto  pred    = arena.textOf(pattern);
          for (const auto clause : meta.beliefsOf(pred)) {
            if (const auto fact = arena.linked(clause, core.dims().grab,
                                               DimVector::POS)) {
              auto attempt = env;
              if (meta.unifyCollect(pattern, *fact, attempt) &&
                  solve(i + 1, attempt))
                return true;
            }
          }
          return false;
        };
        if (!patterns.empty() || holdsHead != noCell)
          holds = solve(0, seededEnv);
      }
      if (!holds) {
        // The restorer inherits the monitor's bound args — a
        // parameterized maintain's restorer works on the same values.
        // The refs re-describe off the stored chain so a marked arg
        // survives this re-adoption's own fold.
        std::vector<CellRef> argRefs;
        auto child = meta.adoptGoal(
            goalCell, turn,
            static_cast<std::size_t>(
                setting("vagent.condition_turn_fuel", 256)),
            intentionArgs(intention, &argRefs), argRefs);
        if (!child) {
          faultIntention(intention, "restorer");
          continue;
        }
        meta.setIntentionRef(*child, "delegator", intention);
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
      faultIntention(intention, "no_plan");
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
      faultIntention(intention, "body_replaced");
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
          faultIntention(intention, "no_body");
          continue;
        }
        const auto outcome = meta.runFragment(
            *head, withOwner(intentionArgs(intention), intention),
            exec.cursor, maxCycles);
        ok = outcome.refusal == MetaRunOutcome::Refusal::None;
      }
    } else if (stepIdx < spine.size()) {
      const auto member = spine[stepIdx];
      if (arena.linked(member, dims.verb, DimVector::POS).has_value())
        ok = runHostStep(member, intention);
      else {
        const auto outcome = meta.runFragment(
            member, withOwner(intentionArgs(intention), intention),
            exec.cursor, maxCycles);
        ok = outcome.refusal == MetaRunOutcome::Refusal::None;
      }
    }
    if (!ok) {
      faultIntention(intention, "body");
      continue;
    }
    const auto next = stepIdx + 1;
    if (next >= std::max<std::size_t>(spine.size(), 1)) {
      // The body's last member ran. For an achieve goal that is not
      // success yet — the guide's contract is that a plan's end
      // without `succeed when` counts as a failed plan: eval the
      // goal's succeedTest, failure goes back down the ladder.
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
          bool refused   = false;
          bool succeeded = true;
          if (const auto rec =
                  varsRecord(arena, core, goal, "succeedTest", false))
            if (const auto head =
                    arena.linked(*rec, core.dims().values, DimVector::POS)) {
              const auto outcome = runCondition(*head, intention);
              condFault(intention, outcome);
              refused   = outcome.refusal != MetaRunOutcome::Refusal::None;
              succeeded = produces(outcome);
            }
          // §6.4: a refused succeedTest is "unchanged" — the intention
          // parks past the spine end so the next settle re-tests the
          // condition rather than re-running the last member, and
          // never faults on a verdict it never got.
          if (refused) {
            meta.setIntentionField(intention, "step",
                                   static_cast<std::int64_t>(next));
            continue;
          }
          if (!succeeded) {
            faultIntention(intention, "succeed_unmet");
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
      // on this completion. A remote delegator hears it as
      // outcome{achieved} — the same resume, over the wire.
      const auto delegator =
          meta.intentionField(intention, "delegator").value_or(0);
      if (remoteMemberRef(static_cast<CellRef>(delegator))) {
        stageOutcome(intention, "achieved");
      } else if (delegator != 0) {
        meta.setIntentionField(static_cast<CellRef>(delegator), "delegated", 0);
        meta.setIntentionField(static_cast<CellRef>(delegator), "ready", 1);
      }
      raise("GoalAchieved",
            {{"intention",
              refCell(arena, core, intention)},
             {"goal", arena.makeCell(goalNameOf(intention))}});
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
                    .delegator   = refField("delegator"),
                    .percept     = read("percept"),
                    .correlation = read("correlation"),
                    .fault       = read("fault"),
                    .cell        = entry,
                    .targetMember = refField("targetMember"),
                    .capacity    = read("capacity"),
                    .preempts =
                        varsRecord(arena, core, entry, "preempts", false)
                            .has_value()});
  }
  return entries;
}

std::vector<Delivery> VagentExecutor::route(zigzag::ArenaManifold &arena,
                                            zigzag::vortex::VortexCore &core,
                                            zigzag::CellRef programRoot,
                                            const OutboxEntry &entry) {
  // Kinds the drain handles without routing — effect/cancel are host
  // work answered on the request's correlation, spawn a wire request
  // — are never percepts; an empty target would broadcast them.
  if (entry.kind == "effect" || entry.kind == "cancel" ||
      entry.kind == "spawn")
    return {};
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
        deliveries.push_back(Delivery{
            .agent = member,
            // An ask reply overrides the percept name — its payload
            // is the bare answer value, so the question name rides
            // the entry's own `percept` field instead.
            .percept = entry.percept.empty() ? entry.payload : entry.percept,
            .id          = entry.id,
            .kind        = entry.kind,
            .payloadCell = entry.payloadCell,
            .correlation = entry.correlation,
            .sender      = entry.sender,
            .delegator   = entry.delegator,
            .fault       = entry.fault});
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
      delivery.kind == "achieve" || delivery.kind == "ask" ||
      delivery.kind == "refuse" || delivery.kind == "outcome" ||
      delivery.kind == "cancel") {
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
                       refCell(arena, core, ref)));
    };
    refField("grab", delivery.payloadCell);
    refField("sender", delivery.sender);
    refField("delegator", delivery.delegator);
    // The delivery's semantic correlation is the request's durable
    // token — a wire requestId carries a transport leg suffix the
    // remote side cannot reconstruct. "ask" echoes it on the reply's
    // percept and "achieve" on the refuse/outcome replies, so either
    // sender-side wait resolves exactly one request.
    const auto token =
        delivery.correlation.empty() ? delivery.id : delivery.correlation;
    if (!token.empty())
      if (const auto rec =
              varsRecord(arena, core, record, "correlation", true))
        zigzag::expectWritten(
            arena.link(*rec, core.dims().values, DimVector::POS,
                       arena.makeCell(token)));
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
  // An ask reply's typed fault: the wake match reads it before the
  // payload and takes the sender's `else` cause instead of binding.
  if (!delivery.fault.empty())
    if (const auto rec = varsRecord(arena, core, seen, "fault", true))
      zigzag::expectWritten(arena.link(*rec, core.dims().values,
                                       DimVector::POS,
                                       arena.makeCell(delivery.fault)));
  // The term envelope rides as a scalar ref — the durable record of
  // what the percept carried, so it must survive a fold as a marked
  // ref rather than a dead position. AwaitTerm is belief-form (the
  // checker's term() requires a predicate), so nothing consumes it
  // today; the schema is what a future field-matching event await reads.
  if (delivery.payloadCell != noCell)
    if (const auto rec = varsRecord(arena, core, seen, "grab", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     refCell(arena, core, delivery.payloadCell)));
  journalAppend(arena, delivery.agent, seen, core.findOrMintDimension("d.seen"),
                static_cast<std::size_t>(setting("vagent.journal_size", 64)));
  const auto pending = core.findOrMintDimension("d.pending");
  const auto pend    = arena.makeCell(delivery.percept);
  // The pending trigger points back at its own seen record — the
  // drain hands the guard eval the delivered payload's ref, not just
  // the name, so `where` on the handler sees the event's args.
  if (const auto rec = varsRecord(arena, core, pend, "grab", true))
    zigzag::expectWritten(arena.link(
        *rec, core.dims().values, DimVector::POS,
        refCell(arena, core, seen)));
  zigzag::expectWritten(arena.link(
      zigzag::rankTail(arena, delivery.agent, pending), pending,
      DimVector::POS, pend));
}

void VagentExecutor::deliveryFailed(zigzag::ArenaManifold &arena,
                                    zigzag::vortex::VortexCore &core,
                                    const OutboxEntry &entry,
                                    std::string_view cause) const {
  if (entry.sender == noCell) return;
  // The sender-side half of §7.3's honest bound: the request's own id
  // keys the dedupe, so the outbox rank's persistence cannot turn one
  // unroutable entry into a stream of identical failures. The key is
  // namespaced because +d.delivered is shared with `deliver`'s
  // arrival marks — an entry whose broadcast leg already delivered
  // back to its own sender must not swallow a remote leg's failure.
  // An id-less entry raises once per call — the caller's job to avoid.
  if (!entry.id.empty() &&
      !adapterOnce(arena, core, entry.sender,
                   std::format("deliveryfailed:{}", entry.id)))
    return;
  // The event payload rides the +d.step/+d.name envelope the raise
  // path mints — `occurrence.to` resolves no member for an unknown
  // name, so the target text is the honest Agent-typed value.
  const auto payload = arena.makeCell("DeliveryFailed");
  perceptField(arena, core, payload, "to", arena.makeCell(entry.target));
  perceptField(arena, core, payload, "kind", arena.makeCell(entry.kind));
  perceptField(arena, core, payload, "cause", arena.makeCell(cause));
  queueAdapterPercept(
      arena, core, entry.sender, "DeliveryFailed", payload,
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
}

void VagentExecutor::childDown(zigzag::ArenaManifold &arena,
                               zigzag::vortex::VortexCore &core,
                               zigzag::CellRef agent,
                               zigzag::CellRef child) const {
  if (agent == noCell || child == noCell) return;
  // One down per member: the key rides the member's ref so the
  // ActorDown trailing an orderly `stop` (which pre-marks the same
  // key beside ChildStopping) and a redelivered mesh report both
  // collapse to the first raise.
  if (!adapterOnce(arena, core, agent,
                   std::format("childdown:{}", child)))
    return;
  // The settle's `Down(child)` rung is markPeerWaits with the death
  // cause — "down" is what the else ladder reports for a member the
  // mesh reported lost.
  markPeerWaits(arena, core, agent, child, "down");
  const auto payload = arena.makeCell("ChildDown");
  perceptField(arena, core, payload, "child",
               refCell(arena, core, child));
  queueAdapterPercept(
      arena, core, agent, "ChildDown", payload,
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
}

void VagentExecutor::childStopping(zigzag::ArenaManifold &arena,
                                   zigzag::vortex::VortexCore &core,
                                   zigzag::CellRef agent,
                                   zigzag::CellRef member,
                                   std::string_view cause) const {
  if (agent == noCell || member == noCell) return;
  // §5.x's closed cause set — "stop" is the executor's own verb;
  // "terminate", "lease" and "init" arrive adapter-side. The dedupe
  // key is shared with `stop` so the parent reports one
  // ChildStopping regardless of which side observed the death.
  if (!adapterOnce(arena, core, agent,
                   std::format("childstopping:{}", member)))
    return;
  // The trailing ActorDown reports the same death — pre-marking its
  // key keeps "one ChildStopping even when ActorDown follows" a
  // journal fact rather than a race.
  (void)adapterOnce(arena, core, agent,
                    std::format("childdown:{}", member));
  // The member's own `on Destroy` runs the same handler a `stop`
  // would trigger — queued, not raised, so it dispatches inside the
  // member's own settle.
  if (const auto rec =
          varsRecord(arena, core, member, "destroying", true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, DimVector::POS,
                   arena.makeScalarCell(std::int64_t{1})));
  queueAdapterPercept(
      arena, core, member, "Destroy", arena.makeCell("Destroy"),
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
  markPeerWaits(arena, core, agent, member, cause);
  const auto payload = arena.makeCell("ChildStopping");
  perceptField(arena, core, payload, "child",
               refCell(arena, core, member));
  perceptField(arena, core, payload, "cause", arena.makeCell(cause));
  queueAdapterPercept(
      arena, core, agent, "ChildStopping", payload,
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
}

void VagentExecutor::spawnFailed(zigzag::ArenaManifold &arena,
                                 zigzag::vortex::VortexCore &core,
                                 zigzag::CellRef agent,
                                 zigzag::CellRef child) const {
  if (agent == noCell || child == noCell) return;
  if (!adapterOnce(arena, core, agent,
                   std::format("spawnfailed:{}", child)))
    return;
  // §8.6: the refused name's staged deliveries fail too — the outbox
  // rank persists, so each rides DeliveryFailed's own per-id dedupe
  // rather than unlinking what the adapter might still inspect.
  std::string refusedName;
  if (const auto n = arena.linked(child, core.dims().name, DimVector::POS))
    refusedName = std::string{arena.textOf(*n)};
  if (!refusedName.empty())
    for (const auto &entry : drainOutbox(arena, core, agent))
      if (entry.target == refusedName)
        deliveryFailed(arena, core, entry, "spawn_refused");
  const auto payload = arena.makeCell("SpawnFailed");
  perceptField(arena, core, payload, "child",
               refCell(arena, core, child));
  queueAdapterPercept(
      arena, core, agent, "SpawnFailed", payload,
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
}

void VagentExecutor::stoppedByParent(zigzag::ArenaManifold &arena,
                                     zigzag::vortex::VortexCore &core,
                                     zigzag::CellRef agent) const {
  if (agent == noCell) return;
  // §4.5's orderly stop as the wire asks for it: the `stop`
  // performative landed after the adapter's parent check, so the
  // sequence the in-turn verb runs locally runs here on the real
  // agent — `destroying` parks every phase but the pending drain,
  // where the queued Destroy dispatches under the member's own settle.
  if (const auto rec =
          varsRecord(arena, core, agent, "destroying", true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, DimVector::POS,
                   arena.makeScalarCell(std::int64_t{1})));
  queueAdapterPercept(
      arena, core, agent, "Destroy", arena.makeCell("Destroy"),
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
  markPeerWaits(arena, core, agent, agent, "stop");
}

void VagentExecutor::moved(zigzag::ArenaManifold &arena,
                           zigzag::vortex::VortexCore &core,
                           zigzag::CellRef agent,
                           zigzag::CellRef member) const {
  if (agent == noCell || member == noCell) return;
  // The dedupe keys the new reference: one Moved per pid, a second
  // migration arrives on a different member cell and reports again.
  if (!adapterOnce(arena, core, agent, std::format("moved:{}", member)))
    return;
  // §5.4's "receivers update $peers": every vars record still holding
  // a same-name member that isn't this reference rebinds to it — a
  // member ref is the honest stale-marker because names, not cells,
  // are the durable peer identity.
  std::string name;
  if (const auto n = arena.linked(member, core.dims().name, DimVector::POS))
    name = std::string{arena.textOf(*n)};
  if (!name.empty())
    for (const CellRef rec :
         zigzag::rankAfter(arena, agent, core.dims().vars)) {
      const auto v = arena.linked(rec, core.dims().values, DimVector::POS);
      if (!v) continue;
      const auto ref = arena.asInt64(*v);
      if (!ref) continue;
      const auto held = static_cast<CellRef>(*ref);
      if (held == member) continue;
      const auto heldName =
          arena.linked(held, core.dims().name, DimVector::POS);
      if (!heldName || arena.textOf(*heldName) != name) continue;
      zigzag::expectWritten(
          arena.unlink(rec, core.dims().values, DimVector::POS));
      zigzag::expectWritten(
          arena.link(rec, core.dims().values, DimVector::POS,
                     refCell(arena, core, member)));
    }
  const auto payload = arena.makeCell("Moved");
  perceptField(arena, core, payload, "name",
               refCell(arena, core, member));
  queueAdapterPercept(
      arena, core, agent, "Moved", payload,
      static_cast<std::size_t>(setting("vagent.journal_size", 64)));
}

void VagentExecutor::injectPercept(
    zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
    zigzag::CellRef agent, std::string_view name,
    const std::vector<std::pair<std::string, zigzag::vortex::CellValue>> &
        fields,
    std::string_view id, std::string_view kind,
    std::string_view correlation, zigzag::CellRef sender) const {
  if (agent == noCell || name.empty()) return;
  // marshalTermExpr's literal shape: dual channels — the typed bits
  // boundValue reads first, the text belief-term equality walks.
  const auto literal = [&](const zigzag::vortex::CellValue &value) {
    return std::visit(
        [&](const auto &v) -> CellRef {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>) {
            return arena.makeCell(v);
          } else if constexpr (std::is_same_v<T, bool>) {
            const auto cell =
                arena.makeCell(v ? std::string{"true"}
                                 : std::string{"false"});
            zigzag::expectWritten(arena.setValueBits(
                cell, xanadu::ValueKind::Bool,
                static_cast<std::uint64_t>(v)));
            return cell;
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            const auto cell =
                arena.makeCell(std::format("{}", v));
            zigzag::expectWritten(arena.setValueBits(
                cell, xanadu::ValueKind::Int64,
                std::bit_cast<std::uint64_t>(v)));
            return cell;
          } else {
            const auto cell =
                arena.makeCell(std::format("{}", v));
            zigzag::expectWritten(
                arena.setValueBits(cell, xanadu::ValueKind::Double,
                                   xanadu::canonicalDoubleBits(v)));
            return cell;
          }
        },
        value);
  };
  const auto payload = arena.makeCell(name);
  for (const auto &[field, value] : fields)
    perceptField(arena, core, payload, field, literal(value));
  deliver(arena, core,
          Delivery{.agent       = agent,
                   .percept     = std::string{name},
                   .id          = std::string{id},
                   .kind        = std::string{kind},
                   .payloadCell = payload,
                   .correlation = std::string{correlation},
                   .sender      = sender,
                   // A wire achieve's delegator can't name the remote
                   // intention — the sender member carries it, and the
                   // adoption's replies stage back member-addressed.
                   .delegator   = kind == "achieve" ? sender : noCell});
}

void VagentExecutor::resumed(zigzag::ArenaManifold &arena,
                             zigzag::vortex::VortexCore &core,
                             zigzag::CellRef agent) const {
  if (agent == noCell) return;
  // §5.4: an effect wait cannot follow the device — the in-flight
  // binding belonged to the old pid's host. The mark goes through
  // the same awaitFault rung a Down writes, so the else ladder's
  // cause is "migrated", not "down".
  for (const CellRef intention :
       zigzag::rankAfter(arena, agent,
                         core.findOrMintDimension("d.intention"))) {
    if (intentionFlag(arena, core, intention, "waiting") == 0) continue;
    if (!varsRecord(arena, core, intention, "awaitEffect", false))
      continue;
    if (const auto rec =
            varsRecord(arena, core, intention, "awaitFault", true))
      zigzag::expectWritten(
          arena.link(*rec, core.dims().values, DimVector::POS,
                     arena.makeCell("migrated")));
  }
}

} // namespace xanadu::vagent
