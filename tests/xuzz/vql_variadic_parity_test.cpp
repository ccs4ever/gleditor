#include <gtest/gtest.h>

#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct VariadicHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  const DimRef items   = coordinator.resolveDimension("d.items");
  const DimRef more    = coordinator.resolveDimension("d.more");
  const DimRef title   = coordinator.resolveDimension("d.title");
  const CellRef first  = arena.makeScalarCell(std::int64_t{1});
  const CellRef second = arena.makeScalarCell(std::int64_t{2});
  const CellRef third  = arena.makeScalarCell(std::int64_t{3});
  const CellRef extra  = arena.makeCell("extra");
  const CellRef last   = arena.makeCell("last");

  VariadicHarness() {
    zigzag::expectWritten(
        arena.link(core.home(), items, DimVector::POS, first));
    zigzag::expectWritten(arena.link(first, items, DimVector::POS, second));
    zigzag::expectWritten(arena.link(second, items, DimVector::POS, third));
    zigzag::expectWritten(arena.link(core.home(), more, DimVector::POS, extra));
    zigzag::expectWritten(arena.link(extra, more, DimVector::POS, last));
    zigzag::expectWritten(
        arena.link(first, title, DimVector::POS, arena.makeCell("one")));
  }
};

TEST(VQLVariadicParityTest, FlattensArgumentStreamsInOrderAndKeepsDuplicates) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute(
        "def collect($head: Cell, ...$rest): Stream<Cell> := $rest; "
        "collect(##, ##/d.items, ##/d.more[-1], ##/d.items[-1])");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{h.first, h.second, h.third, h.last,
                                             h.third}));
  }
}

TEST(VQLVariadicParityTest, EmptyRestAndFixedParameterDefault) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto empty = executor.execute(
        "def collect(...$rest): Stream<Cell> := $rest; collect()");
    ASSERT_TRUE(empty) << empty.error().message;
    EXPECT_TRUE(empty->empty());

    const auto defaulted = executor.execute(
        "def head($head: Cell := ##, ...$rest): Cell := $head; head()");
    ASSERT_TRUE(defaulted) << defaulted.error().message;
    EXPECT_EQ(*defaulted, (std::vector<CellRef>{h.core.home()}));
  }
}

TEST(VQLVariadicParityTest, TypeChecksEachArgumentStreamBeforeFlattening) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto valid = executor.execute(
        "def collect(...$rest: Stream<Int>): Stream<Int> := $rest; "
        "collect(##/d.items, ##/d.items[-1])");
    ASSERT_TRUE(valid) << valid.error().message;
    EXPECT_EQ(*valid,
              (std::vector<CellRef>{h.first, h.second, h.third, h.third}));

    const auto invalid = executor.execute(
        "def collect(...$rest: Stream<Int>): Stream<Int> := $rest; "
        "collect(##/d.items, \"bad\")");
    ASSERT_TRUE(invalid) << invalid.error().message;
    EXPECT_TRUE(invalid->empty());

    const auto cardinality =
        executor.execute("def collect(...$rest: Cell): Stream<Cell> := $rest; "
                         "collect(##/d.items)");
    ASSERT_TRUE(cardinality) << cardinality.error().message;
    EXPECT_TRUE(cardinality->empty());
  }
}

TEST(VQLVariadicParityTest, RestCaptureDoesNotLeakShapeFieldsIntoScope) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute(
        "def reveal(...$items: Cell { +d.title: $title: String }): String "
        ":= $title; let $title := \"outer\"; reveal(##/d.items[-3])");
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(h.arena.textOf(result->front()), "outer");
  }
}

TEST(VQLVariadicParityTest, CallerScopeUfcsAndRepeatedCallsStayIsolated) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto query =
        "def collect($head: Cell, ...$rest): Stream<Cell> := $rest; "
        "let $rest := ##/d.more; "
        "collect(##, $rest, ##/d.items)";
    const auto expected =
        std::vector<CellRef>{h.extra, h.last, h.first, h.second, h.third};
    for (int repeat = 0; repeat < 2; ++repeat) {
      const auto result = executor.execute(query);
      ASSERT_TRUE(result) << result.error().message;
      EXPECT_EQ(*result, expected);
    }

    const auto ufcs = executor.execute(
        "def collect($head: Cell, ...$rest): Stream<Cell> := $rest; "
        "##/d.items/collect(##/d.more)");
    ASSERT_TRUE(ufcs) << ufcs.error().message;
    EXPECT_EQ(*ufcs, (std::vector<CellRef>{h.extra, h.last, h.extra, h.last,
                                           h.extra, h.last}));
  }
}

TEST(VQLVariadicParityTest, LateBadArgumentDoesNotPartiallyReturnRest) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result =
        executor.execute("def collect(...$rest: Int): Stream<Int> := $rest; "
                         "collect(1, 2, \"bad\")");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result->empty());
  }
}

TEST(VQLVariadicParityTest, NestedCallsEvaluateBeforeCallerActivation) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto variadic = executor.execute(
        "def collect(...$rest: Stream<Int>): Stream<Int> := $rest; "
        "collect(collect(1, 2), 3)");
    ASSERT_TRUE(variadic) << variadic.error().message;
    ASSERT_EQ(variadic->size(), 3u);
    EXPECT_EQ(h.arena.asInt64((*variadic)[0]), 1);
    EXPECT_EQ(h.arena.asInt64((*variadic)[1]), 2);
    EXPECT_EQ(h.arena.asInt64((*variadic)[2]), 3);

    const auto ordinary =
        executor.execute("def identity($value: Int): Int := $value; "
                         "identity(identity(7))");
    ASSERT_TRUE(ordinary) << ordinary.error().message;
    ASSERT_EQ(ordinary->size(), 1u);
    EXPECT_EQ(h.arena.asInt64(ordinary->front()), 7);
  }
}

TEST(VQLVariadicParityTest, RecursiveBodiesAndDefaultsStillRefuseInVortex) {
  VariadicHarness h;
  QueryExecutor executor(h.engine, h.vm, {.mode = QueryExecutionMode::Vortex});
  for (const auto query : {
           "def recurse(): Int := recurse(); recurse()",
           "def recurse($value: Int := recurse()): Int := $value; recurse()",
       }) {
    SCOPED_TRACE(query);
    const auto result = executor.execute(query);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, QueryExecutionError::Category::Compile);
    EXPECT_NE(result.error().message.find("recursive compiled call"),
              std::string::npos);
  }
}

TEST(VQLVariadicParityTest, ReturnPromiseChecksFlattenedBodyResults) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    VariadicHarness h;
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute(
        "def broken(...$rest: Int): String := $rest; broken(7)");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category,
              QueryExecutionError::Category::TypeContract);
  }
}

} // namespace
} // namespace xanadu::vql
