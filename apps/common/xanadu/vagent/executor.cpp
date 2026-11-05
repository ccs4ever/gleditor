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
        turn = *current;
        zigzag::expectWritten(
            arena.unlink(*rec, core.dims().values, DimVector::POS));
      }
    }
    zigzag::expectWritten(arena.link(*rec, core.dims().values, DimVector::POS,
                                     arena.makeScalarCell(turn + 1)));
  }

  // Journal + dispatch. A wake turn (message absent) carries no percept:
  // it journals nothing and dispatches no `on` handler.
  MetaRuntime meta(core, vm, binds);
  if (exec.message) {
    const std::string &eventName = exec.message->value;
    journalAppend(arena, agent, arena.makeCell(eventName), dims.seen,
                  static_cast<std::size_t>(setting("vagent.journal_size", 64)));
    const auto eventSym = eventSymbolNamed(arena, core, programRoot, eventName);
    if (eventSym) {
      const auto maxCycles = static_cast<std::size_t>(std::min<std::uint64_t>(
          exec.grant.maxInstructions == 0 ? 100'000
                                          : exec.grant.maxInstructions,
          std::uint64_t{1'000'000}));
      for (const CellRef clause : meta.clausesOf(*eventSym)) {
        // M0 clauses carry a full compileFragment head on +d.spin — run
        // it directly; learn-clause graphs lacking the descriptor go
        // through body_of's session wrapper instead.
        const auto spin =
            arena.linked(clause, core.dims().spin, DimVector::POS);
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
        if (outcome.refusal != MetaRunOutcome::Refusal::None) {
          return failTurn();
        }
      }
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

} // namespace xanadu::vagent
