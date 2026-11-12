#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/run_vm.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

TEST(LanguageExecutionCutoffTest, PrologCompilerRefusesHostResolution) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::vprolog::Compiler compiler(core);

  auto &stdlib = compiler.stdlib();
  const std::vector<zigzag::CellRef> noGoals;
  const auto onSolution = [](const zigzag::vortex::LogicSolution &) {
    return true;
  };

  EXPECT_THROW(compiler.solve({}, 1), std::logic_error);
  EXPECT_THROW(compiler.solveOnce({}), std::logic_error);
  EXPECT_THROW(stdlib.solveOnce(zigzag::CellRef{}), std::logic_error);
  EXPECT_THROW(stdlib.solveOnce(std::span<const zigzag::CellRef>{noGoals}),
               std::logic_error);
  EXPECT_THROW(stdlib.solveQuery(zigzag::CellRef{}), std::logic_error);
  EXPECT_THROW(stdlib.solveQuery(std::span<const zigzag::CellRef>{noGoals}),
               std::logic_error);
  EXPECT_THROW(stdlib.solve(zigzag::CellRef{}, onSolution), std::logic_error);
  EXPECT_THROW(
      stdlib.solve(std::span<const zigzag::CellRef>{noGoals}, onSolution),
      std::logic_error);
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
