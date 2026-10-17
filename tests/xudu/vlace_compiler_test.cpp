/**
 * @file vlace_compiler_test.cpp
 * @brief Stage 3 end-to-end evaluation tests for vlace: parse -> evaluate over
 *        an ArenaManifold -> rendered value.
 */
#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

// Parse + evaluate a whole program, returning the rendered result or an error
// string prefixed with "ERR:".
std::string run(std::string_view src) {
  Parser parser(src);
  auto prog = parser.parseProgram();
  if (!prog) {
    return "ERR:parse:" + prog.error().detail;
  }
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Compiler compiler(core, vm);
  auto v = compiler.evaluateProgram(*prog);
  if (!v) {
    return "ERR:" + std::string(compileErrorName(v.error().code)) + ":" +
           v.error().detail;
  }
  return Compiler::show(*v);
}

TEST(VlaceCompilerTest, IntegerLiteral) { EXPECT_EQ(run("42"), "42"); }

TEST(VlaceCompilerTest, Addition) { EXPECT_EQ(run("1 + 2"), "3"); }

TEST(VlaceCompilerTest, ArithmeticPrecedence) {
  EXPECT_EQ(run("1 + 2 * 3"), "7");
  EXPECT_EQ(run("2 * 3 + 1"), "7");
}

TEST(VlaceCompilerTest, LeftAssocSubtraction) {
  EXPECT_EQ(run("10 - 2 - 3"), "5");
}

TEST(VlaceCompilerTest, Division) { EXPECT_EQ(run("20 / 4"), "5"); }

TEST(VlaceCompilerTest, FloatArithmetic) { EXPECT_EQ(run("1.5 + 2.5"), "4"); }

TEST(VlaceCompilerTest, Comparison) {
  EXPECT_EQ(run("3 < 5"), "True");
  EXPECT_EQ(run("5 < 3"), "False");
  EXPECT_EQ(run("4 == 4"), "True");
}

TEST(VlaceCompilerTest, BooleanShortCircuit) {
  EXPECT_EQ(run("True && False"), "False");
  EXPECT_EQ(run("True || False"), "True");
}

TEST(VlaceCompilerTest, LetBinding) {
  EXPECT_EQ(run("let x = 10 in x + 5"), "15");
}

TEST(VlaceCompilerTest, NestedLet) {
  EXPECT_EQ(run("let x = 2 in let y = 3 in x * y"), "6");
}

TEST(VlaceCompilerTest, IfTrue) {
  EXPECT_EQ(run("if 3 < 5 then 100 else 200"), "100");
}

TEST(VlaceCompilerTest, IfFalse) {
  EXPECT_EQ(run("if 5 < 3 then 100 else 200"), "200");
}

TEST(VlaceCompilerTest, Lambda) {
  EXPECT_EQ(run("let sq = fn x => x * x in sq 7"), "49");
}

TEST(VlaceCompilerTest, LambdaTwoArgs) {
  EXPECT_EQ(run("let add = fn x y => x + y in add 3 4"), "7");
}

TEST(VlaceCompilerTest, PartialApplication) {
  EXPECT_EQ(run("let add = fn x y => x + y in let inc = add 1 in inc 41"),
            "42");
}

TEST(VlaceCompilerTest, TopLevelValueDecl) {
  EXPECT_EQ(run("square x = x * x ; square 9"), "81");
}

TEST(VlaceCompilerTest, DollarApplication) {
  EXPECT_EQ(run("let f = fn x => x + 1 in f $ 2 * 3"), "7");
}

TEST(VlaceCompilerTest, RecursiveFactorialTopLevel) {
  // Recursion through a top-level decl whose closure captures globals.
  EXPECT_EQ(run("fact n = if n == 0 then 1 else n * fact (n - 1) ; fact 5"),
            "120");
}

TEST(VlaceCompilerTest, UnboundIdentifierError) {
  EXPECT_EQ(run("x + 1"), "ERR:UnboundIdentifier:unbound identifier 'x'");
}

TEST(VlaceCompilerTest, StringConcat) {
  EXPECT_EQ(run("\"foo\" ++ \"bar\""), "foobar");
}

} // namespace
