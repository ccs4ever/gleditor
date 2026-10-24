/**
 * @file vagent_belief_test.cpp
 * @brief Tests for vagent beliefs (design/vagent-agent-language.md §6.1, §6.5,
 * §9.1): assert, retract, sources, keyed replacement, queries, belief events
 * and the settle loop, run through the compiler's two routines with a hand-set
 * `$turn` as the executor adapter would leave it.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/runtime_abi.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kMaxCycles = 4'000'000;

CellValue I(std::int64_t v) { return v; }
CellValue F(double v) { return v; }
CellValue Str(const char *v) { return std::string(v); }

using Fact = std::vector<CellValue>;

struct Harness {
  ArenaManifold arena;
  VortexCore core{arena};
  Compiler compiler{core};
  VortexVM vm{core};
  Unit unit;
  CompiledAgent ca;
  AgentDims dims;
  std::string lastError;

  bool compile(const std::string &src, const std::string &agent) {
    auto parsed = parseUnit(src);
    if (!parsed) {
      lastError = formatParseError(parsed.error());
      return false;
    }
    unit   = std::move(*parsed);
    auto r = compiler.compileAgent(unit, agent);
    if (!r) {
      lastError = std::string(compileErrorName(r.error().code)) + ": " +
                  r.error().detail;
      return false;
    }
    ca   = *r;
    dims = registerDimensions(arena);
    vm.setDeweyTracking(false);
    setField("$name", Str("me"));
    return true;
  }

  // -- structure helpers ----------------------------------------------------
  CellRef link(CellRef c, zigzag::DimRef d) const {
    return arena.linked(c, d, DimVector::POS);
  }
  void chain(CellRef from, zigzag::DimRef dim, CellRef to) {
    zigzag::expectWritten(arena.link(from, dim, DimVector::POS, to));
  }
  CellRef fieldOf(CellRef owner, const std::string &name) const {
    for (CellRef n = link(owner, core.dims().vars); n != noCell;
         n         = link(n, core.dims().vars)) {
      if (arena.textOf(n) == name) return link(n, core.dims().values);
    }
    return noCell;
  }
  CellRef nameCell(const std::string &name) const {
    for (CellRef n = link(ca.agentCell, core.dims().vars); n != noCell;
         n         = link(n, core.dims().vars)) {
      if (arena.textOf(n) == name) return n;
    }
    return noCell;
  }
  CellRef fieldCell(const std::string &name) const {
    return fieldOf(ca.agentCell, name);
  }
  CellValue field(const std::string &name) const {
    return core.render(fieldCell(name));
  }
  void setField(const std::string &name, const CellValue &v) {
    (void)core.value(fieldCell(name), 0, -1, v);
  }
  std::int64_t intField(const std::string &name) const {
    return std::get<std::int64_t>(field(name));
  }
  CellRef scalarCell(const CellValue &v) {
    if (std::holds_alternative<std::int64_t>(v))
      return arena.makeScalarCell(std::get<std::int64_t>(v));
    if (std::holds_alternative<double>(v))
      return arena.makeScalarCell(std::get<double>(v));
    if (std::holds_alternative<bool>(v))
      return arena.makeScalarCell(std::get<bool>(v));
    return arena.makeCell(std::get<std::string>(v));
  }
  CellRef listCell(const std::vector<CellValue> &xs) {
    const CellRef l = arena.makeCell(std::string(kListContent));
    CellRef prev    = noCell;
    for (const auto &x : xs) {
      const CellRef e = scalarCell(x);
      chain(prev == noCell ? l : prev,
            prev == noCell ? core.dims().grab : core.dims().step, e);
      prev = e;
    }
    return l;
  }
  /// An `Opt<Agent>` or `Opt<Space>` holding @p value: a list cell of one.
  CellRef optCell(const std::string &value) {
    const CellRef l = arena.makeCell(std::string(kListContent));
    chain(l, core.dims().grab, arena.makeCell(value));
    return l;
  }
  CellRef
  makeEventCells(const std::string &type,
                 const std::vector<std::pair<std::string, CellRef>> &declared,
                 const std::string &space = "inner") {
    const CellRef e                                    = arena.makeCell(type);
    std::vector<std::pair<std::string, CellRef>> cells = declared;
    cells.emplace_back("source", optCell("peer"));
    cells.emplace_back("space", optCell(space));
    cells.emplace_back("time", arena.makeScalarCell(std::int64_t{1000}));
    cells.emplace_back("ordinal", arena.makeScalarCell(std::int64_t{1}));
    CellRef prev = e;
    for (const auto &[n, v] : cells) {
      const CellRef nc = arena.makeCell(n);
      chain(prev, core.dims().vars, nc);
      chain(nc, core.dims().values, v);
      prev = nc;
    }
    return e;
  }
  void setTurn(CellRef event) {
    const CellRef rec = arena.makeCell();
    const CellRef tn  = arena.makeCell(std::string(kTurnTime));
    chain(rec, core.dims().vars, tn);
    chain(tn, core.dims().values, arena.makeScalarCell(std::int64_t{1000}));
    const CellRef en = arena.makeCell(std::string(kTurnEvent));
    chain(tn, core.dims().vars, en);
    chain(en, core.dims().values, event);
    chain(nameCell(std::string(kFieldTurn)), core.dims().values, rec);
  }

  // -- running --------------------------------------------------------------
  bool run(CellRef entry) {
    const CellRef cursor = vm.spawnCursor(entry);
    vm.pushChoicePoint(cursor, ca.failOp);
    const auto r  = vm.run(cursor, kMaxCycles);
    lastError     = r.errorMessage;
    const bool ok = r.success && vm.getCursorOpcode(cursor) == ca.haltOp;
    vm.clearChoicePoints();
    return ok;
  }
  bool init() { return run(ca.initEntry); }
  bool dispatch(const std::string &type,
                const std::vector<std::pair<std::string, CellValue>> &fs = {},
                const std::string &space = "inner") {
    std::vector<std::pair<std::string, CellRef>> cells;
    for (const auto &[n, v] : fs) cells.emplace_back(n, scalarCell(v));
    setTurn(makeEventCells(type, cells, space));
    return run(ca.dispatchEntry);
  }
  bool dispatchCells(const std::string &type,
                     const std::vector<std::pair<std::string, CellRef>> &cells,
                     const std::string &space = "inner") {
    setTurn(makeEventCells(type, cells, space));
    return run(ca.dispatchEntry);
  }

  // -- beliefs --------------------------------------------------------------
  CellRef predicate(const std::string &name) const {
    for (CellRef p = link(ca.agentCell, dims.belief); p != noCell;
         p         = link(p, dims.belief)) {
      if (arena.textOf(p) == name) return p;
    }
    return noCell;
  }
  std::vector<CellRef> clauses(const std::string &pred) const {
    std::vector<CellRef> out;
    for (CellRef c = link(predicate(pred), core.dims().clause); c != noCell;
         c         = link(c, core.dims().clause)) {
      out.push_back(c);
    }
    return out;
  }
  Fact termArgs(CellRef clause) const {
    Fact out;
    const CellRef term = link(clause, core.dims().grab);
    for (CellRef a = link(term, core.dims().grab); a != noCell;
         a         = link(a, core.dims().step)) {
      out.push_back(core.render(a));
    }
    return out;
  }
  std::vector<Fact> facts(const std::string &pred) const {
    std::vector<Fact> out;
    for (const CellRef c : clauses(pred)) out.push_back(termArgs(c));
    return out;
  }
  std::vector<std::string> sources(CellRef clause) const {
    std::vector<std::string> out;
    for (CellRef s = link(clause, dims.source); s != noCell;
         s         = link(s, dims.source)) {
      out.push_back(arena.textOf(s));
    }
    return out;
  }
  void addSource(CellRef clause, const std::string &who) {
    const CellRef s = arena.makeCell(who);
    CellRef tail    = clause;
    while (link(tail, dims.source) != noCell) tail = link(tail, dims.source);
    chain(tail, dims.source, s);
  }
  std::size_t pendingCount() const {
    std::size_t n = 0;
    for (CellRef e = link(ca.agentCell, dims.pending); e != noCell;
         e         = link(e, dims.pending)) {
      ++n;
    }
    return n;
  }
  std::vector<CellRef> outbox() const {
    std::vector<CellRef> out;
    for (CellRef e = link(ca.agentCell, dims.outbox); e != noCell;
         e         = link(e, dims.outbox)) {
      out.push_back(e);
    }
    return out;
  }
  std::vector<CellValue> listValues(CellRef list) const {
    std::vector<CellValue> out;
    for (CellRef e = link(list, core.dims().grab); e != noCell;
         e         = link(e, core.dims().step)) {
      out.push_back(core.render(e));
    }
    return out;
  }
};

constexpr const char *kPrelude = R"vagent(
event Tick { x: Int }
event Ping { n: Int }
event Tock { y: Int }
predicate color(c: Str)
predicate n(v: Int)
predicate pair(a: Int, b: Int)
predicate at(key who: Str, x: Float, y: Float)
predicate tags(who: Str, names: [Str])
)vagent";

std::string program(const std::string &agentBody) {
  return std::string(kPrelude) + "agent A {\n" + agentBody + "\n}\n";
}

// -- assert, duplicates, sources ---------------------------------------------

TEST(VagentBelief, BelieveAddsAFactWithTheAgentAsItsSource) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  on Ping { believe color("red") }
)"),
                        "A"))
      << h.lastError;
  ASSERT_NE(h.predicate("color"), noCell);
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}})) << h.lastError;
  EXPECT_EQ(h.facts("color"), (std::vector<Fact>{{Str("red")}}));
  EXPECT_EQ(h.sources(h.clauses("color")[0]), (std::vector<std::string>{"me"}));
  EXPECT_EQ(h.pendingCount(), 0U);
}

TEST(VagentBelief, PredicateCellsHangOnTheAgentsBeliefRank) {
  Harness h;
  ASSERT_TRUE(h.compile(program(""), "A")) << h.lastError;
  for (const char *p : {"color", "n", "pair", "at", "tags"}) {
    EXPECT_NE(h.predicate(p), noCell) << p;
  }
}

TEST(VagentBelief, ADuplicateAddsNeitherAClauseNorASecondSource) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var adds: Int := 0
  on +color(?c) { adds := adds + 1 }
  on Ping { believe color("red") }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(2)}}));
  EXPECT_EQ(h.clauses("color").size(), 1U);
  EXPECT_EQ(h.sources(h.clauses("color")[0]).size(), 1U);
  EXPECT_EQ(h.intField("adds"), 1);
}

TEST(VagentBelief, ASecondSourceIsAddedWithoutAnEventAndForgetKeepsTheFact) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var adds: Int := 0
  var drops: Int := 0
  on +color(?c) { adds := adds + 1 }
  on -color(?c) { drops := drops + 1 }
  on Ping { believe color("red") }
  on Tick { forget color("red") }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  h.addSource(h.clauses("color")[0], "peer-b");
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  ASSERT_EQ(h.clauses("color").size(), 1U);
  EXPECT_EQ(h.sources(h.clauses("color")[0]),
            (std::vector<std::string>{"peer-b"}));
  EXPECT_EQ(h.intField("adds"), 1);
  EXPECT_EQ(h.intField("drops"), 0);
}

TEST(VagentBelief, BelievingAnotherAgentsFactJoinsItsSourceList) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  on Ping { believe color("red") }
)"),
                        "A"))
      << h.lastError;
  // A fact asserted by a peer first (as a `tell` will leave it), then ours.
  h.setField("$name", Str("peer-a"));
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  h.setField("$name", Str("me"));
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(2)}}));
  ASSERT_EQ(h.clauses("color").size(), 1U);
  EXPECT_EQ(h.sources(h.clauses("color")[0]),
            (std::vector<std::string>{"peer-a", "me"}));
}

TEST(VagentBelief, ForgetRetractsWithRankRepairAndRaisesMinusOncePerFact) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var drops: Int := 0
  on -color(?c) { drops := drops + 1 }
  on Ping { believe color("a")
            believe color("b")
            believe color("c") }
  on Tick { forget color("b") }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.facts("color"), (std::vector<Fact>{{Str("a")}, {Str("c")}}));
  EXPECT_EQ(h.intField("drops"), 1);
  // Forgetting what is not there changes nothing and raises nothing.
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(2)}}));
  EXPECT_EQ(h.intField("drops"), 1);
  // The repaired rank still takes new facts at its tail.
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(2)}}));
  EXPECT_EQ(h.facts("color"),
            (std::vector<Fact>{{Str("a")}, {Str("c")}, {Str("b")}}));
}

TEST(VagentBelief, ForgetWithAWildcardRemovesEveryMatchingFact) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var drops: Int := 0
  on -n(?v) { drops := drops + 1 }
  on Ping { believe n(1)
            believe n(2)
            believe n(3) }
  on Tick { forget n(_) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_EQ(h.clauses("n").size(), 3U);
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_TRUE(h.clauses("n").empty());
  EXPECT_EQ(h.intField("drops"), 3);
}

TEST(VagentBelief, ForgetByAValueArgumentSparesTheOthers) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  on Ping { believe n(1)
            believe n(2)
            believe n(3)
            forget n(2) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.facts("n"), (std::vector<Fact>{{I(1)}, {I(3)}}));
}

// -- keyed predicates, revise ------------------------------------------------

TEST(VagentBelief, ABeliefWithTheSameKeyReplacesTheSourcesEarlierFact) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var adds: Int := 0
  var drops: Int := 0
  on +at(?w, ?x, ?y) { adds := adds + 1 }
  on -at(?w, ?x, ?y) { drops := drops + 1 }
  on Ping { believe at("w", 1.0, 2.0)
            believe at("v", 7.0, 8.0) }
  on Tick { believe at("w", 3.0, 4.0) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.facts("at"), (std::vector<Fact>{{Str("v"), F(7.0), F(8.0)},
                                              {Str("w"), F(3.0), F(4.0)}}));
  EXPECT_EQ(h.intField("adds"), 3);
  EXPECT_EQ(h.intField("drops"), 1);
}

TEST(VagentBelief, ReviseReplacesByKeyAndRaisesNothingWhenNothingChanged) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var adds: Int := 0
  var drops: Int := 0
  on +at(?w, ?x, ?y) { adds := adds + 1 }
  on -at(?w, ?x, ?y) { drops := drops + 1 }
  on Ping { revise at("w", 5.0, 6.0) }
  on Tick { revise at("w", 9.0, 9.5) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(2)}}));
  EXPECT_EQ(h.intField("adds"), 1);
  EXPECT_EQ(h.intField("drops"), 0);
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.facts("at"), (std::vector<Fact>{{Str("w"), F(9.0), F(9.5)}}));
  EXPECT_EQ(h.intField("adds"), 2);
  EXPECT_EQ(h.intField("drops"), 1);
}

TEST(VagentBelief, ReviseOnAnUnkeyedPredicateReplacesEveryOwnFact) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  on Ping { believe color("red")
            believe color("green") }
  on Tick { revise color("blue") }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_EQ(h.clauses("color").size(), 2U);
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.facts("color"), (std::vector<Fact>{{Str("blue")}}));
}

TEST(VagentBelief, ReviseLeavesAFactAnotherSourceStillAsserts) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var drops: Int := 0
  on -color(?c) { drops := drops + 1 }
  on Ping { believe color("red") }
  on Tick { revise color("blue") }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  h.addSource(h.clauses("color")[0], "peer-b");
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.facts("color"), (std::vector<Fact>{{Str("red")}, {Str("blue")}}));
  EXPECT_EQ(h.sources(h.clauses("color")[0]),
            (std::vector<std::string>{"peer-b"}));
  EXPECT_EQ(h.intField("drops"), 0);
}

// -- queries -----------------------------------------------------------------

TEST(VagentBelief, BelievesBindsVariablesIntoLocals) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var gx: Float := 0.0
  var gy: Float := 0.0
  var found: Int := 0
  on Ping { believe at("w", 1.5, 2.5) }
  on Tick {
    if believes at("w", ?x, ?y) then { gx := x
                                       gy := y
                                       found := 1 }
  }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("found"), 0);
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(2)}}));
  EXPECT_EQ(h.intField("found"), 1);
  EXPECT_EQ(std::get<double>(h.field("gx")), 1.5);
  EXPECT_EQ(std::get<double>(h.field("gy")), 2.5);
}

TEST(VagentBelief, BelievesBacktracksAcrossClausesWithinAStatement) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var pick: Int := 0
  on Ping { believe n(1)
            believe n(2)
            believe n(3) }
  on Tick { believes n(?v) & v > 1
            pick := v }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("pick"), 2);
}

TEST(VagentBelief, BelievesFailsWhenNothingMatches) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var hit: Int := 0
  var miss: Int := 0
  on Ping { believe n(1) }
  on Tick { if believes n(7) then hit := 1 else miss := 1 }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("hit"), 0);
  EXPECT_EQ(h.intField("miss"), 1);
}

TEST(VagentBelief, NotBelievesSucceedsOnlyWhenNothingMatches) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var seen: Int := 0
  on Ping { if not believes n(1) then { believe n(1)
                                        seen := seen + 1 } }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(2)}}));
  EXPECT_EQ(h.intField("seen"), 1);
}

TEST(VagentBelief, ARepeatedVariableMustMatchTheSameValue) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var same: Int := 0
  on Ping { believe pair(1, 2)
            believe pair(3, 3) }
  on Tick { if believes pair(?x, ?x) then same := x }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("same"), 3);
}

TEST(VagentBelief, BelievesFromSelfRestrictsToTheAgentsOwnSource) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var own: Int := 0
  var any: Int := 0
  on Ping { believe n(1) }
  on Tick { believe n(2) }
  on Tock { if believes n(?v) from self then own := v
            if believes n(?w) then any := w }
)"),
                        "A"))
      << h.lastError;
  h.setField("$name", Str("peer-a"));
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  h.setField("$name", Str("me"));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tock", {{"y", I(1)}}));
  EXPECT_EQ(h.intField("own"), 2);
  EXPECT_EQ(h.intField("any"), 1);
}

TEST(VagentBelief, FindallListsBindingsInAssertionOrder) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var xs: [Int] := []
  var count: Int := 0
  on Ping { believe n(3)
            believe n(1)
            believe n(2) }
  on Tick { xs := findall(?v, n(?v))
            count := *findall(?w, n(?w)) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.listValues(h.fieldCell("xs")),
            (std::vector<CellValue>{I(3), I(1), I(2)}));
  EXPECT_EQ(h.intField("count"), 3);
}

TEST(VagentBelief, FindallOverAFilteredGoalAndAnEmptyBase) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var sums: [Int] := []
  var empty: Int := 9
  on Ping { believe pair(1, 10)
            believe pair(2, 20)
            believe pair(1, 30) }
  on Tick { sums := findall(?b, pair(1, ?b))
            empty := *findall(?b, pair(7, ?b)) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init());
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.listValues(h.fieldCell("sums")),
            (std::vector<CellValue>{I(10), I(30)}));
  EXPECT_EQ(h.intField("empty"), 0);
}

TEST(VagentBelief, ForBelievesRunsTheBodyOncePerMatchingFact) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var total: Int := 0
  var visits: Int := 0
  on Ping { believe n(1)
            believe n(2)
            believe n(3) }
  on Tick { for believes n(?v) do { total := total + v
                                    visits := visits + 1 } }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("total"), 6);
  EXPECT_EQ(h.intField("visits"), 3);
}

TEST(VagentBelief, ForBelievesVisitsTheListAsItWasWhenTheLoopStarted) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var visits: Int := 0
  on Ping { believe n(1)
            believe n(2) }
  on Tick { for believes n(?v) do { visits := visits + 1
                                    believe n(v + 10)
                                    forget n(2) } }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("visits"), 2);
  EXPECT_EQ(h.facts("n"), (std::vector<Fact>{{I(1)}, {I(11)}, {I(12)}}));
}

TEST(VagentBelief, ListValuedArgumentsAreStoredAsTermsAndMatchByUnify) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var got: Int := 0
  on Ping { believe tags("w", ["a", "b"]) }
  on Tick { if believes tags("w", ["a", "b"]) then got := 1 }
  on Tock { if believes tags("w", ["a", "c"]) then got := 2 }
)"
                                ""),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  ASSERT_EQ(h.clauses("tags").size(), 1U);
  const CellRef term = h.link(h.clauses("tags")[0], h.core.dims().grab);
  const CellRef lst =
      h.link(h.link(term, h.core.dims().grab), h.core.dims().step);
  EXPECT_EQ(h.listValues(lst), (std::vector<CellValue>{Str("a"), Str("b")}));
  ASSERT_TRUE(h.dispatch("Tick", {{"x", I(1)}}));
  EXPECT_EQ(h.intField("got"), 1);
  h.setField("got", I(0));
  ASSERT_TRUE(h.dispatch("Tock", {{"y", I(1)}}));
  EXPECT_EQ(h.intField("got"), 0);
}

TEST(VagentBelief, InitialBeliefsAreAssertedBeforeInitializeRuns) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var seen: Int := 0
  believe n(5)
  on Initialize { if believes n(?v) then seen := v }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  EXPECT_EQ(h.intField("seen"), 5);
  EXPECT_EQ(h.facts("n"), (std::vector<Fact>{{I(5)}}));
  EXPECT_EQ(h.sources(h.clauses("n")[0]), (std::vector<std::string>{"me"}));
}

TEST(VagentBelief, AFailingHandlerUndoesItsBeliefsAndItsEvents) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var adds: Int := 0
  on +n(?v) { adds := adds + 1 }
  on Ping { believe n(1)
            fail }
  on Ping { believe n(2) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.facts("n"), (std::vector<Fact>{{I(2)}}));
  EXPECT_EQ(h.intField("adds"), 1);
  EXPECT_EQ(h.pendingCount(), 0U);
}

// -- belief events and the settle loop ----------------------------------------

TEST(VagentBelief, AddTriggersFireOncePerChangeWithBoundArguments) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var last: Int := 0
  var adds: Int := 0
  on +n(?v) { last := v
              adds := adds + 1 }
  on Ping { believe n(4)
            believe n(4)
            believe n(5) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.intField("adds"), 2);
  EXPECT_EQ(h.intField("last"), 5);
}

TEST(VagentBelief, TriggerGuardsSeeTheBoundVariablesAndPriorityOrdersHandlers) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var order: Int := 0
  var big: Int := 0
  on +n(?v) [v > 10] { big := big + 1 }
  on +n(?v) priority 5 { order := order * 10 + 1 }
  on +n(?v) { order := order * 10 + 2 }
  on Ping { believe n(4)
            believe n(40) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.intField("big"), 1);
  EXPECT_EQ(h.intField("order"), 1212);
}

TEST(VagentBelief, TriggerArgumentsMayBeValuesThatMustMatch) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var hits: Int := 0
  on +at("w", ?x, ?y) { hits := hits + 1 }
  on Ping { believe at("w", 1.0, 2.0)
            believe at("v", 1.0, 2.0) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.intField("hits"), 1);
}

TEST(VagentBelief, EventsRaisedByHandlersAreSettledInLaterRounds) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var trail: Int := 0
  on +n(?v) [v < 3] { trail := trail * 10 + v
                      believe n(v + 1) }
  on +n(?v) [v >= 3] { trail := trail * 10 + v }
  on Ping { believe n(0) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.intField("trail"), 123);
  EXPECT_EQ(h.pendingCount(), 0U);
}

TEST(VagentBelief, SettleBeyondTheDepthBoundIsAHardFailureThatUndoesTheTurn) {
  const std::string body = R"(
  on +n(?v) [v < 3] { believe n(v + 1) }
  on Ping { believe n(0) }
)";
  {
    Harness h;
    h.compiler.setBeliefEventDepth(4);
    ASSERT_TRUE(h.compile(program(body), "A")) << h.lastError;
    EXPECT_TRUE(h.dispatch("Ping", {{"n", I(1)}})) << h.lastError;
    EXPECT_EQ(h.clauses("n").size(), 4U);
  }
  {
    Harness h;
    h.compiler.setBeliefEventDepth(3);
    ASSERT_TRUE(h.compile(program(body), "A")) << h.lastError;
    EXPECT_FALSE(h.dispatch("Ping", {{"n", I(1)}}));
    EXPECT_TRUE(h.clauses("n").empty()) << "the turn's writes are undone";
  }
}

TEST(VagentBelief, AnInfiniteChainFailsAtTheDefaultDepth) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  on +n(?v) { believe n(v + 1) }
  on Ping { believe n(0) }
)"),
                        "A"))
      << h.lastError;
  EXPECT_FALSE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_TRUE(h.clauses("n").empty());
}

TEST(VagentBelief, ConsumeStopsLowerBeliefHandlers) {
  Harness h;
  ASSERT_TRUE(h.compile(program(R"(
  var low: Int := 0
  var high: Int := 0
  on +n(?v) priority 2 { high := high + 1
                         consume }
  on +n(?v) { low := low + 1 }
  on Ping { believe n(1) }
)"),
                        "A"))
      << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping", {{"n", I(1)}}));
  EXPECT_EQ(h.intField("high"), 1);
  EXPECT_EQ(h.intField("low"), 0);
}

// -- §12.1: the coordinator's belief logic
// --------------------------------------

constexpr const char *kSearch = R"vagent(
module demo:search
import std:agent

predicate pending(query_id: Str)
predicate reported(query_id: Str, partition: Int)
predicate hit(query_id: Str, text: Str)

event SearchQuery { query_id: Str, pattern: Str }
event SubTask { query_id: Str, partition: Int, pattern: Str }
event PartitionResult { query_id: Str, partition: Int, hits: [Str] }
event SearchCompleted { query_id: Str, hits: [Str] }

agent Worker {
  on SubTask as t {
    emit PartitionResult{query_id: t.query_id, partition: t.partition,
                         hits: scan_partition(t.partition, t.pattern)} for t.source
  }
  def scan_partition(index: Int, pattern: Str): [Str] {
    ["alpha-" ++ text(index), "beta-" ++ text(index)]
  }
}

agent SearchCoordinator(partitions: Int) {
  var workers: [Agent] := []

  on Initialize {
    for _ in 1 to partitions do workers := workers ++ [spawn Worker() in inner]
  }

  on SearchQuery as q [q.space == outer] {
    believe pending(q.query_id)
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner for workers[i]
  }

  on PartitionResult as r [r.space == inner & not believes reported(r.query_id, r.partition)] {
    believe reported(r.query_id, r.partition)
    for h in r.hits do believe hit(r.query_id, h)
    if *findall(?p, reported(r.query_id, ?p)) == partitions then {
      emit SearchCompleted{query_id: r.query_id, hits: findall(?h, hit(r.query_id, ?h))} in outer
      forget pending(r.query_id)
    }
  }
}
)vagent";

struct Search : Harness {
  void result(std::int64_t partition) {
    const std::string p = std::to_string(partition);
    ASSERT_TRUE(dispatchCells(
        "PartitionResult",
        {{"query_id", arena.makeCell("q1")},
         {"partition", arena.makeScalarCell(partition)},
         {"hits",
          listCell({Str(("alpha-" + p).c_str()), Str(("beta-" + p).c_str())})}},
        "inner"))
        << lastError;
  }
  std::size_t eventsOfType(const std::string &type) const {
    std::size_t n = 0;
    for (const CellRef e : outbox()) {
      const CellRef ev = fieldOf(e, std::string(kOutboxEvent));
      if (ev != noCell && arena.textOf(ev) == type) ++n;
    }
    return n;
  }
};

TEST(VagentBelief, TheCoordinatorGathersEveryPartitionOnceAndCompletes) {
  Search h;
  ASSERT_TRUE(h.compile(kSearch, "SearchCoordinator")) << h.lastError;
  h.setField("partitions", I(3));
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatchCells("SearchQuery",
                              {{"query_id", h.arena.makeCell("q1")},
                               {"pattern", h.arena.makeCell("x")}},
                              "outer"))
      << h.lastError;
  EXPECT_EQ(h.facts("pending"), (std::vector<Fact>{{Str("q1")}}));
  EXPECT_EQ(h.eventsOfType("SubTask"), 3U);

  h.result(0);
  h.result(1);
  // A duplicate delivery of partition 1 fails the guard and changes nothing.
  h.result(1);
  EXPECT_EQ(h.facts("reported").size(), 2U);
  EXPECT_EQ(h.facts("hit").size(), 4U);
  EXPECT_EQ(h.eventsOfType("SearchCompleted"), 0U);

  h.result(2);
  EXPECT_EQ(h.facts("reported").size(), 3U);
  ASSERT_EQ(h.eventsOfType("SearchCompleted"), 1U);
  EXPECT_TRUE(h.facts("pending").empty());

  CellRef done = noCell;
  for (const CellRef e : h.outbox()) {
    const CellRef ev = h.fieldOf(e, std::string(kOutboxEvent));
    if (ev != noCell && h.arena.textOf(ev) == "SearchCompleted") done = ev;
  }
  ASSERT_NE(done, noCell);
  EXPECT_EQ(
      h.listValues(h.fieldOf(done, "hits")),
      (std::vector<CellValue>{Str("alpha-0"), Str("beta-0"), Str("alpha-1"),
                              Str("beta-1"), Str("alpha-2"), Str("beta-2")}));
}

} // namespace
