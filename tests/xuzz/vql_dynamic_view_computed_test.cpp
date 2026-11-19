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
  bindView("v", "{ +d.top := ./+d.priority/value() }");
  EXPECT_EQ(textsOf("$v/+d.top/value()"), "high");
  engine.execute("$task/+d.priority/value(\"changed\")");
  EXPECT_EQ(textsOf("$v/+d.top/value()"), "changed");
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
  bindView("v", "{ +d.first := ./+d.priority }");
  const auto reached = engine.execute("$v/+d.first");
  ASSERT_EQ(reached.size(), 1u);
  EXPECT_EQ(arena.textOf(reached.front()), "high");
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
  expectRefusal("$v/+d.label%\"bad\"", "computed dimension 'd.label'");
  EXPECT_THROW(engine.execute("$v/+d.label%\"bad\""), std::runtime_error);
}

TEST_F(VQLDynamicViewComputedTest, EagerAssignmentCannotMixWithAComputedRule) {
  expectRefusal("$task{ +d.label := ./value(), +d.note: \"x\" }",
                "cannot be mixed with a pivot or computed rule");
}

TEST_F(VQLDynamicViewComputedTest, ExpressionsOutsideTheStaticSubsetRefuse) {
  engine.setVariable("other", engine.execute("$task"));
  expectRefusal("$task{ +d.l := $other/value() }", "computed dimension 'd.l'");
  expectRefusal("$task{ +d.l := ./+d.priority[./value() = \"high\"]/value() }",
                "computed dimension 'd.l'");
  expectRefusal("$task{ +d.l := ./+d.priority%\"x\" }",
                "computed dimension 'd.l'");
  expectRefusal("$task{ +d.l := ./+d.priority/first() }",
                "computed dimension 'd.l'");
  expectRefusal("$task{ +d.l := ./+d.priority/$other }",
                "computed dimension 'd.l'");
  expectRefusal("$task{ +d.l := 1 + 2 }", "computed dimension 'd.l'");
}

TEST_F(VQLDynamicViewComputedTest, RulesThatNestOrReferenceViewsRefuse) {
  bindView("inner", "{ +d.a := ./value() }");
  expectRefusal("$task{ +d.b := $inner/+d.a/value() }",
                "computed dimension 'd.b'");
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
  expectRefusal("weave { apply_shape($task, { +d.l := ./value() }) }",
                "computed");
}

TEST_F(VQLDynamicViewComputedTest, StepsOnAComputedResultRefuseNotWalkWrong) {
  bindView("v", "{ +d.label := ./value() }");
  // The oracle gives a fresh plain cell; a following write must not land on a
  // program-owned or source cell.
  expectRefusal("$v/+d.label/+d.x%\"y\"", "computed dimension");
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
  const auto before = footprint();
  EXPECT_THROW(engine.execute("$task{ +d.l := $nope/value() }"),
               std::runtime_error);
  EXPECT_EQ(footprint(), before);
}

} // namespace
