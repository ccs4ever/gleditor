/**
 * @file vql_parser_test.cpp
 * @brief Unit tests for VQL Recursive Descent Parser.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/lexer.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace {

using namespace xanadu::vql;

TEST(VQLParserTest, BarePathExpressions) {
  Parser parser("##/d.people/d.name");
  auto query = parser.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(query.expr));

  const auto &path = std::get<PathExpression>(query.expr);
  EXPECT_EQ(path.anchor.kind, AnchorKind::Home);
  EXPECT_FALSE(path.anchor.derefMaster);
  ASSERT_EQ(path.steps.size(), 2u);

  ASSERT_TRUE(
      std::holds_alternative<SignedDimensionStep>(path.steps[0].selector));
  const auto &step0 = std::get<SignedDimensionStep>(path.steps[0].selector);
  EXPECT_EQ(step0.dimName, "d.people");
  EXPECT_EQ(step0.direction, zigzag::DimVector::POS);
  EXPECT_EQ(step0.placement, Placement::Default);
  EXPECT_FALSE(path.steps[0].derefMaster);

  ASSERT_TRUE(
      std::holds_alternative<SignedDimensionStep>(path.steps[1].selector));
  const auto &step1 = std::get<SignedDimensionStep>(path.steps[1].selector);
  EXPECT_EQ(step1.dimName, "d.name");
}

TEST(VQLParserTest, NamedStoreAndDerefMaster) {
  // ##math/d.stdlib>
  Parser p1("##math/d.stdlib>");
  auto q1 = p1.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q1.expr));
  const auto &path1 = std::get<PathExpression>(q1.expr);
  EXPECT_EQ(path1.anchor.kind, AnchorKind::NamedStore);
  EXPECT_EQ(path1.anchor.name, "math");
  ASSERT_EQ(path1.steps.size(), 1u);
  EXPECT_TRUE(path1.steps[0].derefMaster);

  // ##/d.stores>[d.name = "core"]>
  Parser p2("##/d.stores>[d.name = \"core\"]>");
  auto q2 = p2.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q2.expr));
  const auto &path2 = std::get<PathExpression>(q2.expr);
  EXPECT_EQ(path2.anchor.kind, AnchorKind::Home);
  ASSERT_EQ(path2.steps.size(), 1u);
  EXPECT_TRUE(path2.steps[0].derefMaster);
  ASSERT_EQ(path2.steps[0].predicates.size(), 1u);

  // Anchor with suffix >: $target>
  Parser p3("$target>");
  auto q3 = p3.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q3.expr));
  const auto &path3 = std::get<PathExpression>(q3.expr);
  EXPECT_EQ(path3.anchor.kind, AnchorKind::Variable);
  EXPECT_EQ(path3.anchor.name, "target");
  EXPECT_TRUE(path3.anchor.derefMaster);
}

TEST(VQLParserTest, PlacementsAndYields) {
  // Placements: ::from, ::rank, ::head, ::tail
  Parser p1("##/d.child::from/-d.parent::rank/d.root::head/d.leaf::tail");
  auto q1           = p1.parseQuery();
  const auto &path1 = std::get<PathExpression>(q1.expr);
  ASSERT_EQ(path1.steps.size(), 4u);

  EXPECT_EQ(std::get<SignedDimensionStep>(path1.steps[0].selector).placement,
            Placement::From);
  EXPECT_EQ(std::get<SignedDimensionStep>(path1.steps[1].selector).placement,
            Placement::Rank);
  EXPECT_EQ(std::get<SignedDimensionStep>(path1.steps[1].selector).direction,
            zigzag::DimVector::NEG);
  EXPECT_EQ(std::get<SignedDimensionStep>(path1.steps[2].selector).placement,
            Placement::Head);
  EXPECT_EQ(std::get<SignedDimensionStep>(path1.steps[3].selector).placement,
            Placement::Tail);

  // Yield modes: !new, !last, !both, !keep, !
  Parser p2("##/d.a!new/d.b!last/d.c!both/d.d!keep/d.e!");
  auto q2           = p2.parseQuery();
  const auto &path2 = std::get<PathExpression>(q2.expr);
  ASSERT_EQ(path2.steps.size(), 5u);

  EXPECT_EQ(path2.steps[0].yieldMode, YieldMode::New);
  EXPECT_EQ(path2.steps[1].yieldMode, YieldMode::Last);
  EXPECT_EQ(path2.steps[2].yieldMode, YieldMode::Both);
  EXPECT_EQ(path2.steps[3].yieldMode, YieldMode::Keep);
  EXPECT_EQ(path2.steps[4].yieldMode, YieldMode::Keep);
}

TEST(VQLParserTest, CreationSugarAndRejections) {
  // Empty create: /d.items%
  Parser p1("##/d.items%");
  auto q1           = p1.parseQuery();
  const auto &path1 = std::get<PathExpression>(q1.expr);
  const auto &s1    = std::get<SignedDimensionStep>(path1.steps[0].selector);
  ASSERT_EQ(s1.creates.size(), 1u);
  EXPECT_EQ(s1.creates[0].kind, CreateValue::Kind::Empty);

  // Literal create: /d.items%"active"
  Parser p2("##/d.items%\"active\"");
  auto q2           = p2.parseQuery();
  const auto &path2 = std::get<PathExpression>(q2.expr);
  const auto &s2    = std::get<SignedDimensionStep>(path2.steps[0].selector);
  ASSERT_EQ(s2.creates.size(), 1u);
  EXPECT_EQ(s2.creates[0].kind, CreateValue::Kind::Literal);
  EXPECT_EQ(s2.creates[0].literal, "active");

  // Clone generator create: /d.items%%
  Parser p3("##/d.items%%");
  auto q3           = p3.parseQuery();
  const auto &path3 = std::get<PathExpression>(q3.expr);
  const auto &s3    = std::get<SignedDimensionStep>(path3.steps[0].selector);
  ASSERT_EQ(s3.creates.size(), 2u);
  EXPECT_EQ(s3.creates[0].kind, CreateValue::Kind::Empty);
  EXPECT_EQ(s3.creates[1].kind, CreateValue::Kind::Empty);

  // Rejection of ::from%
  EXPECT_THROW(
      {
        Parser pBad1("##/d.items::from%");
        pBad1.parseQuery();
      },
      ParseError);

  // Rejection of ::rank%
  EXPECT_THROW(
      {
        Parser pBad2("##/d.items::rank%");
        pBad2.parseQuery();
      },
      ParseError);
}

TEST(VQLParserTest, CloneTailJoins) {
  Parser parser("##/d.a >< ##/d.b >< %\"master\"");
  auto query       = parser.parseQuery();
  const auto &path = std::get<PathExpression>(query.expr);
  EXPECT_EQ(path.anchor.kind, AnchorKind::Home);
  ASSERT_TRUE(path.cloneTail.has_value());
  ASSERT_EQ(path.cloneTail->operands.size(), 2u);

  // Operand 1 is ##/d.b
  ASSERT_NE(path.cloneTail->operands[0].path, nullptr);
  EXPECT_EQ(path.cloneTail->operands[0].path->anchor.kind, AnchorKind::Home);

  // Operand 2 is %"master"
  ASSERT_TRUE(path.cloneTail->operands[1].bareCreate.has_value());
  EXPECT_EQ(path.cloneTail->operands[1].bareCreate->kind,
            CreateValue::Kind::Literal);
  EXPECT_EQ(path.cloneTail->operands[1].bareCreate->literal, "master");
}

TEST(VQLParserTest, RangeClampsAndSlicingSugar) {
  // Range clamp [1]
  Parser p1("##/d.items[1]");
  auto q1           = p1.parseQuery();
  const auto &path1 = std::get<PathExpression>(q1.expr);
  ASSERT_TRUE(path1.steps[0].rangeClamp.has_value());
  EXPECT_EQ(path1.steps[0].rangeClamp->start, 1);
  EXPECT_EQ(path1.steps[0].rangeClamp->end, 1);

  // Range clamp [-1]
  Parser p2("##/d.items[-1]");
  auto q2           = p2.parseQuery();
  const auto &path2 = std::get<PathExpression>(q2.expr);
  ASSERT_TRUE(path2.steps[0].rangeClamp.has_value());
  EXPECT_EQ(path2.steps[0].rangeClamp->start, -1);
  EXPECT_EQ(path2.steps[0].rangeClamp->end, -1);

  // Range clamp [2, 5]
  Parser p3("##/d.items[2, 5]");
  auto q3           = p3.parseQuery();
  const auto &path3 = std::get<PathExpression>(q3.expr);
  ASSERT_TRUE(path3.steps[0].rangeClamp.has_value());
  EXPECT_EQ(path3.steps[0].rangeClamp->start, 2);
  EXPECT_EQ(path3.steps[0].rangeClamp->end, 5);

  // Slicing sugar: $header.[9, 3] desugars to FunctionInvocation value(., 9, 3)
  Parser p4("$header.[9, 3]");
  auto q4           = p4.parseQuery();
  const auto &path4 = std::get<PathExpression>(q4.expr);
  ASSERT_EQ(path4.steps.size(), 1u);
  ASSERT_TRUE(
      std::holds_alternative<FunctionInvocation>(path4.steps[0].selector));
  const auto &fn = std::get<FunctionInvocation>(path4.steps[0].selector);
  EXPECT_EQ(fn.name, "value");
  ASSERT_EQ(fn.args.size(), 3u);
  // Arg 0 is context "."
  ASSERT_TRUE(
      std::holds_alternative<std::shared_ptr<PathExpression>>(fn.args[0].kind));
  auto dotPath = std::get<std::shared_ptr<PathExpression>>(fn.args[0].kind);
  EXPECT_EQ(dotPath->anchor.kind, AnchorKind::Context);
  // Arg 1 is 9
  ASSERT_TRUE(std::holds_alternative<ScalarLiteral>(fn.args[1].kind));
  EXPECT_EQ(
      std::get<std::int64_t>(std::get<ScalarLiteral>(fn.args[1].kind).value),
      9);
  // Arg 2 is 3
  ASSERT_TRUE(std::holds_alternative<ScalarLiteral>(fn.args[2].kind));
  EXPECT_EQ(
      std::get<std::int64_t>(std::get<ScalarLiteral>(fn.args[2].kind).value),
      3);
}

TEST(VQLParserTest, PredicatesAndBooleanLogic) {
  // [d.age > 30 and d.status = "active"]
  Parser parser("##/d.people[d.age > 30 and d.status = \"active\"]");
  auto query       = parser.parseQuery();
  const auto &path = std::get<PathExpression>(query.expr);
  ASSERT_EQ(path.steps[0].predicates.size(), 1u);
  const auto &pred = path.steps[0].predicates[0];
  ASSERT_EQ(pred.terms.size(), 1u);
  ASSERT_EQ(pred.terms[0].factors.size(), 2u);

  // Factor 0: d.age > 30
  EXPECT_FALSE(pred.terms[0].factors[0].negated);
  ASSERT_TRUE(
      std::holds_alternative<ComparisonExpr>(pred.terms[0].factors[0].test));
  const auto &cmp0 = std::get<ComparisonExpr>(pred.terms[0].factors[0].test);
  EXPECT_EQ(cmp0.op, CompOp::GreaterThan);

  // Factor 1: d.status = "active"
  EXPECT_FALSE(pred.terms[0].factors[1].negated);
  ASSERT_TRUE(
      std::holds_alternative<ComparisonExpr>(pred.terms[0].factors[1].test));
  const auto &cmp1 = std::get<ComparisonExpr>(pred.terms[0].factors[1].test);
  EXPECT_EQ(cmp1.op, CompOp::Equal);

  // Negation: [not d.deleted or d.forced]
  Parser p2("##/d.items[not d.deleted or d.forced]");
  auto q2           = p2.parseQuery();
  const auto &path2 = std::get<PathExpression>(q2.expr);
  const auto &pred2 = path2.steps[0].predicates[0];
  ASSERT_EQ(pred2.terms.size(), 2u);
  EXPECT_TRUE(pred2.terms[0].factors[0].negated);
  EXPECT_FALSE(pred2.terms[1].factors[0].negated);
}

TEST(VQLParserTest, FLWORAndExecutionBlocks) {
  // for $x in ##/d.people let $name := $x/d.name where $x/d.age > 18 return
  // $name
  Parser parser(
      "for $x in ##/d.people let $name := $x/d.name where $x/d.age > 18 return "
      "$name");
  auto query = parser.parseQuery();
  ASSERT_TRUE(std::holds_alternative<ExecutionBlock>(query.expr));

  const auto &block = std::get<ExecutionBlock>(query.expr);
  ASSERT_EQ(block.bindings.size(), 2u);

  // Binding 0: for $x in ##/d.people
  ASSERT_TRUE(std::holds_alternative<ForClause>(block.bindings[0]));
  const auto &forClause = std::get<ForClause>(block.bindings[0]);
  EXPECT_EQ(forClause.varName, "x");
  EXPECT_EQ(forClause.inPath.anchor.kind, AnchorKind::Home);

  // Binding 1: let $name := $x/d.name
  ASSERT_TRUE(std::holds_alternative<LetClause>(block.bindings[1]));
  const auto &letClause = std::get<LetClause>(block.bindings[1]);
  EXPECT_EQ(letClause.varName, "name");

  // Where: $x/d.age > 18
  ASSERT_TRUE(block.where.has_value());
  ASSERT_EQ(block.where->condition.terms.size(), 1u);

  // Return: $name
  ASSERT_TRUE(std::holds_alternative<ReturnClause>(block.action.clause));
  const auto &ret = std::get<ReturnClause>(block.action.clause);
  ASSERT_EQ(ret.items.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<PathExpression>(ret.items[0].item));
  const auto &retPath = std::get<PathExpression>(ret.items[0].item);
  EXPECT_EQ(retPath.anchor.kind, AnchorKind::Variable);
  EXPECT_EQ(retPath.anchor.name, "name");
}

TEST(VQLParserTest, WeaveBlock) {
  // weave { /d.target/d.child% }
  Parser parser("weave { /d.target/d.child% }");
  auto query = parser.parseQuery();
  ASSERT_TRUE(std::holds_alternative<ExecutionBlock>(query.expr));
  const auto &block = std::get<ExecutionBlock>(query.expr);
  ASSERT_TRUE(std::holds_alternative<EffectClause>(block.action.clause));
  const auto &effect = std::get<EffectClause>(block.action.clause);
  ASSERT_EQ(effect.items.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<PathExpression>(effect.items[0].item));
}

TEST(VQLParserTest, ExtendedShapesAndProjections) {
  // Shape projection on path: ##/d.people{ * }
  Parser p1("##/d.people{ * }");
  auto q1 = p1.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q1.expr));
  const auto &path1 = std::get<PathExpression>(q1.expr);
  ASSERT_TRUE(path1.shapeProjection.has_value());
  ASSERT_EQ(path1.shapeProjection->rules.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<ShapeWildcard>(
      path1.shapeProjection->rules[0].rule));
  EXPECT_EQ(std::get<ShapeWildcard>(path1.shapeProjection->rules[0].rule),
            ShapeWildcard::Everything);

  // Shape predicate on step: ##/d.item[{ +d.tag: "important", !d.archived }]
  Parser p2("##/d.item[{ +d.tag: \"important\", !d.archived }]");
  auto q2 = p2.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q2.expr));
  const auto &path2 = std::get<PathExpression>(q2.expr);
  ASSERT_EQ(path2.steps.size(), 1u);
  ASSERT_EQ(path2.steps[0].shapePredicates.size(), 1u);
  const auto &shape = path2.steps[0].shapePredicates[0];
  ASSERT_EQ(shape.rules.size(), 2u);

  // Rule 0: +d.tag: "important"
  ASSERT_TRUE(std::holds_alternative<DimensionShapeRule>(shape.rules[0].rule));
  const auto &r0 = std::get<DimensionShapeRule>(shape.rules[0].rule);
  EXPECT_FALSE(r0.negated);
  EXPECT_EQ(r0.dimName, "d.tag");
  EXPECT_EQ(r0.direction, zigzag::DimVector::POS);
  ASSERT_NE(r0.targetValue, nullptr);

  // Rule 1: !d.archived
  ASSERT_TRUE(std::holds_alternative<DimensionShapeRule>(shape.rules[1].rule));
  const auto &r1 = std::get<DimensionShapeRule>(shape.rules[1].rule);
  EXPECT_TRUE(r1.negated);
  EXPECT_EQ(r1.dimName, "d.archived");

  // Nested shape: { $shape, !d.tag }
  Parser p3("{ $shape, !d.tag }");
  auto q3 = p3.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q3.expr));
  const auto &path3 = std::get<PathExpression>(q3.expr);
  ASSERT_TRUE(path3.shapeProjection.has_value());
  ASSERT_EQ(path3.shapeProjection->rules.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<std::string>(
      path3.shapeProjection->rules[0].rule));
  EXPECT_EQ(std::get<std::string>(path3.shapeProjection->rules[0].rule),
            "shape");
}

TEST(VQLParserTest, StoreGeneratorsAndSelectors) {
  // current()/slices(##/d.tag = "foo")
  Parser p1("current()/slices(##/d.tag = \"foo\")");
  auto q1 = p1.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q1.expr));
  const auto &path1 = std::get<PathExpression>(q1.expr);
  EXPECT_EQ(path1.anchor.kind, AnchorKind::StoreGenerator);
  ASSERT_TRUE(path1.anchor.generator.has_value());
  EXPECT_EQ(path1.anchor.generator->opName, "current");
  ASSERT_EQ(path1.steps.size(), 1u);
  ASSERT_TRUE(
      std::holds_alternative<SliceSelectorStep>(path1.steps[0].selector));
  const auto &sl = std::get<SliceSelectorStep>(path1.steps[0].selector);
  EXPECT_NE(sl.predicateFilter, nullptr);

  // topic("foo")[1]/slices("name")/##
  Parser p2("topic(\"foo\")[1]/slices(\"name\")/##");
  auto q2 = p2.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q2.expr));
  const auto &path2 = std::get<PathExpression>(q2.expr);
  EXPECT_EQ(path2.anchor.kind, AnchorKind::StoreGenerator);
  ASSERT_TRUE(path2.anchor.generator.has_value());
  EXPECT_EQ(path2.anchor.generator->opName, "topic");
  ASSERT_EQ(path2.steps.size(), 2u);
  ASSERT_TRUE(
      std::holds_alternative<SliceSelectorStep>(path2.steps[0].selector));
  const auto &sl2 = std::get<SliceSelectorStep>(path2.steps[0].selector);
  ASSERT_TRUE(sl2.nameFilter.has_value());
  ASSERT_TRUE(
      std::holds_alternative<RootSelectorStep>(path2.steps[1].selector));
  EXPECT_EQ(std::get<RootSelectorStep>(path2.steps[1].selector).kind,
            RootSelectorStep::Kind::Genesis);

  // docs("name")
  Parser p3("docs(\"name\")");
  auto q3 = p3.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q3.expr));
  const auto &path3 = std::get<PathExpression>(q3.expr);
  EXPECT_EQ(path3.anchor.kind, AnchorKind::StoreGenerator);
  ASSERT_TRUE(path3.anchor.generator.has_value());
  EXPECT_EQ(path3.anchor.generator->opName, "docs");
  ASSERT_EQ(path3.anchor.generator->args.size(), 1u);
}

TEST(VQLParserTest, VersionPinningAndMicroversionLiterals) {
  // slice("bar")@(88, 2a9)
  Parser p1("slice(\"bar\")@(88, 2a9)");
  auto q1 = p1.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q1.expr));
  const auto &path1 = std::get<PathExpression>(q1.expr);
  EXPECT_EQ(path1.anchor.kind, AnchorKind::StoreGenerator);
  ASSERT_TRUE(path1.anchor.versionPin.has_value());
  EXPECT_EQ(path1.anchor.versionPin->versions.size(), 2u);
  EXPECT_EQ(path1.anchor.versionPin->versions[0].versionStr, "88");
  EXPECT_EQ(path1.anchor.versionPin->versions[1].versionStr, "2a9");

  // ##@(2a5, 8c5)
  Parser p2("##@(2a5, 8c5)");
  auto q2 = p2.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q2.expr));
  const auto &path2 = std::get<PathExpression>(q2.expr);
  EXPECT_EQ(path2.anchor.kind, AnchorKind::Home);
  ASSERT_TRUE(path2.anchor.versionPin.has_value());
  EXPECT_EQ(path2.anchor.versionPin->versions.size(), 2u);
  EXPECT_EQ(path2.anchor.versionPin->versions[0].versionStr, "2a5");
  EXPECT_EQ(path2.anchor.versionPin->versions[1].versionStr, "8c5");

  // Step version pin: ##/d.doc@(88 .. 2a9)
  Parser p3("##/d.doc@(88 .. 2a9)");
  auto q3 = p3.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q3.expr));
  const auto &path3 = std::get<PathExpression>(q3.expr);
  ASSERT_EQ(path3.steps.size(), 1u);
  ASSERT_TRUE(path3.steps[0].versionPin.has_value());
  EXPECT_TRUE(path3.steps[0].versionPin->isRange);
  EXPECT_EQ(path3.steps[0].versionPin->versions.size(), 2u);
  EXPECT_EQ(path3.steps[0].versionPin->versions[0].versionStr, "88");
  EXPECT_EQ(path3.steps[0].versionPin->versions[1].versionStr, "2a9");
}

TEST(VQLParserTest, DestructuringInForClause) {
  // for { ., +d.tag: $t, -d.1: $prev } in ##/d.people return $t
  Parser parser("for { ., +d.tag: $t, -d.1: $prev } in ##/d.people return $t");
  auto query = parser.parseQuery();
  ASSERT_TRUE(std::holds_alternative<ExecutionBlock>(query.expr));
  const auto &block = std::get<ExecutionBlock>(query.expr);
  ASSERT_EQ(block.bindings.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ForClause>(block.bindings[0]));
  const auto &fc = std::get<ForClause>(block.bindings[0]);
  ASSERT_TRUE(fc.destructurePattern.has_value());
  const auto &pattern = *fc.destructurePattern;
  ASSERT_EQ(pattern.fields.size(), 3u);

  EXPECT_TRUE(pattern.fields[0].isContext);

  EXPECT_FALSE(pattern.fields[1].isContext);
  EXPECT_EQ(pattern.fields[1].dimName, "d.tag");
  EXPECT_EQ(pattern.fields[1].direction, zigzag::DimVector::POS);
  EXPECT_EQ(pattern.fields[1].varName, "t");

  EXPECT_FALSE(pattern.fields[2].isContext);
  EXPECT_EQ(pattern.fields[2].dimName, "d.1");
  EXPECT_EQ(pattern.fields[2].direction, zigzag::DimVector::NEG);
  EXPECT_EQ(pattern.fields[2].varName, "prev");
}

TEST(VQLParserTest, MorphismsAndVirtualDimensions) {
  // Sprout morphism: ##/d.node/=> [ "child1", "child2" ]
  Parser p1("##/d.node/=> [ \"child1\", \"child2\" ]");
  auto q1 = p1.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q1.expr));
  const auto &path1 = std::get<PathExpression>(q1.expr);
  ASSERT_EQ(path1.steps.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<PathMorphism>(path1.steps[1].selector));
  const auto &m1 = std::get<PathMorphism>(path1.steps[1].selector);
  EXPECT_EQ(m1.op, PathMorphism::Op::Arrow);
  EXPECT_TRUE(m1.isSproutList);
  EXPECT_EQ(m1.targets.size(), 2u);

  // Dimension pivot: ##/d.1 <=> d.tag
  Parser p2("##/d.1 <=> d.tag");
  auto q2 = p2.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q2.expr));
  const auto &path2 = std::get<PathExpression>(q2.expr);
  ASSERT_EQ(path2.steps.size(), 1u);
  ASSERT_TRUE(
      std::holds_alternative<DimensionPivotStep>(path2.steps[0].selector));
  const auto &piv = std::get<DimensionPivotStep>(path2.steps[0].selector);
  EXPECT_EQ(piv.fromDim, "d.1");
  EXPECT_EQ(piv.toDim, "d.tag");

  // Bidirectional step: ##/~d.tag
  Parser p3("##/~d.tag");
  auto q3 = p3.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(q3.expr));
  const auto &path3 = std::get<PathExpression>(q3.expr);
  ASSERT_EQ(path3.steps.size(), 1u);
  ASSERT_TRUE(
      std::holds_alternative<SignedDimensionStep>(path3.steps[0].selector));
  const auto &sd = std::get<SignedDimensionStep>(path3.steps[0].selector);
  EXPECT_TRUE(sd.isBidir);
  EXPECT_EQ(sd.dimName, "d.tag");
}

TEST(VQLParserTest, DefDeclarations) {
  Parser parser("def my_filter($x) := $x/d.tag; ##/d.items");
  auto query = parser.parseQuery();
  ASSERT_EQ(query.defs.size(), 1u);
  EXPECT_EQ(query.defs[0].name, "my_filter");
  ASSERT_EQ(query.defs[0].params.size(), 1u);
  EXPECT_EQ(query.defs[0].params[0], "x");
  ASSERT_TRUE(std::holds_alternative<PathExpression>(query.expr));
}

TEST(VQLParserTest, MultiStatementScriptAndMounts) {
  const std::string script = R"(
    mount ##btpk($worker_alpha_key, "tasks")#active as +d.alpha;
    mount ##btpk($worker_beta_key, "tasks")#active as +d.beta;

    for $worker in ##/+d.alpha
    where $worker/+d.status = "idle"
    let $job := ##/d.pending_jobs[1]
    weave {
      $worker/+d.ctl%"run_task"! {
        "job_id" => $job/+d.id,
        "dataset" => $job/+d.dataset_key,
        "strategy" => "map_reduce"
      },
      $job/link(+d.status, 0),
      $job/link(+d.status, "dispatched")
    }

    watch ##/+d.alpha/+d.completed as $done {
      weave ##/d.telemetry%"record"! {
        "worker" => "alpha",
        "job_id" => $done/+d.id,
        "elapsed" => $done/+d.elapsed_ms,
        "success" => $done/+d.exit_code = 0
      }
    }
  )";

  Parser parser(script);
  auto query = parser.parseQuery();

  ASSERT_EQ(query.mounts.size(), 2u);
  EXPECT_EQ(query.mounts[0].dimName, "d.alpha");
  EXPECT_EQ(query.mounts[0].direction, zigzag::DimVector::POS);
  EXPECT_EQ(query.mounts[0].storeAnchor.method, "btpk");
  EXPECT_EQ(query.mounts[0].storeAnchor.sliceName, "active");

  EXPECT_EQ(query.mounts[1].dimName, "d.beta");
  EXPECT_EQ(query.mounts[1].direction, zigzag::DimVector::POS);
  EXPECT_EQ(query.mounts[1].storeAnchor.method, "btpk");
  EXPECT_EQ(query.mounts[1].storeAnchor.sliceName, "active");

  ASSERT_EQ(query.body.statements.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<ExecutionBlock>(query.body.statements[0]));
  EXPECT_TRUE(std::holds_alternative<WatchStatement>(query.body.statements[1]));

  const auto &watchStmt = std::get<WatchStatement>(query.body.statements[1]);
  ASSERT_TRUE(watchStmt.asVar.has_value());
  EXPECT_EQ(*watchStmt.asVar, "done");
  ASSERT_NE(watchStmt.body, nullptr);
  ASSERT_EQ(watchStmt.body->statements.size(), 1u);
  EXPECT_TRUE(
      std::holds_alternative<WeaveStatement>(watchStmt.body->statements[0]));
}

TEST(VQLParserTest, WatchStatementWithSinceAndUntil) {
  Parser parser(R"(
    watch ##topic("ci-pipelines")/d.active[d.status = "failed"] as $failed_job since @88 until $cond {
      weave ##/d.alerts%"alert"! {
        "job_id" => $failed_job/+d.id
      }
    }
  )");
  auto query = parser.parseQuery();
  ASSERT_EQ(query.body.statements.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<WatchStatement>(query.body.statements[0]));
  const auto &watchStmt = std::get<WatchStatement>(query.body.statements[0]);

  ASSERT_TRUE(watchStmt.asVar.has_value());
  EXPECT_EQ(*watchStmt.asVar, "failed_job");

  ASSERT_TRUE(watchStmt.sinceVersion.has_value());
  EXPECT_EQ(watchStmt.sinceVersion->versionStr, "88");

  ASSERT_TRUE(watchStmt.untilCondition.has_value());
  ASSERT_NE(watchStmt.body, nullptr);
  ASSERT_EQ(watchStmt.body->statements.size(), 1u);
  EXPECT_TRUE(
      std::holds_alternative<WeaveStatement>(watchStmt.body->statements[0]));
}

TEST(VQLParserTest, PerspectiveBlock) {
  Parser parser(R"(
    with perspective("alice") {
      for $note in $doc/+d.comment return $note
    }
  )");
  auto query = parser.parseQuery();
  ASSERT_EQ(query.body.statements.size(), 1u);
  ASSERT_TRUE(
      std::holds_alternative<PerspectiveBlock>(query.body.statements[0]));
  const auto &pBlock = std::get<PerspectiveBlock>(query.body.statements[0]);
  EXPECT_EQ(pBlock.perspectiveName, "alice");
  ASSERT_NE(pBlock.body, nullptr);
  ASSERT_EQ(pBlock.body->statements.size(), 1u);
  EXPECT_TRUE(
      std::holds_alternative<ExecutionBlock>(pBlock.body->statements[0]));
}

TEST(VQLParserTest, PerspectiveStep) {
  Parser parser("/$cell/@alice:+d.comment");
  auto query = parser.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(query.expr));
  const auto &path = std::get<PathExpression>(query.expr);
  ASSERT_EQ(path.steps.size(), 2u);

  ASSERT_TRUE(std::holds_alternative<PerspectiveStep>(path.steps[1].selector));
  const auto &pStep = std::get<PerspectiveStep>(path.steps[1].selector);
  EXPECT_EQ(pStep.perspective, "alice");
  ASSERT_NE(pStep.targetStep, nullptr);
  ASSERT_TRUE(
      std::holds_alternative<SignedDimensionStep>(pStep.targetStep->selector));
  const auto &sd = std::get<SignedDimensionStep>(pStep.targetStep->selector);
  EXPECT_EQ(sd.dimName, "d.comment");
  EXPECT_EQ(sd.direction, zigzag::DimVector::POS);
}

TEST(VQLParserTest, RootSelectorSteps) {
  {
    Parser p("/##");
    auto q = p.parseQuery();
    ASSERT_TRUE(std::holds_alternative<PathExpression>(q.expr));
    const auto &path = std::get<PathExpression>(q.expr);
    ASSERT_EQ(path.steps.size(), 1u);
    ASSERT_TRUE(
        std::holds_alternative<RootSelectorStep>(path.steps[0].selector));
    EXPECT_EQ(std::get<RootSelectorStep>(path.steps[0].selector).kind,
              RootSelectorStep::Kind::Genesis);
  }
  {
    Parser p("/#");
    auto q = p.parseQuery();
    ASSERT_TRUE(std::holds_alternative<PathExpression>(q.expr));
    const auto &path = std::get<PathExpression>(q.expr);
    ASSERT_EQ(path.steps.size(), 1u);
    ASSERT_TRUE(
        std::holds_alternative<RootSelectorStep>(path.steps[0].selector));
    EXPECT_EQ(std::get<RootSelectorStep>(path.steps[0].selector).kind,
              RootSelectorStep::Kind::RootMetacells);
  }
  {
    Parser p("/^");
    auto q = p.parseQuery();
    ASSERT_TRUE(std::holds_alternative<PathExpression>(q.expr));
    const auto &path = std::get<PathExpression>(q.expr);
    ASSERT_EQ(path.steps.size(), 1u);
    ASSERT_TRUE(
        std::holds_alternative<RootSelectorStep>(path.steps[0].selector));
    EXPECT_EQ(std::get<RootSelectorStep>(path.steps[0].selector).kind,
              RootSelectorStep::Kind::ActiveCursor);
  }
  {
    Parser p("/^^");
    auto q = p.parseQuery();
    ASSERT_TRUE(std::holds_alternative<PathExpression>(q.expr));
    const auto &path = std::get<PathExpression>(q.expr);
    ASSERT_EQ(path.steps.size(), 1u);
    ASSERT_TRUE(
        std::holds_alternative<RootSelectorStep>(path.steps[0].selector));
    EXPECT_EQ(std::get<RootSelectorStep>(path.steps[0].selector).kind,
              RootSelectorStep::Kind::AllCursors);
  }
  {
    Parser p("/$slice/^worker");
    auto q = p.parseQuery();
    ASSERT_TRUE(std::holds_alternative<PathExpression>(q.expr));
    const auto &path = std::get<PathExpression>(q.expr);
    ASSERT_EQ(path.steps.size(), 2u);
    ASSERT_TRUE(
        std::holds_alternative<RootSelectorStep>(path.steps[1].selector));
    const auto &rs = std::get<RootSelectorStep>(path.steps[1].selector);
    EXPECT_EQ(rs.kind, RootSelectorStep::Kind::NamedCursor);
    EXPECT_EQ(rs.name, "worker");
  }
}

} // namespace
