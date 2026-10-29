/**
 * @file vql_vm_only_test.cpp
 * @brief VQL frontend and compiler guarantees for VM-only execution.
 */
#include <gtest/gtest.h>

#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VortexVM vm{core};
};

TEST(VQLVMOnlyTest, ExecutorDefaultsToVortexAndDirectModeRefuses) {
  Harness harness;
  EXPECT_EQ(QueryExecutionOptions{}.mode, QueryExecutionMode::Vortex);

  QueryExecutor vortex(harness.coordinator, harness.vm);
  const auto result = vortex.execute("def answer(): Int := 42; answer()");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(harness.arena.asInt64(result->front()), 42);

  QueryExecutor direct(harness.coordinator, harness.vm,
                       {.mode = QueryExecutionMode::Direct});
  const auto refused = direct.execute("def answer(): Int := 42; answer()");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().category, QueryExecutionError::Category::Compile);
}

TEST(VQLVMOnlyTest, MissingBindingsProduceEmptyStreamsRatherThanFalseCells) {
  Harness harness;
  QueryExecutor executor(harness.coordinator, harness.vm);
  const auto result = executor.execute("$missing>");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLVMOnlyTest, CompiledActivationOutlivesItsAst) {
  Harness harness;
  VQLCompiler compiler(harness.coordinator, harness.vm);
  CompilationResult compiled;
  {
    Parser parser("def answer(): Int := 42; answer()");
    const auto ast = parser.parseQuery();
    compiled       = compiler.compile(ast);
  }

  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.cursor);
  ASSERT_TRUE(compiled.terminalOpcode);
  EXPECT_EQ(harness.vm.getOpcodeKind(*compiled.terminalOpcode),
            OpcodeKind::Halt);
  ASSERT_TRUE(harness.vm.run(*compiled.cursor, 1000).success);

  std::vector<CellRef> values;
  for (const auto slot : compiled.resultSlots) {
    const auto slotValues = harness.vm.resultCells(slot);
    values.insert(values.end(), slotValues.begin(), slotValues.end());
  }
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(harness.arena.asInt64(values.front()), 42);
}

TEST(VQLVMOnlyTest, UnsupportedStatementRollsBackAssemblyAndCanRetry) {
  Harness harness;
  VQLCompiler compiler(harness.coordinator, harness.vm);
  const auto initialCells      = harness.arena.cellCount();
  const auto initialDimensions = harness.arena.dimensions();
  const std::vector<zigzag::DimLink> initialHomeLinks(
      harness.arena.dimensionsOf(harness.core.home()).begin(),
      harness.arena.dimensionsOf(harness.core.home()).end());
  const auto initialCursors = harness.vm.activeCursors();

  Parser parser("##/d.step");
  auto query = parser.parseQuery();
  query.body.statements.emplace_back(std::get<PathExpression>(query.expr));
  query.expr = ExecutionBlock{};
  query.body.statements.emplace_back(WatchStatement{});

  const auto failed = compiler.compile(query);
  ASSERT_FALSE(failed.success);
  EXPECT_EQ(harness.arena.cellCount(), initialCells);
  EXPECT_EQ(harness.arena.dimensions(), initialDimensions);
  const auto homeLinks = harness.arena.dimensionsOf(harness.core.home());
  EXPECT_EQ(std::vector<zigzag::DimLink>(homeLinks.begin(), homeLinks.end()),
            initialHomeLinks);
  EXPECT_EQ(harness.vm.activeCursors(), initialCursors);

  const auto retried = compiler.compile("def answer(): Int := 7; answer()");
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  ASSERT_TRUE(retried.cursor);
  ASSERT_TRUE(harness.vm.run(*retried.cursor, 1000).success);
}

} // namespace
} // namespace xanadu::vql
