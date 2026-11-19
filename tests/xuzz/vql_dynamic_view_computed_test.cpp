// Q-VIEW-B: a computed dimension of a dynamic view is lowered into the step
// taken on the view's handle, so it is evaluated against the source each time
// the view is traversed. What cannot be lowered refuses by name.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;

struct VQLDynamicViewComputedTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};
  zigzag::CellRef source{};

  void SetUp() override {
    source              = arena.makeCell("task");
    const auto priority = engine.resolveDimension("d.priority");
    const auto high     = arena.makeCell("high");
    zigzag::expectWritten(arena.link(source, priority, false, high));
    zigzag::expectWritten(
        arena.link(high, priority, false, arena.makeCell("urgent")));
    zigzag::expectWritten(arena.link(source, engine.resolveDimension("d.owner"),
                                     false, arena.makeCell("Alice")));
    engine.setVariable("task", source);
  }

  void bindView(const std::string &name, const std::string &shape) {
    const auto view = engine.execute("$task" + shape);
    ASSERT_EQ(view.size(), 1u);
    engine.setVariable(name, view);
  }

  std::string textsOf(const std::string &query) {
    std::string joined;
    for (const auto cell : engine.execute(query))
      joined += (joined.empty() ? "" : ",") + arena.textOf(cell);
    return joined;
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

  void expectRefusal(const std::string &query, const std::string &fragment) {
    const auto before = footprint();
    EXPECT_THAT(refusalOf(query), testing::HasSubstr(fragment)) << query;
    EXPECT_EQ(footprint(), before) << query;
  }
};

TEST_F(VQLDynamicViewComputedTest, ComputedValueFollowsTheSourceBetweenWalks) {
  bindView("v", "{ +d.top := ./+d.owner/value() }");
  EXPECT_EQ(textsOf("$v/+d.top/value()"), "Alice");
  engine.execute("$task/+d.owner/value(\"Bob\")");
  EXPECT_EQ(textsOf("$v/+d.top/value()"), "Bob");
  // The source never gains the virtual link.
  EXPECT_FALSE(arena.linkedMinting(source, engine.resolveDimension("d.top"),
                                   zigzag::DimVector::POS));
}

TEST_F(VQLDynamicViewComputedTest, LiteralAndCountExpressionsLower) {
  bindView("v", "{ +d.kind := \"note\", +d.n := ./+d.priority/count() }");
  EXPECT_EQ(textsOf("$v/+d.kind/value()"), "note");
  const auto count = engine.execute("$v/+d.n");
  ASSERT_EQ(count.size(), 1u);
  EXPECT_EQ(arena.asInt64(count.front()), 2);
}

TEST_F(VQLDynamicViewComputedTest, ComputedCellIsAFreshCopyNotASourceCell) {
  bindView("v", "{ +d.first := ./+d.owner }");
  const auto reached = engine.execute("$v/+d.first");
  ASSERT_EQ(reached.size(), 1u);
  EXPECT_EQ(arena.textOf(reached.front()), "Alice");
  // The oracle answers a fresh cell holding the value, never the linked cell,
  // so a walk from it would find none of the source's links; the lowering
  // refuses that walk instead of answering it.
  expectRefusal("$v/+d.first/+d.priority", "after computed dimension");
}

TEST_F(VQLDynamicViewComputedTest, OtherRulesStillApplyAroundAComputedOne) {
  bindView("open", "{ *, +d.label := ./value() }");
  EXPECT_EQ(textsOf("$open/+d.label/value()"), "task");
  EXPECT_EQ(textsOf("$open/+d.priority/value()"), "high,urgent");
  bindView("closed", "{ !*, +d.label := ./value() }");
  EXPECT_EQ(textsOf("$closed/+d.label/value()"), "task");
  EXPECT_TRUE(engine.execute("$closed/+d.priority").empty());
  bindView("both", "{ d.1 <=> d.priority, +d.label := ./value() }");
  EXPECT_EQ(textsOf("$both/+d.1/value()"), "high,urgent");
  EXPECT_EQ(textsOf("$both/+d.label/value()"), "task");
}

TEST_F(VQLDynamicViewComputedTest, ADirectionOnlyAnswersItsOwnDirection) {
  bindView("v", "{ +d.label := ./value() }");
  EXPECT_TRUE(engine.execute("$v/-d.label/value()").empty());
}

TEST_F(VQLDynamicViewComputedTest, MissingSourceValueAnswersNothing) {
  bindView("v", "{ +d.gone := ./+d.absent/value() }");
  EXPECT_TRUE(engine.execute("$v/+d.gone").empty());
}

TEST_F(VQLDynamicViewComputedTest, ReadsLeakNothingAndReleaseTheProgram) {
  bindView("v", "{ +d.label := ./value() }");
  zigzag::vortex::VortexVM vm(engine.core());
  xanadu::vql::QueryExecutor executor{engine, vm};
  const auto warm = executor.execute("$v/+d.label/value()");
  ASSERT_TRUE(warm) << warm.error().message;
  const auto before = footprint();
  const auto read   = executor.execute("$v/+d.label/value()");
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_FALSE(executor.programRetainedBecause());
  EXPECT_EQ(footprint(), before);
}

TEST_F(VQLDynamicViewComputedTest, WritingAComputedDimensionRefusesByName) {
  bindView("v", "{ +d.label := ./value() }");
  expectRefusal("$v/+d.label%\"bad\"",
                "computed dimension 'd.label' of a dynamic view cannot be "
                "written");
  EXPECT_THROW(engine.execute("$v/+d.label%\"bad\""), std::runtime_error);
}

TEST_F(VQLDynamicViewComputedTest, EagerAssignmentCannotMixWithAComputedRule) {
  expectRefusal("$task{ +d.label := ./value(), +d.note: \"x\" }",
                "cannot be mixed with a pivot or computed rule");
}

TEST_F(VQLDynamicViewComputedTest, ExpressionsOutsideTheStaticSubsetRefuse) {
  engine.setVariable("other", engine.execute("$task"));
  expectRefusal("$task{ +d.l := $other/value() }",
                "cannot be compiled to Vortex: its path does not start at the "
                "source cell");
  expectRefusal("$task{ +d.l := ./+d.priority[./value() = \"high\"]/value() }",
                "a step of its path carries a predicate");
  expectRefusal("$task{ +d.l := ./+d.priority%\"x\" }",
                "a step of its path is not a read along a plain dimension");
  expectRefusal("$task{ +d.l := ./+d.priority/first() }",
                "function first() is not value() or count()");
  expectRefusal("$task{ +d.l := ./+d.priority/$other }",
                "a step of its path is not a read along a plain dimension");
  expectRefusal("$task{ +d.l := 1 + 2 }",
                "its expression is not a literal or a path");
}

TEST_F(VQLDynamicViewComputedTest, RulesThatNestOrReferenceViewsRefuse) {
  bindView("inner", "{ +d.a := ./value() }");
  expectRefusal("$task{ +d.b := $inner/+d.a/value() }",
                "its path does not start at the source cell");
}

TEST_F(VQLDynamicViewComputedTest,
       AShapeVariableCarriesAComputedRuleIntoAView) {
  const auto view =
      engine.execute("let $s := { +d.a := ./value() } return $task{ $s }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("v", view);
  EXPECT_EQ(textsOf("$v/+d.a/value()"), "task");
}

TEST_F(VQLDynamicViewComputedTest, ComputedRulesRefuseInStaticProjections) {
  // A guard that the projection forms do not route into the view lowering;
  // the base refused them too, with this message.
  expectRefusal("weave { apply_shape($task, { +d.l := ./value() }) }",
                "pivot, computed-dimension, variable and nested rules are not "
                "lowered");
}

TEST_F(VQLDynamicViewComputedTest, StepsOnAComputedResultRefuseNotWalkWrong) {
  bindView("v", "{ +d.label := ./value() }");
  // The oracle gives a fresh plain cell; a following write must not land on a
  // program-owned or source cell.
  expectRefusal("$v/+d.label/+d.x%\"y\"",
                "a step after computed dimension 'd.label'");
}

TEST_F(VQLDynamicViewComputedTest, AComputedViewHandleOfAnotherEngineRefuses) {
  VQLEngine other{engine.core()};
  other.setVariable("task", source);
  const auto view = other.execute("$task{ +d.label := ./value() }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("v", view);
  expectRefusal("$v/+d.label/value()", "dynamic view handle");
}

TEST_F(VQLDynamicViewComputedTest, FailedProjectionLeavesNoHandleOrMark) {
  // The first rule is lowerable and the second is not: nothing of the first
  // may be minted before the refusal.
  expectRefusal("$task{ +d.l := ./value(), +d.m := $nope/value() }",
                "its path does not start at the source cell");
}

TEST_F(VQLDynamicViewComputedTest, APathAnsweringTwoCellsFailsAtRunTimeByName) {
  // The rank holds two cells; keeping the first and dropping the second
  // would answer `high` as if it were the whole answer.
  bindView("v", "{ +d.top := ./+d.priority/value(), +d.e := ./+d.priority }");
  const auto before = footprint();
  EXPECT_THAT(refusalOf("$v/+d.top/value()"),
              testing::HasSubstr("computed dimension 'd.top'"));
  EXPECT_THAT(refusalOf("$v/+d.top/value()"),
              testing::HasSubstr("more than one cell"));
  EXPECT_THAT(refusalOf("$v/+d.e"), testing::HasSubstr("more than one cell"));
  EXPECT_EQ(footprint(), before);
}

} // namespace
