/**
 * @file vlace_io_test.cpp
 * @brief Stage 7 tests: the typed IO/effects boundary
 *        (design/vlace-functional-language.md §6.5) and the Option/Result
 *        error model (§6.6).
 *
 * IO is an ordinary ADT whose values are *descriptions* an expression may build
 * purely; performing one happens only at the boundary when the driver runs the
 * top-level action (runIO). These tests check three things the spec makes
 * normative: (1) building an IO value writes nothing and type-checks as `IO a`;
 * (2) the runner performs effects in order, threading values through `bind`;
 * (3) Option/Result are in scope and type as recoverable absence/failure.
 */
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/vlace/driver.hpp"

namespace {

using xanadu::vlace::Driver;

struct IORun {
  std::string out;
  std::string value;
  std::string type;
  bool ok{false};
  std::string err;
};

// Run an IO program with a scripted stdin, returning {stdout, final-value,
// static-type}. Type-checking stays on so these also prove the IO types.
IORun runIO(const std::string &src, const std::string &stdinText = "") {
  Driver d;
  std::istringstream in(stdinText);
  std::ostringstream out;
  auto r = d.runIO(src, in, out);
  IORun result;
  result.out = out.str();
  if (r) {
    result.ok    = true;
    result.value = r->value;
    result.type  = r->type;
  } else {
    result.err = Driver::formatError(r.error());
  }
  return result;
}

// A pure `run` (no effect performed) for the type-only checks.
std::string typeOf(const std::string &src) {
  Driver d;
  auto r = d.run(src);
  return r ? r->type : std::string("ERR:") + Driver::formatError(r.error());
}

// --- Building IO values is pure and typed ---------------------------------

TEST(VlaceIOTest, PureHasIOType) { EXPECT_EQ(typeOf("pure 5"), "IO Int"); }

TEST(VlaceIOTest, PutStrLnHasIOUnitType) {
  EXPECT_EQ(typeOf("putStrLn \"hello\""), "IO Unit");
}

TEST(VlaceIOTest, GetLineHasIOStringType) {
  EXPECT_EQ(typeOf("getLine"), "IO String");
}

TEST(VlaceIOTest, BindSequencesTypes) {
  // bind getLine putStrLn :: IO Unit  (getLine : IO String, putStrLn : String
  // -> IO Unit); the monadic plumbing unifies String with the continuation's
  // domain.
  EXPECT_EQ(typeOf("bind getLine putStrLn"), "IO Unit");
}

TEST(VlaceIOTest, BindResultTypeFollowsContinuation) {
  EXPECT_EQ(typeOf("bind (pure 3) (fn n => pure (n + 1))"), "IO Int");
}

TEST(VlaceIOTest, IllTypedBindOfNonIORejected) {
  Driver d;
  auto r = d.run("bind 5 (fn n => pure n)"); // 5 is not IO a
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().phase, "type");
}

// --- Running IO performs effects in order ---------------------------------

TEST(VlaceIOTest, PureRunReturnsWrappedValue) {
  auto r = runIO("pure 42");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.value, "42");
  EXPECT_EQ(r.out, ""); // pure performs no effect
}

TEST(VlaceIOTest, PutStrWritesWithoutNewline) {
  auto r = runIO("putStr \"ab\"");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, "ab");
}

TEST(VlaceIOTest, PutStrLnWritesWithNewline) {
  auto r = runIO("putStrLn \"line\"");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, "line\n");
}

TEST(VlaceIOTest, ThenSequencesTwoEffectsLeftToRight) {
  // Infix >> is `then`: perform both, keep the second's value.
  auto r = runIO("putStr \"a\" >> putStr \"b\"");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, "ab");
}

TEST(VlaceIOTest, InfixBindOperatorThreadsValue) {
  // Infix >>= is `bind`.
  auto r = runIO("getLine >>= putStrLn", "echo\n");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, "echo\n");
}

TEST(VlaceIOTest, InfixOperatorsType) {
  EXPECT_EQ(typeOf("getLine >>= putStrLn"), "IO Unit");
  EXPECT_EQ(typeOf("putStr \"a\" >> getLine"), "IO String");
}

TEST(VlaceIOTest, BindThreadsValueIntoContinuation) {
  // echo: read a line, print it back.
  auto r = runIO("bind getLine putStrLn", "world\n");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, "world\n");
}

TEST(VlaceIOTest, BindComputesOnTheReadValue) {
  auto r = runIO("bind (pure 10) (fn n => pure (n * n))");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.value, "100");
  EXPECT_EQ(r.out, "");
}

TEST(VlaceIOTest, NestedBindSequencesThreeEffects) {
  // Read two lines, print them joined by a dash, using infix >>= and >>.
  const char *src = "getLine >>= (fn a => getLine >>= (fn b => "
                    "putStr a >> putStr \"-\" >> putStr b))";
  auto r          = runIO(src, "foo\nbar\n");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.out, "foo-bar");
}

// --- Monad law witnesses (left identity, right identity) ------------------

TEST(VlaceIOTest, LeftIdentityObserved) {
  // bind (pure x) f == f x : both evaluate to the same wrapped value.
  auto law1 = runIO("bind (pure 7) (fn n => pure (n + 1))");
  auto law2 = runIO("(fn n => pure (n + 1)) 7");
  ASSERT_TRUE(law1.ok) << law1.err;
  ASSERT_TRUE(law2.ok) << law2.err;
  EXPECT_EQ(law1.value, law2.value);
}

TEST(VlaceIOTest, RightIdentityObserved) {
  // bind m pure == m : running `bind (pure 9) pure` yields 9.
  auto r = runIO("bind (pure 9) pure");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.value, "9");
}

// --- Option / Result error model (§6.6) -----------------------------------

TEST(VlaceIOTest, SomeTypesAsOption) {
  EXPECT_EQ(typeOf("Some 5"), "Option Int");
}

TEST(VlaceIOTest, NoneTypesAsPolymorphicOption) {
  EXPECT_EQ(typeOf("None"), "Option a");
}

TEST(VlaceIOTest, OkTypesAsResult) {
  EXPECT_EQ(typeOf("Ok 3"), "Result a Int");
}

TEST(VlaceIOTest, OptionCaseEvaluates) {
  auto r = runIO("pure (case Some 10 of None => 0 ; Some n => n)");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.value, "10");
}

TEST(VlaceIOTest, ResultCaseEvaluates) {
  auto r = runIO("pure (case Ok 3 of Err e => 0 ; Ok n => n + 100)");
  ASSERT_TRUE(r.ok) << r.err;
  EXPECT_EQ(r.value, "103");
}

TEST(VlaceIOTest, NonIOProgramRejectedByRunner) {
  // Running a non-IO value as the entry point is an error: the program must
  // describe an effect. Typing is off so the runner's own guard is exercised.
  Driver d;
  d.withTypeChecking(false);
  std::istringstream in;
  std::ostringstream out;
  auto r = d.runIO("1 + 2", in, out);
  ASSERT_FALSE(r);
}

} // namespace
