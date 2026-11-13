#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/run_vm.hpp"
#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

TEST(LanguageExecutionCutoffTest, PrologResolutionRunsOnTheVm) {
  using namespace zigzag::vortex;
  zigzag::ArenaManifold arena;
  VortexCore core(arena);
  xanadu::vprolog::Compiler compiler(core);
  auto &stdlib = compiler.stdlib();
  {
    xanadu::vprolog::Parser parser(
        "app([], L, L).\napp([H|T], L, [H|R]) :- app(T, L, R).\n"
        "gen(z).\ngen(s(X)) :- gen(X).\n");
    compiler.compileProgram(parser.parseProgram());
  }
  auto ctx = stdlib.makeLogicContext(compiler.customPredicates());

  // 1. VM work is observable and grows with the problem; a host recursion
  // would never touch the VM's cycle counter.
  const auto cyclesFor = [&](const char *query) {
    const auto before = ctx.vm.totalCycles();
    const auto sols   = compiler.solve(compiler.compileQuery(query), 10);
    EXPECT_EQ(sols.size(), 1u);
    return ctx.vm.totalCycles() - before;
  };
  const auto small = cyclesFor("app([1,2],[3],X).");
  const auto large = cyclesFor("app([1,2,3,4,5,6],[7],X).");
  EXPECT_GT(small, 0u);
  EXPECT_GT(large, small);

  // 2. The VM budget bounds an infinite generator and surfaces as an error.
  stdlib.setLogicCycleBudget(1000);
  const auto before = ctx.vm.totalCycles();
  EXPECT_THROW(compiler.solve(compiler.compileQuery("gen(X)."), 1'000'000),
               std::runtime_error);
  const auto spent = ctx.vm.totalCycles() - before;
  EXPECT_GE(spent, 900u);
  EXPECT_LE(spent, 1100u);
  stdlib.setLogicCycleBudget(100000);

  // 3. The compiled entry graph is ordinary VM code: it holds Choice, Unify,
  // InvokeFrame and ReturnFrame, and spawning it directly gives the answer.
  const auto query = compiler.compileQuery("app([1,2],[3],X).");
  auto lowered     = logic::lowerQuery(ctx, query.goals);
  std::set<OpcodeKind> kinds;
  std::set<zigzag::CellRef> seen;
  std::vector<zigzag::CellRef> work{lowered.entry};
  while (!work.empty()) {
    const auto op = work.back();
    work.pop_back();
    if (!seen.insert(op).second) continue;
    const auto kind = ctx.vm.getOpcodeKind(op);
    if (!kind) continue;
    kinds.insert(*kind);
    if (const auto next =
            arena.linked(op, core.dims().spin, zigzag::DimVector::POS))
      work.push_back(*next);
    const auto inputs = core.inputsOf(op);
    if (*kind == OpcodeKind::InvokeFrame && !inputs.empty()) {
      if (const auto d = ctx.vm.frameDescriptor(inputs.front()); d && d->entry)
        work.push_back(*d->entry);
    }
    if (*kind == OpcodeKind::Choice && !inputs.empty()) {
      if (const auto id = arena.asInt64(inputs.front()))
        work.push_back(static_cast<zigzag::CellRef>(*id));
    }
  }
  EXPECT_TRUE(kinds.contains(OpcodeKind::Choice));
  EXPECT_TRUE(kinds.contains(OpcodeKind::Unify));
  EXPECT_TRUE(kinds.contains(OpcodeKind::InvokeFrame));
  EXPECT_TRUE(kinds.contains(OpcodeKind::ReturnFrame));
  const auto cursor = ctx.vm.spawnCursor(lowered.entry);
  const auto run    = ctx.vm.run(cursor, 10000);
  ASSERT_TRUE(run.success) << run.errorMessage;
  ASSERT_EQ(ctx.vm.getCursorOpcode(cursor), lowered.haltOp);
  EXPECT_EQ(stdlib.renderTerm(lowered.variables.front().first),
            std::optional<std::string>{"[1, 2, 3]"});

  // 4. Empty goals succeed without calling back.
  bool called = false;
  EXPECT_TRUE(stdlib.solve(std::span<const zigzag::CellRef>{},
                           [&](const LogicSolution &) {
                             called = true;
                             return true;
                           }));
  EXPECT_FALSE(called);
  EXPECT_TRUE(compiler.solve({}, 1).empty());
}

TEST(LanguageExecutionCutoffTest, VlaceCompilerRefusesAstExecution) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  xanadu::vlace::Compiler compiler(core, vm);
  xanadu::vlace::Parser parser("1 + 2");
  auto program = parser.parseProgram();
  ASSERT_TRUE(program);

  // Evaluation runs on the VM: the answer comes back through read-back, and
  // the VM's cycle counter shows it executed the lowered graph.
  const auto cyclesBefore = vm.totalCycles();
  const auto result       = compiler.evaluateProgram(*program);
  ASSERT_TRUE(result);
  EXPECT_EQ(xanadu::vlace::Compiler::show(*result), "3");
  EXPECT_GT(vm.totalCycles(), cyclesBefore);
  const auto expression = compiler.evaluate(*program->result);
  ASSERT_TRUE(expression);
  EXPECT_EQ(xanadu::vlace::Compiler::show(*expression), "3");

  // The same evaluation under a tiny budget fails as an exhausted budget.
  {
    zigzag::ArenaManifold limitedArena;
    zigzag::vortex::VortexCore limitedCore(limitedArena);
    zigzag::vortex::VortexVM limitedVm(limitedCore);
    xanadu::vlace::Compiler limited(limitedCore, limitedVm);
    limitedArena.setExecutionLimits(1'000'000, 0, 0);
    const auto starved = limited.evaluateProgram(*program);
    ASSERT_FALSE(starved);
    EXPECT_NE(starved.error().detail.find("budget"), std::string::npos);
  }
  {
    xanadu::vlace::lower::setCycleLimit(compiler, 1);
    const auto starved = compiler.evaluateProgram(*program);
    ASSERT_FALSE(starved);
    EXPECT_NE(starved.error().detail.find("budget"), std::string::npos);
    xanadu::vlace::lower::setCycleLimit(compiler, std::nullopt);
  }

  const auto forced = compiler.forceValue({});
  ASSERT_FALSE(forced);
  EXPECT_NE(forced.error().detail.find("VM thunk forcing"), std::string::npos);

  EXPECT_THROW((void)compiler.render({}, 1), std::logic_error);

  std::istringstream input;
  std::ostringstream output;
  const auto io = compiler.runIO({}, input, output);
  ASSERT_FALSE(io);
  EXPECT_TRUE(output.str().empty());
}

TEST(LanguageExecutionCutoffTest,
     VlaceDriversReportCompileRefusalWithoutEffects) {
  xanadu::vlace::Driver driver;
  const auto parseError = driver.run("(");
  ASSERT_FALSE(parseError);
  EXPECT_EQ(parseError.error().phase, "parse");
  const auto typeError = driver.run("1 + \"x\"");
  ASSERT_FALSE(typeError);
  EXPECT_EQ(typeError.error().phase, "type");

  auto result = driver.run("1 + 2");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->value, "3");
  EXPECT_EQ(result->type, "Int");

  std::istringstream input;
  std::ostringstream output;
  auto ioResult = driver.runIO("putStr \"must not print\"", input, output);
  ASSERT_FALSE(ioResult);
  EXPECT_EQ(ioResult.error().phase, "compile");
  EXPECT_TRUE(output.str().empty());
}

} // namespace
