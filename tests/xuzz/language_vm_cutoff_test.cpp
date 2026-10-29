#include <gtest/gtest.h>

#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vlace/parser.hpp"
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

  const auto result = compiler.evaluateProgram(*program);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().detail.find("VM lowering"), std::string::npos);
  const auto expression = compiler.evaluate(*program->result);
  ASSERT_FALSE(expression);
  EXPECT_NE(expression.error().detail.find("VM lowering"), std::string::npos);

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
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().phase, "compile");
  EXPECT_NE(result.error().detail.find("VM lowering"), std::string::npos);

  std::istringstream input;
  std::ostringstream output;
  auto ioResult = driver.runIO("putStr \"must not print\"", input, output);
  ASSERT_FALSE(ioResult);
  EXPECT_EQ(ioResult.error().phase, "compile");
  EXPECT_TRUE(output.str().empty());
}

} // namespace
