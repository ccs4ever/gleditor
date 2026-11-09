/**
 * @file vql_execution_test.cpp
 * @brief Tests the frontend-neutral VQL direct and Vortex execution service.
 */
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine direct{coordinator};
  VortexVM vm{core};

  [[nodiscard]] QueryExecutor executor(const QueryExecutionMode mode) {
    return QueryExecutor(direct, vm, {.mode = mode});
  }
};

std::vector<std::string> render(Harness &harness,
                                const std::vector<CellRef> &cells) {
  std::vector<std::string> values;
  for (const auto cell : cells) {
    const auto value = harness.core.render(cell);
    if (!value) continue;
    values.push_back(std::visit(
        [](const auto &held) -> std::string {
          using T = std::remove_cvref_t<decltype(held)>;
          if constexpr (std::same_as<T, std::string>)
            return held;
          else if constexpr (std::same_as<T, bool>)
            return held ? "true" : "false";
          else
            return std::to_string(held);
        },
        *value));
  }
  return values;
}

TEST(VQLExecutionTest, TypedCallsRunThroughEitherRequestedEngine) {
  const auto run = [](const QueryExecutionMode mode)
      -> std::expected<std::vector<std::string>, QueryExecutionError> {
    Harness harness;
    auto executor     = harness.executor(mode);
    const auto result = executor.execute(
        "def identity($value: Int): Int := $value; identity(17)");
    if (!result) return std::unexpected(result.error());
    return render(harness, *result);
  };
  const auto direct = run(QueryExecutionMode::Direct);
  const auto vortex = run(QueryExecutionMode::Vortex);
  ASSERT_FALSE(direct);
  EXPECT_EQ(direct.error().category, QueryExecutionError::Category::Compile);
  ASSERT_TRUE(vortex) << vortex.error().message;
  EXPECT_EQ(*vortex, (std::vector<std::string>{"17"}));
}

TEST(VQLExecutionTest, ExternalCellBindingsKeepTheirIdentityInBothModes) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness harness;
    const auto value = harness.arena.makeScalarCell(std::int64_t{41});
    harness.direct.setVariable("external", value);
    auto executor     = harness.executor(mode);
    const auto result = executor.execute(
        "def identity($x: Int): Int := $x; identity($external)");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, std::vector<CellRef>{value});
  }
}

TEST(VQLExecutionTest, ParameterMismatchFiltersWithoutChangingEngineMode) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness harness;
    auto executor     = harness.executor(mode);
    const auto result = executor.execute(
        "def identity($value: Int): Int := $value; identity(\"bad\")");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->empty());
  }
}

TEST(VQLExecutionTest, ReturnMismatchHasTheSameStructuredErrorCategory) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness harness;
    auto executor     = harness.executor(mode);
    const auto result = executor.execute(
        "def broken($value: Int): String := $value; broken(17)");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category,
              QueryExecutionError::Category::TypeContract);
    EXPECT_EQ(result.error().message,
              "broken() returned a value outside its declared type");
  }
}

TEST(VQLExecutionTest, ContextCellsReachBothExecutionModes) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness harness;
    const auto title      = harness.coordinator.resolveDimension("d.title");
    const CellRef context = harness.arena.makeCell();
    const CellRef value   = harness.arena.makeCell("local title");
    zigzag::expectWritten(
        harness.arena.link(context, title, DimVector::POS, value));

    auto executor     = harness.executor(mode);
    const auto result = executor.execute("./+d.title", {context});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(render(harness, *result),
              (std::vector<std::string>{"local title"}));
  }
}

TEST(VQLExecutionTest, ParseAndUnknownTypeErrorsAreStructured) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness harness;
    auto executor    = harness.executor(mode);
    const auto parse = executor.execute("def nope(");
    ASSERT_FALSE(parse);
    EXPECT_EQ(parse.error().category, QueryExecutionError::Category::Parse);

    const auto type =
        executor.execute("def typed($value: MissingType): Cell := $value; ##");
    ASSERT_FALSE(type);
    EXPECT_EQ(type.error().category,
              QueryExecutionError::Category::TypeContract);
    EXPECT_NE(type.error().message.find("MissingType"), std::string::npos);
  }
}

TEST(VQLExecutionTest, VortexCycleBudgetIsReportedWithoutFallback) {
  Harness harness;
  QueryExecutor executor(harness.direct, harness.vm,
                         {.mode = QueryExecutionMode::Vortex, .maxCycles = 0});
  const auto result = executor.execute("##");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Budget);
}

} // namespace
TEST(VQLExecutionTest, FiltersMixedCandidatesWithoutLosingIdentity) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness harness;
    const auto yesRoot = harness.arena.makeCell("yes root");
    const auto noRoot  = harness.arena.makeCell("no root");
    const auto yes     = harness.arena.makeCell("yes");
    const auto no      = harness.arena.makeCell("no");
    const auto child   = harness.coordinator.resolveDimension("d.child");
    zigzag::expectWritten(
        harness.arena.link(yesRoot, child, DimVector::POS, yes));
    zigzag::expectWritten(
        harness.arena.link(noRoot, child, DimVector::POS, no));
    auto executor = harness.executor(mode);
    const auto result =
        executor.execute("def keep($x: Cell): Bool := $x = \"yes\"; "
                         "./d.child[keep(.)]",
                         {yesRoot, noRoot});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, std::vector<CellRef>{yes});
  }
}

TEST(VQLExecutionTest, NonFinalVariadicSignaturesFailBeforeExecution) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness h;
    auto executor = h.executor(mode);
    for (const auto source : {"def f(...$rest, $later): Any := $rest; ##",
                              "def f(...$first, ...$rest): Any := $rest; ##"}) {
      SCOPED_TRACE(source);
      const auto result = executor.execute(source);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().category,
                QueryExecutionError::Category::TypeContract);
      EXPECT_NE(result.error().message.find("variadic"), std::string::npos);
    }
  }
}

TEST(VQLExecutionTest, InvalidSignatureRegistersNoDefinitionsInDirectEngine) {
  Harness h;
  EXPECT_THROW(
      h.direct.execute("def installed(): Int := 7; "
                       "def invalid(...$rest, $later): Any := $rest; ##"),
      TypeContractError);
  EXPECT_THROW(h.direct.execute("installed()"), std::exception);
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled =
      compiler.compile("def invalid(...$rest, $later): Any := $rest; ##");
  EXPECT_FALSE(compiled.success);
  EXPECT_NE(compiled.errorMessage.find("variadic parameter must be last"),
            std::string::npos);
}

TEST(VQLExecutionTest, PreboundTypedRequestBudgetFailureCanRecoverInBothModes) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    Harness h;
    const auto value = h.arena.makeCell("prebound");
    const std::vector<CellRef> expected(128, value);
    h.direct.setVariable("seed", expected);
    auto executor     = h.executor(mode);
    const auto source = "def collect(...$xs: Stream<Cell>): Stream<Cell> := "
                        "$xs; collect($seed)";
    // Request-entry failure is separate from the native matcher probe tests.
    h.arena.setExecutionLimits(100'000, 0, 100'000);
    const auto exhausted = executor.execute(source);
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().category,
              QueryExecutionError::Category::Budget);
    h.arena.setExecutionLimits(100'000, 100'000, 100'000);
    const auto recovered = executor.execute(source);
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(*recovered, expected);
  }
}

} // namespace xanadu::vql
