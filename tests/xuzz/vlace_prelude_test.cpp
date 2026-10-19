/**
 * @file vlace_prelude_test.cpp
 * @brief Stage 6 prelude tests: the std:fun prelude (prelude.hpp), prepended
 *        to a program as vlace source and evaluated (§7.4). Type-checking is
 *        off (the Compiler path only evaluates); full prelude typing is a
 *        later increment.
 */
#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/prelude.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

// Prepend the prelude to `src`, parse, evaluate (no type check), deeply render.
std::string runWithPrelude(std::string_view src) {
  const std::string full = std::string(kPrelude) + "\n" + std::string(src);
  Parser parser(full);
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

TEST(VlacePreludeTest, PreludeParses) {
  // A trivial program proves the whole prelude block parses and binds.
  EXPECT_EQ(runWithPrelude("id 42"), "42");
}

TEST(VlacePreludeTest, Combinators) {
  EXPECT_EQ(runWithPrelude("const 1 2"), "1");
  EXPECT_EQ(runWithPrelude("flip const 1 2"), "2");
  EXPECT_EQ(runWithPrelude("compose (fn x => x + 1) (fn x => x * 2) 5"), "11");
}

TEST(VlacePreludeTest, Booleans) {
  EXPECT_EQ(runWithPrelude("not True"), "False");
  EXPECT_EQ(runWithPrelude("and_ True False"), "False");
  EXPECT_EQ(runWithPrelude("or_ False True"), "True");
}

TEST(VlacePreludeTest, Tuples) {
  EXPECT_EQ(runWithPrelude("fst (1, 2)"), "1");
  EXPECT_EQ(runWithPrelude("snd (1, 2)"), "2");
}

TEST(VlacePreludeTest, Map) {
  EXPECT_EQ(runWithPrelude("map (fn x => x * x) [1, 2, 3, 4]"),
            "[1, 4, 9, 16]");
}

TEST(VlacePreludeTest, Filter) {
  EXPECT_EQ(runWithPrelude("filter (fn x => x > 2) [1, 2, 3, 4]"), "[3, 4]");
}

TEST(VlacePreludeTest, Foldr) {
  EXPECT_EQ(runWithPrelude("foldr (fn x y => x + y) 0 [1, 2, 3, 4]"), "10");
}

TEST(VlacePreludeTest, Foldl) {
  EXPECT_EQ(runWithPrelude("foldl (fn acc x => acc - x) 0 [1, 2, 3]"), "-6");
}

TEST(VlacePreludeTest, Length) {
  EXPECT_EQ(runWithPrelude("length [10, 20, 30, 40, 50]"), "5");
  EXPECT_EQ(runWithPrelude("length []"), "0");
}

TEST(VlacePreludeTest, Append) {
  EXPECT_EQ(runWithPrelude("append [1, 2] [3, 4]"), "[1, 2, 3, 4]");
}

TEST(VlacePreludeTest, Reverse) {
  EXPECT_EQ(runWithPrelude("reverse [1, 2, 3]"), "[3, 2, 1]");
}

TEST(VlacePreludeTest, TakeDrop) {
  EXPECT_EQ(runWithPrelude("take 3 [1, 2, 3, 4, 5]"), "[1, 2, 3]");
  EXPECT_EQ(runWithPrelude("drop 2 [1, 2, 3, 4, 5]"), "[3, 4, 5]");
}

TEST(VlacePreludeTest, TakeFromInfiniteRepeat) {
  // Laziness: repeat builds an infinite list; take forces only five.
  EXPECT_EQ(runWithPrelude("take 5 (repeat 7)"), "[7, 7, 7, 7, 7]");
}

TEST(VlacePreludeTest, TakeFromIterate) {
  // iterate (+1) 0 = [0, 1, 2, ...]; take 4 is [0, 1, 2, 3].
  EXPECT_EQ(runWithPrelude("take 4 (iterate (fn x => x + 1) 0)"),
            "[0, 1, 2, 3]");
}

TEST(VlacePreludeTest, Composed) {
  // length (filter even (map double [1..])) style composition over a finite
  // list: map then filter then length.
  EXPECT_EQ(
      runWithPrelude("length (filter (fn x => x > 4) (map (fn x => x * 2) "
                     "[1, 2, 3, 4]))"),
      "2");
}

} // namespace
