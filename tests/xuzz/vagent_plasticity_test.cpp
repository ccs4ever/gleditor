// Phase-7 plasticity-surface exercises (design/vagent-metaprogramming-
// implementation.md :576-591; design/vagent-metaprogramming.md §8.1–§8.5
// and the §9.2/§9.3 worked examples — §9.1's splice belongs to the
// expand suite and §9.4's `static` folds at compile time, so neither is
// re-run here). Test-only: every assertion rides machinery built
// through phase 6 — weight/synapse cells, the graft_*/learn_* verbs,
// reify/run, genesis, and the P16 checkpoint ceremony.
//
// The adapter boundary is stated per test rather than smoothed over:
// the examples' verb sequences — learn_*, set_weight, sprout_synapse,
// graft_*, reify, a `run` of a fitness fragment, and the mark-bounded
// cut/fail — execute on MetaRuntime today. What needs the actor
// executor (implementation.md :586-587 — "the full actor executor,
// vagent iterations 4–5+") is exactly: event delivery firing the `on …`
// handlers, `believes`/`believe` predicate state, and plan *selection*
// consuming the clause bank. The one test that marks that boundary is
// at the bottom; nothing else is faked with a stub executor.
#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vagent/genesis.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/p16.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::MicroversionId;
using xanadu::Store;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;

// ---------------------------------------------------------------------------
// Engine-side harness — the mutation suite's shape: one ephemeral arena, a
// minted bind set, and a lineage bound so the durable learn_* arms open.
// `demo` is the worked examples' module: bare goal names resolve on its
// d.vars.
// ---------------------------------------------------------------------------

struct PlasticityHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{noCell};
  CellRef imageBoundary{noCell};
  CellRef agentCell{noCell};
  CellRef ownModule{noCell};
  MetaBindSet binds;
  std::optional<MetaRuntime> meta;

  explicit PlasticityHarness(bool lineage = true) {
    programRoot   = arena.makeCell();
    imageBoundary = arena.makeCell();
    agentCell     = arena.makeCell();
    auto minted   = mintMetaBindSet(arena, core, programRoot, agentCell);
    EXPECT_TRUE(minted.has_value()) << (minted ? "" : minted.error());
    if (!minted) return;
    binds = *minted;
    if (lineage)
      binds.lineage = MetaLineage{
          programRoot, static_cast<std::uint32_t>(
                           arena.denseOf(imageBoundary).value_or(0) + 1)};
    ownModule       = mintModule("demo");
    binds.ownModule = ownModule;
    meta.emplace(core, vm, binds);
  }
  MetaRuntime &rt() { return *meta; }

  /// A module cell on the program root's +d.stdlib — program-side by
  /// reachability, minted after imageBoundary.
  CellRef mintModule(std::string_view name) {
    const auto module = arena.makeCell(name);
    zigzag::expectWritten(
        arena.link(programRoot, core.dims().stdlib, Posward, module));
    return module;
  }
  /// A symbol cell (content = name) on a module's +d.vars tail.
  CellRef mintSymbol(CellRef module, std::string_view name) {
    const auto symbol = arena.makeCell(name);
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, module, core.dims().vars),
                   core.dims().vars, Posward, symbol));
    return symbol;
  }
};

struct FixtureHead {
  CellRef head{noCell};
  CellRef entry{noCell};
};

/// A self-contained fragment head — the mutation suite's shape: +d.entry,
/// a descriptor carrying result/produced/pure/closed/arity plus the
/// ordered param chain, and a minimal +d.recipe (two literals and the
/// entry-edge tuple) that reifies into a head with its own entry twin.
FixtureHead
mintFragmentHead(PlasticityHarness &h, OpcodeKind op, std::string_view label,
                 bool pure, std::string_view resultType = "()",
                 std::vector<std::pair<std::string, std::string>> params = {}) {
  const auto entry = h.vm.mintOpcode(op);
  const auto head  = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(head, h.binds.dims.entry, Posward, entry));
  zigzag::expectWritten(
      h.arena.link(head, h.core.dims().name, Posward, h.arena.makeCell(label)));

  const auto &dims      = h.core.dims();
  const auto descriptor = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(head, dims.values, Posward, descriptor));
  const auto field = [&](std::string_view key, CellRef value) {
    const auto rec = h.arena.makeCell(key);
    zigzag::expectWritten(h.arena.link(rec, dims.values, Posward, value));
    zigzag::expectWritten(
        h.arena.link(zigzag::rankTail(h.arena, descriptor, dims.vars),
                     dims.vars, Posward, rec));
    return rec;
  };
  const auto resultRecord = field("result", h.arena.makeCell());
  zigzag::expectWritten(h.arena.link(resultRecord, dims.name, Posward,
                                     h.arena.makeCell(resultType)));
  field("produced", h.arena.makeCell());
  field("pure", h.arena.makeScalarCell(pure));
  field("closed", h.arena.makeScalarCell(true));
  field("arity",
        h.arena.makeScalarCell(static_cast<std::int64_t>(params.size())));
  CellRef tail = noCell;
  for (const auto &[pname, ptype] : params) {
    const auto cell = h.arena.makeCell();
    zigzag::expectWritten(
        h.arena.link(cell, dims.name, Posward, h.arena.makeCell(pname)));
    zigzag::expectWritten(
        h.arena.link(cell, dims.values, Posward, h.arena.makeCell(ptype)));
    if (tail == noCell)
      zigzag::expectWritten(h.arena.link(descriptor, dims.grab, Posward, cell));
    else
      zigzag::expectWritten(h.arena.link(tail, dims.step, Posward, cell));
    tail = cell;
  }

  // Recipe: literal(head), literal(entry), the entry-edge tuple.
  const auto member = [&](CellRef pos) {
    return h.arena.makeScalarCell(static_cast<std::int64_t>(pos));
  };
  const auto anchor = member(h.binds.dims.entry);
  zigzag::expectWritten(
      h.arena.link(anchor, dims.name, Posward, h.arena.makeCell("d.entry")));
  const CellRef recipe[] = {member(head),
                            member(entry),
                            member(head),
                            anchor,
                            h.arena.makeScalarCell(std::int64_t{1}),
                            member(entry)};
  zigzag::expectWritten(
      h.arena.link(head, h.binds.dims.recipe, Posward, recipe[0]));
  for (std::size_t i = 0; i + 1 < std::size(recipe); ++i)
    zigzag::expectWritten(
        h.arena.link(recipe[i], dims.step, Posward, recipe[i + 1]));
  return {head, entry};
}

/// A fixture head whose recipe covers its whole emitted shape — label
/// cell, entry op, descriptor and every field, param cells included —
/// so a reified copy carries the same apparatus (what `code"…"`
/// emission produces; the minimal recipe above twins only head and
/// entry). Needed wherever the *copy* is the graft argument:
/// graft_plan's signature gates read the arg's own descriptor.
FixtureHead mintSelfDescribingHead(
    PlasticityHarness &h, OpcodeKind op, std::string_view label, bool pure,
    std::string_view resultType,
    std::vector<std::pair<std::string, std::string>> params = {}) {
  const auto &dims     = h.core.dims();
  const auto entry     = h.vm.mintOpcode(op);
  const auto head      = h.arena.makeCell();
  const auto labelCell = h.arena.makeCell(label);
  zigzag::expectWritten(h.arena.link(head, h.binds.dims.entry, Posward, entry));
  zigzag::expectWritten(h.arena.link(head, dims.name, Posward, labelCell));

  const auto descriptor = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(head, dims.values, Posward, descriptor));
  std::vector<std::pair<CellRef, CellRef>> records; // {record, value}
  const auto field = [&](std::string_view key, CellRef value) {
    const auto rec = h.arena.makeCell(key);
    zigzag::expectWritten(h.arena.link(rec, dims.values, Posward, value));
    zigzag::expectWritten(
        h.arena.link(zigzag::rankTail(h.arena, descriptor, dims.vars),
                     dims.vars, Posward, rec));
    records.emplace_back(rec, value);
    return rec;
  };
  const auto resultVal   = h.arena.makeCell();
  const auto resultType_ = h.arena.makeCell(resultType);
  const auto resultRec   = field("result", resultVal);
  zigzag::expectWritten(
      h.arena.link(resultRec, dims.name, Posward, resultType_));
  field("produced", h.arena.makeCell());
  field("pure", h.arena.makeScalarCell(pure));
  field("closed", h.arena.makeScalarCell(true));
  field("arity",
        h.arena.makeScalarCell(static_cast<std::int64_t>(params.size())));

  struct ParamCells {
    CellRef cell{noCell}, name{noCell}, type{noCell};
  };
  std::vector<ParamCells> paramCells;
  CellRef paramTail = noCell;
  for (const auto &[pname, ptype] : params) {
    ParamCells pc{.cell = h.arena.makeCell(),
                  .name = h.arena.makeCell(pname),
                  .type = h.arena.makeCell(ptype)};
    zigzag::expectWritten(h.arena.link(pc.cell, dims.name, Posward, pc.name));
    zigzag::expectWritten(h.arena.link(pc.cell, dims.values, Posward, pc.type));
    if (paramTail == noCell)
      zigzag::expectWritten(
          h.arena.link(descriptor, dims.grab, Posward, pc.cell));
    else
      zigzag::expectWritten(
          h.arena.link(paramTail, dims.step, Posward, pc.cell));
    paramTail = pc.cell;
    paramCells.push_back(pc);
  }

  // Recipe members: literals for every cell, then (from, anchor, dir,
  // to) tuples for every edge — the emission taxonomy verbatim.
  std::vector<CellRef> members;
  const auto member = [&](CellRef pos) {
    const auto m = h.arena.makeScalarCell(static_cast<std::int64_t>(pos));
    members.push_back(m);
    return m;
  };
  const auto anchor = [&](DimRef dim, std::string_view dimName) {
    const auto m = member(dim);
    zigzag::expectWritten(
        h.arena.link(m, dims.name, Posward, h.arena.makeCell(dimName)));
  };
  const auto edge = [&](CellRef from, DimRef dim, std::string_view dimName,
                        CellRef to) {
    member(from);
    anchor(dim, dimName);
    members.push_back(h.arena.makeScalarCell(std::int64_t{1}));
    member(to);
  };
  member(head);
  member(labelCell);
  member(entry);
  member(descriptor);
  for (const auto &[rec, val] : records) {
    member(rec);
    member(val);
  }
  member(resultType_);
  for (const auto &pc : paramCells) {
    member(pc.cell);
    member(pc.name);
    member(pc.type);
  }
  edge(head, dims.name, "d.name", labelCell);
  edge(head, h.binds.dims.entry, "d.entry", entry);
  edge(head, dims.values, "d.values", descriptor);
  // The d.vars chain links record-tail to record-tail: descriptor's one
  // posward edge names the first record only.
  CellRef recPrev = descriptor;
  for (const auto &[rec, val] : records) {
    edge(recPrev, dims.vars, "d.vars", rec);
    edge(rec, dims.values, "d.values", val);
    if (rec == resultRec) edge(rec, dims.name, "d.name", resultType_);
    recPrev = rec;
  }
  for (std::size_t i = 0; i < paramCells.size(); ++i) {
    const auto &pc = paramCells[i];
    if (i == 0) edge(descriptor, dims.grab, "d.grab", pc.cell);
    edge(pc.cell, dims.name, "d.name", pc.name);
    edge(pc.cell, dims.values, "d.values", pc.type);
    if (i + 1 < paramCells.size())
      edge(pc.cell, dims.step, "d.step", paramCells[i + 1].cell);
  }
  zigzag::expectWritten(
      h.arena.link(head, h.binds.dims.recipe, Posward, members.front()));
  for (std::size_t i = 0; i + 1 < members.size(); ++i)
    zigzag::expectWritten(
        h.arena.link(members[i], dims.step, Posward, members[i + 1]));
  return {head, entry};
}

/// A declared clause on `host`'s +d.clause rank: +d.name label (skipped
/// when empty — a name-less clause), +d.grab the condition head,
/// +d.spin the action head — §8.2's one rule.
CellRef mintClause(PlasticityHarness &h, CellRef host, std::string_view label,
                   CellRef test, CellRef body) {
  const auto clause = h.arena.makeCell();
  if (!label.empty())
    zigzag::expectWritten(h.arena.link(clause, h.core.dims().name, Posward,
                                       h.arena.makeCell(label)));
  if (test != noCell)
    zigzag::expectWritten(
        h.arena.link(clause, h.core.dims().grab, Posward, test));
  if (body != noCell)
    zigzag::expectWritten(
        h.arena.link(clause, h.core.dims().spin, Posward, body));
  zigzag::expectWritten(
      h.arena.link(zigzag::rankTail(h.arena, host, h.core.dims().clause),
                   h.core.dims().clause, Posward, clause));
  return clause;
}

/// The agent's `d.vars` record whose content is `key` verbatim — §8.1's
/// `cell +d.vars → [key] +d.values → scalar` record convention.
std::optional<CellRef>
varsRecordOf(ArenaManifold &arena, CellRef owner,
             const zigzag::vortex::SystemDimensions &dims,
             std::string_view key) {
  for (const auto rec : zigzag::rankAfter(arena, owner, dims.vars))
    if (arena.textOf(rec) == key) return rec;
  return std::nullopt;
}

/// A junction's `from`/`to` descriptor field — endpoint labels are
/// qualified-name *strings* (content on a d.vars record's +d.values),
/// never links (§8.1: the eviction problem stays off the endpoints).
std::optional<std::string>
junctionField(ArenaManifold &arena, CellRef junction,
              const zigzag::vortex::SystemDimensions &dims,
              std::string_view key) {
  const auto record = varsRecordOf(arena, junction, dims, key);
  if (!record) return std::nullopt;
  const auto value = arena.linked(*record, dims.values, Posward);
  if (!value) return std::nullopt;
  return std::string{arena.textOf(*value)};
}

/// The agent's +d.synapse rank — the junction cells §8.3's loop walks.
std::vector<CellRef> junctions(ArenaManifold &arena, CellRef agentCell,
                               DimRef synapse) {
  std::vector<CellRef> out;
  for (const auto cell : zigzag::rankAfter(arena, agentCell, synapse))
    out.push_back(cell);
  return out;
}

/// A runnable fitness fragment for §8.5's `run(fitness, [candidate])`:
/// the canonical copy ops write `score` into the descriptor's result
/// cell and `true` into produced, then a Return tail — the shape the
/// run convention spills, calls and restores. One `c: Cell` parameter
/// so the candidate binds name-keyed like every run argument. The score
/// is fixture-fixed: at engine level the fragment stands in for "a
/// `def`, a `succeed when` clause, or a `run` of a `Code<Float>`
/// fitness fragment" — the loop's bounded in-turn run is what's under
/// test.
FixtureHead mintFitnessHead(PlasticityHarness &h, double score,
                            std::string_view label) {
  const auto &dims    = h.core.dims();
  const auto result   = h.arena.makeCell();
  const auto produced = h.arena.makeScalarCell(false);
  const auto zero     = h.arena.makeScalarCell(std::int64_t{0});
  const auto minusOne = h.arena.makeScalarCell(std::int64_t{-1});
  const auto scoreBox = h.arena.makeScalarCell(score);
  const auto yes      = h.arena.makeScalarCell(true);

  const auto copyValue = [&](CellRef target, CellRef source) {
    const auto op = h.vm.mintOpcode(OpcodeKind::Value);
    h.core.bindInput(op, target);
    h.core.bindInput(op, zero);
    h.core.bindInput(op, minusOne);
    h.core.bindInput(op, source);
    h.core.bindOutput(op, h.arena.makeCell());
    return op;
  };
  const auto writeResult   = copyValue(result, scoreBox);
  const auto writeProduced = copyValue(produced, yes);
  const auto ret           = h.vm.mintOpcode(OpcodeKind::Return);
  zigzag::expectWritten(
      h.arena.link(writeResult, dims.spin, Posward, writeProduced));
  zigzag::expectWritten(h.arena.link(writeProduced, dims.spin, Posward, ret));

  const auto head = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(head, h.binds.dims.entry, Posward, writeResult));
  zigzag::expectWritten(
      h.arena.link(head, dims.name, Posward, h.arena.makeCell(label)));
  const auto descriptor = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(head, dims.values, Posward, descriptor));
  const auto field = [&](std::string_view key, CellRef value) {
    const auto rec = h.arena.makeCell(key);
    zigzag::expectWritten(h.arena.link(rec, dims.values, Posward, value));
    zigzag::expectWritten(
        h.arena.link(zigzag::rankTail(h.arena, descriptor, dims.vars),
                     dims.vars, Posward, rec));
    return rec;
  };
  field("result", result);
  field("produced", produced);
  field("arity", h.arena.makeScalarCell(std::int64_t{1}));
  const auto param = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(param, dims.name, Posward, h.arena.makeCell("c")));
  zigzag::expectWritten(
      h.arena.link(param, dims.values, Posward, h.arena.makeCell("Cell")));
  zigzag::expectWritten(h.arena.link(descriptor, dims.grab, Posward, param));
  return {head, writeResult};
}

/// The clauses on `host` whose +d.name starts `patch:` — the learned
/// half of a clause bank; the serial is a session draw, so tests key on
/// the prefix, never the number.
std::vector<CellRef> patchClauses(ArenaManifold &arena, CellRef host,
                                  DimRef clauseDim, DimRef name) {
  std::vector<CellRef> out;
  for (const auto c : zigzag::rankAfter(arena, host, clauseDim)) {
    const auto label = arena.linked(c, name, Posward);
    if (label && arena.textOf(*label).starts_with("patch:")) out.push_back(c);
  }
  return out;
}

/// The d.patch rank's newest record, or noCell.
CellRef latestPatch(ArenaManifold &arena, CellRef agentCell, DimRef patch) {
  return zigzag::rankTail(arena, agentCell, patch);
}

/// The head names a patch records on its +d.patch-head chain, in order —
/// the registered-head identities genesis re-resolves by name.
std::vector<std::string>
patchHeadNames(ArenaManifold &arena,
               const zigzag::vortex::SystemDimensions &dims, DimRef patchHead,
               CellRef patch) {
  std::vector<std::string> out;
  for (auto rec = arena.linked(patch, patchHead, Posward);
       rec && arena.contains(*rec);
       rec = arena.linked(*rec, dims.step, Posward)) {
    const auto name = arena.linked(*rec, dims.values, Posward);
    out.push_back(name ? std::string{arena.textOf(*name)} : std::string{});
  }
  return out;
}

// ===========================================================================
// §8.1 — weights and synapses are cells and ranks
// ===========================================================================

TEST(VagentPlasticityTest, SynapseJunctionsOwnDistinctFloatCells) {
  PlasticityHarness h;
  // sprout_synapse mints a junction on the agent's +d.synapse rank:
  // endpoint labels as qualified-name strings on its descriptor, plus
  // its own +d.weight scalar — never a label cell, so a junction is
  // never itself an endpoint name.
  const auto j = h.rt().sproutSynapse("demo:Feed.a", "demo:Feed.b", 0.25);
  ASSERT_TRUE(j.has_value()) << j.error();
  const auto &dims = h.core.dims();
  EXPECT_EQ(junctions(h.arena, h.agentCell, h.binds.dims.synapse).size(), 1U);
  EXPECT_EQ(junctionField(h.arena, *j, dims, "from"), "demo:Feed.a");
  EXPECT_EQ(junctionField(h.arena, *j, dims, "to"), "demo:Feed.b");
  EXPECT_FALSE(h.rt().labelOf(*j).has_value())
      << "a junction carries no +d.name — it is never an endpoint name";

  // The junction arm of weight_of reads the junction's own +d.weight
  // scalar — a Float carried as the cell's bits, never an edge record.
  const auto first = h.arena.linked(*j, h.binds.dims.weight, Posward);
  ASSERT_TRUE(first.has_value());
  EXPECT_DOUBLE_EQ(h.arena.asDouble(*first).value_or(0.0), 0.25);
  EXPECT_EQ(h.rt().weightOf(*j), 0.25);

  // set_weight's junction arm mints a fresh scalar on that edge —
  // mint-and-repoint, never an in-place write.
  ASSERT_TRUE(h.rt().setWeight(*j, 0.9).has_value());
  const auto second = h.arena.linked(*j, h.binds.dims.weight, Posward);
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_EQ(h.rt().weightOf(*j), 0.9);

  // Each junction owns a *distinct* FloatCell by mechanics: the scalar's
  // -d.weight is single-occupancy, so a second junction taking the
  // first's scalar evicts the first's edge — the spec's reason a
  // synapse is a cell, not a bare A → B link.
  const auto j2 = h.rt().sproutSynapse("demo:Feed.a", "demo:Feed.c", 0.5);
  ASSERT_TRUE(j2.has_value());
  const auto w1 = h.arena.linked(*j, h.binds.dims.weight, Posward);
  const auto w2 = h.arena.linked(*j2, h.binds.dims.weight, Posward);
  ASSERT_TRUE(w1.has_value() && w2.has_value());
  EXPECT_NE(*w1, *w2);
  zigzag::expectWritten(h.arena.link(*j2, h.binds.dims.weight, Posward, *w1));
  EXPECT_FALSE(h.arena.linked(*j, h.binds.dims.weight, Posward).has_value())
      << "W's one -d.weight slot evicted the first junction's edge";
  EXPECT_FALSE(h.rt().weightOf(*j).has_value())
      << "a weight-less, name-less junction disengages weight_of";
}

TEST(VagentPlasticityTest, SynapseRankCarriesFanInAndFanOut) {
  PlasticityHarness h;
  // Fan-in is N junctions naming the same `to` label; fan-out likewise —
  // the endpoints are strings, so sharing a name mints no shared cell.
  for (const auto &[from, to] : {std::pair{"demo:a", "demo:shared"},
                                 {"demo:b", "demo:shared"},
                                 {"demo:shared", "demo:c"}})
    ASSERT_TRUE(h.rt().sproutSynapse(from, to, 0.1).has_value());
  const auto rank = junctions(h.arena, h.agentCell, h.binds.dims.synapse);
  ASSERT_EQ(rank.size(), 3U);
  const auto &dims = h.core.dims();
  EXPECT_EQ(junctionField(h.arena, rank[0], dims, "to"), "demo:shared");
  EXPECT_EQ(junctionField(h.arena, rank[1], dims, "to"), "demo:shared");
  EXPECT_EQ(junctionField(h.arena, rank[2], dims, "from"), "demo:shared");
}

TEST(VagentPlasticityTest, ClauseWeightsKeyTheAgentsVarsRecords) {
  PlasticityHarness h;
  const auto feed   = h.mintSymbol(h.ownModule, "Feed");
  const auto clause = mintClause(h, feed, "demo:Feed.a", noCell, noCell);
  const auto &dims  = h.core.dims();

  // weight_of disengages on no record — the [w] presence test in §9.3's
  // else arm reads exactly this.
  EXPECT_FALSE(h.rt().weightOf(clause).has_value());
  EXPECT_FALSE(
      varsRecordOf(h.arena, h.agentCell, dims, "demo:Feed.a").has_value());

  // First write *seeds*: the agent's d.vars record keyed by the clause's
  // qualified +d.name is minted, then its value slot written.
  ASSERT_TRUE(h.rt().setWeight(clause, 0.7).has_value());
  const auto record = varsRecordOf(h.arena, h.agentCell, dims, "demo:Feed.a");
  ASSERT_TRUE(record.has_value())
      << "the clause arm keys a d.vars record on the agent, never an "
         "edge on the clause";
  const auto value = h.arena.linked(*record, dims.values, Posward);
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(h.arena.asDouble(*value).value_or(0.0), 0.7);
  EXPECT_EQ(h.rt().weightOf(clause), 0.7);

  // A second write re-points the same record — never a second record.
  ASSERT_TRUE(h.rt().setWeight(clause, 0.2).has_value());
  EXPECT_EQ(varsRecordOf(h.arena, h.agentCell, dims, "demo:Feed.a"), record);
  EXPECT_EQ(h.rt().weightOf(clause), 0.2);

  // A +d.name-bearing target that is neither junction nor clause — a
  // fragment head's label — keys a d.vars record by that label verbatim.
  const auto head = mintFragmentHead(h, OpcodeKind::Return, "frag-head",
                                     /*pure=*/true);
  ASSERT_TRUE(h.rt().setWeight(head.head, 0.3).has_value());
  EXPECT_TRUE(
      varsRecordOf(h.arena, h.agentCell, dims, "frag-head").has_value());
  EXPECT_EQ(h.rt().weightOf(head.head), 0.3);

  // The refusals: a +d.name-less clause resolves no key; a bare cell
  // (op cell, program structure — the places §8.1 says weights never
  // live) likewise.
  const auto anonymous = mintClause(h, feed, "", noCell, noCell);
  EXPECT_FALSE(h.rt().setWeight(anonymous, 0.5).has_value());
  EXPECT_FALSE(h.rt().setWeight(h.arena.makeCell(), 0.5).has_value());
}

// ===========================================================================
// §8.2 — the plan library is the synaptic structure
// ===========================================================================

TEST(VagentPlasticityTest, PlanLibraryIsTheSynapticStructure) {
  PlasticityHarness h;
  const auto feed  = h.mintSymbol(h.ownModule, "Feed");
  const auto &dims = h.core.dims();

  // A goal's d.clause rank is a bank of candidate rules: each clause's
  // where test (+d.grab) is the condition, its +d.spin body the action.
  const auto mkRule = [&](std::string_view label) {
    const auto test =
        mintFragmentHead(h, OpcodeKind::Return, "src:t", true, "Bool");
    const auto body = mintFragmentHead(h, OpcodeKind::Return, "src:b", false);
    return mintClause(h, feed, label, test.head, body.head);
  };
  const auto alpha = mkRule("demo:Feed.alpha");
  const auto beta  = mkRule("demo:Feed.beta");

  // Plan selection reads the bank in declaration order — clauses_of is
  // the engine-level spelling of that walk.
  ASSERT_EQ(h.rt().clausesOf(feed).size(), 2U);
  EXPECT_EQ(h.rt().clausesOf(feed)[0], alpha);
  EXPECT_EQ(h.rt().clausesOf(feed)[1], beta);

  // The condition/action halves resolve through the reflect wrappers —
  // a wrapper head delegates +d.entry to its attach, never to the root
  // op directly (single-occupancy would evict the attach's own edge).
  const auto grab = h.arena.linked(alpha, dims.grab, Posward);
  const auto spin = h.arena.linked(alpha, dims.spin, Posward);
  ASSERT_TRUE(grab.has_value() && spin.has_value());
  const auto wrappedTest = h.rt().testOf(alpha);
  const auto wrappedBody = h.rt().bodyOf(alpha);
  ASSERT_TRUE(wrappedTest.has_value() && wrappedBody.has_value());
  EXPECT_EQ(h.arena.linked(*wrappedTest, h.binds.dims.entry, Posward), grab);
  EXPECT_EQ(h.arena.linked(*wrappedBody, h.binds.dims.entry, Posward), spin);

  // A weight record naming the clause is its strength — one keyed
  // record per clause on the agent's d.vars.
  ASSERT_TRUE(h.rt().setWeight(alpha, 0.4).has_value());
  ASSERT_TRUE(h.rt().setWeight(beta, 0.6).has_value());
  EXPECT_EQ(h.rt().weightOf(alpha), 0.4);
  EXPECT_EQ(h.rt().weightOf(beta), 0.6);
  EXPECT_TRUE(
      varsRecordOf(h.arena, h.agentCell, dims, "demo:Feed.alpha").has_value());

  // Growing a rule is learn_plan — appended at the tail of the bank.
  const auto test =
      mintFragmentHead(h, OpcodeKind::Return, "src:near", true, "Bool");
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "src:go", false);
  const auto learned =
      h.rt().learnPlan("demo:Feed", "near_water", test.head, body.head);
  ASSERT_TRUE(learned.has_value()) << learned.error();
  ASSERT_EQ(h.rt().clausesOf(feed).size(), 3U);
  EXPECT_EQ(h.rt().clausesOf(feed).back(), *learned)
      << "a learned clause appends at the rank tail";
  const auto learnedLabel = h.rt().labelOf(*learned);
  ASSERT_TRUE(learnedLabel.has_value());
  EXPECT_TRUE(learnedLabel->starts_with("patch:"));

  // Strengthening is set_weight on the learned clause too — keyed by
  // the minted patch:<n> label.
  ASSERT_TRUE(h.rt().setWeight(*learned, 0.1).has_value());
  EXPECT_EQ(h.rt().weightOf(*learned), 0.1);

  // Pruning is prune/unlearn: declared and learned alike leave the bank
  // with rank order repaired.
  ASSERT_TRUE(h.rt().prune(beta).has_value());
  EXPECT_EQ(h.rt().clausesOf(feed).size(), 2U);
  EXPECT_EQ(h.rt().clausesOf(feed).front(), alpha);
  ASSERT_TRUE(h.rt().unlearn(*learned).has_value());
  ASSERT_EQ(h.rt().clausesOf(feed).size(), 1U);
  EXPECT_EQ(h.rt().clausesOf(feed).front(), alpha);
}

// ===========================================================================
// §8.3 + §9.3 — Hebbian growth cycles
// ===========================================================================

TEST(VagentPlasticityTest, HebbianGrowthOfARuleRank) {
  PlasticityHarness h;
  h.mintSymbol(h.ownModule, "Feed");
  const auto &dims = h.core.dims();

  // §9.3's `on Sighting` body, minus the gated `believes` guard: grow a
  // candidate rule into the Feed rank.
  const auto grow = [&](std::string_view alias) {
    const auto test =
        mintFragmentHead(h, OpcodeKind::Return, "src:t", true, "Bool");
    const auto body   = mintFragmentHead(h, OpcodeKind::Return, "src:b", false);
    const auto clause = h.rt().learnPlan("Feed", alias, test.head, body.head);
    EXPECT_TRUE(clause.has_value()) << clause.error();
    return clause;
  };
  ASSERT_TRUE(grow("near_water").has_value());
  ASSERT_TRUE(grow("near_shade").has_value());
  const auto goal = h.rt().symbolNamed("demo", "Feed");
  ASSERT_TRUE(goal.has_value());
  ASSERT_EQ(h.rt().clausesOf(*goal).size(), 2U);

  // §9.3's `on Meal` body verbatim at engine level: for c in
  // clauses_of(symbol_named("demo","Feed")) — weight_of disengaged →
  // seed 0.1, else reinforce w + 0.1. All scalar writes, all trailed.
  constexpr double kLr = 0.1;
  const auto reinforce = [&] {
    for (const auto c : h.rt().clausesOf(*goal)) {
      const auto w = h.rt().weightOf(c);
      if (w)
        EXPECT_TRUE(h.rt().setWeight(c, *w + kLr).has_value());
      else
        EXPECT_TRUE(h.rt().setWeight(c, kLr).has_value());
    }
  };
  reinforce();
  for (const auto c : h.rt().clausesOf(*goal))
    EXPECT_DOUBLE_EQ(h.rt().weightOf(c).value_or(0.0), 0.1)
        << "first write seeds the record the else arm mints";
  reinforce();
  for (const auto c : h.rt().clausesOf(*goal))
    EXPECT_DOUBLE_EQ(h.rt().weightOf(c).value_or(0.0), 0.2)
        << "the Hebbian update is visible through weight_of";

  // …and through the records themselves: keyed by the patch:<n> label,
  // mint-and-repoint — the old value cell is never rewritten in place.
  const auto clause = h.rt().clausesOf(*goal).front();
  const auto label  = h.rt().labelOf(clause);
  ASSERT_TRUE(label.has_value());
  const auto record = varsRecordOf(h.arena, h.agentCell, dims, *label);
  ASSERT_TRUE(record.has_value());
  const auto value = h.arena.linked(*record, dims.values, Posward);
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(h.arena.asDouble(*value).value_or(0.0), 0.2);
}

TEST(VagentPlasticityTest, HebbianCycleOverTheSynapseRank) {
  PlasticityHarness h;
  // §8.3's loop: for s in cells"$a/+d.synapse" — an ordinary rank of
  // junction cells; the update is the focus-slot write through s, not
  // to the box.
  ASSERT_TRUE(
      h.rt().sproutSynapse("demo:p.food", "demo:Feed.a", 0.05).has_value());
  ASSERT_TRUE(
      h.rt().sproutSynapse("demo:p.water", "demo:Feed.a", 0.3).has_value());
  ASSERT_TRUE(
      h.rt().sproutSynapse("demo:p.food", "demo:Feed.b", 0.6).has_value());

  constexpr double kLr = 0.1;
  const auto expected  = {0.15, 0.4, 0.7};
  for (const auto s : junctions(h.arena, h.agentCell, h.binds.dims.synapse)) {
    const auto w = h.rt().weightOf(s);
    ASSERT_TRUE(w.has_value());
    EXPECT_TRUE(h.rt().setWeight(s, *w + kLr).has_value());
  }
  const auto rank = junctions(h.arena, h.agentCell, h.binds.dims.synapse);
  auto want       = expected.begin();
  for (const auto s : rank)
    EXPECT_DOUBLE_EQ(h.rt().weightOf(s).value_or(0.0), *want++);
}

// ===========================================================================
// §8.5 — searching programs is searching cells
// ===========================================================================

TEST(VagentPlasticityTest, ProgramSearchIsCellSearch) {
  PlasticityHarness h;
  const auto feed  = h.mintSymbol(h.ownModule, "Feed");
  const auto &dims = h.core.dims();

  // The incumbent rule on the bank, weighted — the bar candidates score
  // against.
  const auto incTest =
      mintFragmentHead(h, OpcodeKind::Return, "src:it", true, "Bool");
  const auto incBody = mintFragmentHead(h, OpcodeKind::Return, "src:ib", false);
  const auto incumbent =
      mintClause(h, feed, "demo:Feed.incumbent", incTest.head, incBody.head);
  ASSERT_TRUE(h.rt().setWeight(incumbent, 0.5).has_value());

  const auto baseRule =
      mintSelfDescribingHead(h, OpcodeKind::Return, "src:rule", true, "Bool");
  const auto candBody =
      mintFragmentHead(h, OpcodeKind::Return, "src:cb", false);
  const auto perturbed =
      mintFragmentHead(h, OpcodeKind::Return, "src:p", true, "Bool");
  const auto fitnessLow  = mintFitnessHead(h, 0.4, "fitness-low");
  const auto fitnessHigh = mintFitnessHead(h, 0.9, "fitness-high");

  // candidate := reify(base_rule) — a fresh closed copy with a fresh
  // head; the search loop's first step runs outside the mark so the
  // candidate survives a failed trial.
  const auto candidate = h.rt().reify(baseRule.head);
  ASSERT_TRUE(candidate.has_value()) << candidate.error();
  EXPECT_NE(*candidate, baseRule.head);
  EXPECT_TRUE(h.rt().labelOf(*candidate).value_or("").starts_with("reify:"));
  EXPECT_TRUE(h.rt().verifyClosed(*candidate).closed)
      << "the candidate is a closed copy — §8.5's first step";

  // -- Attempt A: score < incumbent → fail, backtrack drains ----------
  const auto mark = h.arena.mark();
  const auto clauseA =
      h.rt().graftPlan("Feed", "candidate", *candidate, candBody.head);
  ASSERT_TRUE(clauseA.has_value()) << clauseA.error();
  // Mutate acts on the INSTALLED clause cell, not the Code.
  ASSERT_TRUE(h.rt().graftWhere(*clauseA, perturbed.head).has_value());
  ASSERT_TRUE(h.rt().setWeight(*clauseA, 0.9).has_value());
  const auto low = h.rt().runFragment(
      fitnessLow.head, {CellValue{static_cast<std::int64_t>(*candidate)}});
  EXPECT_EQ(low.refusal, MetaRunOutcome::Refusal::None) << low.detail;
  EXPECT_TRUE(low.produced);
  ASSERT_TRUE(low.result.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(*low.result), 0.4);

  h.arena.release(mark); // fail — drains to the pre-graft mark.
  EXPECT_EQ(h.rt().clausesOf(feed).size(), 1U)
      << "the rejected candidate's clause is gone, incumbent stands";
  EXPECT_FALSE(
      varsRecordOf(h.arena, h.agentCell, dims, "candidate").has_value())
      << "the in-mark weight record drained with the branch";
  // Free undo: the install consumption links trailed away — the same
  // candidate and perturbation graft cleanly on the retry.
  const auto retry =
      h.rt().graftPlan("Feed", "candidate", *candidate, candBody.head);
  ASSERT_TRUE(retry.has_value()) << retry.error();
  ASSERT_TRUE(h.rt().graftWhere(*retry, perturbed.head).has_value());

  // -- Attempt B: score >= incumbent → cut, candidate stands ----------
  const auto markB = h.arena.mark();
  ASSERT_TRUE(h.rt().setWeight(*retry, 0.9).has_value());
  const auto high = h.rt().runFragment(
      fitnessHigh.head, {CellValue{static_cast<std::int64_t>(*candidate)}});
  EXPECT_EQ(high.refusal, MetaRunOutcome::Refusal::None) << high.detail;
  EXPECT_TRUE(high.produced);
  ASSERT_TRUE(high.result.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(*high.result), 0.9);

  h.arena.discard(markB); // cut — the mark's bindings stand.
  EXPECT_EQ(h.rt().clausesOf(feed).size(), 2U);
  EXPECT_EQ(h.rt().clausesOf(feed).back(), *retry);
  EXPECT_EQ(h.rt().weightOf(*retry), 0.9);
}

// ===========================================================================
// §9.2 — a plan that tightens its own where
// ===========================================================================

TEST(VagentPlasticityTest, LearnWhereTightensAPlansOwnWhere) {
  PlasticityHarness h;
  const auto guard = h.mintSymbol(h.ownModule, "Guard");
  const auto &dims = h.core.dims();
  const auto original =
      mintFragmentHead(h, OpcodeKind::Return, "src:p07", true, "Bool");
  const auto clause =
      mintClause(h, guard, "demo:Guard.cautious", original.head, noCell);

  // plan_named resolves the clause by its qualified name — the §9.2
  // handler's lookup.
  const auto found = h.rt().planNamed("demo:Guard.cautious");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, clause);

  // learn_where validates, re-points +d.grab at the tightened test's
  // installed copy, and records the patch.
  const auto tighter =
      mintFragmentHead(h, OpcodeKind::Return, "src:p09", true, "Bool");
  ASSERT_TRUE(h.rt().learnWhere(*found, tighter.head).has_value());
  const auto grabbed = h.arena.linked(clause, dims.grab, Posward);
  ASSERT_TRUE(grabbed.has_value());
  EXPECT_NE(*grabbed, tighter.head);
  EXPECT_NE(*grabbed, original.head);
  EXPECT_TRUE(h.rt().labelOf(*grabbed).value_or("").starts_with("reify:"));

  // The replaced test was program-original: the patch records no prior
  // head — genesis restores the skeleton's own attachment for free, and
  // unlearn simply detaches. The +d.patch-head chain names the one
  // registered head this install produced.
  const auto patch = latestPatch(h.arena, h.agentCell, h.binds.dims.patch);
  ASSERT_NE(patch, noCell);
  EXPECT_EQ(patchHeadNames(h.arena, dims, h.binds.dims.patchHead, patch).size(),
            1U)
      << "a program-original attach records no prior head";

  // The change affects the *next* plan selection: where is evaluated
  // per selection, and test_of now wraps the tightened attach — a
  // label-less memo miss on the old key would hang, so the new copy's
  // reify: label keys a fresh wrapper.
  const auto wrapped = h.rt().testOf(clause);
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(h.arena.linked(*wrapped, h.binds.dims.entry, Posward), grabbed);

  // A learned attach *does* record its prior: a second learn_where's
  // chain names the fresh copy plus the first's registered head.
  const auto tightest =
      mintFragmentHead(h, OpcodeKind::Return, "src:p95", true, "Bool");
  ASSERT_TRUE(h.rt().learnWhere(clause, tightest.head).has_value());
  const auto patch2 = latestPatch(h.arena, h.agentCell, h.binds.dims.patch);
  ASSERT_NE(patch2, noCell);
  EXPECT_NE(patch2, patch);
  EXPECT_EQ(
      patchHeadNames(h.arena, dims, h.binds.dims.patchHead, patch2).size(), 2U)
      << "replacing a learned attach records the prior's registered head";

  // Pure-required: the condition structure can grow and re-point but
  // cannot itself act — an impure-flagged test refuses the where arm.
  const auto impure =
      mintFragmentHead(h, OpcodeKind::Return, "src:act", false, "Bool");
  EXPECT_FALSE(h.rt().learnWhere(clause, impure.head).has_value());
  EXPECT_FALSE(h.rt().graftWhere(clause, impure.head).has_value());

  // Unlearn the second re-point: a reified prior restores. Unlearn the
  // first: the program-original attach is not re-faked — it detaches
  // and genesis restores the skeleton's own attachment.
  ASSERT_TRUE(h.rt().unlearn(clause).has_value());
  const auto restored = h.arena.linked(clause, dims.grab, Posward);
  ASSERT_TRUE(restored.has_value());
  EXPECT_TRUE(h.rt().labelOf(*restored).value_or("").starts_with("reify:"));
  ASSERT_TRUE(h.rt().unlearn(clause).has_value());
  EXPECT_FALSE(h.arena.linked(clause, dims.grab, Posward).has_value());
}

// ===========================================================================
// The acceptance property: a learned-weight schedule survives
// checkpoint → resume. The p16 test's driver shape: learn → flag →
// re-kind → publish (regenesisCycle) → restore → runGenesis.
// ===========================================================================

/// The fixed filing list: the three name arrays, verbatim.
std::vector<std::string_view> filedNames() {
  std::vector<std::string_view> out;
  for (const auto name : kGenesisDimensionNames) out.push_back(name);
  for (const auto name : kMetaDimensionNames) out.push_back(name);
  for (const auto name : kAgentDimensionNames) out.push_back(name);
  return out;
}

/// Store-side authoring, same shape as the genesis fixture's Author.
struct Author {
  Store &store;

  CellRef cell(std::string_view text, MicroversionId &at) {
    at             = store.makeCell(at, text);
    const auto ref = store.cellRefOf(at);
    EXPECT_TRUE(ref.has_value()) << "makeCell produced no cell";
    return ref.value_or(noCell);
  }
  CellRef scalar(std::int64_t value, MicroversionId &at) {
    at             = store.makeScalarCell(at, value);
    const auto ref = store.cellRefOf(at);
    EXPECT_TRUE(ref.has_value()) << "makeScalarCell produced no cell";
    return ref.value_or(noCell);
  }
  void link(CellRef from, DimRef dim, CellRef to, MicroversionId &at) {
    at = store.setLink(at, from, dim, Posward, to);
  }
};

DimRef dimAt(const std::map<std::string, DimRef> &dim, std::string_view name) {
  return dim.at(std::string{name});
}

template <typename F> DimRef dimAt(const F &f, std::string_view name) {
  return dimAt(f.dim, name);
}

/// A minimal program image — the p16 fixture plus the §9.3 shape: a
/// `demo` module on the stdlib chain carrying the `Feed` goal the rule
/// rank grows on.
struct Image {
  Store store;
  MicroversionId at;
  std::map<std::string, DimRef> dim;
  CellRef programRoot{noCell}, sysModule{noCell}, libModule{noCell};
  CellRef demoModule{noCell};
  CellRef hostSym{noCell}, goalSym{noCell};
  CellRef progHeadA{noCell}, progHeadB{noCell};
  CellRef opA{noCell}, opB{noCell}, imageRecord{noCell};
  std::uint32_t imageExtent{0};
};

void buildImage(Image &f) {
  Author a{f.store};
  f.at        = a.store.sliceGenesis({});
  auto filing = fileDimensionNames(a.store, f.at, filedNames());
  ASSERT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
  for (const auto name : filedNames())
    f.dim[std::string{name}] =
        filing->fold.dimensionNamed(name, a.store).value_or(noCell);
  f.at = filing->head;

  f.programRoot = a.cell("program-root", f.at);
  f.sysModule   = a.cell("sys", f.at);
  f.libModule   = a.cell("lib", f.at);
  f.demoModule  = a.cell("demo", f.at);
  a.link(f.programRoot, dimAt(f, "d.stdlib"), f.sysModule, f.at);
  a.link(f.sysModule, dimAt(f, "d.stdlib"), f.libModule, f.at);
  a.link(f.libModule, dimAt(f, "d.stdlib"), f.demoModule, f.at);
  f.hostSym = a.cell("boot", f.at);
  a.link(f.sysModule, dimAt(f, "d.vars"), f.hostSym, f.at);
  f.goalSym = a.cell("Feed", f.at);
  a.link(f.demoModule, dimAt(f, "d.vars"), f.goalSym, f.at);

  f.opA = a.scalar(static_cast<std::int64_t>(OpcodeKind::Nop), f.at);
  f.opB = a.scalar(static_cast<std::int64_t>(OpcodeKind::Return), f.at);

  const auto progHead = [&](std::string_view label, CellRef op) {
    const auto head  = a.cell("", f.at);
    const auto entry = dimAt(f, "d.entry");
    a.link(head, entry, op, f.at);
    a.link(head, dimAt(f, "d.name"), a.cell(label, f.at), f.at);
    const auto desc = a.cell("", f.at);
    a.link(head, dimAt(f, "d.values"), desc, f.at);
    CellRef tail    = desc;
    const auto pure = a.cell("pure", f.at);
    a.link(pure, dimAt(f, "d.values"), a.scalar(1, f.at), f.at);
    a.link(tail, dimAt(f, "d.vars"), pure, f.at);
    tail              = pure;
    const auto result = a.cell("result", f.at);
    a.link(result, dimAt(f, "d.name"), a.cell("()", f.at), f.at);
    a.link(tail, dimAt(f, "d.vars"), result, f.at);
    const auto mH = a.scalar(static_cast<std::int64_t>(head), f.at);
    const auto mO = a.scalar(static_cast<std::int64_t>(op), f.at);
    const auto mF = a.scalar(static_cast<std::int64_t>(head), f.at);
    const auto an = a.scalar(static_cast<std::int64_t>(entry), f.at);
    a.link(an, dimAt(f, "d.name"), a.cell("d.entry", f.at), f.at);
    const auto mD = a.scalar(1, f.at);
    const auto mT = a.scalar(static_cast<std::int64_t>(op), f.at);
    a.link(head, dimAt(f, "d.recipe"), mH, f.at);
    a.link(mH, dimAt(f, "d.step"), mO, f.at);
    a.link(mO, dimAt(f, "d.step"), mF, f.at);
    a.link(mF, dimAt(f, "d.step"), an, f.at);
    a.link(an, dimAt(f, "d.step"), mD, f.at);
    a.link(mD, dimAt(f, "d.step"), mT, f.at);
    return head;
  };
  f.progHeadA = progHead("frag-a", f.opA);
  f.progHeadB = progHead("frag-b", f.opB);

  f.imageRecord      = a.cell("image-record", f.at);
  CellRef imgVars    = f.imageRecord;
  const auto opsRec  = a.cell("ops", f.at);
  const auto opsHead = a.cell("", f.at);
  a.link(opsRec, dimAt(f, "d.values"), opsHead, f.at);
  a.link(imgVars, dimAt(f, "d.vars"), opsRec, f.at);
  CellRef tail = opsHead;
  for (const auto op : {f.opA, f.opB}) {
    const auto m = a.scalar(static_cast<std::int64_t>(op), f.at);
    a.link(tail, dimAt(f, "d.step"), m, f.at);
    tail = m;
  }
  f.imageExtent = static_cast<std::uint32_t>(a.store.segmentedOps().size());
}

/// One session: a fold, its fresh WorkerContext, an agent cell, and the
/// bound set the ceremony factory takes.
struct Session {
  std::unique_ptr<zigzag::Manifold> fold;
  std::unique_ptr<WorkerContext> ctx;
  CellRef agentCell{noCell};
  MetaBindSet binds;

  ArenaManifold &arena() { return ctx->arena; }
  zigzag::vortex::VortexCore &core() { return ctx->core; }
};

Session makeSession(Image &f) {
  Session s;
  s.fold = std::make_unique<zigzag::Manifold>(
      f.store.rebuildManifold(f.store.latest()));
  s.ctx       = std::make_unique<WorkerContext>(s.fold.get(), &f.store);
  s.agentCell = s.ctx->arena.makeCell("agent");
  auto binds =
      mintMetaBindSet(s.ctx->arena, s.ctx->core, f.programRoot, s.agentCell);
  EXPECT_TRUE(binds.has_value()) << (binds ? "" : binds.error());
  binds->ownModule = f.demoModule;
  binds->lineage   = MetaLineage{
      .programRef  = f.programRoot,
      .imageExtent = f.imageExtent,
  };
  s.binds = *binds;
  return s;
}

/// Every marked cell the ceremony's sweep enumerates on `head` — the
/// p16 test's inventory walk, verbatim.
std::vector<CellRef> markedOnHead(ArenaManifold &arena,
                                  const MetaBindSet &binds,
                                  zigzag::vortex::VortexCore &core,
                                  CellRef head) {
  std::vector<CellRef> out;
  for (auto rec = arena.linked(head, binds.dims.refs, Posward);
       rec && arena.contains(*rec);
       rec = arena.linked(*rec, core.dims().step, Posward)) {
    const auto marked = arena.linked(*rec, binds.dims.ref, Posward);
    if (!marked || arena.linked(*rec, core.dims().name, Posward)) continue;
    out.push_back(*marked);
  }
  return out;
}

TEST(VagentPlasticityTest, LearnedWeightScheduleSurvivesCheckpointResume) {
  Image f;
  buildImage(f);

  // -- Session 1: §9.3's grow + strengthen over the image fold --------
  auto s0   = makeSession(f);
  auto &a0  = s0.arena();
  auto &co0 = s0.core();
  MetaRuntime meta0(co0, s0.ctx->vm, s0.binds);
  const auto clause =
      meta0.learnPlan("Feed", "near_water", f.progHeadA, f.progHeadB);
  ASSERT_TRUE(clause.has_value()) << clause.error();
  const auto clauseLabel = meta0.labelOf(*clause);
  ASSERT_TRUE(clauseLabel.has_value());
  ASSERT_TRUE(clauseLabel->starts_with("patch:"));

  // The learned-weight schedule: seed, reinforce — the record lives on
  // the agent's d.vars keyed by the patch:<n> label.
  ASSERT_TRUE(meta0.setWeight(*clause, 0.7).has_value());
  ASSERT_TRUE(meta0.setWeight(*clause, 0.8).has_value());
  EXPECT_EQ(meta0.weightOf(*clause), 0.8);

  // A synapse naming the learned rule — junction cell, endpoint labels
  // and own weight scalar are all state-side, so all promote.
  const auto junction = meta0.sproutSynapse("demo:Feed", *clauseLabel, 0.55);
  ASSERT_TRUE(junction.has_value());
  ASSERT_TRUE(meta0.setWeight(*junction, 0.62).has_value());

  // The session's registered heads carry ephemeral positions — prove
  // this checkpoint exercises the real re-kind, not an empty sweep.
  auto heads0       = zigzag::rankAfter(a0, s0.agentCell, s0.binds.dims.code);
  bool sawEphemeral = false;
  for (const auto head : heads0)
    for (const auto marked : markedOnHead(a0, s0.binds, co0, head))
      if (const auto v = a0.asInt64(marked);
          v && zigzag::isEphemeral(static_cast<CellRef>(*v)))
        sawEphemeral = true;
  ASSERT_TRUE(sawEphemeral)
      << "fixture drift: no ephemeral positions to checkpoint";

  // The P16 ceremony with regenesisCycle bound as publish — the p16
  // test's driver: flag → re-kind → publish inside the window →
  // restore.
  std::optional<RegenesisResult> cycle;
  auto c1 = makeP16Ceremony(
      a0, co0, s0.binds, [&](void) -> std::expected<void, std::string> {
        auto c = regenesisCycle(f.store, a0, s0.agentCell, f.imageExtent);
        if (!c) return std::unexpected(c.error());
        cycle = std::move(*c);
        return {};
      });
  ASSERT_TRUE(c1->onFlagged()) << "the flag-time re-kind refused";
  ASSERT_TRUE(c1->onFlaggedTurnEnd().has_value());
  ASSERT_TRUE(cycle.has_value()) << "no publish command ran";
  for (const auto head : heads0)
    for (const auto marked : markedOnHead(a0, s0.binds, co0, head))
      EXPECT_EQ(a0.valueKindOf(marked), xanadu::ValueKind::Int64)
          << "a marked cell stayed re-kinded past the window";

  // -- Resume: genesis over the checkpoint's fold ---------------------
  const GenesisRoots roots{
      .programRoot = f.programRoot,
      .agentCell   = cycle->agentCell,
      .ownModule   = f.demoModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  auto &a1  = cycle->context->arena;
  auto out1 = runGenesis(f.store, f.store, a1, cycle->context->core,
                         cycle->context->vm, roots);
  ASSERT_TRUE(out1.has_value()) << (out1 ? "" : out1.error());
  EXPECT_EQ(out1->patchesReplayed, 1U);
  EXPECT_EQ(out1->patchesDropped, 0U);

  MetaRuntime meta1(cycle->context->core, cycle->context->vm, out1->binds);
  const auto &dims1   = cycle->context->core.dims();
  const auto clauses1 = patchClauses(a1, f.goalSym, dims1.clause, dims1.name);
  ASSERT_EQ(clauses1.size(), 1U) << "the patch clause never installed";
  const auto &resumed = clauses1.front();

  // The acceptance property: the learned-weight schedule resolved
  // post-genesis — the keyed d.vars record promoted whole.
  const auto resumedLabel = meta1.labelOf(resumed);
  ASSERT_TRUE(resumedLabel.has_value());
  EXPECT_EQ(*resumedLabel, *clauseLabel)
      << "the replayed clause carries the patch label verbatim";
  EXPECT_DOUBLE_EQ(meta1.weightOf(resumed).value_or(0.0), 0.8)
      << "the learned weight resolved post-genesis";
  const auto record = varsRecordOf(a1, cycle->agentCell, dims1, *clauseLabel);
  ASSERT_TRUE(record.has_value());
  const auto recordValue = a1.linked(*record, dims1.values, Posward);
  ASSERT_TRUE(recordValue.has_value());
  EXPECT_DOUBLE_EQ(a1.asDouble(*recordValue).value_or(0.0), 0.8);

  // The synapse rank promoted whole: junction cell, endpoint labels as
  // strings, own +d.weight scalar.
  const auto juncs = junctions(a1, cycle->agentCell, out1->binds.dims.synapse);
  ASSERT_EQ(juncs.size(), 1U);
  EXPECT_EQ(junctionField(a1, juncs.front(), dims1, "from"), "demo:Feed");
  EXPECT_EQ(junctionField(a1, juncs.front(), dims1, "to"), *clauseLabel);
  EXPECT_DOUBLE_EQ(meta1.weightOf(juncs.front()).value_or(0.0), 0.62);

  // -- Session 2 on the resumed context: the schedule continues -------
  // Reinforce the resumed clause and grow a second rule — then another
  // checkpoint/resume must carry *both* schedules. This is the arm that
  // separates "writes to fresh ephemeral cells promote" from "overlay
  // writes on already-persistent base cells promote": everything the
  // verbs now touch lives in the checkpoint fold.
  ASSERT_TRUE(meta1.setWeight(resumed, 0.95).has_value());
  const auto clause2 =
      meta1.learnPlan("Feed", "deep_water", f.progHeadA, f.progHeadB);
  ASSERT_TRUE(clause2.has_value()) << clause2.error();
  const auto label2 = meta1.labelOf(*clause2);
  ASSERT_TRUE(label2.has_value() && label2->starts_with("patch:"));
  ASSERT_TRUE(meta1.setWeight(*clause2, 0.3).has_value());

  std::optional<RegenesisResult> cycle2;
  auto c2 = makeP16Ceremony(
      a1, cycle->context->core, out1->binds,
      [&](void) -> std::expected<void, std::string> {
        auto c = regenesisCycle(f.store, a1, cycle->agentCell, f.imageExtent);
        if (!c) return std::unexpected(c.error());
        cycle2 = std::move(*c);
        return {};
      },
      out1->installedCopies);
  ASSERT_TRUE(c2->onFlagged());
  ASSERT_TRUE(c2->onFlaggedTurnEnd().has_value());
  ASSERT_TRUE(cycle2.has_value());

  auto &a2 = cycle2->context->arena;
  const GenesisRoots roots2{
      .programRoot = f.programRoot,
      .agentCell   = cycle2->agentCell,
      .ownModule   = f.demoModule,
      .imageRecord = f.imageRecord,
      .imageExtent = f.imageExtent,
  };
  auto out2 = runGenesis(f.store, f.store, a2, cycle2->context->core,
                         cycle2->context->vm, roots2);
  ASSERT_TRUE(out2.has_value()) << (out2 ? "" : out2.error());
  EXPECT_EQ(out2->patchesReplayed, 2U)
      << "both patch records ride the second checkpoint";
  EXPECT_EQ(out2->patchesDropped, 0U);

  MetaRuntime meta2(cycle2->context->core, cycle2->context->vm, out2->binds);
  const auto &dims2   = cycle2->context->core.dims();
  const auto clauses2 = patchClauses(a2, f.goalSym, dims2.clause, dims2.name);
  ASSERT_EQ(clauses2.size(), 2U);
  std::map<std::string, CellRef> byLabel;
  for (const auto c : clauses2) byLabel[meta2.labelOf(c).value_or("")] = c;
  ASSERT_TRUE(byLabel.contains(*clauseLabel));
  ASSERT_TRUE(byLabel.contains(*label2));
  // The depth-≥2 shadow arm — the finding this test was written
  // against: promote()'s frontier expands only the root, ephemeral
  // cells, and shadowed base cells it can *reach*, so the shadowed
  // patch:<n> records behind the untouched meta.serial record were
  // invisible until regenesisCycle learned to pass the state-side
  // shadowOrder() subset as extraRoots. Program-side shadows stay
  // excluded — a learn_* install rides replay, never promote — so the
  // clauses2 count above is the double-install guard, and the weights
  // below are the durable records.
  EXPECT_DOUBLE_EQ(meta2.weightOf(byLabel[*clauseLabel]).value_or(0.0), 0.95)
      << "the post-resume reinforcement survived the second checkpoint";
  EXPECT_DOUBLE_EQ(meta2.weightOf(byLabel[*label2]).value_or(0.0), 0.3)
      << "the second session's seeded record survived it too";
  const auto juncs2 =
      junctions(a2, cycle2->agentCell, out2->binds.dims.synapse);
  ASSERT_EQ(juncs2.size(), 1U)
      << "the synapse rank crossed both checkpoints once, not twice";
  EXPECT_DOUBLE_EQ(meta2.weightOf(juncs2.front()).value_or(0.0), 0.62);
}

// ===========================================================================
// The adapter boundary — what the phase names as Requires, marked once
// rather than faked.
// ===========================================================================

TEST(VagentPlasticityTest, ActorExecutorBoundaryRemainsGated) {
  // The phase's Requires names the full actor executor (vagent
  // iterations 4–5+, implementation.md :586-587). What sits behind it
  // in §9.2/§9.3 — and only this — is:
  //
  //  - event delivery firing `on Sighting`/`on Meal`/`on Attack`: the
  //    turn machinery that runs the handler bodies whose verb
  //    sequences the tests above execute verb-by-verb;
  //  - `believes`/`believe` predicate state (§9.3's `believes sees` /
  //    `believe learned_rule` guard), and `emit` inside handler turns;
  //  - plan *selection* consuming the clause bank §8.2 describes —
  //    evaluating each clause's where per selection. The bank's shape,
  //    ordering, re-pointing and weight records are all asserted above;
  //    the selecting turn is the executor's.
  //
  // Engine-level stand-ins used above, honestly named: handler bodies
  // were driven as MetaRuntime verb sequences; `if [w]`'s Opt presence
  // test as weight_of's disengage; the score comparison as a fixed
  // fixture value inside a real `run` of a fitness fragment.
  GTEST_SKIP()
      << "§9.2/§9.3's turn-level arms — handler dispatch on event "
         "delivery, believes/believe predicate evaluation, and plan "
         "selection consuming the clause bank — require the actor "
         "executor (vagent iterations 4–5+); the verb-level arms above "
         "run green on today's machinery";
}

} // namespace
