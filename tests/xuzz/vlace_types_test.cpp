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

// -- Record patterns resolve fields by name -----------------------------------

namespace {
// The checker's error as "<Code>: <detail>", or "OK:<type>".
std::string verdict(std::string_view src) {
  Parser parser(src);
  auto prog = parser.parseProgram();
  if (!prog) return "parse:" + prog.error().detail;
  TypeChecker tc;
  auto r = tc.checkProgram(*prog);
  if (!r) {
    return std::string(compileErrorName(r.error().code)) + ": " +
           r.error().detail;
  }
  return "OK:" + *r;
}
constexpr const char *kQ = "data Q = Q { n :: Int, s :: String } ; ";
} // namespace

TEST(VlaceRecordPatternTypesTest, OutOfOrderFieldsAreCheckedByName) {
  // `a` is the String field however the pattern is written, so a + 1 is
  // ill-typed. The positional reading typed it as Int and it ran, printing 1.
  EXPECT_EQ(
      code(std::string(kQ) + "f (Q { s = a, n = b }) = a + 1 ; f (Q 1 \"hi\")"),
      "TypeMismatch");
  // The mirror, well-typed, was refused with "cannot unify String with Int".
  EXPECT_EQ(verdict(std::string(kQ) +
                    "f (Q { s = a, n = b }) = b + 1 ; f (Q 1 \"hi\")"),
            "OK:Int");
}

TEST(VlaceRecordPatternTypesTest, UnlistedFieldsAreWildcards) {
  EXPECT_EQ(verdict(std::string(kQ) + "f (Q { s = a }) = a ; f (Q 1 \"hi\")"),
            "OK:String");
  EXPECT_EQ(
      verdict(std::string(kQ) + "f (Q { n = b }) = b + 1 ; f (Q 1 \"hi\")"),
      "OK:Int");
  EXPECT_EQ(verdict(std::string(kQ) + "f (Q { }) = 7 ; f (Q 1 \"hi\")"),
            "OK:Int");
  // A partial pattern still types the value as the data type, not a function.
  EXPECT_EQ(code(std::string(kQ) + "f (Q { n = b }) = b + 1 ; f 5"),
            "TypeMismatch");
}

TEST(VlaceRecordPatternTypesTest, FieldPatternTypesComeFromTheDeclaration) {
  // The literal 1 cannot match the String field.
  EXPECT_EQ(code(std::string(kQ) + "f (Q { s = 1 }) = 0 ; f (Q 1 \"hi\")"),
            "TypeMismatch");
  EXPECT_EQ(code(std::string(kQ) + "f (Q { n = \"x\" }) = 0 ; f (Q 1 \"hi\")"),
            "TypeMismatch");
}

TEST(VlaceRecordPatternTypesTest, UnknownFieldIsNamed) {
  const std::string v =
      verdict(std::string(kQ) + "f (Q { z = a }) = a ; f (Q 1 \"hi\")");
  EXPECT_NE(v.find("UnboundIdentifier"), std::string::npos) << v;
  EXPECT_NE(v.find("record 'Q' has no field 'z'"), std::string::npos) << v;
}

TEST(VlaceRecordPatternTypesTest, RepeatedFieldInAPatternIsRefused) {
  const std::string v =
      verdict(std::string(kQ) + "f (Q { n = a, n = b }) = a ; f (Q 1 \"hi\")");
  EXPECT_NE(v.find("TypeMismatch"), std::string::npos) << v;
  EXPECT_NE(v.find("repeats field 'n'"), std::string::npos) << v;
}

TEST(VlaceRecordPatternTypesTest, RecordPatternOnAPlainConstructorIsRefused) {
  const std::string v =
      verdict("data P = P Int ; f (P { x = a }) = a ; f (P 1)");
  EXPECT_NE(v.find("'P' is not a record constructor"), std::string::npos) << v;
}

TEST(VlaceRecordPatternTypesTest, DuplicateFieldDeclarationIsRefused) {
  const std::string v = verdict("data S = A { v :: Int, v :: Int } ; 1");
  EXPECT_NE(v.find("TypeMismatch"), std::string::npos) << v;
  EXPECT_NE(v.find("declares field 'v' twice"), std::string::npos) << v;
}

// -- Text and booleans are not numbers ---------------------------------------
// The VM reads text as zero in arithmetic, so only the checker keeps these from
// running; each must be refused before evaluation.

TEST(VlaceTextIsNotNumericTest, ArithmeticOnNonNumbersIsRejected) {
  EXPECT_EQ(code("\"hi\" + 1"), "TypeMismatch");
  EXPECT_EQ(code("1 + \"hi\""), "TypeMismatch");
  EXPECT_EQ(code("True + 1"), "TypeMismatch");
  EXPECT_EQ(code("1 - False"), "TypeMismatch");
  EXPECT_EQ(code("\"a\" * \"b\""), "TypeMismatch");
  EXPECT_EQ(code("\"a\" < 1"), "TypeMismatch");
  EXPECT_EQ(code("(fn a => a + 1) \"hi\""), "TypeMismatch");
  EXPECT_EQ(code("f a = a + 1 ; f \"hi\""), "TypeMismatch");
  EXPECT_EQ(code("f a = a + 1 ; f True"), "TypeMismatch");
  EXPECT_EQ(code("1 / \"x\""), "TypeMismatch");
  EXPECT_EQ(code("0 - \"x\""), "TypeMismatch");
}
