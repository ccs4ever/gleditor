#include <gtest/gtest.h>

#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

TEST(VPLExecutionTest, ExecutesCompiledProgramAndReturnsScalarView) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);

  const auto result = executor.execute("1 + 2");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->view.isScalar());
  EXPECT_EQ(result->view.scalarInt(), 3);
  EXPECT_FALSE(result->disassembly.empty());
}

TEST(VPLExecutionTest, AstCanBeDiscardedAfterSynchronousVmExecution) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);

  auto executeTemporaryAst = [&] {
    Parser parser("6 * 7");
    auto program = parser.parseProgram();
    return executor.execute(*program);
  };
  const auto result = executeTemporaryAst();

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->view.scalarInt(), 42);
}

TEST(VPLExecutionTest, KeepsAssignmentsAcrossSuccessfulExecutions) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);

  const auto assigned = executor.execute("answer ← 40");
  ASSERT_TRUE(assigned) << assigned.error().message;
  const auto binding = executor.getVariable("answer");
  ASSERT_TRUE(binding);
  EXPECT_TRUE(binding->isScalar());
  EXPECT_EQ(binding->scalarInt(), 40);
  const auto used = executor.execute("answer + 2");

  ASSERT_TRUE(used) << used.error().message;
  EXPECT_EQ(used->view.scalarInt(), 42);
}

TEST(VPLExecutionTest, HostUsesPersistentVmExecutorSession) {
  zigzag::vortex::VortexHost host;

  const auto assigned = host.executeVPL("answer ← 40");
  ASSERT_TRUE(assigned.success) << assigned.message;
  const auto used = host.executeVPL("answer + 2");

  ASSERT_TRUE(used.success) << used.message;
  EXPECT_EQ(used.message, "VPL Result: 42");
}

TEST(VPLExecutionTest, PreservesDirectionInInitialRankBindings) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);
  const auto origin = arena.makeCell("rank-origin");
  VplView reversed{origin, {{core.dims().step, zigzag::DimVector::NEG}}};
  executor.setVariable("items", reversed);

  const auto result = executor.execute("items");

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->view.axes().size(), 1);
  EXPECT_EQ(result->view.axes().front().dim, core.dims().step);
  EXPECT_EQ(result->view.axes().front().dir, zigzag::DimVector::NEG);
}

TEST(VPLExecutionTest, PreservesDependenciesThroughInitialBindings) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);
  const auto origin = arena.makeScalarCell(std::int64_t{12});
  const auto source = arena.makeCell("source");
  auto input = VplView::makeScalar(std::int64_t{12}, std::optional{origin});
  input.setDependencies({source});
  executor.setVariable("value", input);

  const auto result = executor.execute("value");

  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->view.dependencies().size(), 1);
  EXPECT_EQ(result->view.dependencies().front(), source);
  const auto dependencies = executor.dependenciesOf(origin);
  ASSERT_EQ(dependencies.size(), 1);
  EXPECT_EQ(dependencies.front(), source);
}

TEST(VPLExecutionTest, ClearingSessionDropsBindingsAndProvenance) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);
  const auto source = arena.makeCell("source");
  auto input        = VplView::makeScalar(std::int64_t{12});
  input.setDependencies({source});
  executor.setVariable("value", input);
  const auto result = executor.execute("value");
  ASSERT_TRUE(result) << result.error().message;
  const auto cell = result->view.origin();
  ASSERT_TRUE(cell);
  executor.clearVariables();
  EXPECT_FALSE(executor.getVariable("value"));
  EXPECT_TRUE(executor.dependenciesOf(*cell).empty());
}

TEST(VPLExecutionTest, MeshRunsCompiledVectorAndRetainsItsRank) {
  namespace mesh = zigzag::vortex::mesh;
  auto result    = mesh::evaluate(mesh::Frontend::Vpl, "1 2 3",
                                  {.maxCycles      = 10000,
                                   .maxCells       = 10000,
                                   .maxSolutions   = 16,
                                   .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  const auto findDimension =
      [&](std::string_view name) -> std::optional<zigzag::DimRef> {
    for (const auto &cell : arena.cells())
      if (arena.textOf(cell.birthOp) == name) return cell.birthOp;
    return std::nullopt;
  };
  const auto items = findDimension("d.mesh.items");
  const auto value = findDimension("d.mesh.value");
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record =
      arena.linked(result->root, *items, zigzag::DimVector::POS);
  ASSERT_TRUE(record);
  auto head = arena.linked(*record, *value, zigzag::DimVector::POS);
  for (std::int64_t expected = 1; expected <= 3; ++expected) {
    ASSERT_TRUE(head);
    EXPECT_EQ(arena.asInt64(*head), expected);
    head = arena.linked(*head, result->context->core.dims().step,
                        zigzag::DimVector::POS);
  }
  EXPECT_FALSE(head);
}

TEST(VPLExecutionTest, RejectsVmBudgetExhaustionAndAllowsRetry) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Executor executor(core, vm);

  const auto zeroBudget = executor.execute("1 + 2", 0);
  ASSERT_FALSE(zeroBudget);
  EXPECT_EQ(zeroBudget.error().stage, ExecutionError::Stage::Budget);

  const auto exhausted = executor.execute("1 + 2", 1);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().stage, ExecutionError::Stage::Budget);

  const auto retried = executor.execute("1 + 2");
  ASSERT_TRUE(retried) << retried.error().message;
  EXPECT_EQ(retried->view.scalarInt(), 3);
}

} // namespace
} // namespace xanadu::vpl
