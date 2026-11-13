/**
 * @file vlace_lower_kernel_test.cpp
 * @brief Card L2: `let`, lambda, application, `$`, letrec and top-level
 *        recursion, thunks and force, lowered to Vortex graphs and run on the
 *        VM -- scoping, closures, currying, laziness and sharing, the budgets,
 *        and the forms that still refuse.
 */
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/kernel.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/run_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

struct Rig {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  Compiler compiler{core, vm};

  std::expected<ValuePtr, EvalError> eval(std::string_view src) {
    Parser parser(src);
    auto prog = parser.parseProgram();
    EXPECT_TRUE(prog.has_value()) << src;
    if (!prog) return std::unexpected(EvalError{});
    return compiler.evaluateProgram(*prog);
  }
  std::string show(std::string_view src) {
    auto v = eval(src);
    return v ? Compiler::show(*v) : "ERR:" + v.error().detail;
  }
  std::optional<CompileError> failure(std::string_view src) {
    auto v = eval(src);
    if (v) return std::nullopt;
    return v.error().code;
  }
};

// A fresh Rig per program: the cycle probe below must not see state left by an
// earlier run.
std::string run(std::string_view src) { return Rig{}.show(src); }

// -- let, scoping, shadowing
// ----------------------------------------------------

TEST(VlaceLowerKernelTest, LetShadowsAndNests) {
  EXPECT_EQ(run("let x = 1 in let x = 2 in x"), "2");
  EXPECT_EQ(run("let x = 1 in let y = (let x = 5 in x) in x + y"), "6");
  EXPECT_EQ(run("let x = 1 in (fn x => x + 1) 10"), "11");
  EXPECT_EQ(run("let x = 4 in (fn y => let x = y * 2 in x + y) 3"), "9");
  EXPECT_EQ(run("let a = 1 + 1 ; b = a * 10 in b + a"), "22");
}

TEST(VlaceLowerKernelTest, LetBindingsAreSharedNotRecomputed) {
  // One thunk, three readers; the shared value is the same each time.
  EXPECT_EQ(run("let a = 2 * 3 in let b = a in b + a + a"), "18");
}

TEST(VlaceLowerKernelTest, LetOnlyBindsSimpleVariables) {
  Rig rig;
  auto v = rig.eval("let (a, b) = (1, 2) in a");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::NonExhaustiveMatch);
  EXPECT_NE(v.error().detail.find("let binds a simple variable"),
            std::string::npos);
}

// -- closures
// -------------------------------------------------------------------

TEST(VlaceLowerKernelTest, ClosuresCaptureByValueAndOutliveTheirScope) {
  EXPECT_EQ(run("let mk = fn n => fn m => n + m in "
                "let a = mk 10 in let b = mk 20 in a 1 + b 2"),
            "33");
  // `make` has returned by the time the closure runs.
  EXPECT_EQ(run("let make = fn n => (fn x => x + n) in (make 5) 1"), "6");
  // Two closures over the same name see their own capture.
  EXPECT_EQ(run("let k = fn v => fn w => v in "
                "let p = k 1 in let q = k 2 in p 0 + q 0"),
            "3");
}

TEST(VlaceLowerKernelTest,
     ACaptureTwoFramesOutIsForwardedThroughTheMiddleFrame) {
  EXPECT_EQ(run("let f = fn a => fn b => fn c => a + b + c in f 1 2 3"), "6");
  EXPECT_EQ(
      run("let a = 100 in (fn x => (fn y => (fn z => a + x + y + z) 1) 2) 3"),
      "106");
}

TEST(VlaceLowerKernelTest, ThunksInsideFramesCaptureFromEveryFrameOut) {
  EXPECT_EQ(run("(fn a => let t = a * 2 in t + a) 5"), "15");
  EXPECT_EQ(run("(fn a => let t = (let u = a + 1 in u * 2) in t + a) 5"), "17");
  // One variable used many times is captured once and read consistently.
  EXPECT_EQ(run("let x = 3 in (fn y => x + x + x * y) 2"), "12");
}

TEST(VlaceLowerKernelTest, ClosureValuesReadBackAsFunctions) {
  Rig rig;
  auto v = rig.eval("fn x => x");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)->kind, Value::Kind::Closure);
  EXPECT_EQ(Compiler::show(*v), "<function>");
  auto partial = rig.eval("let add = fn a b => a + b in add 1");
  ASSERT_TRUE(partial.has_value());
  EXPECT_EQ((*partial)->kind, Value::Kind::Closure);
}

TEST(VlaceLowerKernelTest,
     TwoThunksMemoisingOneClosureDoNotStealItFromEachOther) {
  // `g` and `f` both force to the same closure value; a memo stored as a plain
  // link would evict whichever was forced first.
  EXPECT_EQ(run("let id = fn x => x in let f = id (fn y => y + 1) in "
                "let g = id f in g 1 + f 2 + g 3"),
            "9");
}

// -- currying, partial and over-application, `$`
// ------------------------------------

TEST(VlaceLowerKernelTest, PartialAndOverApplication) {
  EXPECT_EQ(run("let add3 = fn a b c => a + b + c in "
                "let p = add3 1 in let q = p 2 in q 3"),
            "6");
  EXPECT_EQ(run("let id = fn f => f in id (fn x => x * 2) 21"), "42");
  EXPECT_EQ(run("let twice = fn f x => f (f x) in twice (fn n => n + 3) 10"),
            "16");
  EXPECT_EQ(run("let k = fn a => fn b => a in k 7 8"), "7");
}

TEST(VlaceLowerKernelTest, DollarIsApplication) {
  EXPECT_EQ(run("let g = fn x y => x - y in g 10 $ 3"), "7");
  EXPECT_EQ(run("let f = fn x => x * x in f $ 2 + 3"), "25");
  EXPECT_EQ(run("let f = fn x => x + 1 in let h = fn k => k 4 in h $ f"), "5");
}

// -- recursion
// ------------------------------------------------------------------------

TEST(VlaceLowerKernelTest, TopLevelRecursionAndForwardReferences) {
  EXPECT_EQ(run("fact n = if n == 0 then 1 else n * fact (n - 1) ; fact 10"),
            "3628800");
  // `isOdd` is declared after the function that uses it.
  EXPECT_EQ(run("isEven n = if n == 0 then True else isOdd (n - 1) ; "
                "isOdd n = if n == 0 then False else isEven (n - 1) ; "
                "isEven 10"),
            "True");
  EXPECT_EQ(run("isEven n = if n == 0 then True else isOdd (n - 1) ; "
                "isOdd n = if n == 0 then False else isEven (n - 1) ; "
                "isOdd 7"),
            "True");
  // A top-level value, used twice.
  EXPECT_EQ(run("base = 40 + 2 ; base + base"), "84");
  EXPECT_EQ(run("add a b = a + b ; inc = add 1 ; inc (inc 5)"), "7");
}

TEST(VlaceLowerKernelTest, LetrecMutualRecursionAndSelfReference) {
  EXPECT_EQ(run("letrec ev = fn n => if n == 0 then True else od (n - 1) ; "
                "od = fn n => if n == 0 then False else ev (n - 1) in ev 8"),
            "True");
  EXPECT_EQ(run("letrec sum = fn n => if n == 0 then 0 else n + sum (n - 1) "
                "in sum 20"),
            "210");
}

TEST(VlaceLowerKernelTest, AnUnusedBrokenTopLevelDeclarationStaysInert) {
  EXPECT_EQ(run("broken = nope + 1 ; fine = 5 ; fine"), "5");
  Rig rig;
  auto v = rig.eval("broken = nope + 1 ; broken");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::UnboundIdentifier);
  EXPECT_EQ(v.error().detail, "unbound identifier 'nope'");
}

TEST(VlaceLowerKernelTest, ADeclarationSurvivesToALaterEvaluation) {
  Rig rig;
  EXPECT_EQ(rig.show("double n = n * 2 ; 1"), "1");
  EXPECT_EQ(rig.show("double 21"), "42");
  EXPECT_EQ(rig.show("double 4"), "8");
}

// -- laziness
// ---------------------------------------------------------------------------

TEST(VlaceLowerKernelTest, AnUnusedBindingThatWouldDivergeOrFailNeverRuns) {
  EXPECT_EQ(run("letrec loop = loop in 42"), "42");
  EXPECT_EQ(run("let boom = 1 / 0 in 42"), "42");
  EXPECT_EQ(run("let spin = (fn x => x x) (fn x => x x) in 7"), "7");
  // An argument that would fail, never demanded by the callee.
  EXPECT_EQ(run("let ignore = fn a b => b in ignore (1 / 0) 9"), "9");
  EXPECT_EQ(run("const a b = a ; const 3 (1 / 0)"), "3");
}

TEST(VlaceLowerKernelTest, ADemandedFailureStillFails) {
  Rig rig;
  auto v = rig.eval("let boom = 1 / 0 in boom + 1");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::ArithFault);
}

TEST(VlaceLowerKernelTest, ASelfDemandingThunkIsADefinedDivergence) {
  Rig rig;
  auto v = rig.eval("letrec x = x + 1 in x");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
  EXPECT_NE(v.error().detail.find("<<loop>>"), std::string::npos);
}

namespace {

/// The smallest cycle limit under which @p src evaluates, found by doubling
/// then bisecting; nullopt if even a very large limit fails.
std::optional<std::size_t> cycleCost(std::string_view src) {
  const auto passes = [&src](std::size_t limit) {
    Rig rig;
    lower::setCycleLimit(rig.compiler, limit);
    return rig.eval(src).has_value();
  };
  std::size_t high = 64;
  while (!passes(high)) {
    high *= 2;
    if (high > (1U << 24)) return std::nullopt;
  }
  std::size_t low = high / 2;
  while (low + 1 < high) {
    const auto mid             = (low + high) / 2;
    (passes(mid) ? high : low) = mid;
  }
  return high;
}

} // namespace

TEST(VlaceLowerKernelTest, AForcedThunkIsEvaluatedOnce) {
  // `count 60` is the expensive part. Reading its thunk three times must cost
  // far less than three evaluations: if each read re-ran it, the second program
  // would cost about three times the first.
  const std::string body =
      "count n = if n == 0 then 0 else 1 + count (n - 1) ; ";
  const auto once   = cycleCost(body + "let t = count 60 in t");
  const auto thrice = cycleCost(body + "let t = count 60 in t + t + t");
  ASSERT_TRUE(once && thrice);
  EXPECT_LT(*thrice, *once + *once / 2)
      << "once=" << *once << " thrice=" << *thrice;
}

// -- budgets
// ----------------------------------------------------------------------------

TEST(VlaceLowerKernelTest, DivergentRecursionIsABudgetErrorNotACrash) {
  Rig rig;
  auto v = rig.eval("loop x = loop x ; loop 0");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
  EXPECT_NE(v.error().detail.find("budget"), std::string::npos);
  // The VM is usable afterwards.
  EXPECT_EQ(rig.show("1 + 1"), "2");
}

TEST(VlaceLowerKernelTest, RecursionDeeperThanTheFrameBudgetIsRefusedCleanly) {
  Rig rig;
  EXPECT_EQ(rig.show("count n = if n == 0 then 0 else 1 + count (n - 1) ; 0"),
            "0");
  EXPECT_EQ(rig.show("count 100"), "100");
  auto deep = rig.eval("count 100000");
  ASSERT_FALSE(deep.has_value());
  EXPECT_EQ(deep.error().code, CompileError::BudgetExhausted);
}

TEST(VlaceLowerKernelTest, ACycleLimitStopsALazyLoop) {
  Rig rig;
  lower::setCycleLimit(rig.compiler, 2000);
  auto v = rig.eval("spin n = spin (n + 1) ; spin 0");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
}

// -- the two copies
// -----------------------------------------------------------------------

TEST(VlaceLowerKernelTest, ReferenceCopyKeepsAClosureAndAScalar) {
  Rig rig;
  Parser parser("fn x => x + 1");
  auto prog = parser.parseProgram();
  ASSERT_TRUE(prog.has_value());
  lower::LowerContext ctx(rig.compiler);
  auto closure = lower::lowerExpr(ctx, **prog->result);
  ASSERT_TRUE(closure.has_value());
  const auto scalar = ctx.emitConstant(std::int64_t{7});

  // Reference copy of a closure: still applicable.
  const auto closureCopy = ctx.newRegister();
  lower::emitCopyReference(ctx, *closure, closureCopy);
  // Reference copy of a scalar: reads as the scalar.
  const auto scalarRef = ctx.newRegister();
  lower::emitCopyReference(ctx, scalar, scalarRef);
  auto applied = lower::applyOperand(ctx, closureCopy, scalarRef, {});
  ASSERT_TRUE(applied.has_value());
  auto ran = lower::runBlock(ctx, ctx.swapBlock(lower::Block{}), *applied);
  ASSERT_TRUE(ran.has_value());
  auto result = lower::readbackCell(rig.compiler, ran->cell);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ((*result)->kind, Value::Kind::Int);
  EXPECT_EQ((*result)->i, 8);
}

// -- forcing a read-back thunk
// ------------------------------------------------------------

TEST(VlaceLowerKernelTest, ForceValueRunsAnUnforcedThunkOnTheVm) {
  Rig rig;
  Parser parser("let t = 6 * 7 in t");
  auto prog = parser.parseProgram();
  ASSERT_TRUE(prog.has_value());
  lower::LowerContext ctx(rig.compiler);
  // Hand-build an unforced thunk: lowerLazy of a non-trivial expression.
  Parser inner("6 * 7");
  auto expr = inner.parseProgram();
  ASSERT_TRUE(expr.has_value());
  auto thunk = lower::lowerLazy(ctx, **expr->result);
  ASSERT_TRUE(thunk.has_value());
  auto ran = lower::runBlock(ctx, ctx.swapBlock(lower::Block{}), *thunk);
  ASSERT_TRUE(ran.has_value());
  auto read = lower::readbackCell(rig.compiler, ran->cell);
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ((*read)->kind, Value::Kind::Thunk);
  EXPECT_EQ(Compiler::show(*read), "<thunk>");
  auto forced = rig.compiler.forceValue(*read);
  ASSERT_TRUE(forced.has_value());
  EXPECT_EQ((*forced)->kind, Value::Kind::Int);
  EXPECT_EQ((*forced)->i, 42);
  // Forced once: the thunk now reads back as its value.
  auto again = lower::readbackCell(rig.compiler, ran->cell);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ((*again)->kind, Value::Kind::Int);
  EXPECT_EQ((*again)->i, 42);
}

// -- what still refuses
// -------------------------------------------------------------------

TEST(VlaceLowerKernelTest, FormsOwnedByLaterCardsRefuseByName) {
  Rig rig;
  const std::string refusal = "VM lowering is not implemented for VLace";
  // Lists, tuples, constructors, `case` and constructor-pattern parameters
  // became L3's; only the IO builder is still a later card's.
  for (const std::string_view src : {"putStrLn \"x\""}) {
    auto v = rig.eval(src);
    ASSERT_FALSE(v.has_value()) << src;
    EXPECT_NE(v.error().detail.find(refusal), std::string::npos)
        << src << " -> " << v.error().detail;
  }
}

} // namespace

namespace {

using namespace xanadu::vlace;

TEST(VlaceLowerKernelJoinTest, IfSelectsAClosureAndAppliesIt) {
  const std::string defs =
      "let f = fn x => x + 1 in let g = fn y => y * 10 in ";
  EXPECT_EQ(Rig{}.show(defs + "(if True then f else g) 4"), "5");
  EXPECT_EQ(Rig{}.show(defs + "(if False then f else g) 4"), "40");
  EXPECT_EQ(Rig{}.show("(if True then fn x => x + 1 else fn y => y) 1"), "2");
}

TEST(VlaceLowerKernelJoinTest, IfReturningAThunkIsForcedByItsUser) {
  EXPECT_EQ(
      Rig{}.show("let a = 6 * 7 in let b = 1 in (if True then a else b) + 0"),
      "42");
  EXPECT_EQ(Rig{}.show("let a = 6 * 7 in let b = 1 in if False then a else b"),
            "1");
}

TEST(VlaceLowerKernelJoinTest, ScalarArmsStillReadBackAsTheirKind) {
  Rig rig;
  const auto kind = [&rig](std::string_view src) {
    auto v = rig.eval(src);
    EXPECT_TRUE(v.has_value()) << src;
    return v ? (*v)->kind : Value::Kind::Thunk;
  };
  EXPECT_EQ(kind("if True then 1 else 2"), Value::Kind::Int);
  EXPECT_EQ(kind("if True then 1.5 else 2.5"), Value::Kind::Float);
  EXPECT_EQ(kind("if True then \"7\" else \"8\""), Value::Kind::Str);
  EXPECT_EQ(rig.show("if False then \"7\" else \"8\""), "8");
  EXPECT_EQ(kind("if 1 < 2 then True else False"), Value::Kind::Bool);
  EXPECT_EQ(kind("True && False"), Value::Kind::Bool);
  EXPECT_EQ(kind("False || True"), Value::Kind::Bool);
}

TEST(VlaceLowerKernelJoinTest, NestedIfsMixClosureAndScalarArmsAcrossLets) {
  EXPECT_EQ(
      Rig{}.show("let inc = fn x => x + 1 in let n = if True then 10 else 0 in "
                 "let h = if n == 10 then (if False then inc else fn z => z * "
                 "2) else inc in "
                 "h n"),
      "20");
  EXPECT_EQ(Rig{}.show(
                "let inc = fn x => x + 1 in let n = if False then 10 else 5 in "
                "let h = if n == 10 then inc else (if True then fn z => z - 1 "
                "else inc) in "
                "(h n) + (if n == 5 then 100 else 0)"),
            "104");
}

TEST(VlaceLowerKernelJoinTest, NonBoolOperandsOfAndOrIfAreTypeErrors) {
  const std::string_view operands[] = {"(fn x => x)", "1", "\"true\""};
  for (const auto operand : operands) {
    const std::string o(operand);
    for (const std::string &src :
         {o + " && True", "True && " + o, o + " || False", "False || " + o,
          "if " + o + " then 1 else 2"}) {
      Rig rig;
      auto v = rig.eval(src);
      ASSERT_FALSE(v.has_value()) << src;
      EXPECT_EQ(v.error().code, CompileError::TypeMismatch) << src;
      EXPECT_NE(v.error().detail.find("needs a Bool"), std::string::npos)
          << src << " -> " << v.error().detail;
    }
  }
  // A thunk operand is forced first, then judged by what it holds.
  Rig rig;
  auto v = rig.eval("let t = 1 + 1 in if t then 1 else 2");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::TypeMismatch);
}

TEST(VlaceLowerKernelJoinTest, ShortCircuitStillSkipsTheRightOperand) {
  EXPECT_EQ(Rig{}.show("False && (1 / 0 == 0)"), "False");
  EXPECT_EQ(Rig{}.show("True || (1 / 0 == 0)"), "True");
}

TEST(VlaceLowerKernelTest, AFailedForceLeavesATopLevelThunkRunnable) {
  {
    Rig rig;
    EXPECT_EQ(rig.show("x = 1 / 0 ; 0"), "0");
    for (int i = 0; i < 2; ++i) {
      auto v = rig.eval("x");
      ASSERT_FALSE(v.has_value());
      EXPECT_EQ(v.error().code, CompileError::ArithFault) << i;
    }
  }
  {
    Rig rig;
    EXPECT_EQ(rig.show("count n = if n == 0 then 0 else 1 + count (n - 1) ; "
                       "x = count 60 ; 0"),
              "0");
    lower::setCycleLimit(rig.compiler, 150);
    auto starved = rig.eval("x");
    ASSERT_FALSE(starved.has_value());
    EXPECT_EQ(starved.error().code, CompileError::BudgetExhausted);
    lower::setCycleLimit(rig.compiler, std::nullopt);
    EXPECT_EQ(rig.show("x"), "60");
    EXPECT_EQ(rig.show("x"), "60");
  }
  {
    Rig rig;
    EXPECT_EQ(rig.show("x = x + 1 ; 0"), "0");
    for (int i = 0; i < 2; ++i) {
      auto v = rig.eval("x");
      ASSERT_FALSE(v.has_value());
      EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
      EXPECT_NE(v.error().detail.find("<<loop>>"), std::string::npos) << i;
    }
  }
}

TEST(VlaceLowerKernelTest, AFailedNestedForceIsStaleNextRun) {
  Rig rig;
  EXPECT_EQ(rig.show("inner = 1 / 0 ; outer = inner + 1 ; 0"), "0");
  for (int i = 0; i < 2; ++i) {
    auto v = rig.eval("outer");
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, CompileError::ArithFault) << i;
  }
}

TEST(VlaceLowerKernelTest, RunsRestoreTheVmLimitsTheyRaised) {
  Rig rig;
  const auto frameBefore   = rig.vm.frameLimits();
  const auto closureBefore = rig.vm.closureLimits();
  const auto same          = [&] {
    const auto f = rig.vm.frameLimits();
    const auto c = rig.vm.closureLimits();
    return f.maxDepth == frameBefore.maxDepth &&
           f.maxLocalsPerFrame == frameBefore.maxLocalsPerFrame &&
           f.maxBackingCells == frameBefore.maxBackingCells &&
           c.maxValuesCreated == closureBefore.maxValuesCreated &&
           c.maxEnvironmentCellsCreated ==
               closureBefore.maxEnvironmentCellsCreated &&
           c.maxCaptureStreams == closureBefore.maxCaptureStreams;
  };
  EXPECT_EQ(rig.show("let f = fn x => x + 1 in f 1"), "2");
  EXPECT_TRUE(same());
  EXPECT_FALSE(rig.eval("1 / 0").has_value());
  EXPECT_TRUE(same());
  EXPECT_FALSE(rig.eval("loop x = loop x ; loop 0").has_value());
  EXPECT_TRUE(same());
}

TEST(VlaceLowerKernelTest, ApplyingANonFunctionFailsWithoutCrashing) {
  for (const std::string_view src : {"1 2", "(fn x => 5) 1 2", "\"s\" 1"}) {
    Rig rig;
    auto v = rig.eval(src);
    ASSERT_FALSE(v.has_value()) << src;
    // The VM reports the failure; it is not given a VLace-specific name.
    EXPECT_NE(v.error().detail.find("VM failure"), std::string::npos)
        << src << " -> " << v.error().detail;
  }
}

TEST(VlaceLowerKernelTest, PathologicalNestingIsABudgetErrorNotAStackOverflow) {
  std::string src = "1";
  for (int i = 0; i < 1500; ++i) src = "(fn x => x) (" + src + ")";
  Rig rig;
  auto v = rig.eval(src);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
}

} // namespace
