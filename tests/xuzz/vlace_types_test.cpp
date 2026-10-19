/**
 * @file vlace_types_test.cpp
 * @brief Stage 5 tests for the vlace Hindley-Milner type checker (§5.5).
 */
#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/types.hpp"

namespace {

using namespace xanadu::vlace;

// Type-check a program; return the inferred type string, or "ERR:<Code>".
std::string ty(std::string_view src) {
  Parser parser(src);
  auto prog = parser.parseProgram();
  if (!prog) return "ERR:parse:" + prog.error().detail;
  TypeChecker tc;
  auto r = tc.checkProgram(*prog);
  if (!r) return "ERR:" + std::string(compileErrorName(r.error().code));
  return *r;
}

// Just the error code (or "OK") for rejection tests.
std::string code(std::string_view src) {
  Parser parser(src);
  auto prog = parser.parseProgram();
  if (!prog) return "parse";
  TypeChecker tc;
  auto r = tc.checkProgram(*prog);
  return r ? "OK" : std::string(compileErrorName(r.error().code));
}

// -- Well-typed inference ----------------------------------------------------

TEST(VlaceTypesTest, IntLiteral) { EXPECT_EQ(ty("42"), "Int"); }
TEST(VlaceTypesTest, FloatLiteral) { EXPECT_EQ(ty("3.14"), "Float"); }
TEST(VlaceTypesTest, BoolLiteral) { EXPECT_EQ(ty("True"), "Bool"); }
TEST(VlaceTypesTest, StringLiteral) { EXPECT_EQ(ty("\"hi\""), "String"); }

TEST(VlaceTypesTest, Arithmetic) { EXPECT_EQ(ty("1 + 2 * 3"), "Int"); }
TEST(VlaceTypesTest, FloatArithmetic) { EXPECT_EQ(ty("1.0 + 2.0"), "Float"); }

TEST(VlaceTypesTest, Comparison) { EXPECT_EQ(ty("3 < 5"), "Bool"); }

TEST(VlaceTypesTest, IdentityFunction) { EXPECT_EQ(ty("fn x => x"), "a -> a"); }

TEST(VlaceTypesTest, ConstFunction) {
  // fn x y => x  :  a -> b -> a
  EXPECT_EQ(ty("fn x y => x"), "a -> b -> a");
}

TEST(VlaceTypesTest, Application) { EXPECT_EQ(ty("(fn x => x + 1) 5"), "Int"); }

TEST(VlaceTypesTest, IfExpression) {
  EXPECT_EQ(ty("if 3 < 5 then 1 else 2"), "Int");
}

TEST(VlaceTypesTest, ListType) { EXPECT_EQ(ty("[1, 2, 3]"), "[Int]"); }

TEST(VlaceTypesTest, ConsType) { EXPECT_EQ(ty("1 : []"), "[Int]"); }

TEST(VlaceTypesTest, LetBinding) { EXPECT_EQ(ty("let x = 5 in x + 1"), "Int"); }

TEST(VlaceTypesTest, LetGeneralizationUsedAtTwoTypes) {
  // id is generalized, so it can be used at Int and at Bool in one program.
  const char *src = "let id = fn x => x in if id True then id 1 else id 2";
  EXPECT_EQ(ty(src), "Int");
}

TEST(VlaceTypesTest, RecursiveFunctionType) {
  const char *src = "fact n = if n == 0 then 1 else n * fact (n - 1) ; fact";
  EXPECT_EQ(ty(src), "Int -> Int");
}

TEST(VlaceTypesTest, UserAdtConstructorType) {
  const char *src = "data Shape = Circle Int | Rect Int Int ; Circle";
  EXPECT_EQ(ty(src), "Int -> Shape");
}

TEST(VlaceTypesTest, MapType) {
  const char *src =
      "map f xs = case xs of [] => [] ; (h : t) => f h : map f t ; map";
  // map : (a -> b) -> [a] -> [b]
  EXPECT_EQ(ty(src), "(a -> b) -> [a] -> [b]");
}

// -- Rejections --------------------------------------------------------------

TEST(VlaceTypesTest, RejectIntPlusString) {
  EXPECT_EQ(code("1 + \"x\""), "TypeMismatch");
}

TEST(VlaceTypesTest, RejectNumericTowerMixing) {
  // Int and Float are distinct; 1 + 2.0 is a type error (§4.6).
  EXPECT_EQ(code("1 + 2.0"), "TypeMismatch");
}

TEST(VlaceTypesTest, RejectIfNonBoolCondition) {
  EXPECT_EQ(code("if 1 then 2 else 3"), "TypeMismatch");
}

TEST(VlaceTypesTest, RejectIfBranchesDiffer) {
  EXPECT_EQ(code("if True then 1 else \"x\""), "TypeMismatch");
}

TEST(VlaceTypesTest, RejectOccursCheck) {
  // fn x => x x  requires a = a -> b : the occurs check must fire.
  EXPECT_EQ(code("fn x => x x"), "OccursCheckFailed");
}

TEST(VlaceTypesTest, RejectUnbound) {
  EXPECT_EQ(code("y + 1"), "UnboundIdentifier");
}

TEST(VlaceTypesTest, RejectHeterogeneousList) {
  EXPECT_EQ(code("[1, True]"), "TypeMismatch");
}

} // namespace
