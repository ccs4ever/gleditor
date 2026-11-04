#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/records.hpp"

namespace {
using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::Negward;
using zigzag::Posward;
using zigzag::vortex::OpcodeKind;

// ---------------------------------------------------------------------------
// Checker-side helpers: the full parse → resolve → expand → check pipeline.
// ---------------------------------------------------------------------------

std::vector<Diagnostic> diagnostics(std::string_view source) {
  auto program = compile(source);
  return program ? std::vector<Diagnostic>{} : std::move(program.error());
}

std::string describe(const std::vector<Diagnostic> &errors) {
  std::string result;
  for (const auto &error : errors) result += formatDiagnostic(error) + '\n';
  return result;
}

void rejects(std::string_view source, DiagnosticCode code) {
  const auto errors = diagnostics(source);
  EXPECT_FALSE(errors.empty());
  EXPECT_TRUE(std::ranges::any_of(errors, [&](const Diagnostic &error) {
    return error.code == code;
  })) << describe(errors);
}

void accepts(std::string_view source) {
  const auto errors = diagnostics(source);
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

// ---------------------------------------------------------------------------
// Engine-side harness. The lineage boundary models program-image vs
// state-side: cells minted up to `imageBoundary` count as image refs
// (dense id < extent); everything minted after is state-side unless
// program() reaches it over the structural vocabulary.
// ---------------------------------------------------------------------------

struct MutationHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{zigzag::noCell};
  CellRef imageBoundary{zigzag::noCell};
  CellRef agentCell{zigzag::noCell};
  CellRef ownModule{zigzag::noCell};
  MetaBindSet binds;
  std::optional<MetaRuntime> meta;

  explicit MutationHarness(bool lineage = true) {
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
    ownModule       = mintModule("self");
    binds.ownModule = ownModule;
    meta.emplace(core, vm, binds);
  }
  MetaRuntime &rt() { return *meta; }

  /// A module cell filed on the program root's +d.stdlib — minted
  /// *before* the agent/state cells so fixture modules play program.
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
  /// A clause labelled `label` appended on `host`'s +d.clause rank.
  CellRef mintClause(CellRef host, std::string_view label) {
    const auto clause = arena.makeCell();
    zigzag::expectWritten(
        arena.link(clause, core.dims().name, Posward, arena.makeCell(label)));
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, host, core.dims().clause),
                   core.dims().clause, Posward, clause));
    return clause;
  }
};

/// A self-contained fragment head: +d.entry → entry op, a descriptor
/// carrying result/produced/pure/closed/arity plus the ordered param
/// chain, and a +d.recipe that reify can replay into a runnable twin —
/// two literals (head, entry) and the (head, d.entry, +, entry) tuple.
struct FixtureHead {
  CellRef head{zigzag::noCell};
  CellRef entry{zigzag::noCell};
};

FixtureHead
mintFragmentHead(MutationHarness &h, OpcodeKind op, std::string_view label,
                 bool pure, std::string_view resultType = "()",
                 std::vector<std::pair<std::string, std::string>> params = {},
                 std::string_view sourceText                             = "") {
  const auto entry = h.vm.mintOpcode(op);
  const auto head  = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(head, h.binds.dims.entry, Posward, entry));
  const auto shown = label.empty() ? sourceText : label;
  zigzag::expectWritten(
      h.arena.link(head, h.core.dims().name, Posward, h.arena.makeCell(shown)));

  // Descriptor — the emission shape: head +d.values → descriptor;
  // +d.vars records content-keyed with +d.values values; the result
  // record's +d.name carries the type spelling; params ordered on
  // +d.grab → +d.step.
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
  CellRef tail = zigzag::noCell;
  for (const auto &[pname, ptype] : params) {
    const auto cell = h.arena.makeCell();
    zigzag::expectWritten(
        h.arena.link(cell, dims.name, Posward, h.arena.makeCell(pname)));
    zigzag::expectWritten(
        h.arena.link(cell, dims.values, Posward, h.arena.makeCell(ptype)));
    if (tail == zigzag::noCell)
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

/// The +d.refs chain cells — the record taxonomy the closed walk reads.
CellRef refRecord(MutationHarness &h, CellRef marked,
                  std::optional<std::string_view> name,
                  std::optional<std::string_view> role) {
  const auto record = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(record, h.binds.dims.ref, Posward, marked));
  if (name)
    zigzag::expectWritten(h.arena.link(record, h.core.dims().name, Posward,
                                       h.arena.makeCell(*name)));
  if (role)
    zigzag::expectWritten(h.arena.link(record, h.core.dims().grab, Posward,
                                       h.arena.makeCell(*role)));
  return record;
}

void appendRefs(MutationHarness &h, CellRef head, CellRef record) {
  const auto first = h.arena.linked(head, h.binds.dims.refs, Posward);
  if (!first) {
    zigzag::expectWritten(
        h.arena.link(head, h.binds.dims.refs, Posward, record));
    return;
  }
  auto tail = *first;
  for (auto next = h.arena.linked(tail, h.core.dims().step, Posward);
       next && h.arena.contains(*next);
       next = h.arena.linked(tail, h.core.dims().step, Posward))
    tail = *next;
  zigzag::expectWritten(
      h.arena.link(tail, h.core.dims().step, Posward, record));
}

/// The agent's +d.patch rank, newest last.
std::vector<CellRef> patches(MutationHarness &h) {
  std::vector<CellRef> out;
  for (const auto cell :
       zigzag::rankAfter(h.arena, h.agentCell, h.binds.dims.patch))
    out.push_back(cell);
  return out;
}

/// The agent's +d.code rank (registered heads).
std::vector<CellRef> registered(MutationHarness &h) {
  std::vector<CellRef> out;
  for (const auto cell :
       zigzag::rankAfter(h.arena, h.agentCell, h.binds.dims.code))
    out.push_back(cell);
  return out;
}

// ===========================================================================
// Checker rows (§6.1's signature table, phase-4 half)
// ===========================================================================

TEST(VagentMutationTest, MutationRowsCheck) {
  accepts(R"VA(
import std:meta
agent MetaUser {
  def probe(c: Cell, g: Cell): Int {
    graft_plan("demo:Feed", "near_water", code"true", code"()")
    graft_handler("demo:Bumped", "watch", code"true", code"()", 3)
    graft_where(c, code"true")
    graft_body(c, code"()")
    graft_method("demo:Forager", "f", code"42")
    prune(c)
    learn_plan("demo:Feed", "near_water", code"true", code"()")
    learn_handler("demo:Bumped", "watch", code"true", code"()", 1)
    learn_where(c, code"true")
    learn_body(c, code"()")
    learn_method("demo:Forager", "f", code"42")
    unlearn(c)
    set_weight(c, 0.5)
    let j := sprout_synapse("a", "b", 0.25)
    0
  }
}
)VA");
}

TEST(VagentMutationTest, MutationRowsAreTyped) {
  // Arity is checked — a missing operand fails the call.
  rejects(R"VA(
import std:meta
agent MetaUser {
  def probe(c: Cell): Int {
    graft_plan("g", "n", code"true")
    0
  }
}
)VA",
          DiagnosticCode::TypeMismatch);
  // A Cell operand refuses a literal of the wrong kind.
  rejects(R"VA(
import std:meta
agent MetaUser {
  def probe(): Int {
    graft_where("not-a-cell", code"true")
    0
  }
}
)VA",
          DiagnosticCode::TypeMismatch);
  // set_weight's weight is Float.
  rejects(R"VA(
import std:meta
agent MetaUser {
  def probe(c: Cell): Int {
    set_weight(c, "heavy")
    0
  }
}
)VA",
          DiagnosticCode::TypeMismatch);
  // A body that produces a value is not Code<()>.
  rejects(R"VA(
import std:meta
agent MetaUser {
  def probe(c: Cell): Int {
    graft_body(c, code"1")
    0
  }
}
)VA",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentMutationTest, MutationVerbsAreActions) {
  // Every mutation verb is impure — a pure context refuses it like run.
  rejects(R"VA(
import std:meta
agent MetaUser {
  var x: Int := prune(agent_cell())
}
)VA",
          DiagnosticCode::ImpureCondition);
}

TEST(VagentMutationTest, SpliceOnlyLiteralRefusedInEvaluatedPosition) {
  // The graft refusal arm is expand's splice-only marking: a Code-
  // parameterized literal may not reach the verb at all (§6.4's arm
  // rides the existing InvalidPosition diagnostic).
  rejects(R"VA(
import std:meta
agent MetaUser {
  def probe(c: Cell): Int {
    graft_where(c, code"p: Code<Int> | splice p")
    0
  }
}
)VA",
          DiagnosticCode::InvalidPosition);
}

// ===========================================================================
// graft_* — session installs
// ===========================================================================

TEST(VagentMutationTest, GraftPlanInstallsNamedClause) {
  MutationHarness h;
  const auto demo = h.mintModule("demo");
  const auto goal = h.mintSymbol(demo, "Feed");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "src:test", true);
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "src:body", false);

  const auto clause =
      h.rt().graftPlan("demo:Feed", "near_water", test.head, body.head);
  ASSERT_TRUE(clause.has_value()) << clause.error();
  // The clause appends at rank tail, labelled, attach heads linked.
  EXPECT_EQ(h.rt().clausesOf(goal).back(), *clause);
  EXPECT_EQ(h.rt().labelOf(*clause), "near_water");
  EXPECT_EQ(h.arena.linked(*clause, h.core.dims().grab, Posward), test.head);
  EXPECT_EQ(h.arena.linked(*clause, h.core.dims().spin, Posward), body.head);
  // plan_named reports the new kind post-mutation.
  EXPECT_EQ(h.rt().planNamed("near_water"), clause);
}

TEST(VagentMutationTest, GraftRefusesReservedNameAndMissingHost) {
  MutationHarness h;
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true);
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  EXPECT_FALSE(h.rt()
                   .graftPlan("demo:Feed", "patch:9", test.head, body.head)
                   .has_value());
  EXPECT_FALSE(
      h.rt().graftPlan("absent:Feed", "n", test.head, body.head).has_value());
}

TEST(VagentMutationTest, GraftRefusesOpCellAndConsumedHead) {
  MutationHarness h;
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Feed");
  const auto op   = h.vm.mintOpcode(OpcodeKind::Return);
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true);
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  // An op cell is not a head — attachment is +d.entry heads only.
  const auto clause = h.arena.makeCell();
  EXPECT_FALSE(h.rt().graftWhere(clause, op).has_value());
  // A bare data cell is not a head either.
  EXPECT_FALSE(h.rt().graftWhere(clause, h.arena.makeCell()).has_value());
  // A head installed once is consumed — a second graft refuses it
  // anywhere.
  ASSERT_TRUE(h.rt().graftWhere(clause, test.head).has_value());
  EXPECT_EQ(h.arena.linked(clause, h.core.dims().grab, Posward), test.head);
  EXPECT_FALSE(h.rt().graftWhere(clause, test.head).has_value());
  EXPECT_FALSE(
      h.rt().graftPlan("demo:Feed", "n", test.head, body.head).has_value());
}

TEST(VagentMutationTest, GraftWhereRepointsGrabAndKeepsOldGraph) {
  MutationHarness h;
  const auto demo   = h.mintModule("demo");
  const auto goal   = h.mintSymbol(demo, "Guard");
  const auto clause = h.mintClause(goal, "demo:Guard.cautious");
  const auto prior =
      mintFragmentHead(h, OpcodeKind::Return, "src:p", true, "Bool");
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().grab, Posward, prior.head));
  const auto next =
      mintFragmentHead(h, OpcodeKind::Return, "src:n", true, "Bool");

  ASSERT_TRUE(h.rt().graftWhere(clause, next.head).has_value());
  EXPECT_EQ(h.arena.linked(clause, h.core.dims().grab, Posward), next.head);
  // The old graph stays allocated, just detached.
  EXPECT_TRUE(h.arena.contains(prior.head));
  EXPECT_FALSE(
      h.arena.linked(prior.head, h.core.dims().grab, Negward).has_value());
}

TEST(VagentMutationTest, GraftWhereRefusesImpureOrMismatchedTest) {
  MutationHarness h;
  const auto clause = h.arena.makeCell();
  const auto prior = mintFragmentHead(h, OpcodeKind::Return, "p", true, "Bool");
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().grab, Posward, prior.head));
  // An impure-flagged test refuses the pure-required arm.
  const auto impure =
      mintFragmentHead(h, OpcodeKind::Return, "i", false, "Bool");
  EXPECT_FALSE(h.rt().graftWhere(clause, impure.head).has_value());
  // A signature mismatch refuses against the clause's formals.
  const auto wrong = mintFragmentHead(h, OpcodeKind::Return, "w", true, "Int");
  EXPECT_FALSE(h.rt().graftWhere(clause, wrong.head).has_value());
}

TEST(VagentMutationTest, GraftBodyRepointsSpin) {
  MutationHarness h;
  const auto clause = h.arena.makeCell();
  const auto old    = mintFragmentHead(h, OpcodeKind::Return, "o", false);
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().spin, Posward, old.head));
  const auto next = mintFragmentHead(h, OpcodeKind::Return, "n", false);
  ASSERT_TRUE(h.rt().graftBody(clause, next.head).has_value());
  EXPECT_EQ(h.arena.linked(clause, h.core.dims().spin, Posward), next.head);
}

TEST(VagentMutationTest, GraftMethodUnlinksThenInstalls) {
  MutationHarness h;
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Forager");
  const auto code =
      mintFragmentHead(h, OpcodeKind::Return, "src:f", false, "Int");
  const auto symbol = h.rt().graftMethod("demo:Forager", "f", code.head);
  ASSERT_TRUE(symbol.has_value()) << symbol.error();
  // The minted symbol carries the Agent.member spelling on the module's
  // d.vars and +d.values → the head.
  EXPECT_EQ(h.arena.textOf(*symbol), "Forager.f");
  EXPECT_EQ(h.arena.linked(*symbol, h.core.dims().values, Posward), code.head);
  // A second graft replaces — the old symbol leaves the rank.
  const auto code2 =
      mintFragmentHead(h, OpcodeKind::Return, "src:g", false, "Int");
  const auto symbol2 = h.rt().graftMethod("demo:Forager", "f", code2.head);
  ASSERT_TRUE(symbol2.has_value());
  std::vector<CellRef> named;
  for (const auto cell : zigzag::rankAfter(h.arena, demo, h.core.dims().vars))
    if (h.arena.textOf(cell) == "Forager.f") named.push_back(cell);
  ASSERT_EQ(named.size(), 1U);
  EXPECT_EQ(named.front(), *symbol2);
  // The consumed arm: the first head is installed, refusing re-use.
  EXPECT_FALSE(h.rt().graftMethod("demo:Forager", "h", code.head).has_value());
}

TEST(VagentMutationTest, GraftHandlerAppendsWithPriority) {
  MutationHarness h;
  const auto demo  = h.mintModule("demo");
  const auto event = h.mintSymbol(demo, "Bumped");
  const auto guard = mintFragmentHead(h, OpcodeKind::Return, "g", true);
  const auto body  = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  const auto clause =
      h.rt().graftHandler("demo:Bumped", "watch", guard.head, body.head, 7);
  ASSERT_TRUE(clause.has_value()) << clause.error();
  EXPECT_EQ(h.rt().clausesOf(event).back(), *clause);
  EXPECT_EQ(h.rt().labelOf(*clause), "watch");
  const auto record = h.arena.linked(*clause, h.core.dims().vars, Posward);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(h.arena.textOf(*record), "priority");
  EXPECT_EQ(
      h.arena.asInt64(*h.arena.linked(*record, h.core.dims().values, Posward)),
      7);
}

TEST(VagentMutationTest, PruneUnlinksWithRepair) {
  MutationHarness h;
  const auto demo = h.mintModule("demo");
  const auto goal = h.mintSymbol(demo, "Feed");
  const auto c1   = h.mintClause(goal, "demo:Feed.a");
  const auto c2   = h.mintClause(goal, "demo:Feed.b");
  const auto c3   = h.mintClause(goal, "demo:Feed.c");
  ASSERT_EQ(h.rt().clausesOf(goal).size(), 3U);
  ASSERT_TRUE(h.rt().prune(c2).has_value());
  // predecessor→successor repair: goal → c1 → c3.
  const auto clauses = h.rt().clausesOf(goal);
  ASSERT_EQ(clauses.size(), 2U);
  EXPECT_EQ(clauses[0], c1);
  EXPECT_EQ(clauses[1], c3);
  EXPECT_EQ(h.arena.linked(c1, h.core.dims().clause, Posward), c3);
  // A detached clause refuses a second prune.
  EXPECT_FALSE(h.rt().prune(c2).has_value());
}

TEST(VagentMutationTest, MutationRestoresUnderMarkRelease) {
  // §6.4's session arm: graft and prune are ordinary trailed writes —
  // a mark release restores the pre-mutation graph. Cells minted under
  // the mark (the clause cell, the heads' install edges) are gone.
  MutationHarness h;
  const auto demo = h.mintModule("demo");
  const auto goal = h.mintSymbol(demo, "Feed");
  const auto keep = h.mintClause(goal, "demo:Feed.keep");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true);
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);

  const auto mark   = h.arena.mark();
  const auto clause = h.rt().graftPlan("demo:Feed", "m", test.head, body.head);
  ASSERT_TRUE(clause.has_value());
  ASSERT_EQ(h.rt().clausesOf(goal).size(), 2U);
  ASSERT_TRUE(h.rt().prune(keep).has_value());
  EXPECT_EQ(h.rt().clausesOf(goal).size(), 1U);
  h.arena.release(mark);

  // Both mutations replay out: the pruned clause returns, the grafted
  // one is gone, and the consumed heads are installable again.
  const auto clauses = h.rt().clausesOf(goal);
  ASSERT_EQ(clauses.size(), 1U);
  EXPECT_EQ(clauses.front(), keep);
  EXPECT_TRUE(
      h.rt().graftPlan("demo:Feed", "m", test.head, body.head).has_value());
}

TEST(VagentMutationTest, GraftReifiesARegisteredHead) {
  MutationHarness h(true);
  const auto clause = h.arena.makeCell();
  const auto test =
      mintFragmentHead(h, OpcodeKind::Return, "src:r", true, "Bool");
  // Register the head on the agent's d.code — the graft must install a
  // reified copy, never the registered head in place.
  zigzag::expectWritten(
      h.arena.link(h.agentCell, h.binds.dims.code, Posward, test.head));
  ASSERT_TRUE(h.rt().graftWhere(clause, test.head).has_value());
  const auto attach = h.arena.linked(clause, h.core.dims().grab, Posward);
  ASSERT_TRUE(attach.has_value());
  EXPECT_NE(*attach, test.head);
  EXPECT_TRUE(h.rt().labelOf(*attach).value_or("").starts_with("reify:"));
}

// ===========================================================================
// learn_* / unlearn — durable twins
// ===========================================================================

TEST(VagentMutationTest, LearnFailsClosedWithoutLineage) {
  MutationHarness h(/*lineage=*/false);
  const auto demo = h.mintModule("demo");
  const auto goal = h.mintSymbol(demo, "Feed");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true);
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  const auto result =
      h.rt().learnPlan("demo:Feed", "alias", test.head, body.head);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("no program lineage"), std::string::npos);
  // And nothing installed or recorded on the way out.
  EXPECT_TRUE(h.rt().clausesOf(goal).empty());
  EXPECT_TRUE(patches(h).empty());
}

TEST(VagentMutationTest, LearnPlanInstallsClausePatchAndRegistrations) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Feed");
  const auto test =
      mintFragmentHead(h, OpcodeKind::Return, "src:t", true, "Bool");
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "src:b", false);

  const auto clause =
      h.rt().learnPlan("demo:Feed", "near_water", test.head, body.head);
  ASSERT_TRUE(clause.has_value()) << clause.error();
  // The installed clause carries the minted patch:<n> label, never the
  // user alias.
  const auto label = h.rt().labelOf(*clause);
  ASSERT_TRUE(label.has_value());
  EXPECT_TRUE(label->starts_with("patch:"));
  EXPECT_NE(*label, "near_water");
  // The attach heads are fresh installed copies, not the arguments.
  const auto grab = h.arena.linked(*clause, h.core.dims().grab, Posward);
  const auto spin = h.arena.linked(*clause, h.core.dims().spin, Posward);
  ASSERT_TRUE(grab.has_value() && spin.has_value());
  EXPECT_NE(*grab, test.head);
  EXPECT_NE(*spin, body.head);
  // Two registered heads on d.code, each reify-labelled.
  const auto codeRank = registered(h);
  ASSERT_EQ(codeRank.size(), 2U);
  // The patch record: +d.name → the clause label; descriptor carries
  // verb/target/parent; the patch-head chain names both registrations.
  const auto ps = patches(h);
  ASSERT_EQ(ps.size(), 1U);
  EXPECT_EQ(h.rt().labelOf(ps[0]), *label);
  // reflect still reports the true structure.
  EXPECT_EQ(h.rt().planNamed(*label), *clause);
  EXPECT_TRUE(h.rt().testOf(*clause).has_value());
  EXPECT_TRUE(h.rt().bodyOf(*clause).has_value());
}

TEST(VagentMutationTest, LearnPlanDedupesAliasVerbatim) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  const auto goal = h.mintSymbol(demo, "Feed");
  const auto old  = h.mintClause(goal, "near_water");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true, "Bool");
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  ASSERT_TRUE(h.rt()
                  .learnPlan("demo:Feed", "near_water", test.head, body.head)
                  .has_value());
  // The declared clause displaced verbatim; the learned one is the only
  // clause and carries its own patch:<n>.
  const auto clauses = h.rt().clausesOf(goal);
  ASSERT_EQ(clauses.size(), 1U);
  EXPECT_NE(clauses[0], old);
}

TEST(VagentMutationTest, UnlearnRoundTripsLearnPlan) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  const auto goal = h.mintSymbol(demo, "Feed");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true, "Bool");
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  const auto clause = h.rt().learnPlan("demo:Feed", "a", test.head, body.head);
  ASSERT_TRUE(clause.has_value());
  ASSERT_EQ(registered(h).size(), 2U);
  ASSERT_EQ(patches(h).size(), 1U);

  ASSERT_TRUE(h.rt().unlearn(*clause).has_value());
  EXPECT_TRUE(h.rt().clausesOf(goal).empty());
  EXPECT_TRUE(patches(h).empty());
  // Both solely-named registrations drop off d.code.
  EXPECT_TRUE(registered(h).empty());
}

TEST(VagentMutationTest, LearnWhereRepointsAndUnlearnRestoresPrior) {
  MutationHarness h(true);
  const auto demo   = h.mintModule("demo");
  const auto goal   = h.mintSymbol(demo, "Guard");
  const auto clause = h.mintClause(goal, "demo:Guard.cautious");
  const auto original =
      mintFragmentHead(h, OpcodeKind::Return, "src:o", true, "Bool");
  zigzag::expectWritten(
      h.arena.link(clause, h.core.dims().grab, Posward, original.head));

  // First replace — a program-original attach records no prior.
  const auto t1 =
      mintFragmentHead(h, OpcodeKind::Return, "src:t1", true, "Bool");
  ASSERT_TRUE(h.rt().learnWhere(clause, t1.head).has_value());
  const auto first = h.arena.linked(clause, h.core.dims().grab, Posward);
  ASSERT_TRUE(first.has_value());
  EXPECT_NE(*first, t1.head);
  EXPECT_EQ(patches(h).size(), 1U);

  // Second replace — records the first's registered head as prior.
  const auto t2 =
      mintFragmentHead(h, OpcodeKind::Return, "src:t2", true, "Bool");
  ASSERT_TRUE(h.rt().learnWhere(clause, t2.head).has_value());
  EXPECT_EQ(patches(h).size(), 2U);

  // Unlearn the second: the clause re-points through a fresh reify of
  // the recorded prior — a copy, never the registered head itself.
  ASSERT_TRUE(h.rt().unlearn(clause).has_value());
  const auto restored = h.arena.linked(clause, h.core.dims().grab, Posward);
  ASSERT_TRUE(restored.has_value());
  EXPECT_NE(*restored, *first);
  EXPECT_TRUE(h.rt().labelOf(*restored).value_or("").starts_with("reify:"));
  EXPECT_EQ(patches(h).size(), 1U);

  // Unlearn the first: a program-original attachment detaches —
  // genesis restores it, the session does not fake a state→program link.
  ASSERT_TRUE(h.rt().unlearn(clause).has_value());
  EXPECT_FALSE(h.arena.linked(clause, h.core.dims().grab, Posward).has_value());
  EXPECT_TRUE(patches(h).empty());
  EXPECT_TRUE(registered(h).empty());
}

TEST(VagentMutationTest, LearnWhereRefusesAnonymousClause) {
  MutationHarness h(true);
  const auto demo   = h.mintModule("demo");
  const auto goal   = h.mintSymbol(demo, "Guard");
  const auto clause = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(zigzag::rankTail(h.arena, goal, h.core.dims().clause),
                   h.core.dims().clause, Posward, clause));
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true, "Bool");
  const auto result = h.rt().learnWhere(clause, test.head);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("+d.name"), std::string::npos);
}

TEST(VagentMutationTest, LearnBodyRoundTrip) {
  MutationHarness h(true);
  const auto demo   = h.mintModule("demo");
  const auto goal   = h.mintSymbol(demo, "Guard");
  const auto clause = h.mintClause(goal, "demo:Guard.cautious");
  const auto body   = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  ASSERT_TRUE(h.rt().learnBody(clause, body.head).has_value());
  const auto spin = h.arena.linked(clause, h.core.dims().spin, Posward);
  ASSERT_TRUE(spin.has_value());
  EXPECT_NE(*spin, body.head);
  ASSERT_TRUE(h.rt().unlearn(clause).has_value());
  EXPECT_FALSE(h.arena.linked(clause, h.core.dims().spin, Posward).has_value());
}

TEST(VagentMutationTest, LearnMethodRoundTrip) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Forager");
  const auto code =
      mintFragmentHead(h, OpcodeKind::Return, "src:m", false, "Int");
  const auto symbol = h.rt().learnMethod("demo:Forager", "f", code.head);
  ASSERT_TRUE(symbol.has_value()) << symbol.error();
  EXPECT_EQ(h.arena.textOf(*symbol), "Forager.f");
  const auto installed = h.arena.linked(*symbol, h.core.dims().values, Posward);
  ASSERT_TRUE(installed.has_value());
  EXPECT_NE(*installed, code.head);
  ASSERT_EQ(registered(h).size(), 1U);
  ASSERT_EQ(patches(h).size(), 1U);

  ASSERT_TRUE(h.rt().unlearn(*symbol).has_value());
  // The symbol leaves the module's d.vars and the registration follows.
  for (const auto cell : zigzag::rankAfter(h.arena, demo, h.core.dims().vars))
    EXPECT_NE(h.arena.textOf(cell), "Forager.f");
  EXPECT_TRUE(patches(h).empty());
  EXPECT_TRUE(registered(h).empty());
}

TEST(VagentMutationTest, UnlearnRefusesPatchRecordAndStrangers) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Feed");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true, "Bool");
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  ASSERT_TRUE(
      h.rt().learnPlan("demo:Feed", "a", test.head, body.head).has_value());
  const auto patch = patches(h).front();
  // The record is refused by its -d.patch backlink.
  EXPECT_FALSE(h.rt().unlearn(patch).has_value());
  // An unlabelled non-symbol operand names no attachment.
  EXPECT_FALSE(h.rt().unlearn(h.arena.makeCell()).has_value());
  // A label no patch records misses.
  const auto stranger = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(stranger, h.core.dims().name, Posward,
                                     h.arena.makeCell("nobody")));
  EXPECT_FALSE(h.rt().unlearn(stranger).has_value());
}

TEST(VagentMutationTest, LearnRefusesProgramSideUnnamedReference) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Feed");
  const auto test = mintFragmentHead(h, OpcodeKind::Return, "t", true, "Bool");
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);
  // A ref-record marking a slot that names a program-side cell: the
  // module is reachable through program() — closed refuses.
  const auto slot = h.arena.makeScalarCell(static_cast<std::int64_t>(demo));
  appendRefs(h, test.head, refRecord(h, slot, std::nullopt, std::nullopt));
  const auto refused = h.rt().learnPlan("demo:Feed", "a", test.head, body.head);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().find("closed"), std::string::npos);
  // The same head grafts fine — closed is learn's gate, not graft's.
  EXPECT_TRUE(
      h.rt().graftPlan("demo:Feed", "a", test.head, body.head).has_value());
}

TEST(VagentMutationTest, LearnAdmitsFailUntilResumeRefusesFailForever) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Feed");
  const auto forager = h.mintSymbol(demo, "Forager");
  const auto op      = h.vm.mintOpcode(OpcodeKind::Return);
  zigzag::expectWritten(
      h.arena.link(forager, h.core.dims().values, Posward, op));
  const auto body = mintFragmentHead(h, OpcodeKind::Return, "b", false);

  // A run-role record naming an unresolvable label rides: admitted.
  const auto deferred =
      mintFragmentHead(h, OpcodeKind::Return, "d", true, "Bool");
  appendRefs(h, deferred.head,
             refRecord(h, h.arena.makeCell(), "reify:later:0", "run"));
  const auto report = h.rt().verifyClosed(deferred.head);
  EXPECT_TRUE(report.closed) << report.detail;
  EXPECT_FALSE(report.notes.empty());
  const auto learned =
      h.rt().learnPlan("demo:Feed", "a", deferred.head, body.head);
  EXPECT_TRUE(learned.has_value()) << learned.error();

  // A run-role record resolving head-less (demo:Forager's values names
  // an op, never a head) is fail-forever — refused.
  const auto doomed =
      mintFragmentHead(h, OpcodeKind::Return, "x", true, "Bool");
  appendRefs(h, doomed.head,
             refRecord(h, h.arena.makeCell(), "demo:Forager", "run"));
  const auto dead = h.rt().verifyClosed(doomed.head);
  EXPECT_FALSE(dead.closed);
  EXPECT_NE(dead.detail.find("fail-forever"), std::string::npos);
}

TEST(VagentMutationTest, SetWeightDispatchesJunctionAndLabelledClause) {
  MutationHarness h;
  // Junction arm: its own +d.weight takes a fresh scalar.
  const auto junction = h.rt().sproutSynapse("demo:a", "demo:b", 0.25);
  ASSERT_TRUE(junction.has_value());
  EXPECT_TRUE(
      h.arena.rankContains(h.agentCell, *junction, h.binds.dims.synapse));
  ASSERT_TRUE(h.rt().setWeight(*junction, 0.9).has_value());
  EXPECT_EQ(h.rt().weightOf(*junction), 0.9);
  // The scalar is a fresh cell — mint-and-repoint, never in-place.
  const auto first = h.arena.linked(*junction, h.binds.dims.weight, Posward);
  ASSERT_TRUE(h.rt().setWeight(*junction, 0.1).has_value());
  const auto second = h.arena.linked(*junction, h.binds.dims.weight, Posward);
  EXPECT_NE(first, second);
  EXPECT_EQ(h.rt().weightOf(*junction), 0.1);

  // Clause arm: the agent's d.vars record keyed by the label mints on
  // first write.
  const auto clause = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(clause, h.core.dims().name, Posward,
                                     h.arena.makeCell("demo:Feed.a")));
  ASSERT_TRUE(h.rt().setWeight(clause, 0.7).has_value());
  EXPECT_EQ(h.rt().weightOf(clause), 0.7);

  // A name-less, weight-less target is the named refusal.
  EXPECT_FALSE(h.rt().setWeight(h.arena.makeCell(), 0.5).has_value());
}

TEST(VagentMutationTest, SproutSynapseShape) {
  MutationHarness h;
  const auto junction =
      h.rt().sproutSynapse("demo:Feed.a", "demo:Guard.c", 0.5);
  ASSERT_TRUE(junction.has_value());
  const auto fields = std::vector<std::pair<std::string, std::string>>{
      {"from", "demo:Feed.a"}, {"to", "demo:Guard.c"}};
  for (const auto &[key, want] : fields) {
    bool seen = false;
    for (const auto rec :
         zigzag::rankAfter(h.arena, *junction, h.core.dims().vars))
      if (h.arena.textOf(rec) == key) {
        seen = true;
        EXPECT_EQ(
            h.arena.textOf(*h.arena.linked(rec, h.core.dims().values, Posward)),
            want);
      }
    EXPECT_TRUE(seen) << key;
  }
  EXPECT_EQ(h.rt().weightOf(*junction), 0.5);
}

// A covered recipe member naming a *persistent* image-side position is
// internal — the closed walk must not mistake it for an outbound
// program-side edge. An image-resident `code"…"` head records exactly this
// shape (its members sit at ≤ imageExtent); the ephemeral-only internal
// set regressed it to a false refusal.
TEST(VagentMutationTest, VerifyClosedTreatsPersistentCoveredMemberAsInternal) {
  MutationHarness h(true);
  const auto head = mintFragmentHead(h, OpcodeKind::Return, "f", true, "Bool");
  // Append a recipe member storing a persistent position inside the image
  // band (≤ imageExtent) to the recipe's +d.step tail — a covered cell an
  // image-resident fragment legitimately names.
  const auto covered = h.arena.makeScalarCell(std::int64_t{2});
  auto tail          = *h.arena.linked(head.head, h.binds.dims.recipe, Posward);
  for (auto next = h.arena.linked(tail, h.core.dims().step, Posward);
       next && h.arena.contains(*next);
       next = h.arena.linked(tail, h.core.dims().step, Posward))
    tail = *next;
  zigzag::expectWritten(
      h.arena.link(tail, h.core.dims().step, Posward, covered));
  // Its ref-record marks the member — the closed walk resolves pos=2 to
  // internal, not "outbound edge to program-side cell".
  appendRefs(h, head.head, refRecord(h, covered, std::nullopt, std::nullopt));
  const auto report = h.rt().verifyClosed(head.head);
  EXPECT_TRUE(report.closed) << report.detail;
}

// The converse: a recipe member that is NOT covered — a bare program-side
// referent a caller smuggles onto the refs chain — is refused. Positions
// the recipe never covered stay outbound.
TEST(VagentMutationTest, VerifyClosedRefusesUncoveredProgramSideMember) {
  MutationHarness h(true);
  const auto demo = h.mintModule("demo");
  (void)h.mintSymbol(demo, "Feed");
  const auto head = mintFragmentHead(h, OpcodeKind::Return, "f", true, "Bool");
  // A ref-record marking a member whose pos names a program-side cell the
  // recipe never covered — outbound, refused.
  const auto smuggled =
      h.arena.makeScalarCell(static_cast<std::int64_t>(demo));
  appendRefs(h, head.head, refRecord(h, smuggled, std::nullopt, std::nullopt));
  const auto report = h.rt().verifyClosed(head.head);
  EXPECT_FALSE(report.closed);
  EXPECT_NE(report.detail.find("outbound"), std::string::npos)
      << report.detail;
}

// A malformed patch/vars field parses to nullopt rather than throwing —
// the std::stoll site it replaced crashed genesis mid-ceremony on a
// non-numeric record.
TEST(VagentMutationTest, ToInt64RefusesMalformedText) {
  EXPECT_EQ(toInt64("42"), 42);
  EXPECT_EQ(toInt64("-7"), -7);
  EXPECT_EQ(toInt64("0"), 0);
  EXPECT_FALSE(toInt64("notanumber").has_value());
  EXPECT_FALSE(toInt64("12abc").has_value()); // trailing junk rejected
  EXPECT_FALSE(toInt64("").has_value());
  EXPECT_FALSE(toInt64("  9").has_value()); // no whitespace tolerance
}

// The recipe's `±1` direction flag is a field value, not a referent —
// verifyClosed must not read it as an outbound edge to the home cell,
// nor must it count pos=1/pos=-1 covered.
TEST(VagentMutationTest, VerifyClosedDirectionFlagIsNotAReferent) {
  MutationHarness h(true);
  const auto head = mintFragmentHead(h, OpcodeKind::Return, "f", true, "Bool");
  // mark the direction member itself (pos=1) — it should classify as a
  // direction field, never "outbound edge to program-side cell 1".
  const auto dirMember = h.arena.makeScalarCell(std::int64_t{1});
  appendRefs(h, head.head, refRecord(h, dirMember, std::nullopt, std::nullopt));
  const auto report = h.rt().verifyClosed(head.head);
  EXPECT_TRUE(report.closed) << report.detail;
}

TEST(VagentMutationTest, LearnHandlerIsAdditiveWithPriority) {
  MutationHarness h(true);
  const auto demo  = h.mintModule("demo");
  const auto event = h.mintSymbol(demo, "Bumped");
  const auto g1    = mintFragmentHead(h, OpcodeKind::Return, "g1", true);
  const auto b1    = mintFragmentHead(h, OpcodeKind::Return, "b1", false);
  const auto g2    = mintFragmentHead(h, OpcodeKind::Return, "g2", true);
  const auto b2    = mintFragmentHead(h, OpcodeKind::Return, "b2", false);
  ASSERT_TRUE(
      h.rt().learnHandler("demo:Bumped", "w", g1.head, b1.head, 2).has_value());
  ASSERT_TRUE(
      h.rt().learnHandler("demo:Bumped", "w", g2.head, b2.head, 9).has_value());
  // Additive: the same alias installs twice under two patch:<n>.
  EXPECT_EQ(h.rt().clausesOf(event).size(), 2U);
  const auto ps = patches(h);
  ASSERT_EQ(ps.size(), 2U);
}

} // namespace
