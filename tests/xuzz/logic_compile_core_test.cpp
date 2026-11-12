/**
 * @file logic_compile_core_test.cpp
 * @brief Card P1: predicate frames, clause chains, recursion with
 * backtracking through frames, and the solution driver.
 */
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace zigzag;
using namespace zigzag::vortex;
using namespace zigzag::vortex::logic;

struct PrologHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  xanadu::vprolog::Compiler compiler{core};

  void consult(const std::string &source) {
    xanadu::vprolog::Parser parser(source);
    compiler.compileProgram(parser.parseProgram());
  }
  std::vector<std::string> all(const std::string &query, const std::string &var,
                               std::size_t max = 100) {
    const auto compiled = compiler.compileQuery(query);
    std::vector<std::string> out;
    for (const auto &sol : compiler.solve(compiled, max)) {
      const auto it = sol.formatted.find(var);
      out.push_back(it == sol.formatted.end() ? "?" : it->second);
    }
    return out;
  }
};

constexpr const char *kAppend =
    "append2([], L, L).\n"
    "append2([H|T], L, [H|R]) :- append2(T, L, R).\n";

// The spike's first deliverable: the append/3 frame built by hand from the
// declared services, run on the raw VM. This is what proves recursion
// combined with backtracking through frames, without the lowering in between.
TEST(LogicCompileCoreTest, HandBuiltAppendFrameEnumeratesEverySolution) {
  ArenaManifold arena;
  VortexCore core(arena);
  VortexVM vm(core);
  VortexStdLib stdlib(core, vm);
  auto ctx = stdlib.makeLogicContext({});
  vm.setFrameLimits({.maxDepth = 4096});

  const PredicateKey key{"append", 3};
  const auto desc = vm.reserveFrameDescriptor();
  ASSERT_TRUE(desc);

  FrameScope scope;
  scope.predicate      = key;
  const CellRef failOp = vm.mintOpcode(OpcodeKind::Fail);
  ASSERT_TRUE(arena.link(failOp, core.dims().spin, DimVector::POS,
                         vm.mintOpcode(OpcodeKind::Nop)));
  scope.failOp = failOp;
  for (int i = 0; i < 3; ++i) scope.locals.push_back(arena.makeCell());
  const CellRef barrier = arena.makeCell();
  scope.locals.push_back(barrier);
  scope.barrier = barrier;

  Chain chain;
  const std::array<CellRef, 1> barrierOut{barrier};
  appendOp(ctx, chain, OpcodeKind::ChoiceDepth, {}, barrierOut);

  // Clause 1: append([], L, L).
  const Label second = newLabel(ctx);
  emitChoice(ctx, chain, second);
  const CellRef nil     = stdlib.makeTerm("[]", std::span<const CellRef>{});
  const CellRef l1      = stdlib.makeVar();
  const CellRef wantNil = emitTerm(ctx, scope, chain, nil);
  emitUnifyOrFail(ctx, scope, chain, *argumentPort(scope, 0), wantNil);
  ASSERT_TRUE(bindArgumentPort(scope, l1, 1));
  ASSERT_FALSE(bindArgumentPort(scope, l1, 2));
  emitUnifyOrFail(ctx, scope, chain, *argumentPort(scope, 2),
                  variableSlot(ctx, scope, chain, l1));
  const std::array<CellRef, 1> self{*desc};
  appendOp(ctx, chain, OpcodeKind::ReturnFrame, self);

  // Clause 2: append([H|T], L, [H|R]) :- append(T, L, R).
  placeLabel(ctx, chain, second);
  scope.variableSlots.clear();
  const CellRef h = stdlib.makeVar(), t = stdlib.makeVar(),
                l2 = stdlib.makeVar(), r = stdlib.makeVar();
  ASSERT_TRUE(bindArgumentPort(scope, l2, 1));
  emitUnifyOrFail(ctx, scope, chain, *argumentPort(scope, 0),
                  emitTerm(ctx, scope, chain, stdlib.makeCons(h, t)));
  emitUnifyOrFail(ctx, scope, chain, *argumentPort(scope, 2),
                  emitTerm(ctx, scope, chain, stdlib.makeCons(h, r)));
  const std::array<CellRef, 4> call{*desc, variableSlot(ctx, scope, chain, t),
                                    *argumentPort(scope, 1),
                                    variableSlot(ctx, scope, chain, r)};
  appendOp(ctx, chain, OpcodeKind::InvokeFrame, call);
  appendOp(ctx, chain, OpcodeKind::ReturnFrame, self);

  FrameDescriptorSpec spec{.entry = chain.head, .locals = scope.locals};
  for (int i = 0; i < 3; ++i) spec.argumentPorts.emplace_back(scope.locals[i]);
  ASSERT_TRUE(vm.finalizeFrameDescriptor(*desc, spec));

  const auto run = [&](CellRef a, CellRef b, CellRef c, CellRef watch) {
    Chain entry;
    const std::array<CellRef, 4> inv{*desc, a, b, c};
    const CellRef start    = appendOp(ctx, entry, OpcodeKind::InvokeFrame, inv);
    const CellRef halt     = appendOp(ctx, entry, OpcodeKind::Halt);
    const CellRef sentinel = vm.mintOpcode(OpcodeKind::Halt);
    const CellRef cursor   = vm.spawnCursor(start);
    vm.pushChoicePoint(cursor, sentinel);
    std::vector<std::string> out;
    for (;;) {
      const auto res = vm.run(cursor, 100000);
      EXPECT_TRUE(res.success) << res.errorMessage;
      if (!res.success || vm.getCursorOpcode(cursor) != halt) break;
      out.push_back(stdlib.renderTerm(watch).value_or("<none>"));
      vm.setCursorOpcode(cursor, failOp);
    }
    EXPECT_EQ(vm.getCursorOpcode(cursor), sentinel);
    return out;
  };

  const CellRef list_ab =
      stdlib.makeList({arena.makeCell("a"), arena.makeCell("b")});
  const CellRef list_c = stdlib.makeList({arena.makeCell("c")});
  const CellRef x      = stdlib.makeVar("X");
  EXPECT_EQ(run(list_ab, list_c, x, x),
            (std::vector<std::string>{"[a, b, c]"}));

  const CellRef y = stdlib.makeVar("Y");
  const CellRef z = stdlib.makeVar("Z");
  const CellRef whole =
      stdlib.makeList({arena.makeCell("a"), arena.makeCell("b")});
  (void)z;
  EXPECT_EQ(run(y, z, whole, y),
            (std::vector<std::string>{"[]", "[a]", "[a, b]"}));
}

TEST(LogicCompileCoreTest, AppendEnumeratesInRankOrder) {
  PrologHarness h;
  h.consult(kAppend);
  EXPECT_EQ(h.all("append2([a,b],[c],X).", "X"),
            (std::vector<std::string>{"[a, b, c]"}));
  EXPECT_EQ(h.all("append2(X,Y,[a,b]).", "X"),
            (std::vector<std::string>{"[]", "[a]", "[a, b]"}));
  EXPECT_EQ(h.all("append2(X,Y,[a,b]).", "Y"),
            (std::vector<std::string>{"[a, b]", "[b]", "[]"}));
}

TEST(LogicCompileCoreTest, MutualRecursionAndArityMismatch) {
  PrologHarness h;
  h.consult("even(z).\neven(s(X)) :- odd(X).\nodd(s(X)) :- even(X).\n"
            "pair(1).\npair(1, 2).\n");
  EXPECT_EQ(h.all("even(s(s(z))), X = ok.", "X").size(), 1u);
  EXPECT_TRUE(h.all("even(s(z)).", "X").empty());
  EXPECT_EQ(h.all("pair(X).", "X"), (std::vector<std::string>{"1"}));
  EXPECT_EQ(h.all("pair(X, Y).", "Y"), (std::vector<std::string>{"2"}));
  EXPECT_TRUE(h.all("pair(X, Y, Z).", "X").empty());
  EXPECT_TRUE(h.all("nosuch(X).", "X").empty());
}

TEST(LogicCompileCoreTest, MaxSolutionsAndCallbackStopTheSearch) {
  PrologHarness h;
  h.consult("gen(z).\ngen(s(X)) :- gen(X).\n");
  EXPECT_EQ(h.all("gen(X).", "X", 3).size(), 3u);

  const auto query     = h.compiler.compileQuery("gen(X).");
  std::size_t seen     = 0;
  const bool completed = h.compiler.stdlib().solve(
      query.goals, [&](const LogicSolution &) { return ++seen < 2; },
      h.compiler.customPredicates(), 50);
  EXPECT_FALSE(completed);
  EXPECT_EQ(seen, 2u);

  EXPECT_TRUE(h.compiler.stdlib().solve(
      std::span<const CellRef>{}, [](const LogicSolution &) { return true; }));
}

TEST(LogicCompileCoreTest, BudgetHitIsAnErrorNotExhaustion) {
  PrologHarness h;
  h.consult("gen(z).\ngen(s(X)) :- gen(X).\nloop :- loop.\n");
  h.compiler.stdlib().setLogicCycleBudget(2000);
  const auto query = h.compiler.compileQuery("gen(X).");
  std::size_t seen = 0;
  EXPECT_THROW(h.compiler.stdlib().solve(
                   query.goals,
                   [&](const LogicSolution &) {
                     ++seen;
                     return true;
                   },
                   h.compiler.customPredicates(), 1'000'000),
               std::runtime_error);
  EXPECT_GT(seen, 0u);
  EXPECT_THROW(h.compiler.solveOnce(h.compiler.compileQuery("loop.")),
               std::runtime_error);
  // The VM and arena are usable afterwards.
  h.compiler.stdlib().setLogicCycleBudget(100000);
  EXPECT_EQ(h.all("gen(X).", "X", 2).size(), 2u);
}

TEST(LogicCompileCoreTest, FrameLimitsAreRestoredAfterASolve) {
  PrologHarness h;
  h.consult(kAppend);
  auto ctx          = h.compiler.stdlib().makeLogicContext({});
  const auto before = ctx.vm.frameLimits();
  (void)h.all("append2([a],[b],X).", "X");
  const auto after = ctx.vm.frameLimits();
  EXPECT_EQ(before.maxDepth, after.maxDepth);
  EXPECT_EQ(before.maxBackingCells, after.maxBackingCells);
}

TEST(LogicCompileCoreTest, LabelMisuseIsRefused) {
  ArenaManifold arena;
  VortexCore core(arena);
  VortexVM vm(core);
  VortexStdLib stdlib(core, vm);
  auto ctx = stdlib.makeLogicContext({});
  Chain chain;
  const Label label = newLabel(ctx);
  placeLabel(ctx, chain, label);
  EXPECT_THROW(placeLabel(ctx, chain, label), LogicRefusal);
}

TEST(LogicCompileCoreTest, CycleCostIsLinearInClausesAndDepth) {
  PrologHarness h;
  std::string facts;
  for (int i = 0; i < 1000; ++i) facts += "fact(" + std::to_string(i) + ").\n";
  h.consult(facts + "len2([], z).\nlen2([_|T], s(N)) :- len2(T, N).\n");
  h.compiler.stdlib().setLogicCycleBudget(100'000'000);

  using Clock   = std::chrono::steady_clock;
  auto ctx      = h.compiler.stdlib().makeLogicContext({});
  const auto ms = [](auto d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };
  const auto list = [](int n) {
    std::string out = "[";
    for (int i = 0; i < n; ++i) out += (i ? ",a" : "a");
    return out + "]";
  };
  const auto measure = [&](const std::string &query, const char *label) {
    const auto t0 = Clock::now();
    const auto c0 = ctx.vm.totalCycles();
    EXPECT_EQ(h.all(query, "N").size(), 1u) << label;
    const auto cycles = ctx.vm.totalCycles() - c0;
    std::cout << "[measure] " << label << ": " << cycles << " cycles, "
              << ms(Clock::now() - t0) << " ms (information only)\n";
    return cycles;
  };

  // Cycle bounds, not wall time: a quadratic regression shows in cycles too.
  EXPECT_LT(measure("fact(999), N = 1.", "fact(999) of 1000 clauses"),
            20u * 1000u);
  const auto c1000 = measure("len2(" + list(1000) + ", N).", "len2 1000");
  const auto c2000 = measure("len2(" + list(2000) + ", N).", "len2 2000");
  EXPECT_LT(c1000, 50u * 1000u);
  EXPECT_LT(c2000 * 2, c1000 * 5) << "doubling the list must cost under 2.5x";
}

TEST(LogicCompileCoreTest, ThousandsOfClausesAndLargeLiteralsFitOneFrame) {
  PrologHarness h;
  std::string facts;
  for (int i = 0; i < 5000; ++i) facts += "wide(" + std::to_string(i) + ").\n";
  std::string literal = "big([";
  for (int i = 0; i < 5000; ++i) literal += (i ? ",x" : "x");
  literal += "]).\n";
  h.consult(facts + literal);
  h.compiler.stdlib().setLogicCycleBudget(10'000'000);
  EXPECT_EQ(h.all("wide(4999), N = 1.", "N").size(), 1u);
  EXPECT_EQ(h.all("wide(X).", "X", 10000).size(), 5000u);
  EXPECT_EQ(h.all("big(L), N = 1.", "N").size(), 1u);
  EXPECT_EQ(h.all("big([x|T]), N = 1.", "N").size(), 1u);
}

TEST(LogicCompileCoreTest, UnloweredBuiltinsRefuseByNameButUnknownNamesFail) {
  PrologHarness h;
  // A user clause named like a builtin does not replace it: the old solver
  // tried its builtin arms before any clause. Until the owning card lowers
  // them these refuse by name.
  h.consult("true.\nknown(1).\n");
  for (const char *goal :
       {"X is 1.", "true.", "cell_value(1, X).", "setting(a, B).",
        "vortex_function(F).", "known(X), \\+ known(2)."}) {
    try {
      (void)h.all(goal, "X");
      ADD_FAILURE() << goal << " did not refuse";
    } catch (const logic::LogicRefusal &refusal) {
      EXPECT_NE(std::string(refusal.what()).find("awaits P2/P3/P4"),
                std::string::npos)
          << goal;
    }
  }
  EXPECT_TRUE(h.all("nosuch(X).", "X").empty());
  EXPECT_TRUE(h.all("nosuch(X, 1, 2).", "X").empty());
  EXPECT_EQ(h.all("known(X).", "X").size(), 1u);
}

TEST(LogicCompileCoreTest, RepeatedHeadVariablesAndPerActivationRenaming) {
  PrologHarness h;
  h.consult("same(X, X).\nmk(f(X)).\ntwo(P, Q) :- mk(P), mk(Q).\n");
  EXPECT_EQ(h.all("same(a, a).", "V").size(), 1u);
  EXPECT_TRUE(h.all("same(a, b).", "V").empty());
  EXPECT_EQ(h.all("same(f(Y), f(b)).", "Y"), (std::vector<std::string>{"b"}));
  // Two calls of one clause must not share their X.
  EXPECT_EQ(h.all("two(P, Q), P = f(a), Q = f(b).", "P"),
            (std::vector<std::string>{"f(a)"}));
}

TEST(LogicCompileCoreTest, BudgetThrowRestoresFrameLimitsAndLeavesVmUsable) {
  PrologHarness h;
  h.consult("loop :- loop.\nok(1).\n");
  auto ctx          = h.compiler.stdlib().makeLogicContext({});
  const auto before = ctx.vm.frameLimits();
  h.compiler.stdlib().setLogicCycleBudget(500);
  EXPECT_THROW((void)h.compiler.solveOnce(h.compiler.compileQuery("loop.")),
               std::runtime_error);
  const auto after = ctx.vm.frameLimits();
  EXPECT_EQ(before.maxDepth, after.maxDepth);
  EXPECT_EQ(before.maxBackingCells, after.maxBackingCells);
  EXPECT_EQ(before.maxLocalsPerFrame, after.maxLocalsPerFrame);
  EXPECT_EQ(before.maxDescriptorRecords, after.maxDescriptorRecords);
  h.compiler.stdlib().setLogicCycleBudget(100000);
  EXPECT_EQ(h.all("ok(X).", "X").size(), 1u);
}

// -- Hand-built service graphs
// -------------------------------------------------

// One zero-argument frame built from the services, run under a sentinel
// choice, reporting `watch` at every solution. Variables are static cells made
// by the test, bound in place like a query's.
struct Rig {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
  CompileContext ctx = stdlib.makeLogicContext({});
  FrameScope scope;
  Chain chain;
  CellRef desc{};
  std::vector<std::string> out;

  explicit Rig(std::string_view name = "rig") {
    vm.setFrameLimits({.maxDepth = 4096});
    scope.predicate     = {std::string(name), 0};
    const auto reserved = vm.reserveFrameDescriptor();
    desc                = *reserved;
    scope.failOp        = vm.mintOpcode(OpcodeKind::Fail);
    EXPECT_TRUE(arena.link(scope.failOp, core.dims().spin, DimVector::POS,
                           vm.mintOpcode(OpcodeKind::Nop)));
    const CellRef barrier = arena.makeCell();
    scope.locals.push_back(barrier);
    scope.barrier = barrier;
    const std::array<CellRef, 1> o{barrier};
    appendOp(ctx, chain, OpcodeKind::ChoiceDepth, {}, o);
  }

  CellRef atom(std::string_view t) { return arena.makeCell(t); }
  void bind(CellRef var, std::string_view t) {
    emitUnifyOrFail(ctx, scope, chain, var, atom(t));
  }
  // var is bound to each of ts in turn: Choice L2; ..; Jump END; L2: ...
  void alt(CellRef var, std::initializer_list<const char *> ts) {
    const Label end  = newLabel(ctx);
    std::size_t left = ts.size();
    for (const char *t : ts) {
      --left;
      std::optional<Label> next;
      if (left > 0) {
        next = newLabel(ctx);
        emitChoice(ctx, chain, *next);
      }
      bind(var, t);
      if (left > 0) {
        emitJump(ctx, chain, end);
        placeLabel(ctx, chain, *next);
      }
    }
    placeLabel(ctx, chain, end);
  }
  void finish() {
    const std::array<CellRef, 1> self{desc};
    appendOp(ctx, chain, OpcodeKind::ReturnFrame, self);
    const FrameDescriptorSpec spec{.entry = chain.head, .locals = scope.locals};
    ASSERT_TRUE(vm.finalizeFrameDescriptor(desc, spec));
  }
  std::vector<std::string> run(CellRef watch) {
    finish();
    Chain entry;
    const std::array<CellRef, 1> call{desc};
    const CellRef start = appendOp(ctx, entry, OpcodeKind::InvokeFrame, call);
    const CellRef halt  = appendOp(ctx, entry, OpcodeKind::Halt);
    const CellRef sentinel = vm.mintOpcode(OpcodeKind::Halt);
    const CellRef cursor   = vm.spawnCursor(start);
    vm.pushChoicePoint(cursor, sentinel);
    std::vector<std::string> results;
    for (std::size_t i = 0; i < 100; ++i) {
      const auto res = vm.run(cursor, 100000);
      EXPECT_TRUE(res.success) << res.errorMessage;
      if (!res.success || vm.getCursorOpcode(cursor) != halt) break;
      results.push_back(stdlib.renderTerm(watch).value_or("<none>"));
      vm.setCursorOpcode(cursor, scope.failOp);
    }
    EXPECT_EQ(vm.getCursorOpcode(cursor), sentinel);
    return results;
  }
};

TEST(LogicCompileCoreTest, ClauseBodyCutPrunesEarlierChoicesAndKeepsBindings) {
  Rig rig;
  const CellRef x = rig.stdlib.makeVar("X"), y = rig.stdlib.makeVar("Y");
  rig.alt(x, {"a", "b", "c"});
  rig.alt(y, {"p", "q"});
  emitCut(rig.ctx, rig.scope, rig.chain);
  // Both earlier goals' alternatives are gone; the bindings stay.
  EXPECT_EQ(rig.run(rig.stdlib.makeTerm("r", {x, y})),
            (std::vector<std::string>{"r(a, p)"}));
}

TEST(LogicCompileCoreTest, CutKeepsGoalsAfterItAndQueryLevelCutBehavesTheSame) {
  Rig rig("<query>");
  const CellRef x = rig.stdlib.makeVar("X"), y = rig.stdlib.makeVar("Y");
  rig.alt(x, {"a", "b"});
  emitCut(rig.ctx, rig.scope, rig.chain);
  rig.alt(y, {"p", "q"});
  EXPECT_EQ(rig.run(rig.stdlib.makeTerm("r", {x, y})),
            (std::vector<std::string>{"r(a, p)", "r(a, q)"}));
}

TEST(LogicCompileCoreTest, CutInACalleeLeavesTheCallersChoices) {
  Rig rig;
  const CellRef z = rig.stdlib.makeVar("Z"), x = rig.stdlib.makeVar("X"),
                y = rig.stdlib.makeVar("Y");
  // callee: alt(X); cut; alt(Y)
  const auto calleeDesc = rig.vm.reserveFrameDescriptor();
  ASSERT_TRUE(calleeDesc);
  {
    FrameScope inner;
    inner.predicate       = {"callee", 0};
    inner.failOp          = rig.scope.failOp;
    const CellRef barrier = rig.arena.makeCell();
    inner.locals.push_back(barrier);
    inner.barrier = barrier;
    Chain body;
    const std::array<CellRef, 1> o{barrier};
    appendOp(rig.ctx, body, OpcodeKind::ChoiceDepth, {}, o);
    auto &chain = rig.chain;
    auto &scope = rig.scope;
    std::swap(chain, body);
    std::swap(scope, inner);
    rig.alt(x, {"a", "b"});
    emitCut(rig.ctx, rig.scope, rig.chain);
    rig.alt(y, {"p", "q"});
    const std::array<CellRef, 1> self{*calleeDesc};
    appendOp(rig.ctx, rig.chain, OpcodeKind::ReturnFrame, self);
    const FrameDescriptorSpec spec{.entry  = rig.chain.head,
                                   .locals = rig.scope.locals};
    ASSERT_TRUE(rig.vm.finalizeFrameDescriptor(*calleeDesc, spec));
    std::swap(chain, body);
    std::swap(scope, inner);
  }
  rig.alt(z, {"1", "2"});
  const std::array<CellRef, 1> call{*calleeDesc};
  appendOp(rig.ctx, rig.chain, OpcodeKind::InvokeFrame, call);
  EXPECT_EQ(rig.run(rig.stdlib.makeTerm("r", {z, x, y})),
            (std::vector<std::string>{"r(1, a, p)", "r(1, a, q)", "r(2, a, p)",
                                      "r(2, a, q)"}));
}

TEST(LogicCompileCoreTest, CutInsideANegationIsLocalToIt) {
  Rig rig;
  const CellRef x = rig.stdlib.makeVar("X");
  rig.alt(x, {"a", "b"});
  // \+ (!, fail): the cut must not remove the negation's own continuation.
  const NegationScope neg = openNegationScope(rig.ctx, rig.scope, rig.chain);
  const Label cont        = newLabel(rig.ctx);
  emitChoice(rig.ctx, rig.chain, cont);
  emitCut(rig.ctx, rig.scope, rig.chain);
  emitUnifyOrFail(rig.ctx, rig.scope, rig.chain, rig.atom("u"), rig.atom("v"));
  emitCutTo(rig.ctx, rig.chain, neg.barrier);
  appendOp(rig.ctx, rig.chain, OpcodeKind::Jump,
           std::array<CellRef, 1>{rig.arena.makeScalarCell(
               static_cast<std::int64_t>(rig.scope.failOp))});
  closeNegationScope(rig.scope, neg);
  placeLabel(rig.ctx, rig.chain, cont);
  EXPECT_EQ(rig.scope.barrier, neg.outer);
  // Succeeds for both alternatives of X: the caller's choices survived.
  EXPECT_EQ(rig.run(x), (std::vector<std::string>{"a", "b"}));
}

TEST(LogicCompileCoreTest, JumpAndBranchesSkipAndFallThrough) {
  Rig rig;
  const CellRef x = rig.stdlib.makeVar("X");
  const auto skip = [&](auto emit) {
    const Label over = newLabel(rig.ctx);
    emit(over);
    rig.bind(x, "bad"); // would bind X wrongly if reached
    emitUnifyOrFail(rig.ctx, rig.scope, rig.chain, rig.atom("u"),
                    rig.atom("v"));
    placeLabel(rig.ctx, rig.chain, over);
  };
  const CellRef yes = emitConstant(rig.ctx, rig.chain, true);
  const CellRef no  = emitConstant(rig.ctx, rig.chain, false);
  skip([&](Label l) { emitJump(rig.ctx, rig.chain, l); });
  skip([&](Label l) { emitBranchIf(rig.ctx, rig.chain, yes, l); });
  skip([&](Label l) { emitBranchUnless(rig.ctx, rig.chain, no, l); });
  // Not taken: falls through to the next op.
  const Label unused = newLabel(rig.ctx);
  emitBranchIf(rig.ctx, rig.chain, no, unused);
  emitBranchUnless(rig.ctx, rig.chain, yes, unused);
  rig.bind(x, "ok");
  placeLabel(rig.ctx, rig.chain, unused);
  EXPECT_EQ(rig.run(x), (std::vector<std::string>{"ok"}));
}

TEST(LogicCompileCoreTest, UnifyingAVariableWithATermContainingItTerminates) {
  PrologHarness h;
  h.consult("same(X, X).\n");
  // Rational trees: unification binds with no occurs check and returns; the
  // solution is reported as cyclic text instead of recursing in the renderer.
  const auto sols =
      h.compiler.solve(h.compiler.compileQuery("same(Y, f(Y))."), 5);
  ASSERT_EQ(sols.size(), 1u);
  ASSERT_FALSE(sols.front().formatted.empty());
  EXPECT_EQ(sols.front().formatted.begin()->second, "<cyclic>");
}

} // namespace
