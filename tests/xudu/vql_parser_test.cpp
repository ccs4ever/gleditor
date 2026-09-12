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

  // ##/d.stores>[d.name == "core"]>
  Parser p2("##/d.stores>[d.name == \"core\"]>");
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
  // [d.age > 30 and d.status == "active"]
  Parser parser("##/d.people[d.age > 30 and d.status == \"active\"]");
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

  // Factor 1: d.status == "active"
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

} // namespace
