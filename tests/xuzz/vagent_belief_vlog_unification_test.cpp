/**
 * @file vagent_belief_vlog_unification_test.cpp
 * @brief Acceptance battery for Belief / Vlog logic engine unification (P18.1,
 *        P18.2, P18.3).
 *
 * Verifies:
 *  1. Ground fact unification and multi-source provenance isolation on
 * d.source.
 *  2. Deductive Horn clause resolution via solveQueryHelper over base d.clause
 *     and +d.spin subgoals.
 *  3. Stratified negation as failure (not / \+) with ground safety checking
 *     (throwing floundered_negation on unbound variables).
 *  4. Choice-point and mark barrier state isolation ($d_0$, $m_0$), ensuring
 * logic search backtracking never rolls back actor mutable state.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent::runtime {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::vortex::CellValue;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexStdLib;
using zigzag::vortex::VortexVM;

constexpr std::size_t kMaxCycles = 4'000'000;

struct TestHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  Compiler compiler{core};
  VortexVM vm{core};
  Unit unit;
  CompiledAgent ca;
  AgentDims dims;
  std::string lastError;

  bool compile(const std::string &src, const std::string &agent = "Tracker") {
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
    setField("$name", std::string("me"));
    return true;
  }

  CellRef link(CellRef c, zigzag::DimRef d) const {
    return arena.linked(c, d, DimVector::POS).value_or(noCell);
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
    return core.render(fieldCell(name)).value();
  }
  void setField(const std::string &name, const CellValue &v) {
    (void)core.writeValue(fieldCell(name), 0, std::nullopt, v);
  }
  double floatField(const std::string &name) const {
    return std::get<double>(field(name));
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
                const std::vector<std::pair<std::string, CellValue>> &fs = {}) {
    std::vector<std::pair<std::string, CellRef>> cells;
    for (const auto &[n, v] : fs) cells.emplace_back(n, scalarCell(v));
    setTurn(makeEventCells(type, cells));
    return run(ca.dispatchEntry);
  }
};

// -- 1. Ground Fact Unification & Multi-Source Provenance (P18.1, P18.3)
// -------

TEST(VagentBeliefVlogUnificationTest,
     MultiSourceProvenanceAndKeyedExcisionCoexist) {
  constexpr const char *kSrc = R"vagent(
event Ping
predicate at(key who: Str, x: Float, y: Float)

agent Tracker {
  var last_x: Float := 0.0

  on Ping {
    // Assert from distinct sources
    assert at("scout", 12.5, 40.0) from "sensor:gps"
    assert at("scout", 12.0, 39.5) from "scout"

    // Query with source filter: verifies that sensor:gps is isolated
    if believes at("scout", ?x1, ?y1) from "sensor:gps" then {
      last_x := x1
    }
  }
}
)vagent";

  TestHarness h;
  ASSERT_TRUE(h.compile(kSrc, "Tracker")) << h.lastError;
  ASSERT_TRUE(h.init()) << h.lastError;
  ASSERT_TRUE(h.dispatch("Ping")) << h.lastError;

  // Verify that last_x was set to 12.5 from sensor:gps
  EXPECT_DOUBLE_EQ(h.floatField("last_x"), 12.5);
}

// -- 2. Deductive Horn Clause Resolution via solveQueryHelper (P18.2) --------

TEST(VagentBeliefVlogUnificationTest, DeductivePathfindingViaHornRules) {
  ArenaManifold arena;
  VortexCore core{arena};
  vprolog::Compiler prologCompiler{core};

  // Program defining directed graph and recursive path/2 rules
  const std::string progSrc = R"(
    edge(a, b).
    edge(b, c).
    edge(c, d).
    edge(d, e).

    path(X, Y) :- edge(X, Y).
    path(X, Y) :- edge(X, Z), path(Z, Y).
  )";

  vprolog::Parser parser(progSrc);
  vprolog::Program prog = parser.parseProgram();
  prologCompiler.compileProgram(prog);

  // Query: ?- path(a, e).
  vprolog::CompiledQuery q1 = prologCompiler.compileQuery("path(a, e)");
  EXPECT_TRUE(prologCompiler.solveOnce(q1));

  // Query: ?- path(a, Target).
  vprolog::CompiledQuery q2 = prologCompiler.compileQuery("path(a, Target)");
  auto sols                 = prologCompiler.solve(q2);

  // Targets reachable from 'a': b, c, d, e
  ASSERT_EQ(sols.size(), 4U);
  std::vector<std::string> targets;
  for (const auto &sol : sols) {
    targets.push_back(sol.formatted.at("Target"));
  }
  EXPECT_TRUE(std::ranges::contains(targets, "b"));
  EXPECT_TRUE(std::ranges::contains(targets, "c"));
  EXPECT_TRUE(std::ranges::contains(targets, "d"));
  EXPECT_TRUE(std::ranges::contains(targets, "e"));
}

// -- 3. Stratified Negation as Failure & Ground Safety Guard (P18.2) ---------

TEST(VagentBeliefVlogUnificationTest, NegationAsFailureAndFlounderingGuard) {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
  vprolog::Compiler prologCompiler{core};

  const std::string progSrc = R"(
    node(a).
    node(b).
    node(c).
    blocked(b).

    available(X) :- node(X), not(blocked(X)).
  )";

  vprolog::Parser parser(progSrc);
  prologCompiler.compileProgram(parser.parseProgram());

  // Query: ?- available(X). -> should succeed for 'a' and 'c', but not 'b'
  vprolog::CompiledQuery q = prologCompiler.compileQuery("available(X)");
  auto sols                = prologCompiler.solve(q);

  ASSERT_EQ(sols.size(), 2U);
  std::vector<std::string> avail;
  for (const auto &sol : sols) {
    avail.push_back(sol.formatted.at("X"));
  }
  EXPECT_TRUE(std::ranges::contains(avail, "a"));
  EXPECT_TRUE(std::ranges::contains(avail, "c"));
  EXPECT_FALSE(std::ranges::contains(avail, "b"));

  // Floundered negation guard: unbound variable in negation throws
  // runtime_error
  CellRef freeVar     = stdlib.makeVar("Unbound");
  CellRef blockedTerm = stdlib.makeTerm("blocked", {freeVar});
  CellRef notGoal     = stdlib.makeTerm("not", {blockedTerm});

  auto customPreds = prologCompiler.customPredicates();
  EXPECT_THROW(
      { (void)stdlib.solveQuery(notGoal, customPreds); }, std::runtime_error);
}

// -- 4. Choice-Point and Mark Barrier State Isolation (P18.2) ----------------

TEST(VagentBeliefVlogUnificationTest,
     LogicBacktrackingDoesNotRollBackActorState) {
  ArenaManifold arena;
  VortexCore core{arena};
  vprolog::Compiler prologCompiler{core};

  // Setup rules that backtrack
  const std::string progSrc = R"(
    candidate(1).
    candidate(2).
    candidate(3).
    check(3).
    test(X) :- candidate(X), check(X).
  )";
  vprolog::Parser parser(progSrc);
  prologCompiler.compileProgram(parser.parseProgram());

  // Create mutable actor field cell outside the logic query
  CellRef actorField = arena.makeScalarCell(std::int64_t{100});

  // Take pre-query mark barrier m_0
  auto m0 = arena.mark();

  // Run query that backtracks through candidate(1), candidate(2) before finding
  // candidate(3)
  vprolog::CompiledQuery q = prologCompiler.compileQuery("test(Ans)");
  auto sols                = prologCompiler.solve(q);
  ASSERT_EQ(sols.size(), 1U);
  EXPECT_EQ(sols[0].formatted.at("Ans"), "3");

  // Verify that actorField is completely untouched and value is intact
  auto val = arena.asInt64(actorField);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 100);

  // Even if arena is released to m0, actorField (created before m0) is
  // preserved
  arena.release(m0);
  auto valAfter = arena.asInt64(actorField);
  ASSERT_TRUE(valAfter.has_value());
  EXPECT_EQ(*valAfter, 100);
}

} // namespace
} // namespace xanadu::vagent::runtime
