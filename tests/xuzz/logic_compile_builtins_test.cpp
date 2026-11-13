/**
 * @file logic_compile_builtins_test.cpp
 * @brief Card P2: the control and arithmetic builtins, end to end through the
 * compiled VM resolution (vprolog::Compiler::solve over the lowering).
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace zigzag;
using namespace zigzag::vortex;

struct Prolog {
  ArenaManifold arena;
  VortexCore core{arena};
  xanadu::vprolog::Compiler compiler{core};

  void consult(const std::string &source) {
    xanadu::vprolog::Parser parser(source);
    compiler.compileProgram(parser.parseProgram());
  }
  std::vector<std::string> all(const std::string &query, const std::string &var,
                               std::size_t max = 100) {
    const auto compiled = compiler.compileQuery(query);
    std::vector<std::string> out;
    for (const auto &sol : compiler.solve(compiled, max)) {
      const auto it = sol.formatted.find(var);
      out.push_back(it == sol.formatted.end() ? "?" : it->second);
    }
    return out;
  }
  // The query aborts with a std::runtime_error whose text contains every
  // fragment; nothing else (not a quiet failure) is acceptable.
  void raises(const std::string &query,
              const std::vector<std::string> &fragments) {
    try {
      (void)compiler.solve(compiler.compileQuery(query), 1);
      ADD_FAILURE() << query << " did not abort";
    } catch (const std::runtime_error &err) {
      for (const auto &fragment : fragments)
        EXPECT_NE(std::string(err.what()).find(fragment), std::string::npos)
            << query << ": " << err.what();
    }
  }
  bool holds(const std::string &query) {
    return !compiler.solve(compiler.compileQuery(query), 1).empty();
  }
};

using Strings = std::vector<std::string>;

TEST(LogicCompileBuiltinsTest, TrueSucceedsAndFailFails) {
  Prolog p;
  p.consult("ok :- true.\nno :- fail.\nyes(1).\n");
  EXPECT_TRUE(p.holds("true."));
  EXPECT_FALSE(p.holds("fail."));
  EXPECT_TRUE(p.holds("ok."));
  EXPECT_FALSE(p.holds("no."));
  EXPECT_EQ(p.all("yes(X), true.", "X"), (Strings{"1"}));
  EXPECT_TRUE(p.all("yes(X), fail.", "X").empty());
}

TEST(LogicCompileBuiltinsTest, IsOnIntegersFloatsAndMixed) {
  Prolog p;
  EXPECT_EQ(p.all("X is 1 + 2.", "X"), (Strings{"3"}));
  EXPECT_EQ(p.all("X is 7 - 10.", "X"), (Strings{"-3"}));
  EXPECT_EQ(p.all("X is 6 * 7.", "X"), (Strings{"42"}));
  EXPECT_EQ(p.all("X is 1.5 + 2.25.", "X"), (Strings{"3.75"}));
  EXPECT_EQ(p.all("X is 1 + 2.5.", "X"), (Strings{"3.5"}));
  EXPECT_EQ(p.all("X is 2.5 * 2.", "X"), (Strings{"5"}));
  EXPECT_EQ(p.all("X is -(3).", "X"), (Strings{"-3"}));
  EXPECT_EQ(p.all("X is -(2.5).", "X"), (Strings{"-2.5"}));
  EXPECT_EQ(p.all("X is 2 + 3 * 4 - 1.", "X"), (Strings{"13"}));
}

TEST(LogicCompileBuiltinsTest, DivisionRules) {
  Prolog p;
  // `/` is a float quotient even on two integers.
  EXPECT_EQ(p.all("X is 7 / 2.", "X"), (Strings{"3.5"}));
  EXPECT_EQ(p.all("X is 6 / 3.", "X"), (Strings{"2"}));
  EXPECT_EQ(p.all("X is 7 // 2.", "X"), (Strings{"3"}));
  EXPECT_EQ(p.all("X is 7 mod 3.", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("X is 7 rem 3.", "X"), (Strings{"1"}));
  // A zero divisor aborts the query naming the goal and the fault.
  p.raises("X is 1 / 0.",
           {"evaluation error", "zero divisor", "is(X, /(1, 0))"});
  p.raises("X is 1 // 0.", {"zero divisor", "is(X, //(1, 0))"});
  p.raises("X is 1 mod 0.", {"zero divisor", "is(X, mod(1, 0))"});
  p.raises("X is 1 rem 0.", {"zero divisor"});
  p.raises("X is 1 / 0.0.", {"zero divisor"});
  p.raises("1 < 5 // 0.", {"zero divisor", "<(1, //(5, 0))"});
  // The integer divisor is the truncated operand, so 0.5 is a zero divisor.
  p.raises("X is 5 // 0.5.", {"zero divisor"});
}

TEST(LogicCompileBuiltinsTest, IsChecksAnAlreadyBoundLeftSide) {
  Prolog p;
  EXPECT_TRUE(p.holds("3 is 1 + 2."));
  EXPECT_FALSE(p.holds("4 is 1 + 2."));
  EXPECT_TRUE(p.holds("X = 5, X is 2 + 3."));
  EXPECT_FALSE(p.holds("X = 6, X is 2 + 3."));
  EXPECT_EQ(p.all("Y is 2 * 3, Z is Y + 1.", "Z"), (Strings{"7"}));
}

TEST(LogicCompileBuiltinsTest, ComparisonsSucceedAndFail) {
  Prolog p;
  EXPECT_TRUE(p.holds("1 < 2."));
  EXPECT_FALSE(p.holds("2 < 2."));
  EXPECT_TRUE(p.holds("3 > 2."));
  EXPECT_FALSE(p.holds("2 > 2."));
  EXPECT_TRUE(p.holds("2 =< 2."));
  EXPECT_FALSE(p.holds("3 =< 2."));
  EXPECT_TRUE(p.holds("2 >= 2."));
  EXPECT_FALSE(p.holds("1 >= 2."));
  EXPECT_TRUE(p.holds("2 =:= 2."));
  EXPECT_FALSE(p.holds("2 =:= 3."));
  EXPECT_TRUE(p.holds("2 =\\= 3."));
  EXPECT_FALSE(p.holds("2 =\\= 2."));
}

TEST(LogicCompileBuiltinsTest, ComparisonsMixIntegersAndFloats) {
  Prolog p;
  EXPECT_TRUE(p.holds("1 =:= 1.0."));
  EXPECT_FALSE(p.holds("1 =\\= 1.0."));
  EXPECT_TRUE(p.holds("1 < 1.5."));
  EXPECT_TRUE(p.holds("2.5 > 2."));
  EXPECT_TRUE(p.holds("1 + 1 =:= 4 / 2."));
  EXPECT_TRUE(p.holds("2 * 3 > 5.5."));
}

TEST(LogicCompileBuiltinsTest, UnboundAndNonNumericOperandsAbortTheQuery) {
  Prolog p;
  p.raises("X is Y + 1.", {"instantiation error", "Y", "is(X, +(Y, 1))"});
  p.raises("Y < 3.", {"instantiation error", "<(Y, 3)"});
  p.raises("Y =:= 0.", {"instantiation error"});
  p.raises("X is foo + 1.",
           {"type error", "foo", "not a number", "is(X, +(foo, 1))"});
  p.raises("X is foo.", {"type error", "foo"});
  p.raises("X is f(1).", {"type error", "f(1)"});
  p.raises("foo < 3.", {"type error", "foo"});
  p.raises("foo =\\= 1.", {"type error"});
  // A variable bound at run time is reported without the goal text.
  p.raises("X = foo, X >= 0.", {"type error", "non-number"});
  p.raises("X = foo, Y is X * 2.", {"type error"});
  p.raises("X is '5' + 1.", {"type error", "5", "not a number"});
  // An error is not a failure: no later alternative or negation absorbs it.
  p.consult("pick(a).\npick(1).\npick(2).\n");
  p.raises("pick(N), N > 0.", {"type error", "a"});
  p.raises("\\+ X is Y.", {"instantiation error"});
}

TEST(LogicCompileBuiltinsTest, NegationAsFailureLeavesNoBindings) {
  Prolog p;
  p.consult("p(1).\np(2).\nq(2).\n");
  EXPECT_EQ(p.all("p(X), \\+ q(X).", "X"), (Strings{"1"}));
  EXPECT_FALSE(p.holds("\\+ p(1)."));
  EXPECT_TRUE(p.holds("\\+ p(3)."));
  EXPECT_TRUE(p.holds("\\+ fail."));
  EXPECT_FALSE(p.holds("\\+ true."));
  // The unifier of the negated goal is undone: X stays free afterwards.
  EXPECT_EQ(p.all("\\+ \\+ p(X), Y = 1.", "Y"), (Strings{"1"}));
  EXPECT_EQ(p.all("\\+ \\+ X = 1, X = 2.", "X"), (Strings{"2"}));
  EXPECT_EQ(p.all("\\+ X = 1.", "X").size(), 0u);
  EXPECT_EQ(p.all("\\+ 1 > 2, X = ok.", "X"), (Strings{"ok"}));
}

TEST(LogicCompileBuiltinsTest, NestedNegationAndCutInsideIt) {
  Prolog p;
  p.consult("p(1).\np(2).\nq(2).\n"
            // The cut is local to the negated goal: it must not remove the
            // alternatives of the clause that holds the \+.
            "a(X) :- p(X), \\+ (q(X), !, fail).\n"
            "b(X) :- p(X), \\+ (!, fail).\n"
            "c(X) :- p(X), \\+ \\+ q(X).\n");
  // \+ (q(X), !, fail) always succeeds (its goal can never succeed), and the
  // cut inside it must not have removed p(X)'s second alternative.
  EXPECT_EQ(p.all("a(X).", "X"), (Strings{"1", "2"}));
  EXPECT_EQ(p.all("p(X), \\+ (q(X), !).", "X"), (Strings{"1"}));
  // \+ (!, fail) succeeds for every X: the cut is local, the negation holds.
  EXPECT_EQ(p.all("b(X).", "X"), (Strings{"1", "2"}));
  EXPECT_EQ(p.all("c(X).", "X"), (Strings{"2"}));
  EXPECT_TRUE(p.holds("\\+ (!, fail)."));
  EXPECT_TRUE(p.holds("\\+ \\+ \\+ fail."));
  EXPECT_EQ(p.all("p(X), \\+ (p(Y), Y > X).", "X"), (Strings{"2"}));
}

TEST(LogicCompileBuiltinsTest, NotUnifiable) {
  Prolog p;
  EXPECT_TRUE(p.holds("a \\= b."));
  EXPECT_FALSE(p.holds("a \\= a."));
  EXPECT_FALSE(p.holds("X \\= a."));
  EXPECT_TRUE(p.holds("f(a, b) \\= f(a, c)."));
  EXPECT_FALSE(p.holds("f(X, b) \\= f(a, b)."));
  EXPECT_TRUE(p.holds("f(X, X) \\= f(a, b)."));
  // It never binds: X is still free for the next goal.
  EXPECT_EQ(p.all("a \\= b, X = 1.", "X"), (Strings{"1"}));
  p.consult("p(1).\np(2).\n");
  EXPECT_EQ(p.all("p(X), X \\= 1.", "X"), (Strings{"2"}));
}

TEST(LogicCompileBuiltinsTest, CutPrunesAlternativesAndKeepsBindings) {
  Prolog p;
  p.consult("m(1).\nm(2).\nm(3).\n"
            "first(X) :- m(X), !.\n"
            "big(X) :- m(X), X >= 2, !.\n"
            "cls(X, small) :- X < 2, !.\n"
            "cls(X, mid) :- X < 3, !.\n"
            "cls(_, big).\n");
  EXPECT_EQ(p.all("first(X).", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("big(X).", "X"), (Strings{"2"}));
  EXPECT_EQ(p.all("cls(1, C).", "C"), (Strings{"small"}));
  EXPECT_EQ(p.all("cls(2, C).", "C"), (Strings{"mid"}));
  EXPECT_EQ(p.all("cls(9, C).", "C"), (Strings{"big"}));
  // The caller's own choices survive a callee's cut.
  EXPECT_EQ(p.all("m(Y), first(X).", "Y"), (Strings{"1", "2", "3"}));
  // A query-level cut prunes what is to its left and goals to its right run.
  EXPECT_EQ(p.all("m(X), !.", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("m(X), !, m(Y).", "Y"), (Strings{"1", "2", "3"}));
}

TEST(LogicCompileBuiltinsTest, CutInsideABuiltinHeavyClause) {
  Prolog p;
  p.consult("sign(N, neg) :- N < 0, !.\n"
            "sign(N, zero) :- N =:= 0, !.\n"
            "sign(N, pos) :- N > 0.\n"
            "abs2(N, A) :- N < 0, !, A is 0 - N.\n"
            "abs2(N, N).\n"
            "classify(N, odd) :- 1 =:= N mod 2, !.\n"
            "classify(_, even).\n"
            "guard(N, R) :- \\+ N > 5, !, R = small.\n"
            "guard(_, large).\n");
  EXPECT_EQ(p.all("sign(-4, S).", "S"), (Strings{"neg"}));
  EXPECT_EQ(p.all("sign(0, S).", "S"), (Strings{"zero"}));
  EXPECT_EQ(p.all("sign(4, S).", "S"), (Strings{"pos"}));
  EXPECT_EQ(p.all("abs2(-3, A).", "A"), (Strings{"3"}));
  EXPECT_EQ(p.all("abs2(3, A).", "A"), (Strings{"3"}));
  EXPECT_EQ(p.all("classify(7, K).", "K"), (Strings{"odd"}));
  EXPECT_EQ(p.all("classify(8, K).", "K"), (Strings{"even"}));
  EXPECT_EQ(p.all("guard(3, R).", "R"), (Strings{"small"}));
  EXPECT_EQ(p.all("guard(9, R).", "R"), (Strings{"large"}));
}

TEST(LogicCompileBuiltinsTest, ArithmeticRecursionReturnsThroughFrames) {
  Prolog p;
  p.consult("fact(0, 1) :- !.\n"
            "fact(N, F) :- N1 is N - 1, fact(N1, F1), F is N * F1.\n"
            "count(N, N) :- !.\n"
            "count(I, N) :- J is I + 1, count(J, N).\n"
            "sum([], 0).\n"
            "sum([H|T], S) :- sum(T, S1), S is S1 + H.\n"
            "fib(0, 0).\n"
            "fib(1, 1).\n"
            "fib(N, F) :- N > 1, A is N - 1, B is N - 2, fib(A, FA), "
            "fib(B, FB), F is FA + FB.\n");
  EXPECT_EQ(p.all("fact(10, F).", "F"), (Strings{"3628800"}));
  EXPECT_EQ(p.all("count(0, 200).", "X").size(), 1u);
  EXPECT_EQ(p.all("sum([1, 2, 3, 4], S).", "S"), (Strings{"10"}));
  EXPECT_EQ(p.all("fib(12, F).", "F"), (Strings{"144"}));
}

TEST(LogicCompileBuiltinsTest, BudgetHitInsideArithmeticRecursionIsAnError) {
  Prolog p;
  p.consult("spin(N) :- M is N + 1, spin(M).\n");
  p.compiler.stdlib().setLogicCycleBudget(3000);
  const auto query = p.compiler.compileQuery("spin(0).");
  try {
    (void)p.compiler.solveOnce(query);
    FAIL() << "the budget was not reported";
  } catch (const std::runtime_error &err) {
    EXPECT_NE(std::string(err.what()).find("exceeded its cycle budget"),
              std::string::npos)
        << err.what();
  }
  // The VM is usable afterwards.
  p.compiler.stdlib().setLogicCycleBudget(100000);
  EXPECT_EQ(p.all("X is 2 + 2.", "X"), (Strings{"4"}));
}

TEST(LogicCompileBuiltinsTest, DefaultBudgetIsTenMillionCycles) {
  Prolog p;
  EXPECT_EQ(p.compiler.stdlib().logicCycleBudget(), 10'000'000u);
}

TEST(LogicCompileBuiltinsTest, ClauseForABuiltinIsRefusedAtConsult) {
  Prolog p;
  const auto refused = [&](const std::string &source, const std::string &name) {
    try {
      p.consult(source);
      ADD_FAILURE() << source << " was accepted";
    } catch (const logic::LogicRefusal &err) {
      EXPECT_NE(std::string(err.what()).find("cannot redefine builtin " + name),
                std::string::npos)
          << err.what();
    }
  };
  refused("X is Y.\n", "is/2");
  refused("\\+ a.\n", "\\+/1");
  refused("','(a, b).\n", ",/2");
  refused("true.\n", "true/0");
  refused("fail :- true.\n", "fail/0");
  refused("1 < 2.\n", "</2");
  refused("'$eval'(a, b).\n", "$eval/2");
  // The refused clauses left nothing behind.
  EXPECT_TRUE(p.holds("true."));
  EXPECT_FALSE(p.holds("fail."));
  EXPECT_FALSE(p.holds("\\+ true."));
}

// Arithmetic on a variable bound at run time to a compound is evaluated.
TEST(LogicCompileBuiltinsTest, RuntimeBoundExpressionsAreEvaluated) {
  Prolog p;
  EXPECT_EQ(p.all("E = 1+2, X is E.", "X"), (Strings{"3"}));
  EXPECT_EQ(p.all("E = (1+2)*F, F = 4.0, X is E.", "X"), (Strings{"12"}));
  EXPECT_EQ(p.all("E = 7 mod 3, X is E + 1.", "X"), (Strings{"2"}));
  EXPECT_EQ(p.all("E = -(4), X is E * 2.", "X"), (Strings{"-8"}));
  EXPECT_EQ(p.all("A = 2, B = A*3, C = B+A, X is C.", "X"), (Strings{"8"}));
  EXPECT_EQ(p.all("E = 7 / 2, X is E.", "X"), (Strings{"3.5"}));
  EXPECT_TRUE(p.holds("E = 2*3, E > 5."));
  EXPECT_FALSE(p.holds("E = 2*3, E > 6."));
  EXPECT_TRUE(p.holds("E = 2*3, F = 3+3, E =:= F."));
  EXPECT_TRUE(p.holds("E = 2*3, 5 < E."));
  EXPECT_TRUE(p.holds("E = 1+1, E =\\= 3."));
  EXPECT_TRUE(p.holds("E = 1+1, 2 is E."));
  p.consult("val(X, V) :- V is X.\n"
            "pair(E, F) :- E < F.\n");
  EXPECT_EQ(p.all("val(3*4, V).", "V"), (Strings{"12"}));
  EXPECT_TRUE(p.holds("pair(1+1, 2*3)."));
}

TEST(LogicCompileBuiltinsTest, RuntimeEvaluationAbortsOnNonNumbers) {
  Prolog p;
  p.raises("E = 1+Y, X is E.", {"instantiation error"});
  p.raises("E = foo, X is E.", {"type error"});
  p.raises("E = 1+foo, X is E.", {"type error"});
  p.raises("E = f(1), X is E.", {"type error"});
  p.raises("E = 1+f(2), X is E.", {"type error"});
  p.raises("E = foo, E > 1.", {"type error"});
  p.raises("E = 1+Y, E > 1.", {"instantiation error"});
  p.raises("A = '5', X is A + 1.", {"type error"});
  p.raises("E = 1/0, X is E.", {"zero divisor"});
  p.raises("E = 1 mod 0, X is E.", {"zero divisor"});
  p.raises("E = 3+true, X is E.", {"type error"});
  // The engine is usable after an abort.
  EXPECT_EQ(p.all("X is 1 + 1.", "X"), (Strings{"2"}));
}

TEST(LogicCompileBuiltinsTest, DeepRuntimeExpressionRespectsTheCycleBudget) {
  Prolog p;
  p.consult("sumto(0, 0) :- !.\n"
            "sumto(N, E+N) :- N1 is N - 1, sumto(N1, E).\n");
  EXPECT_EQ(p.all("sumto(30, E), X is E.", "X"), (Strings{"465"}));
  p.compiler.stdlib().setLogicCycleBudget(1500);
  const auto query = p.compiler.compileQuery("sumto(200, E), X is E.");
  try {
    (void)p.compiler.solveOnce(query);
    FAIL() << "the budget was not reported";
  } catch (const std::runtime_error &err) {
    EXPECT_NE(std::string(err.what()).find("exceeded its cycle budget"),
              std::string::npos)
        << err.what();
  }
  p.compiler.stdlib().setLogicCycleBudget(kDefaultLogicCycleBudget);
  EXPECT_EQ(p.all("X is 2 + 2.", "X"), (Strings{"4"}));
}

TEST(LogicCompileBuiltinsTest, TypeTestsAndInternalGuards) {
  Prolog p;
  EXPECT_TRUE(p.holds("var(X)."));
  EXPECT_FALSE(p.holds("var(a)."));
  EXPECT_FALSE(p.holds("X = 1, var(X)."));
  EXPECT_TRUE(p.holds("X = f(Y), nonvar(X)."));
  EXPECT_FALSE(p.holds("nonvar(X)."));
  EXPECT_TRUE(p.holds("nonvar(3)."));
  EXPECT_TRUE(p.holds("'$var'(X)."));
  EXPECT_FALSE(p.holds("'$var'(1)."));
  EXPECT_TRUE(p.holds("'$number'(1)."));
  EXPECT_TRUE(p.holds("X = 2.5, '$number'(X)."));
  EXPECT_FALSE(p.holds("'$number'(a)."));
  EXPECT_FALSE(p.holds("'$number'(X)."));
  EXPECT_FALSE(p.holds("'$number'(1+2)."));
  // var/1 on an unbound argument does not bind it.
  EXPECT_EQ(p.all("var(X), X = 4.", "X"), (Strings{"4"}));
}

TEST(LogicCompileBuiltinsTest, ModTakesTheDivisorsSignAndRemTheDividends) {
  Prolog p;
  EXPECT_EQ(p.all("X is -7 mod 2.", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("X is 7 mod -2.", "X"), (Strings{"-1"}));
  EXPECT_EQ(p.all("X is -7 mod -2.", "X"), (Strings{"-1"}));
  EXPECT_EQ(p.all("X is 7 mod 2.", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("X is -6 mod 2.", "X"), (Strings{"0"}));
  EXPECT_EQ(p.all("X is -7 rem 2.", "X"), (Strings{"-1"}));
  EXPECT_EQ(p.all("X is 7 rem -2.", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("E = -7 mod 2, X is E.", "X"), (Strings{"1"}));
  EXPECT_EQ(p.all("A = -7, B = 2, X is A mod B.", "X"), (Strings{"1"}));
  // // truncates toward zero.
  EXPECT_EQ(p.all("X is -7 // 2.", "X"), (Strings{"-3"}));
  EXPECT_EQ(p.all("X is 7 // -2.", "X"), (Strings{"-3"}));
}

TEST(LogicCompileBuiltinsTest, IntegerOverflowAbortsTheQuery) {
  Prolog p;
  p.raises("X is 9223372036854775807 + 1.", {"overflow"});
  // The engine is usable afterwards.
  EXPECT_EQ(p.all("X is 2 + 2.", "X"), (Strings{"4"}));
}

TEST(LogicCompileBuiltinsTest, BoolOperandIsNotANumber) {
  Prolog p;
  // A Bool cell must not pass the numeric test as 1.
  auto &lib          = p.compiler.stdlib();
  const CellRef flag = p.arena.makeScalarCell(true);
  const CellRef var  = lib.makeVar();
  const CellRef eq   = lib.makeTerm("=", {var, flag});
  const CellRef sum =
      lib.makeTerm("+", {var, p.arena.makeScalarCell(std::int64_t{1})});
  const CellRef isGoal  = lib.makeTerm("is", {lib.makeVar(), sum});
  const CellRef goals[] = {eq, isGoal};
  EXPECT_THROW((void)lib.solveOnce(goals, p.compiler.customPredicates()),
               std::runtime_error);
  const CellRef cmp =
      lib.makeTerm(">", {var, p.arena.makeScalarCell(std::int64_t{0})});
  const CellRef cmpGoals[] = {eq, cmp};
  EXPECT_THROW((void)lib.solveOnce(cmpGoals, p.compiler.customPredicates()),
               std::runtime_error);
}

TEST(LogicCompileBuiltinsTest, CutInsideConjunctionUnderNegationIsLocal) {
  Prolog p;
  p.consult("p(1).\np(2).\n"
            "t(X) :- p(X), \\+ (X = 1, !, fail).\n"
            "u(X) :- p(X), \\+ (!, X = 1).\n");
  // The cut inside ','/2 under \+ is local to the negation: p's second
  // alternative survives it. t(1) is refused (the negated goal fails after
  // the cut, so \+ holds), t(2) holds.
  EXPECT_EQ(p.all("t(X).", "X"), (Strings{"1", "2"}));
  EXPECT_EQ(p.all("u(X).", "X"), (Strings{"2"}));
  EXPECT_EQ(p.all("p(X), \\+ (p(Y), !, Y > 1).", "X"), (Strings{"1", "2"}));
}

TEST(LogicCompileBuiltinsTest, DoubleNegationLeavesNoBindings) {
  Prolog p;
  p.consult("p(1).\np(2).\n");
  EXPECT_EQ(p.all("\\+ \\+ p(X), Z = free.", "Z"), (Strings{"free"}));
  EXPECT_EQ(p.all("\\+ \\+ (X = f(Y), Y = 2), Y = 3.", "Y"), (Strings{"3"}));
  EXPECT_EQ(p.all("\\+ \\+ X = 1, var(X), X = 9.", "X"), (Strings{"9"}));
  EXPECT_EQ(p.all("p(X), \\+ \\+ p(X).", "X"), (Strings{"1", "2"}));
}

} // namespace
