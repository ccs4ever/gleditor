// Q-VIEW-A: dynamic views (pivot and mask rules) lower to Vortex. A view
// handle is a cell the compiler keeps in its handle table; a step through it
// is compiled with the view's rules applied, so reads and creations reach the
// physical dimension and the source is never given a virtual link.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;

struct VQLDynamicViewLoweringTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};
  zigzag::CellRef source{};
  zigzag::CellRef high{};
  zigzag::CellRef urgent{};

  void SetUp() override {
    source              = arena.makeCell("task");
    high                = arena.makeCell("high");
    urgent              = arena.makeCell("urgent");
    const auto priority = engine.resolveDimension("d.priority");
    const auto assignee = engine.resolveDimension("d.assignee");
    zigzag::expectWritten(arena.link(source, priority, false, high));
    zigzag::expectWritten(arena.link(high, priority, false, urgent));
    zigzag::expectWritten(
        arena.link(source, assignee, false, arena.makeCell("Alice")));
    engine.setVariable("task", source);
  }

  void bindView(const std::string &name, const std::string &shape) {
    const auto view = engine.execute("$task" + shape);
    ASSERT_EQ(view.size(), 1u);
    engine.setVariable(name, view);
  }

  std::string textOf(const std::string &query) {
    const auto cells = engine.execute(query);
    EXPECT_EQ(cells.size(), 1u) << query;
    return cells.empty() ? std::string{} : arena.textOf(cells.front());
  }

  std::string textsOf(const std::string &query) {
    std::string joined;
    for (const auto cell : engine.execute(query))
      joined += (joined.empty() ? "" : ",") + arena.textOf(cell);
    return joined;
  }

  std::size_t countOf(const std::string &query) {
    return engine.execute(query).size();
  }

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::exception &error) {
      return error.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }

  struct Footprint {
    std::size_t cells;
    std::size_t links;
    std::size_t trail;
    std::uint32_t marks;
    std::size_t handles;
    bool operator==(const Footprint &) const = default;
  };
  [[nodiscard]] Footprint footprint() const {
    return {arena.cellCount(), arena.deadLinks(), arena.trailSize(),
            arena.outstandingMarks(), engine.shapeCount()};
  }

  /// A refused query must leave cells, links, marks and the handle table as
  /// they were, and name what it refused.
  void expectRefusal(const std::string &query, const std::string &fragment) {
    const auto before = footprint();
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(fragment)) << query;
    EXPECT_EQ(footprint(), before) << query;
  }
};

TEST_F(VQLDynamicViewLoweringTest, ProjectionAnswersAHandleCarryingTheValue) {
  const auto view = engine.execute("$task{ d.1 <=> d.priority }");
  ASSERT_EQ(view.size(), 1u);
  EXPECT_NE(view.front(), source);
  EXPECT_EQ(arena.textOf(view.front()), "task");
  EXPECT_EQ(engine.shapeCount(), 1u);
  EXPECT_FALSE(arena.linkedMinting(source, engine.resolveDimension("d.1"),
                                   zigzag::DimVector::POS));
}

TEST_F(VQLDynamicViewLoweringTest, StepsOnTheHandleReadThePivotedDimension) {
  bindView("v", "{ d.1 <=> d.priority }");
  // A dimension walk answers the whole rank: high, then urgent behind it.
  EXPECT_EQ(textsOf("$v/+d.1/value()"), "high,urgent");
  EXPECT_EQ(textsOf("$v/+d.1/+d.1/value()"), "urgent");
  EXPECT_EQ(textsOf("$v/+d.assignee/value()"), "Alice");
}

TEST_F(VQLDynamicViewLoweringTest, MaskRulesHideAndShowInSourceOrder) {
  bindView("only", "{ !*, d.1 <=> d.priority }");
  EXPECT_EQ(textsOf("$only/+d.1/value()"), "high,urgent");
  EXPECT_EQ(countOf("$only/+d.assignee"), 0u);
  bindView("hidden", "{ *, d.1 <=> d.priority, !+d.assignee }");
  EXPECT_EQ(textsOf("$hidden/+d.1/value()"), "high,urgent");
  EXPECT_EQ(countOf("$hidden/+d.assignee"), 0u);
  EXPECT_EQ(textsOf("$hidden/+d.priority/value()"), "high,urgent");
  // A positive mask rule hides every dimension it does not name.
  bindView("named", "{ +d.assignee, d.1 <=> d.priority }");
  EXPECT_EQ(textOf("$named/+d.assignee/value()"), "Alice");
  EXPECT_EQ(countOf("$named/+d.priority"), 0u);
}

TEST_F(VQLDynamicViewLoweringTest,
       CreationThroughPivotWritesPhysicalDimension) {
  const auto plain = engine.execute("$task/+d.fresh/value()");
  EXPECT_TRUE(plain.empty());
  bindView("v", "{ d.1 <=> d.fresh }");
  engine.execute("$v/+d.1%\"made\"");
  EXPECT_EQ(textOf("$task/+d.fresh/value()"), "made");
  EXPECT_FALSE(arena.linkedMinting(source, engine.resolveDimension("d.1"),
                                   zigzag::DimVector::POS));
}

TEST_F(VQLDynamicViewLoweringTest, CreationOnAMaskedDimensionCreatesNothing) {
  bindView("v", "{ !*, d.1 <=> d.fresh }");
  const auto before = footprint();
  EXPECT_EQ(countOf("$v/+d.assignee%\"x\""), 0u);
  EXPECT_EQ(arena.cellCount(), before.cells);
  EXPECT_EQ(countOf("$task/+d.assignee"), 1u);
}

TEST_F(VQLDynamicViewLoweringTest, TwoProjectionsOfOneSourceAreTwoViews) {
  bindView("a", "{ d.1 <=> d.priority }");
  bindView("b", "{ d.1 <=> d.assignee }");
  EXPECT_EQ(textsOf("$a/+d.1/value()"), "high,urgent");
  EXPECT_EQ(textsOf("$b/+d.1/value()"), "Alice");
}

TEST_F(VQLDynamicViewLoweringTest, ComputedDimensionRefusesNamingTheLaterCard) {
  expectRefusal("$task{ +d.label := ./value() }",
                "computed dimension 'd.label'");
  expectRefusal("$task{ d.1 <=> d.priority, +d.label := ./value() }",
                "computed dimension 'd.label'");
}

TEST_F(VQLDynamicViewLoweringTest, EagerAssignmentCannotMixWithAPivot) {
  expectRefusal("$task{ d.1 <=> d.priority, +d.note: \"x\" }",
                "cannot be mixed with a pivot");
}

TEST_F(VQLDynamicViewLoweringTest, ASourceComputedAtRunTimeRefuses) {
  expectRefusal("##{ d.1 <=> d.priority }", "source computed at run time");
  expectRefusal("let $t := $task/+d.priority return $t{ d.1 <=> d.x }",
                "source computed at run time");
}

TEST_F(VQLDynamicViewLoweringTest, ProjectingAHandleRefuses) {
  bindView("v", "{ d.1 <=> d.priority }");
  expectRefusal("$v{ d.2 <=> d.assignee }", "dynamic view handle");
  expectRefusal("$v{*}", "dynamic view handle");
}

TEST_F(VQLDynamicViewLoweringTest, UnfollowableStepsRefuseByName) {
  bindView("v", "{ d.1 <=> d.priority }");
  expectRefusal("$v/+d.1[./value() = \"high\"]", "predicate");
  expectRefusal("$v/+d.1/first()", "function first()");
  expectRefusal("$v/+d.1/+d.1{*}", "projection");
  expectRefusal("$v/+d.1!keep", "yield mode");
}

TEST_F(VQLDynamicViewLoweringTest, ViewCellsMayNotEscapeThePath) {
  bindView("v", "{ d.1 <=> d.priority }");
  expectRefusal("$v/+d.1", "leaves the path");
  expectRefusal("let $x := $v/+d.1 return $x", "leaves the path");
  expectRefusal("for $c in $v/+d.1 return $c/+d.1", "leaves the path");
}

TEST_F(VQLDynamicViewLoweringTest, HandlesMayNotBeLoopedOrPassedOrAdded) {
  bindView("v", "{ d.1 <=> d.priority }");
  expectRefusal("for $c in $v return $c/+d.1/value()", "dynamic view handle");
  expectRefusal("def f($c) := $c/+d.1/value(); f($v)", "dynamic view handle");
  expectRefusal("let $s := $v + 1 return $s", "dynamic view");
}

TEST_F(VQLDynamicViewLoweringTest, HandleTableFollowsTheHandleCell) {
  bindView("v", "{ d.1 <=> d.priority }");
  EXPECT_EQ(engine.shapeCount(), 1u);
  // A handle is a cell the caller keeps, so it stays resolvable after a later
  // query rebinds the variable; the entry goes with the cell, not the name.
  (void)engine.execute("let $v := 1 return $v");
  EXPECT_EQ(engine.shapeCount(), 1u);
}

TEST_F(VQLDynamicViewLoweringTest, ProgramIsReleasedAfterReadsAndProjections) {
  zigzag::vortex::VortexVM vm(engine.core());
  xanadu::vql::QueryExecutor executor{engine, vm};
  const auto cells = arena.cellCount();
  const auto view  = executor.execute("$task{ d.1 <=> d.priority }");
  ASSERT_TRUE(view) << view.error().message;
  EXPECT_FALSE(executor.programRetainedBecause());
  EXPECT_EQ(arena.cellCount(), cells + 1);
  engine.setVariable("v", *view);
  const auto read = executor.execute("$v/+d.1/value()");
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_FALSE(executor.programRetainedBecause());
  const auto created = executor.execute("$v/+d.1%\"x\"");
  ASSERT_TRUE(created) << created.error().message;
  ASSERT_TRUE(executor.programRetainedBecause());
  EXPECT_THAT(*executor.programRetainedBecause(),
              testing::HasSubstr("existed before"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

} // namespace
