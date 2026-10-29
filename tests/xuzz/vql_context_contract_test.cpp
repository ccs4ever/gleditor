#include <gtest/gtest.h>

#include <stdexcept>
#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine session{coordinator};
  zigzag::vortex::VortexVM vm{core};

  void link(std::string_view dimension, CellRef from, CellRef to) {
    const auto dim = coordinator.resolveDimension(dimension);
    zigzag::expectWritten(arena.link(from, dim, zigzag::DimVector::POS, to));
  }

  [[nodiscard]] std::vector<CellRef> execute(std::string_view source) {
    QueryExecutor executor(session, vm);
    const auto result = executor.execute(source);
    if (!result) throw std::runtime_error(result.error().message);
    return *result;
  }
};

void populate(Harness &harness, const CellRef parent, const CellRef argument,
              const CellRef parentChild, const CellRef argumentChild) {
  harness.link("d.parents", harness.core.home(), parent);
  harness.link("d.arguments", harness.core.home(), argument);
  harness.link("d.child", parent, parentChild);
  harness.link("d.child", argument, argumentChild);
}

TEST(VQLContextContractTest, TypedParameterContextCaptureOverridesCaller) {
  Harness harness;
  const auto parent        = harness.arena.makeCell("caller");
  const auto argument      = harness.arena.makeCell("argument");
  const auto parentChild   = harness.arena.makeCell("caller child");
  const auto argumentChild = harness.arena.makeCell("argument child");
  populate(harness, parent, argument, parentChild, argumentChild);

  const auto result =
      harness.execute("def child($value: Cell { . }): Cell := ./d.child; "
                      "let $argument := ##/d.arguments; "
                      "##/d.parents/child($argument)");

  EXPECT_EQ(result, (std::vector<CellRef>{argumentChild}));
}

TEST(VQLContextContractTest,
     DestructuredParameterContextCaptureOverridesCaller) {
  Harness harness;
  const auto parent        = harness.arena.makeCell("caller");
  const auto argument      = harness.arena.makeCell("argument");
  const auto parentChild   = harness.arena.makeCell("caller child");
  const auto argumentChild = harness.arena.makeCell("argument child");
  populate(harness, parent, argument, parentChild, argumentChild);

  const auto result = harness.execute("def child({ . }): Cell := ./d.child; "
                                      "let $argument := ##/d.arguments; "
                                      "##/d.parents/child($argument)");

  EXPECT_EQ(result, (std::vector<CellRef>{argumentChild}));
}

TEST(VQLContextContractTest, ExplicitContextParameterOverridesCaller) {
  Harness harness;
  const auto parent        = harness.arena.makeCell("caller");
  const auto argument      = harness.arena.makeCell("argument");
  const auto parentChild   = harness.arena.makeCell("caller child");
  const auto argumentChild = harness.arena.makeCell("argument child");
  populate(harness, parent, argument, parentChild, argumentChild);

  const auto result = harness.execute("def child(.): Cell := ./d.child; "
                                      "let $argument := ##/d.arguments; "
                                      "##/d.parents/child($argument)");

  EXPECT_EQ(result, (std::vector<CellRef>{argumentChild}));
}

TEST(VQLContextContractTest, DefaultPathUsesEarlierParameterContextCapture) {
  Harness harness;
  const auto parent        = harness.arena.makeCell("caller");
  const auto argument      = harness.arena.makeCell("argument");
  const auto parentChild   = harness.arena.makeCell("caller child");
  const auto argumentChild = harness.arena.makeCell("argument child");
  populate(harness, parent, argument, parentChild, argumentChild);

  const auto result = harness.execute(
      "def child($value: Cell { . }, $fallback := ./d.child): Cell := "
      "$fallback; def invoke($value: Cell): Cell := child($value); "
      "let $argument := ##/d.arguments; "
      "##/d.parents/invoke($argument)");

  EXPECT_EQ(result, (std::vector<CellRef>{argumentChild}));
}

TEST(VQLContextContractTest, UnionArmWithoutContextCaptureKeepsCallerContext) {
  Harness harness;
  const auto parent        = harness.arena.makeCell("caller");
  const auto argument      = harness.arena.makeCell("plain argument");
  const auto parentChild   = harness.arena.makeCell("caller child");
  const auto argumentChild = harness.arena.makeCell("argument child");
  populate(harness, parent, argument, parentChild, argumentChild);

  const auto result = harness.execute(
      "def child($value: Cell { +d.required: Int, . } | String): Cell := "
      "./d.child; let $argument := ##/d.arguments; "
      "##/d.parents/child($argument)");

  EXPECT_EQ(result, (std::vector<CellRef>{parentChild}));
}

TEST(VQLContextContractTest, UnionArmWithContextCaptureOverridesCallerContext) {
  Harness harness;
  const auto parent        = harness.arena.makeCell("caller");
  const auto argument      = harness.arena.makeCell("typed argument");
  const auto parentChild   = harness.arena.makeCell("caller child");
  const auto argumentChild = harness.arena.makeCell("argument child");
  populate(harness, parent, argument, parentChild, argumentChild);
  harness.link("d.required", argument,
               harness.arena.makeScalarCell(std::int64_t{9}));

  const auto result = harness.execute(
      "def child($value: Cell { +d.required: Int, . } | String): Cell := "
      "./d.child; let $argument := ##/d.arguments; "
      "##/d.parents/child($argument)");

  EXPECT_EQ(result, (std::vector<CellRef>{argumentChild}));
}

TEST(VQLContextContractTest, ExecutorDoesNotRetainItsLocalCoordinator) {
  Harness harness;
  QueryExecutor executor(harness.session, harness.vm);

  ASSERT_TRUE(executor.execute("##"));
  EXPECT_FALSE(harness.vm.contractCoordinator());
}

TEST(VQLContextContractTest,
     ExecutorRestoresCallerCoordinatorAfterSuccessAndFailure) {
  Harness harness;
  MultiStoreCoordinator prior(harness.core);
  harness.vm.setContractCoordinator(prior);
  QueryExecutor executor(harness.session, harness.vm);

  ASSERT_TRUE(executor.execute("##"));
  ASSERT_TRUE(harness.vm.contractCoordinator());
  EXPECT_EQ(&harness.vm.contractCoordinator()->get(), &prior);

  EXPECT_FALSE(executor.execute("def broken(): Int := \"bad\"; broken()"));
  ASSERT_TRUE(harness.vm.contractCoordinator());
  EXPECT_EQ(&harness.vm.contractCoordinator()->get(), &prior);
}

} // namespace
} // namespace xanadu::vql
