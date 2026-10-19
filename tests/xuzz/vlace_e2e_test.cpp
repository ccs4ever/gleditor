/**
 * @file vlace_e2e_test.cpp
 * @brief Stage 4 end-to-end tests: ADTs, pattern matching, and laziness.
 */
#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

// Parse + evaluate a program and deeply render the result (forcing lazy
// values), or return "ERR:<Code>:<detail>".
std::string run(std::string_view src) {
  Parser parser(src);
  auto prog = parser.parseProgram();
  if (!prog) return "ERR:parse:" + prog.error().detail;
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Compiler compiler(core, vm);
  auto v = compiler.evaluateProgram(*prog);
  if (!v) {
    return "ERR:" + std::string(compileErrorName(v.error().code)) + ":" +
           v.error().detail;
  }
  return compiler.render(*v);
}

TEST(VlaceE2ETest, EmptyList) { EXPECT_EQ(run("[]"), "[]"); }

TEST(VlaceE2ETest, ListLiteral) { EXPECT_EQ(run("[1, 2, 3]"), "[1, 2, 3]"); }

TEST(VlaceE2ETest, ConsExpression) { EXPECT_EQ(run("1 : 2 : []"), "[1, 2]"); }

TEST(VlaceE2ETest, CaseOnListEmpty) {
  EXPECT_EQ(run("case [] of [] => 0 ; (h : t) => 1"), "0");
}

TEST(VlaceE2ETest, CaseOnListCons) {
  EXPECT_EQ(run("case [7, 8] of [] => 0 ; (h : t) => h"), "7");
}

TEST(VlaceE2ETest, CaseGuard) {
  EXPECT_EQ(run("case 5 of n if n > 3 => 100 ; _ => 0"), "100");
  EXPECT_EQ(run("case 2 of n if n > 3 => 100 ; _ => 0"), "0");
}

TEST(VlaceE2ETest, UserAdtAndCase) {
  // A user ADT with a case over its constructors.
  const char *src =
      "data Shape = Circle Int | Rect Int Int ;"
      "area s = case s of Circle r => 3 * r * r ; Rect w h => w * h ;"
      "area (Rect 4 5)";
  EXPECT_EQ(run(src), "20");
}

TEST(VlaceE2ETest, AdtConstructorValue) {
  const char *src = "data Tree a = Leaf a | Node (Tree a) (Tree a) ;"
                    "Node (Leaf 1) (Leaf 2)";
  // General constructor rendering: "Node Leaf 1 Leaf 2" (nested args are not
  // parenthesized in Stage 4's shallow renderer; the shape is unambiguous
  // enough for a round-trip check of ctor building + application).
  EXPECT_EQ(run(src), "Node Leaf 1 Leaf 2");
}

TEST(VlaceE2ETest, MapOverList) {
  // A recursive map defined in vlace source, squaring a list.
  const char *src =
      "map f xs = case xs of [] => [] ; (h : t) => f h : map f t ;"
      "map (fn x => x * x) [1, 2, 3]";
  EXPECT_EQ(run(src), "[1, 4, 9]");
}

TEST(VlaceE2ETest, LetrecLength) {
  const char *src =
      "letrec len = fn xs => case xs of [] => 0 ; (h : t) => 1 + len t "
      "in len [10, 20, 30, 40]";
  EXPECT_EQ(run(src), "4");
}

TEST(VlaceE2ETest, InfiniteStreamTakeFinite) {
  // from n = n : from (n+1) is infinite; take 5 forces only five tails (§9.2).
  const char *src = "from n = n : from (n + 1) ;"
                    "take k xs = if k == 0 then [] else "
                    "case xs of [] => [] ; (h : t) => h : take (k - 1) t ;"
                    "take 5 (from 0)";
  EXPECT_EQ(run(src), "[0, 1, 2, 3, 4]");
}

TEST(VlaceE2ETest, LazyLetDoesNotDivergeWhenUnused) {
  // A binding that would loop forever is never forced because it is unused.
  const char *src = "letrec loop = loop in 42";
  EXPECT_EQ(run(src), "42");
}

TEST(VlaceE2ETest, NonExhaustiveMatchError) {
  // No alt matches Cons -> runtime NonExhaustiveMatch.
  auto out = run("case [1] of [] => 0");
  EXPECT_EQ(out, "ERR:NonExhaustiveMatch:no case alternative matched the "
                 "scrutinee");
}

TEST(VlaceE2ETest, NonLinearPatternError) {
  auto out = run("case (1, 1) of (x, x) => x");
  EXPECT_EQ(out, "ERR:NonLinearPattern:'x' bound twice in one pattern");
}

TEST(VlaceE2ETest, TuplePatternAndRender) {
  EXPECT_EQ(run("case (1, 2) of (a, b) => a + b"), "3");
  EXPECT_EQ(run("(1, 2, 3)"), "(1, 2, 3)");
}

} // namespace
