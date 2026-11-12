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

TEST(LogicCompileCoreTest, DeepRecursionAndLargeFactTableMeasurement) {
  PrologHarness h;
  std::string facts;
  for (int i = 0; i < 1000; ++i) facts += "fact(" + std::to_string(i) + ").\n";
  h.consult(facts + "len2([], z).\nlen2([_|T], s(N)) :- len2(T, N).\n");
  h.compiler.stdlib().setLogicCycleBudget(100'000'000);

  using Clock = std::chrono::steady_clock;
  auto ctx    = h.compiler.stdlib().makeLogicContext({});
  auto t0     = Clock::now();
  auto c0     = ctx.vm.totalCycles();
  EXPECT_EQ(h.all("fact(999).", "X").size(), 1u);
  auto ms = [](auto d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };
  std::cout << "[measure] fact(999) over 1000 clauses: "
            << ctx.vm.totalCycles() - c0 << " cycles, " << ms(Clock::now() - t0)
            << " ms\n";

  std::string list = "[";
  for (int i = 0; i < 1000; ++i) list += (i ? ",a" : "a");
  list += "]";
  t0 = Clock::now();
  c0 = ctx.vm.totalCycles();
  EXPECT_EQ(h.all("len2(" + list + ", N).", "N").size(), 1u);
  std::cout << "[measure] len2 over a 1000-element list: "
            << ctx.vm.totalCycles() - c0 << " cycles, " << ms(Clock::now() - t0)
            << " ms\n";
}

} // namespace
