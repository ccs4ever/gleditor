/**
 * @file vlace_lower_data_test.cpp
 * @brief Card L3: constructors, tuples, lists, `data` declarations and `case`
 *        pattern matching lowered to Vortex graphs and run on the VM -- every
 *        pattern form, first-match order, the run-time and compile-time match
 *        errors, field laziness and sharing, infinite lists, budgets, read-back
 *        of partly forced values, and the forms that still refuse.
 */
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/vlace/lower_context.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/render_error.hpp"
#include "common/xanadu/vlace/run_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

// No type checker here: the Compiler is driven directly, so everything the
// checker would normally rule out reaches the lowering and must be refused or
// failed by it.
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
  /// The deeply rendered result, or "ERR:<Code>:<detail>".
  std::string render(std::string_view src) {
    auto v = eval(src);
    if (!v) {
      return "ERR:" + std::string(compileErrorName(v.error().code)) + ":" +
             v.error().detail;
    }
    return compiler.render(*v);
  }
  std::optional<EvalError> failure(std::string_view src) {
    auto v = eval(src);
    if (v) return std::nullopt;
    return v.error();
  }
};

std::string run(std::string_view src) { return Rig{}.render(src); }

const std::string lengthDef =
    "len xs = case xs of [] => 0 ; (h : t) => 1 + len t ; ";
const std::string takeDef =
    "take k xs = if k == 0 then [] else case xs of [] => [] ; "
    "(h : t) => h : take (k - 1) t ; ";

// -- every pattern form
// -------------------------------------------------------------

TEST(VlaceLowerDataTest,
     VariableAndWildcardPatternsMatchAnythingWithoutForcing) {
  EXPECT_EQ(run("case 5 of n => n + 1"), "6");
  EXPECT_EQ(run("case 5 of _ => 9"), "9");
  EXPECT_EQ(run("case \"s\" of s => s ++ \"!\""), "s!");
  // Neither pattern inspects the scrutinee, so a diverging one never runs.
  EXPECT_EQ(run("loop x = loop x ; case loop 0 of _ => 1"), "1");
  EXPECT_EQ(run("loop x = loop x ; case loop 0 of v => 2"), "2");
}

TEST(VlaceLowerDataTest, LiteralPatternsOfEachScalarKind) {
  EXPECT_EQ(run("case 3 of 1 => \"a\" ; 3 => \"b\" ; _ => \"c\""), "b");
  EXPECT_EQ(run("case 4 of 1 => \"a\" ; 3 => \"b\" ; _ => \"c\""), "c");
  EXPECT_EQ(run("case 2.5 of 1.5 => \"a\" ; 2.5 => \"b\" ; _ => \"c\""), "b");
  EXPECT_EQ(run("case \"hi\" of \"yo\" => 1 ; \"hi\" => 2 ; _ => 3"), "2");
  EXPECT_EQ(run("case 'x' of 'y' => 1 ; 'x' => 2 ; _ => 3"), "2");
  EXPECT_EQ(run("case True of False => 1 ; True => 2"), "2");
  EXPECT_EQ(run("case 1 == 2 of True => \"t\" ; False => \"f\""), "f");
}

TEST(VlaceLowerDataTest, ALiteralPatternDoesNotMatchAnotherKind) {
  // Eq compares variants, as it always did: 1 is not 1.0, and neither is True.
  EXPECT_EQ(run("case 1 of 1.0 => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case 1.0 of 1 => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case True of 1 => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case 1 of True => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case \"1\" of 1 => 1 ; _ => 2"), "2");
}

TEST(VlaceLowerDataTest, ConstructorPatternsBindFieldsByPosition) {
  const std::string shapes =
      "data S = Dot | Circle Int | Rect Int Int | Tri Int Int Int ; ";
  EXPECT_EQ(run(shapes + "case Dot of Dot => 0 ; _ => 1"), "0");
  EXPECT_EQ(run(shapes + "case Circle 7 of Circle r => r"), "7");
  EXPECT_EQ(run(shapes + "case Rect 3 4 of Rect w h => w * 10 + h"), "34");
  EXPECT_EQ(run(shapes + "case Tri 1 2 3 of Tri a b c => a * 100 + b * 10 + c"),
            "123");
  // A wildcard skips a field without reading it.
  EXPECT_EQ(run(shapes + "case Tri 1 2 3 of Tri _ b _ => b"), "2");
  EXPECT_EQ(run(shapes + "case Tri 1 2 3 of Tri _ _ c => c"), "3");
  EXPECT_EQ(run(shapes + "case Tri 1 2 3 of Tri _ _ _ => 0"), "0");
}

TEST(VlaceLowerDataTest, TuplePatternsOfSeveralSizes) {
  EXPECT_EQ(run("case (1, 2) of (a, b) => a + b"), "3");
  EXPECT_EQ(run("case (1, 2, 3) of (a, b, c) => a * 100 + b * 10 + c"), "123");
  EXPECT_EQ(run("case (1, 2, 3, 4) of (_, _, c, d) => c + d"), "7");
  // A pair pattern does not match a triple.
  EXPECT_EQ(run("case (1, 2, 3) of (a, b) => 0 ; _ => 9"), "9");
  EXPECT_EQ(run("case (1, 2) of (a, b, c) => 0 ; _ => 9"), "9");
}

TEST(VlaceLowerDataTest, ListSugarAndConsPatterns) {
  EXPECT_EQ(run("case [] of [] => 0 ; (h : t) => 1"), "0");
  EXPECT_EQ(run("case [7, 8] of [] => 0 ; (h : t) => h"), "7");
  EXPECT_EQ(run("case [7, 8] of [a, b] => a + b ; _ => 0"), "15");
  EXPECT_EQ(run("case [7] of [a, b] => a + b ; _ => 0"), "0");
  EXPECT_EQ(run("case [7, 8, 9] of (a : b : rest) => rest ; _ => []"), "[9]");
  EXPECT_EQ(run("case [1, 2] of (a : (b : [])) => a + b ; _ => 0"), "3");
}

TEST(VlaceLowerDataTest, NestedPatternsMatchLayerByLayer) {
  EXPECT_EQ(run("case ((1, 2), 3) of ((a, b), c) => a + b + c"), "6");
  EXPECT_EQ(run("case Some (Some 4) of Some (Some n) => n ; _ => 0"), "4");
  EXPECT_EQ(run("case Some None of Some (Some n) => n ; Some None => 100 ; "
                "_ => 0"),
            "100");
  EXPECT_EQ(run("case [(1, True), (2, False)] of "
                "((a, True) : (b, False) : _) => a + b ; _ => 0"),
            "3");
  EXPECT_EQ(run("case (1, [2, 3]) of (a, (b : _)) => a + b ; _ => 0"), "3");
  // A literal inside a constructor inside a tuple.
  EXPECT_EQ(run("case (Some 3, 4) of (Some 2, _) => 1 ; (Some 3, 4) => 2 ; "
                "_ => 3"),
            "2");
}

TEST(VlaceLowerDataTest, BuiltInOptionAndResultConstructors) {
  EXPECT_EQ(run("case Some 5 of None => 0 ; Some n => n"), "5");
  EXPECT_EQ(run("case None of None => 0 ; Some n => n"), "0");
  EXPECT_EQ(run("case Ok 1 of Ok n => n ; Err e => 0"), "1");
  EXPECT_EQ(run("case Err \"bad\" of Ok n => \"ok\" ; Err e => e"), "bad");
  EXPECT_EQ(run("Some 5"), "Some 5");
  EXPECT_EQ(run("Err \"x\""), "Err x");
}

// -- order and guards
// ---------------------------------------------------------------

TEST(VlaceLowerDataTest, TheFirstMatchingAlternativeWins) {
  EXPECT_EQ(run("case 1 of _ => \"first\" ; 1 => \"second\""), "first");
  EXPECT_EQ(
      run("case [1] of (h : _) => \"cons\" ; [x] => \"one\" ; _ => \"?\""),
      "cons");
  EXPECT_EQ(run("case (1, 2) of (1, _) => \"a\" ; (_, 2) => \"b\" ; "
                "(_, _) => \"c\""),
            "a");
  EXPECT_EQ(run("case (3, 2) of (1, _) => \"a\" ; (_, 2) => \"b\" ; "
                "(_, _) => \"c\""),
            "b");
  EXPECT_EQ(run("case (3, 4) of (1, _) => \"a\" ; (_, 2) => \"b\" ; "
                "(_, _) => \"c\""),
            "c");
}

TEST(VlaceLowerDataTest, AGuardFallsThroughToTheNextAlternative) {
  EXPECT_EQ(run("case 5 of n if n > 3 => 100 ; _ => 0"), "100");
  EXPECT_EQ(run("case 2 of n if n > 3 => 100 ; _ => 0"), "0");
  EXPECT_EQ(run("case Some 5 of Some n if n < 3 => 1 ; Some n if n > 4 => 2 ; "
                "_ => 3"),
            "2");
  EXPECT_EQ(run("case Some 4 of Some n if n < 3 => 1 ; Some n if n > 4 => 2 ; "
                "_ => 3"),
            "3");
}

TEST(VlaceLowerDataTest, AFailedAlternativeLeavesNoBindingBehind) {
  // `a` bound by the first alternative's pattern is not in scope in the second.
  EXPECT_EQ(run("a = 100 ; case (1, 2) of (a, 3) => a ; (_, b) => a + b"),
            "102");
  EXPECT_EQ(run("a = 100 ; case Some 7 of Some a if a > 9 => 0 ; _ => a"),
            "100");
}

TEST(VlaceLowerDataTest, ANonBoolGuardIsATypeMismatchAtRunTime) {
  const auto err = Rig{}.failure("case 1 of n if n => 1 ; _ => 2");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::TypeMismatch);
  EXPECT_EQ(err->detail, "case guard must be Bool");
}

// -- match errors
// -------------------------------------------------------------------

TEST(VlaceLowerDataTest, NoMatchingAlternativeFailsAtRunTimeWithItsLocation) {
  Rig rig;
  const auto err = rig.failure("case [1] of [] => 0");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonExhaustiveMatch);
  EXPECT_EQ(err->detail, "no case alternative matched the scrutinee");
  EXPECT_EQ(err->loc.line, 1U);
  EXPECT_EQ(err->loc.column, 1U);
  // The location is the `case`'s own, wherever it sits in the program.
  const auto inner = rig.failure("f x = 1 + (case x of 1 => 2) ;\nf 5");
  ASSERT_TRUE(inner.has_value());
  EXPECT_EQ(inner->code, CompileError::NonExhaustiveMatch);
  EXPECT_EQ(inner->loc.line, 1U);
  EXPECT_GT(inner->loc.column, 1U);
  // The VM is usable afterwards.
  EXPECT_EQ(rig.render("1 + 1"), "2");
}

TEST(VlaceLowerDataTest, AGuardedLastAlternativeCanFallOffTheEnd) {
  const auto err = Rig{}.failure("case 1 of n if n > 5 => n");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonExhaustiveMatch);
}

TEST(VlaceLowerDataTest, NoAlternativesAtAllFailsTheSameWay) {
  const auto err = Rig{}.failure("data T = A | B ; case B of A => 1");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonExhaustiveMatch);
}

TEST(VlaceLowerDataTest, ARepeatedPatternVariableIsRefusedAtCompileTime) {
  EXPECT_EQ(run("case (1, 1) of (x, x) => x"),
            "ERR:NonLinearPattern:'x' bound twice in one pattern");
  EXPECT_EQ(run("case [1, 2] of (x : x : _) => x ; _ => 0"),
            "ERR:NonLinearPattern:'x' bound twice in one pattern");
  EXPECT_EQ(run("case Some (1, 2) of Some (y, y) => y ; _ => 0"),
            "ERR:NonLinearPattern:'y' bound twice in one pattern");
  // The same name in two different alternatives is fine.
  EXPECT_EQ(run("case (1, 2) of (x, 3) => x ; (x, y) => x + y"), "3");
}

TEST(VlaceLowerDataTest, ANonLinearPatternIsACompileErrorEvenInAnUnreachedAlt) {
  Rig rig;
  const auto err = rig.failure("case 1 of _ => 0 ; (x, x) => 1");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonLinearPattern);
  // Compile time: nothing ran, so a diverging body elsewhere is not reached.
  const auto before =
      rig.failure("loop x = loop x ; case loop 0 of (x, x) => 1");
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->code, CompileError::NonLinearPattern);
}

TEST(VlaceLowerDataTest, ALambdaParameterPatternMismatchFailsAtRunTime) {
  EXPECT_EQ(run("(fn (a, b) => a + b) (1, 2)"), "3");
  EXPECT_EQ(run("(fn (Some n) => n) (Some 4)"), "4");
  const auto err = Rig{}.failure("(fn (Some n) => n) None");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonExhaustiveMatch);
  EXPECT_EQ(err->detail, "lambda argument did not match its parameter pattern");
}

TEST(VlaceLowerDataTest, FunctionsMayTakeConstructorPatternsAsParameters) {
  EXPECT_EQ(run("fst (a, _) = a ; snd (_, b) = b ; fst (1, 2) + snd (10, 20)"),
            "21");
  EXPECT_EQ(run("addPair (a, b) = a + b ; addPair (3, 4)"), "7");
}

// -- laziness and sharing
// -----------------------------------------------------------

TEST(VlaceLowerDataTest, ADivergingFieldNeverInspectedNeverRuns) {
  EXPECT_EQ(run("loop x = loop x ; case (1, loop 0) of (a, _) => a"), "1");
  EXPECT_EQ(run("loop x = loop x ; case Some (loop 0) of Some _ => 7"), "7");
  EXPECT_EQ(run("loop x = loop x ; case [1, loop 0] of (h : _) => h"), "1");
  EXPECT_EQ(run("loop x = loop x ; case 1 : loop 0 of (h : _) => h"), "1");
  EXPECT_EQ(run("boom = 1 / 0 ; case (2, boom) of (a, _) => a"), "2");
  // The scrutinee is forced to weak head normal form only.
  EXPECT_EQ(run("loop x = loop x ; case (loop 0, loop 1) of (_, _) => \"ok\""),
            "ok");
}

TEST(VlaceLowerDataTest, ABoundFieldIsOnlyForcedWhereItIsUsed) {
  EXPECT_EQ(run("loop x = loop x ; case (1, loop 0) of (a, b) => a"), "1");
  EXPECT_EQ(run("boom = 1 / 0 ; case (5, boom) of (a, b) => a + 1"), "6");
  const auto err =
      Rig{}.failure("boom = 1 / 0 ; case (5, boom) of (a, b) => b");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::ArithFault);
}

namespace {

/// The smallest cycle limit under which @p src evaluates (see the kernel
/// tests); nullopt if even a very large limit fails.
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

TEST(VlaceLowerDataTest, AFieldSharedByTwoSlotsIsEvaluatedOnce) {
  // `count 60` is the expensive part; the pair holds it twice and the body
  // reads both. If each field slot re-ran it, three reads would cost about
  // three times one.
  const std::string body =
      "count n = if n == 0 then 0 else 1 + count (n - 1) ; ";
  const auto once   = cycleCost(body + "let t = count 60 in case (t, 1) of "
                                         "(a, _) => a");
  const auto thrice = cycleCost(
      body + "let t = count 60 in case (t, t) of (a, b) => a + b + t");
  ASSERT_TRUE(once && thrice);
  EXPECT_LT(*thrice, *once + *once / 2)
      << "once=" << *once << " thrice=" << *thrice;
}

TEST(VlaceLowerDataTest,
     AFieldIsForcedOnceThroughTheRecursionThatReadsItOften) {
  const std::string body =
      "count n = if n == 0 then 0 else 1 + count (n - 1) ; "
      "sumN k p = if k == 0 then 0 else (case p of (a, _) => a) + "
      "sumN (k - 1) p ; ";
  const auto one  = cycleCost(body + "sumN 1 (count 60, 0)");
  const auto many = cycleCost(body + "sumN 6 (count 60, 0)");
  ASSERT_TRUE(one && many);
  // Six reads of the field cost the sums, not six evaluations of `count 60`.
  EXPECT_LT(*many, *one * 2) << "one=" << *one << " many=" << *many;
}

TEST(VlaceLowerDataTest, ASharedVariableInTwoConstructorsKeepsItsValue) {
  // The variable is the field of both pairs; neither steals it from the other.
  EXPECT_EQ(
      run("let x = 3 * 3 in case (x, (x, x)) of (a, (b, c)) => a + b + c"),
      "27");
  EXPECT_EQ(run("let x = 5 in let p = (x, 1) in let q = (x, 2) in "
                "case p of (a, _) => case q of (b, _) => a + b"),
            "10");
  EXPECT_EQ(run("let xs = [1, 2] in (xs, xs)"), "([1, 2], [1, 2])");
  EXPECT_EQ(run("k = 4 ; [k, k, k]"), "[4, 4, 4]");
  EXPECT_EQ(run("one = Some 1 ; [one, one]"), "[Some 1, Some 1]");
}

TEST(VlaceLowerDataTest, TheSameLiteralBuiltManyTimesGivesIndependentCells) {
  EXPECT_EQ(run("mk n = if n == 0 then [] else 7 : mk (n - 1) ; mk 5"),
            "[7, 7, 7, 7, 7]");
  EXPECT_EQ(run("mk n = if n == 0 then [] else \"s\" : mk (n - 1) ; mk 3"),
            "[s, s, s]");
  EXPECT_EQ(run("mk n = if n == 0 then [] else True : mk (n - 1) ; mk 2"),
            "[True, True]");
  EXPECT_EQ(run("mk n = if n == 0 then None else Some (mk (n - 1)) ; mk 3"),
            "Some Some Some None");
}

// -- infinite structures
// ------------------------------------------------------------

TEST(VlaceLowerDataTest, AnInfiniteListIsConsumedFinitely) {
  EXPECT_EQ(run("from n = n : from (n + 1) ;" + takeDef + "take 5 (from 0)"),
            "[0, 1, 2, 3, 4]");
  EXPECT_EQ(run(takeDef + "letrec ones = 1 : ones in take 4 ones"),
            "[1, 1, 1, 1]");
  EXPECT_EQ(run(takeDef + "letrec xs = 1 : 2 : xs in take 5 xs"),
            "[1, 2, 1, 2, 1]");
  EXPECT_EQ(run("from n = n : from (n + 1) ; case from 10 of (a : b : _) => "
                "a + b"),
            "21");
}

TEST(VlaceLowerDataTest, ALazyListFeedsAMapWithoutForcingTheRest) {
  EXPECT_EQ(run("from n = n : from (n + 1) ;" + takeDef +
                "map f xs = case xs of [] => [] ; (h : t) => f h : map f t ;"
                "take 4 (map (fn x => x * x) (from 1))"),
            "[1, 4, 9, 16]");
}

// -- budgets
// ----------------------------------------------------------------------

TEST(VlaceLowerDataTest, RecursionOverAListWithinTheBudgetWorks) {
  EXPECT_EQ(run("range n = if n == 0 then [] else n : range (n - 1) ;" +
                lengthDef + "len (range 200)"),
            "200");
}

TEST(VlaceLowerDataTest, RecursionOverALongListHitsTheBudgetCleanly) {
  Rig rig;
  const std::string src =
      "range n = if n == 0 then [] else n : range (n - 1) ;" + lengthDef +
      "len (range 100000)";
  const auto err = rig.failure(src);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::BudgetExhausted);
  EXPECT_NE(err->detail.find("budget"), std::string::npos);
  // Nothing was left half-run: the same Compiler still evaluates.
  EXPECT_EQ(rig.render("case (1, 2) of (a, b) => a + b"), "3");
}

TEST(VlaceLowerDataTest, ACycleLimitStopsAPatternLoop) {
  Rig rig;
  lower::setCycleLimit(rig.compiler, 3000);
  const auto v =
      rig.eval("spin xs = case xs of (h : t) => spin (h : t) ; spin [1]");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
}

TEST(VlaceLowerDataTest, ALongListLiteralDoesNotOverflowTheHostStack) {
  std::string src = "[";
  for (int i = 0; i < 3000; ++i)
    src += (i == 0 ? "" : ", ") + std::to_string(i);
  src += "]";
  Rig rig;
  const auto v = rig.eval(src);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)->kind, Value::Kind::Ctor);
  EXPECT_EQ((*v)->tag, "Cons");
}

TEST(VlaceLowerDataTest, DeeplyNestedCasesAreRefusedAsABudget) {
  std::string src;
  for (int i = 0; i < 1100; ++i) src += "case 1 of _ => ";
  src += "0";
  Rig rig;
  const auto err = rig.failure(src);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::BudgetExhausted);
}

// -- data declarations
// --------------------------------------------------------------

TEST(VlaceLowerDataTest, DataDeclarationsWithSeveralConstructorsAndArities) {
  const std::string tree = "data Tree = Leaf | One Int | Node Tree Int Tree ; "
                           "size t = case t of Leaf => 0 ; One _ => 1 ; "
                           "Node l _ r => size l + 1 + size r ; ";
  EXPECT_EQ(run(tree + "size (Node (Node Leaf 1 Leaf) 2 (One 3))"), "3");
  EXPECT_EQ(run(tree + "Node Leaf 1 (One 2)"), "Node Leaf 1 One 2");
  EXPECT_EQ(run("data Shape = Circle Int | Rect Int Int ;"
                "area s = case s of Circle r => 3 * r * r ; Rect w h => w * h ;"
                "area (Rect 4 5)"),
            "20");
}

TEST(VlaceLowerDataTest, ConstructorsAreCurriedFunctions) {
  EXPECT_EQ(run("data P = P Int Int ; (P 1) 2"), "P 1 2");
  EXPECT_EQ(run("data P = P Int Int ; let f = P 1 in f 2"), "P 1 2");
  EXPECT_EQ(run("data P = P Int Int ; let f = P in f 3 4"), "P 3 4");
  EXPECT_EQ(run("map f xs = case xs of [] => [] ; (h : t) => f h : map f t ;"
                "map Some [1, 2]"),
            "[Some 1, Some 2]");
  EXPECT_EQ(run("data P = P Int Int ; (P $ 1) 2"), "P 1 2");
}

TEST(VlaceLowerDataTest, ConstructorsMayBeRedeclaredByALaterProgram) {
  Rig rig;
  EXPECT_EQ(rig.render("data T = A Int ; case A 1 of A n => n"), "1");
  EXPECT_EQ(rig.render("data T = A Int Int ; case A 1 2 of A m n => m + n"),
            "3");
}

TEST(VlaceLowerDataTest, ReservedConstructorTagsAreRefusedInData) {
  for (const std::string_view tag :
       {"Some", "None", "Ok", "Err", "Nil", "Cons", "Cell", "Dim", "Unit",
        "Tuple", "True", "False", "SInt", "SFloat", "SBool"}) {
    const auto err = Rig{}.failure("data W = " + std::string(tag) + " Int ; 1");
    ASSERT_TRUE(err.has_value()) << tag;
    EXPECT_EQ(err->code, CompileError::TypeMismatch) << tag;
    EXPECT_EQ(err->detail, "data constructor '" + std::string(tag) +
                               "' is reserved by the runtime")
        << tag;
  }
  // A refused declaration declares nothing, not even its legal constructors.
  Rig rig;
  EXPECT_TRUE(rig.failure("data W = Fine Int | Some Int ; 1").has_value());
  const auto unknown = rig.failure("Fine 1");
  ASSERT_TRUE(unknown.has_value());
  EXPECT_EQ(unknown->code, CompileError::UnboundIdentifier);
}

TEST(VlaceLowerDataTest, ADuplicateConstructorInOneDeclarationIsRefused) {
  const auto err = Rig{}.failure("data T = A Int | A ; 1");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::TypeMismatch);
  EXPECT_EQ(err->detail, "data constructor 'A' is declared twice");
}

TEST(VlaceLowerDataTest, ConstructorArityAndNameErrors) {
  const std::string d = "data T = A Int Int | B ; ";
  // A pattern with the wrong number of arguments.
  auto err = Rig{}.failure(d + "case B of A x => x ; _ => 0");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::TypeMismatch);
  EXPECT_EQ(err->detail,
            "constructor 'A' takes 2 arguments but the pattern has 1");
  err = Rig{}.failure(d + "case B of B x => x ; _ => 0");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->detail,
            "constructor 'B' takes 0 arguments but the pattern has 1");
  err = Rig{}.failure("case True of True x => x ; _ => 0");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->detail, "constructor 'True' takes no arguments");
  // An unknown constructor, in an expression and in a pattern.
  err = Rig{}.failure("Nope 1");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::UnboundIdentifier);
  EXPECT_EQ(err->detail, "unknown constructor 'Nope'");
  err = Rig{}.failure("case 1 of Nope x => x ; _ => 0");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::UnboundIdentifier);
  EXPECT_EQ(err->detail, "unknown constructor 'Nope' in pattern");
}

TEST(VlaceLowerDataTest, RuntimeOwnedConstructorsCannotBeBuilt) {
  for (const std::string_view src : {"Cell 1", "Dim 1", "Unit"}) {
    const auto err = Rig{}.failure(src);
    ASSERT_TRUE(err.has_value()) << src;
    EXPECT_EQ(err->code, CompileError::TypeMismatch) << src;
    EXPECT_NE(err->detail.find("is runtime-owned and cannot be constructed"),
              std::string::npos)
        << src;
  }
}

// -- a value that is not a term never matches as one
// ---------------------------------------

TEST(VlaceLowerDataTest, AScalarThatLooksLikeATagIsNotThatConstructor) {
  // The type checker is not here to stop these: the match must still be right.
  EXPECT_EQ(run("case \"Nil\" of [] => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case \"Cons\" of (h : t) => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case \"Some\" of Some x => x ; _ => 0"), "0");
  EXPECT_EQ(run("case \"Tuple\" of (a, b) => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case 5 of Some x => x ; _ => 0"), "0");
  EXPECT_EQ(run("case True of Some x => x ; _ => 0"), "0");
  EXPECT_EQ(run("case fn x => x of Some y => y ; _ => 0"), "0");
}

TEST(VlaceLowerDataTest, ATermIsNotAStringLiteralOfItsTag) {
  EXPECT_EQ(run("case [] of \"Nil\" => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case None of \"None\" => 1 ; _ => 2"), "2");
  EXPECT_EQ(run("case Some 1 of \"Some\" => 1 ; _ => 2"), "2");
}

TEST(VlaceLowerDataTest, ATermOfOneArityDoesNotMatchAPatternOfAnother) {
  EXPECT_EQ(run("data A = T Int ; data B = T2 Int Int ; "
                "case T 1 of T2 a b => 1 ; T x => 2"),
            "2");
  EXPECT_EQ(run("case Some 1 of None => 1 ; _ => 2"), "2");
}

// -- read-back
// ------------------------------------------------------------------------

TEST(VlaceLowerDataTest, ConstructedValuesReadBackAsConstructors) {
  Rig rig;
  auto v = rig.eval("(1, True, \"three\")");
  ASSERT_TRUE(v.has_value());
  const auto &t = **v;
  EXPECT_EQ(t.kind, Value::Kind::Ctor);
  EXPECT_EQ(t.tag, "Tuple");
  EXPECT_EQ(t.ctorArity, 3U);
  ASSERT_EQ(t.args.size(), 3U);
  auto a = rig.compiler.forceValue(t.args[0]);
  auto b = rig.compiler.forceValue(t.args[1]);
  auto c = rig.compiler.forceValue(t.args[2]);
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ((*a)->kind, Value::Kind::Int);
  EXPECT_EQ((*a)->i, 1);
  EXPECT_EQ((*b)->kind, Value::Kind::Bool);
  EXPECT_TRUE((*b)->b);
  EXPECT_EQ((*c)->kind, Value::Kind::Str);
  EXPECT_EQ((*c)->s, "three");
}

TEST(VlaceLowerDataTest, ANullaryConstructorReadsBackAsOne) {
  Rig rig;
  auto nil = rig.eval("[]");
  ASSERT_TRUE(nil.has_value());
  EXPECT_EQ((*nil)->kind, Value::Kind::Ctor);
  EXPECT_EQ((*nil)->tag, "Nil");
  EXPECT_EQ((*nil)->ctorArity, 0U);
  EXPECT_TRUE((*nil)->args.empty());
  EXPECT_EQ(Compiler::show(*nil), "Nil");
  // The string "Nil" is not that constructor.
  auto str = rig.eval("\"Nil\"");
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ((*str)->kind, Value::Kind::Str);
}

TEST(VlaceLowerDataTest, AFieldReadsBackUnforcedUntilItIsForced) {
  Rig rig;
  auto v = rig.eval("loop x = loop x ; (1 + 1, loop 0)");
  ASSERT_TRUE(v.has_value());
  ASSERT_EQ((*v)->args.size(), 2U);
  // Neither field has run; the handle on the first forces to 2.
  EXPECT_EQ((*v)->args[0]->kind, Value::Kind::Thunk);
  auto first = rig.compiler.forceValue((*v)->args[0]);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)->kind, Value::Kind::Int);
  EXPECT_EQ((*first)->i, 2);
  // Forcing the second is the divergence, reported as a budget error.
  auto second = rig.compiler.forceValue((*v)->args[1]);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, CompileError::BudgetExhausted);
  // The VM is still usable.
  EXPECT_EQ(rig.render("2 * 2"), "4");
}

TEST(VlaceLowerDataTest, ForcingAFieldTwiceGivesTheSameValue) {
  Rig rig;
  auto v = rig.eval("(6 * 7, 0)");
  ASSERT_TRUE(v.has_value());
  auto once  = rig.compiler.forceValue((*v)->args[0]);
  auto twice = rig.compiler.forceValue((*v)->args[0]);
  ASSERT_TRUE(once && twice);
  EXPECT_EQ((*once)->i, 42);
  EXPECT_EQ((*twice)->i, 42);
  EXPECT_EQ((*once)->cell, (*twice)->cell);
}

TEST(VlaceLowerDataTest, ARenderedListIsForcedAsFarAsItIsShown) {
  EXPECT_EQ(run("[1 + 1, 2 * 3, 10 - 1]"), "[2, 6, 9]");
  EXPECT_EQ(run("[(1, 2), (3, 4)]"), "[(1, 2), (3, 4)]");
  EXPECT_EQ(run("[[1], [], [2, 3]]"), "[[1], [], [2, 3]]");
  EXPECT_EQ(run("(Some 1, None)"), "(Some 1, None)");
  EXPECT_EQ(run("1 : 2"), "[1 | 2]");
}

TEST(VlaceLowerDataTest, RenderingHonoursTheDepthBudget) {
  Rig rig;
  auto v = rig.eval("(1, (2, (3, 4)))");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(rig.compiler.render(*v, 10), "(1, (2, (3, 4)))");
  EXPECT_EQ(rig.compiler.render(*v, 2), "(1, (..., ...))");
  EXPECT_EQ(rig.compiler.render(*v, 0), "...");
}

TEST(VlaceLowerDataTest, AFailingFieldFailsTheRenderInsteadOfShowingAMarker) {
  Rig rig;
  auto v = rig.eval("(1, 1 / 0)");
  ASSERT_TRUE(v.has_value());
  try {
    (void)rig.compiler.render(*v);
    FAIL() << "the render succeeded";
  } catch (const lower::RenderFailure &failure) {
    EXPECT_EQ(failure.error().code, CompileError::ArithFault);
  }
  // Through the Driver the same failure is the eval phase's error, code kept.
  Driver driver;
  driver.withTypeChecking(false);
  auto r = driver.run("(1, 1 / 0)");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().phase, "eval");
  EXPECT_EQ(r.error().code, CompileError::ArithFault);
  // A failing tail of a list is the same.
  auto list = rig.eval("[1, 2 / 0]");
  ASSERT_TRUE(list.has_value());
  EXPECT_THROW((void)rig.compiler.render(*list), lower::RenderFailure);
}

TEST(VlaceLowerDataTest, ACutOffListIsClosedWithAnEllipsis) {
  Rig rig;
  auto inf = rig.eval("from n = n : from (n + 1) ; from 0");
  ASSERT_TRUE(inf.has_value());
  EXPECT_EQ(rig.compiler.render(*inf, 5), "[0, 1, 2, 3, 4, ...]");
  auto cyc = rig.eval("letrec xs = 1 : xs in xs");
  ASSERT_TRUE(cyc.has_value());
  EXPECT_EQ(rig.compiler.render(*cyc, 3), "[1, 1, 1, ...]");
  // A list that fits the budget exactly, or ends early, closes plainly.
  auto three = rig.eval("[1, 2, 3]");
  ASSERT_TRUE(three.has_value());
  EXPECT_EQ(rig.compiler.render(*three, 3), "[1, 2, 3]");
  EXPECT_EQ(rig.compiler.render(*three, 10), "[1, 2, 3]");
  EXPECT_EQ(rig.compiler.render(*three, 2), "[1, 2, ...]");
}

TEST(VlaceLowerDataTest, AForcedLongListReadsBackWithoutRecursingToItsDepth) {
  Rig rig;
  auto v = rig.eval("range n = if n == 0 then [] else n : range (n - 1) ;"
                    "range 300");
  ASSERT_TRUE(v.has_value());
  // Force every cell, then read the head back: the read does not walk the tail.
  std::string text = rig.compiler.render(*v);
  EXPECT_EQ(text.substr(0, 12), "[300, 299, 2");
  auto again = rig.eval("range n = if n == 0 then [] else n : range (n - 1) ;"
                        "case range 300 of (h : _) => h");
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ((*again)->i, 300);
}

// -- nesting beyond the budget, linearity across parameters, more scrutinees
// ------------------------------------------------------------------------

TEST(VlaceLowerDataTest, DeeplyNestedConstructionEndsInABudgetError) {
  const int depth = 1500;
  Rig rig;
  // [[[[...]]]]
  std::string lists = std::string(depth, '[') + "1" + std::string(depth, ']');
  auto err          = rig.failure(lists);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::BudgetExhausted);
  // ((((1 : 2) : 3) : 4) ...), the head of each cons is the next one.
  std::string cons = std::string(depth, '(') + "1";
  for (int i = 0; i < depth; ++i) cons += " : 2)";
  err = rig.failure(cons);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::BudgetExhausted);
  // Some (Some (... 1)) as an expression.
  std::string some;
  for (int i = 0; i < depth; ++i) some += "Some (";
  some += "1" + std::string(depth, ')');
  err = rig.failure(some);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::BudgetExhausted);
  // The VM is usable afterwards.
  EXPECT_EQ(rig.render("[[1], [2]]"), "[[1], [2]]");
}

TEST(VlaceLowerDataTest, ADeeplyNestedConstructorPatternEndsInABudgetError) {
  const int depth = 1500;
  std::string pattern;
  for (int i = 0; i < depth; ++i) pattern += "Some (";
  pattern += "n" + std::string(depth, ')');
  Rig rig;
  const auto err = rig.failure("case None of " + pattern + " => 1 ; _ => 0");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::BudgetExhausted);
  // Within the budget the same shape matches.
  std::string small;
  for (int i = 0; i < 20; ++i) small += "Some (";
  small += "n" + std::string(20, ')');
  std::string value;
  for (int i = 0; i < 20; ++i) value += "Some (";
  value += "7" + std::string(20, ')');
  EXPECT_EQ(rig.render("case " + value + " of " + small + " => n"), "7");
}

TEST(VlaceLowerDataTest, ParameterPatternsOfOneFunctionShareALinearityScope) {
  for (const std::string_view src :
       {"(fn (a, b) (c, a) => a) (1, 2) (3, 4)", "f (a, b) (c, a) = a ; 0",
        "(fn x x => x) 1 2", "(fn (Some a) [a, b] => b) (Some 1) [2, 3]"}) {
    const auto err = Rig{}.failure(src);
    ASSERT_TRUE(err.has_value()) << src;
    EXPECT_EQ(err->code, CompileError::NonLinearPattern) << src;
  }
  // Distinct names across parameters, and the same name in different
  // lambdas or alternatives, are fine.
  EXPECT_EQ(run("(fn (a, b) (c, d) => a + b + c + d) (1, 2) (3, 4)"), "10");
  EXPECT_EQ(run("(fn (a, b) => (fn (a, c) => a + c)) (1, 2) (3, 4)"), "7");
  EXPECT_EQ(run("case (1, 2) of (a, _) => a ; (_, a) => a"), "1");
  // A pattern that binds nothing is fine however often it is used.
  EXPECT_EQ(run("case (1, 2) of (_, _) => 0"), "0");
  EXPECT_EQ(run("g (_, _) (_, _) = 5 ; g (1, 2) (3, 4)"), "5");
}

TEST(VlaceLowerDataTest, OnlyALaterAlternativeMayInspectAnUnforcedScrutinee) {
  // The first alternative is a guarded variable: the scrutinee stays a thunk
  // until the second one looks at it, and a thunk that fails fails there.
  EXPECT_EQ(run("case 1 + 1 of x if False => 0 ; 2 => 5 ; _ => 6"), "5");
  EXPECT_EQ(run("case Some (1 + 1) of s if False => 0 ; Some n => n"), "2");
  const auto err =
      Rig{}.failure("case 1 / 0 of x if False => 0 ; 2 => 5 ; _ => 6");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::ArithFault);
  // If nothing ever inspects it, it never runs.
  EXPECT_EQ(run("case 1 / 0 of x if False => 0 ; y => 3"), "3");
}

TEST(VlaceLowerDataTest, AClosureScrutineeMatchesOnlyAVariableOrWildcard) {
  EXPECT_EQ(run("case (fn x => x + 1) of f => f 1"), "2");
  EXPECT_EQ(run("case (fn x => x) of _ => 4"), "4");
  EXPECT_EQ(run("case (fn x => x) of 1 => 1 ; \"fn\" => 2 ; None => 3 ; "
                "(a, b) => 4 ; f => f 9"),
            "9");
  EXPECT_EQ(run("case (fn x => x) of Some y => y ; (a : b) => a ; _ => 0"),
            "0");
}

TEST(VlaceLowerDataTest, OpaqueCapabilityTagsMatchButCannotBeBuilt) {
  // Patterns on Cell, Dim and Unit are accepted (and match nothing a program
  // can make); expressions naming them are refused.
  EXPECT_EQ(run("case 5 of Cell n => n ; Dim n => n ; Unit => 1 ; _ => 0"),
            "0");
  for (const std::string_view src :
       {"Cell 1", "Dim 2", "Unit", "(fn n => Cell n) 1"}) {
    const auto err = Rig{}.failure(src);
    ASSERT_TRUE(err.has_value()) << src;
    EXPECT_EQ(err->code, CompileError::TypeMismatch) << src;
  }
  // `Cell` of the wrong arity in a pattern is an arity error.
  const auto bad = Rig{}.failure("case 1 of Cell a b => a ; _ => 0");
  ASSERT_TRUE(bad.has_value());
  EXPECT_EQ(bad->code, CompileError::TypeMismatch);
}

TEST(VlaceLowerDataTest, EmptyParenthesesAreNotAnExpression) {
  Parser parser("()");
  const auto prog = parser.parseProgram();
  EXPECT_FALSE(prog.has_value());
}

TEST(VlaceLowerDataTest, ALongForcedListReadsBackAndRenders) {
  Rig rig;
  const auto v = rig.eval("from n = n : from (n + 1) ;" + takeDef +
                          "take 3000 (from 1)");
  ASSERT_TRUE(v.has_value());
  const auto text = rig.compiler.render(*v, 20000);
  EXPECT_EQ(text.substr(0, 13), "[1, 2, 3, 4, ");
  EXPECT_NE(text.find(", 3000]"), std::string::npos);
  EXPECT_EQ(text.find("..."), std::string::npos);
}

// -- what still refuses
// -------------------------------------------------------------

TEST(VlaceLowerDataTest, DestructuringLetStillRefusesByName) {
  const auto err = Rig{}.failure("let (a, b) = (1, 2) in a");
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonExhaustiveMatch);
  EXPECT_NE(err->detail.find("let binds a simple variable"), std::string::npos);
}

TEST(VlaceLowerDataTest, FormsOfLaterCardsRefuseByName) {
  const std::string refusal = "VM lowering is not implemented for VLace";
  Rig rig;
  for (const std::string_view src :
       {"data P = P { a :: Int } ; case P 1 of P { a = x } => x ; _ => 0",
        "putStrLn \"x\"", "pure 1", "makeText \"x\""}) {
    const auto err = rig.failure(src);
    ASSERT_TRUE(err.has_value()) << src;
    EXPECT_NE(err->detail.find(refusal), std::string::npos)
        << src << " -> " << err->detail;
  }
}

// The parser never produces a record expression, so build one by hand.
TEST(VlaceLowerDataTest, RecordConstructionStillRefusesByName) {
  Rig rig;
  Parser parser("data P = P { a :: Int } ; 0");
  auto prog = parser.parseProgram();
  ASSERT_TRUE(prog.has_value());
  auto ctor  = std::make_unique<ExprNode>();
  ctor->node = ECtor{.name = "P"};
  auto one   = std::make_unique<ExprNode>();
  one->node = ELiteral{.lit = Literal{.kind = LiteralKind::Int, .intValue = 1}};
  ERecordUpdate update{.record = std::move(ctor), .fields = {}};
  update.fields.emplace_back("a", std::move(one));
  auto result  = std::make_unique<ExprNode>();
  result->node = std::move(update);
  prog->result = std::move(result);
  const auto v = rig.compiler.evaluateProgram(*prog);
  ASSERT_FALSE(v.has_value());
  EXPECT_NE(v.error().detail.find("VM lowering is not implemented for VLace"),
            std::string::npos);
  // The declaration itself registered: its field names are the L4 tables.
  EXPECT_EQ(rig.compiler.lowerState().ctors().fieldOwner("a"),
            std::optional<std::string>{"P"});
}

} // namespace
