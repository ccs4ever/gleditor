// Q-SHAPEVAR: shape-valued variables, arguments and function results. A shape
// is syntax, so every one of these resolves while the query is compiled and
// lands on the static projection paths; nothing here asks the VM to hold one.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::TypeContractError;
using xanadu::vql::VQLEngine;
using zigzag::DimVector;

/// Home carries one neighbour on each of three dimensions, so a projection's
/// effect is read off which of them are still linked.
struct VQLShapeVariableTest : testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  zigzag::DimRef keepDim = engine.resolveDimension("d.keep");
  zigzag::DimRef dropDim = engine.resolveDimension("d.drop");
  zigzag::DimRef tagDim  = engine.resolveDimension("d.tag");

  void SetUp() override {
    for (const auto dimension : {keepDim, dropDim, tagDim})
      ASSERT_TRUE(arena.link(core.home(), dimension, DimVector::POS,
                             arena.makeCell("neighbour")));
  }

  [[nodiscard]] bool linked(const zigzag::DimRef dimension) const {
    return arena.linked(core.home(), dimension, DimVector::POS).has_value();
  }

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::runtime_error &error) {
      return error.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }
};

TEST_F(VQLShapeVariableTest, VariableBoundAndUsedInOneQuery) {
  ASSERT_EQ(engine.execute("let $s := { +d.keep } return ##{ $s }").front(),
            core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(dropDim));
  EXPECT_FALSE(linked(tagDim));
}

TEST_F(VQLShapeVariableTest, VariableBoundThenUsedInALaterExecute) {
  const auto handle = engine.execute("let $s := { +d.keep } return $s");
  ASSERT_EQ(handle.size(), 1u);
  EXPECT_EQ(engine.getVariable("s"), handle);
  EXPECT_EQ(engine.shapeCount(), 1u);

  ASSERT_EQ(engine.execute("##{ $s }").front(), core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, FunctionResultNamedInsideAShape) {
  ASSERT_EQ(
      engine.execute("def keep_shape() := { +d.keep }; ##{ keep_shape() }")
          .front(),
      core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, ShapePassedAsAFunctionArgument) {
  ASSERT_EQ(
      engine.execute("def mask($m) := ##{ $m }; mask({ +d.keep })").front(),
      core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, ShapeVariablePassedAsAFunctionArgument) {
  ASSERT_EQ(engine
                .execute("def mask($m) := ##{ $m }; "
                         "let $s := { +d.tag } return mask($s)")
                .front(),
            core.home());
  EXPECT_TRUE(linked(tagDim));
  EXPECT_FALSE(linked(keepDim));
}

TEST_F(VQLShapeVariableTest, ShapeFunctionTakingAShapeArgument) {
  ASSERT_EQ(engine
                .execute("def wider($s) := { $s, +d.tag }; "
                         "let $k := { +d.keep } return ##{ wider($k) }")
                .front(),
            core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_TRUE(linked(tagDim));
  EXPECT_FALSE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, NestedShapeLiteralInsideAShape) {
  ASSERT_EQ(engine.execute("let $a := { +d.keep } return ##{ $a, { +d.tag } }")
                .front(),
            core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_TRUE(linked(tagDim));
  EXPECT_FALSE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, TwoVariablesConcatenateRightMostRuleWins) {
  ASSERT_EQ(engine
                .execute("let $a := { +d.keep, +d.tag }; "
                         "let $b := { !d.tag }; return ##{ $a, $b }")
                .front(),
            core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(tagDim));
  EXPECT_FALSE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, ShapeVariableInProjectAndApplyShape) {
  const auto kept =
      engine.execute("let $s := { +d.keep } return project(##, $s)");
  ASSERT_EQ(kept.size(), 1u);
  // project() answers a copy and leaves home untouched.
  EXPECT_TRUE(linked(dropDim));
  EXPECT_TRUE(linked(keepDim));

  ASSERT_EQ(engine
                .execute("def pick() := { +d.keep }; "
                         "apply_shape(##, pick())")
                .front(),
            core.home());
  EXPECT_FALSE(linked(dropDim));
  EXPECT_TRUE(linked(keepDim));
}

TEST_F(VQLShapeVariableTest, ShapeInAVariableKeepsItsOwnWildcardOrder) {
  ASSERT_EQ(engine.execute("let $s := { !*, +d.keep } return ##{ +d.tag, $s }")
                .front(),
            core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(tagDim));
}

TEST_F(VQLShapeVariableTest, SelfReferenceIsRefusedByName) {
  EXPECT_THROW(engine.execute("let $a := { $a } return ##{ $a }"),
               TypeContractError);
}

TEST_F(VQLShapeVariableTest, MutuallyRecursiveShapeFunctionsAreRefusedByName) {
  EXPECT_THAT(refusalOf("def a() := { b() }; def b() := { a() }; "
                        "##{ a() }"),
              testing::HasSubstr("shape"));
  EXPECT_THAT(
      refusalOf("def a() := { b() }; def b() := { a() }; "
                "##{ a() }"),
      testing::AnyOf(testing::HasSubstr("'a'"), testing::HasSubstr("'b'")));
}

TEST_F(VQLShapeVariableTest, UnboundVariableKeepsTheTypeContractError) {
  EXPECT_THROW(engine.execute("##{ $missing }"), TypeContractError);
  EXPECT_THROW(engine.execute("##{ $a, +d.keep }"), TypeContractError);
}

TEST_F(VQLShapeVariableTest, ShapeVariableWhereAValueIsExpectedIsRefused) {
  // Written inline the compiler still knows the operand is a shape handle.
  EXPECT_THAT(refusalOf("let $s := { +d.keep }; let $x := $s + 1; return $x"),
              testing::AllOf(testing::HasSubstr("shape value"),
                             testing::HasSubstr("'+'")));
  // Behind a call frame the value is only known at run time: the VM names the
  // failed operation rather than reading a shape as a number.
  EXPECT_THAT(refusalOf("def pure add($l: Any, $r: Any): Any := $l + $r; "
                        "let $s := { +d.keep } return add($s, 1)"),
              testing::HasSubstr("VqlAdd"));
}

TEST_F(VQLShapeVariableTest, VariableHoldingAScalarIsNotAShape) {
  const auto message = refusalOf("let $n := 3 return ##{ $n }");
  EXPECT_THAT(message, testing::HasSubstr("not statically known"));
  EXPECT_THAT(message, testing::HasSubstr("$n"));
}

TEST_F(VQLShapeVariableTest,
       PivotAndComputedRulesInAVariableKeepTheirRefusals) {
  EXPECT_THAT(refusalOf("let $s := { d.1 <=> d.priority } return ##{ $s }"),
              testing::HasSubstr("dimension pivot"));
  EXPECT_THAT(refusalOf("let $s := { +d.label := ./value() } return ##{ $s }"),
              testing::HasSubstr("computed dimension"));
  EXPECT_THAT(refusalOf("def p() := { d.1 <=> d.priority }; ##{ p() }"),
              testing::HasSubstr("dimension pivot"));
}

TEST_F(VQLShapeVariableTest, ShapeOfAnotherEngineIsNotStaticallyKnown) {
  const auto handle = engine.execute("let $s := { +d.keep } return $s");
  ASSERT_EQ(handle.size(), 1u);

  VQLEngine other{core};
  other.setVariable("s", handle);
  EXPECT_EQ(other.shapeCount(), 0u);
  EXPECT_THAT([&] { (void)other.execute("##{ $s }"); },
              testing::ThrowsMessage<std::runtime_error>(
                  testing::HasSubstr("not statically known")));
  EXPECT_TRUE(linked(dropDim));
}

TEST_F(VQLShapeVariableTest, FailedQueryLeavesNoHalfRegisteredShape) {
  EXPECT_ANY_THROW(engine.execute(
      "let $s := { +d.keep }; let $t := $nosuch + 1; return $t"));
  EXPECT_EQ(engine.shapeCount(), 0u);
  EXPECT_TRUE(engine.getVariable("s").empty());
  EXPECT_THROW(engine.execute("##{ $s }"), TypeContractError);
}

TEST_F(VQLShapeVariableTest, ClearVariablesDropsTheTable) {
  (void)engine.execute("let $s := { +d.keep } return $s");
  ASSERT_EQ(engine.shapeCount(), 1u);
  engine.clearVariables();
  EXPECT_EQ(engine.shapeCount(), 0u);
  EXPECT_THROW(engine.execute("##{ $s }"), TypeContractError);
}

TEST_F(VQLShapeVariableTest, RepeatedQueriesAreBoundedByTheShapesHeld) {
  constexpr int rounds = 25;
  // A rebound variable leaves the cell it held behind, whatever it held; the
  // shape must cost no more than a scalar does.
  (void)engine.execute("let $n := 1 return $n");
  const auto scalarBefore = arena.cellCount();
  for (int round = 0; round < rounds; ++round)
    (void)engine.execute("let $n := 1 return $n");
  const auto scalarGrowth = arena.cellCount() - scalarBefore;

  (void)engine.execute("let $s := { +d.keep } return $s");
  const auto shapeBefore = arena.cellCount();
  for (int round = 0; round < rounds; ++round)
    (void)engine.execute("let $s := { +d.keep } return $s");
  EXPECT_LE(arena.cellCount() - shapeBefore, scalarGrowth);
  // The table is bounded by the shapes a variable or a result still names,
  // not by the queries run.
  EXPECT_EQ(engine.shapeCount(), 1u);

  (void)engine.execute("let $t := { +d.tag } return $t");
  EXPECT_EQ(engine.shapeCount(), 2u);
  (void)engine.execute("let $s := 1 return $s");
  EXPECT_EQ(engine.shapeCount(), 1u);
}

TEST_F(VQLShapeVariableTest,
       ProgramReleaseAfterAShapeVariableQueryKeepsTheHandle) {
  const auto handle = engine.execute("let $s := { +d.keep } return $s");
  ASSERT_EQ(handle.size(), 1u);
  // Returned handles are carried out of the released program: later queries
  // may mint cells without reusing the handle's slot for something else.
  for (int round = 0; round < 5; ++round)
    (void)engine.execute("let $x := 1 return $x");
  EXPECT_EQ(engine.shapeCount(), 1u);
  ASSERT_EQ(engine.execute("##{ $s }").front(), core.home());
  EXPECT_TRUE(linked(keepDim));
  EXPECT_FALSE(linked(dropDim));
}

} // namespace
