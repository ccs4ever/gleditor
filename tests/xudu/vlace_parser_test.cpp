/**
 * @file vlace_parser_test.cpp
 * @brief Stage 2 unit tests for the vlace parser + AST.
 */
#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "common/xanadu/vlace/parser.hpp"

namespace {

using namespace xanadu::vlace;

// Parse a single expression or fail the test.
Expr parseExpr(std::string_view src) {
  Parser p(src);
  auto r = p.parseExpressionOnly();
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
  return r ? std::move(*r) : nullptr;
}

Program parseProg(std::string_view src) {
  Parser p(src);
  auto r = p.parseProgram();
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
  return r ? std::move(*r) : Program{};
}

TEST(VlaceParserTest, IntegerLiteral) {
  auto e = parseExpr("42");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<ELiteral>(e->node));
  EXPECT_EQ(std::get<ELiteral>(e->node).lit.intValue, 42);
}

TEST(VlaceParserTest, ArithmeticPrecedence) {
  // 1 + 2 * 3  parses as  1 + (2 * 3)
  auto e = parseExpr("1 + 2 * 3");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EBinOp>(e->node));
  const auto &top = std::get<EBinOp>(e->node);
  EXPECT_EQ(top.op, "+");
  ASSERT_TRUE(std::holds_alternative<EBinOp>(top.rhs->node));
  EXPECT_EQ(std::get<EBinOp>(top.rhs->node).op, "*");
  // left is the literal 1
  ASSERT_TRUE(std::holds_alternative<ELiteral>(top.lhs->node));
}

TEST(VlaceParserTest, LeftAssocSubtraction) {
  // 10 - 2 - 3  parses as  (10 - 2) - 3
  auto e = parseExpr("10 - 2 - 3");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EBinOp>(e->node));
  const auto &top = std::get<EBinOp>(e->node);
  EXPECT_EQ(top.op, "-");
  ASSERT_TRUE(std::holds_alternative<EBinOp>(top.lhs->node)); // left-nested
  ASSERT_TRUE(std::holds_alternative<ELiteral>(top.rhs->node));
}

TEST(VlaceParserTest, RightAssocCons) {
  // 1 : 2 : [] parses as 1 : (2 : [])
  auto e = parseExpr("1 : 2 : []");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EBinOp>(e->node));
  const auto &top = std::get<EBinOp>(e->node);
  EXPECT_EQ(top.op, ":");
  ASSERT_TRUE(std::holds_alternative<EBinOp>(top.rhs->node)); // right-nested
}

TEST(VlaceParserTest, ApplicationBindsTighterThanOperators) {
  // f x + g y  parses as  (f x) + (g y)
  auto e = parseExpr("f x + g y");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EBinOp>(e->node));
  const auto &top = std::get<EBinOp>(e->node);
  EXPECT_EQ(top.op, "+");
  EXPECT_TRUE(std::holds_alternative<EApp>(top.lhs->node));
  EXPECT_TRUE(std::holds_alternative<EApp>(top.rhs->node));
}

TEST(VlaceParserTest, Application) {
  // f a b  parses as  (f a) b
  auto e = parseExpr("f a b");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EApp>(e->node));
  const auto &outer = std::get<EApp>(e->node);
  EXPECT_TRUE(std::holds_alternative<EApp>(outer.fn->node)); // (f a)
}

TEST(VlaceParserTest, LetExpression) {
  auto e = parseExpr("let x = 1 in x + 2");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<ELet>(e->node));
  const auto &lt = std::get<ELet>(e->node);
  EXPECT_FALSE(lt.recursive);
  ASSERT_EQ(lt.bindings.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<PVar>(lt.bindings[0].pattern->node));
  EXPECT_EQ(std::get<PVar>(lt.bindings[0].pattern->node).name, "x");
}

TEST(VlaceParserTest, LetrecExpression) {
  auto e = parseExpr("letrec f = fn n => f n in f 0");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<ELet>(e->node));
  EXPECT_TRUE(std::get<ELet>(e->node).recursive);
}

TEST(VlaceParserTest, IfExpression) {
  auto e = parseExpr("if x then 1 else 2");
  ASSERT_TRUE(e);
  EXPECT_TRUE(std::holds_alternative<EIf>(e->node));
}

TEST(VlaceParserTest, LambdaExpression) {
  auto e = parseExpr("fn x y => x + y");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<ELambda>(e->node));
  EXPECT_EQ(std::get<ELambda>(e->node).params.size(), 2u);
}

TEST(VlaceParserTest, CaseExpression) {
  auto e = parseExpr("case xs of [] => 0 ; (h : t) => 1");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<ECase>(e->node));
  const auto &c = std::get<ECase>(e->node);
  ASSERT_EQ(c.alts.size(), 2u);
  // first alt pattern is Nil constructor
  ASSERT_TRUE(std::holds_alternative<PConstructor>(c.alts[0].pattern->node));
  EXPECT_EQ(std::get<PConstructor>(c.alts[0].pattern->node).ctor, "Nil");
  // second alt pattern is Cons (from h : t)
  ASSERT_TRUE(std::holds_alternative<PConstructor>(c.alts[1].pattern->node));
  EXPECT_EQ(std::get<PConstructor>(c.alts[1].pattern->node).ctor, "Cons");
}

TEST(VlaceParserTest, CaseGuard) {
  auto e = parseExpr("case x of n if n > 0 => 1 ; _ => 0");
  ASSERT_TRUE(e);
  const auto &c = std::get<ECase>(e->node);
  ASSERT_EQ(c.alts.size(), 2u);
  EXPECT_TRUE(c.alts[0].guard.has_value());
  EXPECT_FALSE(c.alts[1].guard.has_value());
}

TEST(VlaceParserTest, ListSugar) {
  // [1, 2, 3] => 1 : 2 : 3 : []
  auto e = parseExpr("[1, 2, 3]");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EBinOp>(e->node));
  EXPECT_EQ(std::get<EBinOp>(e->node).op, ":");
}

TEST(VlaceParserTest, Tuple) {
  auto e = parseExpr("(1, 2, 3)");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<ETuple>(e->node));
  EXPECT_EQ(std::get<ETuple>(e->node).elems.size(), 3u);
}

TEST(VlaceParserTest, GroupingVsTuple) {
  auto g = parseExpr("(1 + 2)");
  ASSERT_TRUE(g);
  EXPECT_TRUE(std::holds_alternative<EBinOp>(g->node)); // grouping, not tuple
}

TEST(VlaceParserTest, DataDeclaration) {
  auto prog = parseProg("data Tree a = Leaf a | Node (Tree a) (Tree a) ; Leaf");
  ASSERT_EQ(prog.decls.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<DataDecl>(prog.decls[0]));
  const auto &dd = std::get<DataDecl>(prog.decls[0]);
  EXPECT_EQ(dd.name, "Tree");
  EXPECT_EQ(dd.params.size(), 1u);
  ASSERT_EQ(dd.ctors.size(), 2u);
  EXPECT_EQ(dd.ctors[0].name, "Leaf");
  EXPECT_EQ(dd.ctors[1].name, "Node");
  EXPECT_EQ(dd.ctors[1].argTypes.size(), 2u);
}

TEST(VlaceParserTest, ValueDeclarationWithParams) {
  auto prog = parseProg("square x = x * x ; square 5");
  ASSERT_EQ(prog.decls.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ValueDecl>(prog.decls[0]));
  const auto &vd = std::get<ValueDecl>(prog.decls[0]);
  EXPECT_EQ(vd.name, "square");
  EXPECT_EQ(vd.params.size(), 1u);
  ASSERT_TRUE(prog.result.has_value());
}

TEST(VlaceParserTest, OperatorAsFunction) {
  auto e = parseExpr("(+)");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EVar>(e->node));
  EXPECT_EQ(std::get<EVar>(e->node).name, "+");
}

TEST(VlaceParserTest, UnexpectedTokenError) {
  Parser p("let x =");
  auto r = p.parseExpressionOnly();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, CompileError::UnexpectedToken);
}

TEST(VlaceParserTest, DollarLowestPrecedence) {
  // f $ x + 1  parses as  f (x + 1)  because $ is level 0
  auto e = parseExpr("f $ x + 1");
  ASSERT_TRUE(e);
  ASSERT_TRUE(std::holds_alternative<EBinOp>(e->node));
  const auto &top = std::get<EBinOp>(e->node);
  EXPECT_EQ(top.op, "$");
  EXPECT_TRUE(std::holds_alternative<EBinOp>(top.rhs->node)); // x + 1
}

} // namespace
