/**
 * @file vprolog_compiler_test.cpp
 * @brief Unit tests for VProlog AST compiler lowering to Vortex/Vlog.
 */
#include "common/xanadu/vprolog/compiler.hpp"

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vprolog;
using namespace zigzag;

struct CompilerTestHarness {
  ArenaManifold arena;
  vortex::VortexCore core{arena};
  Compiler compiler{core};
};

TEST(VPrologCompilerTest, LowerAtomsNumbersAndStrings) {
  CompilerTestHarness h;
  std::unordered_map<std::string, CellRef> varMap;

  // Atom
  Term atomTerm{Atom{"hello"}};
  CellRef atomCell = h.compiler.lowerTerm(atomTerm, varMap);
  EXPECT_NE(atomCell, noCell);
  EXPECT_EQ(h.core.arena().textOf(atomCell), "hello");

  // Integer
  Term intTerm{Number{static_cast<std::int64_t>(42)}};
  CellRef intCell = h.compiler.lowerTerm(intTerm, varMap);
  EXPECT_NE(intCell, noCell);
  EXPECT_EQ(h.core.arena().asInt64(intCell), 42);

  // Float
  Term floatTerm{Number{3.14}};
  CellRef floatCell = h.compiler.lowerTerm(floatTerm, varMap);
  EXPECT_NE(floatCell, noCell);
  EXPECT_DOUBLE_EQ(*h.core.arena().asDouble(floatCell), 3.14);

  // String
  Term strTerm{String{"prolog_string"}};
  CellRef strCell = h.compiler.lowerTerm(strTerm, varMap);
  EXPECT_NE(strCell, noCell);
  EXPECT_EQ(h.core.arena().textOf(strCell), "prolog_string");
}

TEST(VPrologCompilerTest, LowerVariablesAndScoping) {
  CompilerTestHarness h;
  std::unordered_map<std::string, CellRef> varMap;

  // Anonymous variable _
  Term anon1{Var{.name = "_", .isAnonymous = true}};
  Term anon2{Var{.name = "_", .isAnonymous = true}};
  CellRef a1 = h.compiler.lowerTerm(anon1, varMap);
  CellRef a2 = h.compiler.lowerTerm(anon2, varMap);
  EXPECT_NE(a1, noCell);
  EXPECT_NE(a2, noCell);
  EXPECT_NE(a1, a2); // Each anonymous var is distinct

  // Named variable X repeated
  Term varX1{Var{.name = "X", .isAnonymous = false}};
  Term varX2{Var{.name = "X", .isAnonymous = false}};
  CellRef x1 = h.compiler.lowerTerm(varX1, varMap);
  CellRef x2 = h.compiler.lowerTerm(varX2, varMap);
  EXPECT_EQ(x1, x2); // Shares same cell reference

  // Named variable Y
  Term varY{Var{.name = "Y", .isAnonymous = false}};
  CellRef y = h.compiler.lowerTerm(varY, varMap);
  EXPECT_NE(x1, y);

  // Variable name is attached on core.dims().name
  CellRef nameCell = h.core.arena().linked(x1, h.core.dims().name, false);
  EXPECT_NE(nameCell, noCell);
  EXPECT_EQ(h.core.arena().textOf(nameCell), "X");
}

TEST(VPrologCompilerTest, LowerCompoundsAndLists) {
  CompilerTestHarness h;
  std::unordered_map<std::string, CellRef> varMap;

  // Compound: point(10, 20)
  Compound pt;
  pt.functor = "point";
  pt.args.push_back(Term{Number{static_cast<std::int64_t>(10)}});
  pt.args.push_back(Term{Number{static_cast<std::int64_t>(20)}});
  CellRef ptCell = h.compiler.lowerTerm(Term{pt}, varMap);

  EXPECT_EQ(h.core.arena().textOf(ptCell), "point");
  auto args = h.compiler.vlog().argumentsOf(ptCell);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(h.core.arena().asInt64(args[0]), 10);
  EXPECT_EQ(h.core.arena().asInt64(args[1]), 20);

  // List: [a, b]
  List lst;
  lst.elements.push_back(Term{Atom{"a"}});
  lst.elements.push_back(Term{Atom{"b"}});
  CellRef lstCell = h.compiler.lowerTerm(Term{lst}, varMap);

  EXPECT_EQ(h.compiler.vlog().m.textOf(lstCell), ".");
  auto listArgs = h.compiler.vlog().argumentsOf(lstCell);
  ASSERT_EQ(listArgs.size(), 2u);
  EXPECT_EQ(h.core.arena().textOf(listArgs[0]), "a");
}

TEST(VPrologCompilerTest, CompileFactAndRuleClauses) {
  CompilerTestHarness h;

  // Fact: parent(pam, bob).
  Parser p1("parent(pam, bob).");
  Clause factClause = p1.parseClause();
  CellRef c1        = h.compiler.compileClause(factClause);
  EXPECT_NE(c1, noCell);

  // Rule: ancestor(X, Y) :- parent(X, Y).
  Parser p2("ancestor(X, Y) :- parent(X, Y).");
  Clause ruleClause = p2.parseClause();
  CellRef c2        = h.compiler.compileClause(ruleClause);
  EXPECT_NE(c2, noCell);

  // Predicate list should have "parent" and "ancestor"
  auto customPreds = h.compiler.customPredicates();
  EXPECT_EQ(customPreds.size(), 2u);
}

TEST(VPrologCompilerTest, CompileAndSolveFactsQuery) {
  CompilerTestHarness h;

  const std::string progSrc = R"(
    father(john, mary).
    father(john, tom).
    father(bob, john).
  )";

  Parser parser(progSrc);
  Program prog = parser.parseProgram();
  h.compiler.compileProgram(prog);

  // Query 1: ?- father(john, Child).
  CompiledQuery q1 = h.compiler.compileQuery("father(john, Child)");
  ASSERT_EQ(q1.variables.size(), 1u);
  EXPECT_EQ(q1.variables[0].first, "Child");

  auto sols1 = h.compiler.solve(q1);
  ASSERT_EQ(sols1.size(), 2u);
  EXPECT_EQ(sols1[0].formatted["Child"], "mary");
  EXPECT_EQ(sols1[1].formatted["Child"], "tom");

  // Query 2: ?- father(Who, mary).
  CompiledQuery q2 = h.compiler.compileQuery("father(Who, mary)");
  auto sols2       = h.compiler.solve(q2);
  ASSERT_EQ(sols2.size(), 1u);
  EXPECT_EQ(sols2[0].formatted["Who"], "john");

  // Query 3: ?- father(nobody, mary). -> 0 solutions
  CompiledQuery q3 = h.compiler.compileQuery("father(nobody, mary)");
  EXPECT_FALSE(h.compiler.solveOnce(q3));
}

TEST(VPrologCompilerTest, CompileAndSolveRulesAndRecursion) {
  CompilerTestHarness h;

  const std::string progSrc = R"(
    parent(pam, bob).
    parent(bob, ann).
    parent(bob, pat).

    ancestor(X, Y) :- parent(X, Y).
    ancestor(X, Y) :- parent(X, Z), ancestor(Z, Y).
  )";

  Parser parser(progSrc);
  h.compiler.compileProgram(parser.parseProgram());

  // Query: ?- ancestor(pam, Descendant).
  CompiledQuery q = h.compiler.compileQuery("ancestor(pam, Descendant)");
  auto sols       = h.compiler.solve(q);

  // Descendants of pam: bob (direct), ann (via bob), pat (via bob)
  ASSERT_EQ(sols.size(), 3u);
  std::vector<std::string> results;
  for (const auto &sol : sols) {
    results.push_back(sol.formatted.at("Descendant"));
  }
  EXPECT_TRUE(std::find(results.begin(), results.end(), "bob") !=
              results.end());
  EXPECT_TRUE(std::find(results.begin(), results.end(), "ann") !=
              results.end());
  EXPECT_TRUE(std::find(results.begin(), results.end(), "pat") !=
              results.end());
}

TEST(VPrologCompilerTest, CompileAndSolvePrologAppend) {
  CompilerTestHarness h;

  const std::string progSrc = R"(
    my_append([], L, L).
    my_append([H | T], L, [H | R]) :- my_append(T, L, R).
  )";

  Parser parser(progSrc);
  h.compiler.compileProgram(parser.parseProgram());

  // Concatenation: ?- my_append([1, 2], [3, 4], Out).
  CompiledQuery q1 = h.compiler.compileQuery("my_append([1, 2], [3, 4], Out)");
  auto sols1       = h.compiler.solve(q1);
  ASSERT_EQ(sols1.size(), 1u);
  EXPECT_EQ(sols1[0].formatted["Out"], "[1, 2, 3, 4]");

  // Reversible splitting: ?- my_append(A, B, [x, y]).
  CompiledQuery q2 = h.compiler.compileQuery("my_append(A, B, [x, y])");
  auto sols2       = h.compiler.solve(q2);
  ASSERT_EQ(sols2.size(), 3u);
  EXPECT_EQ(sols2[0].formatted["A"], "[]");
  EXPECT_EQ(sols2[0].formatted["B"], "[x, y]");
  EXPECT_EQ(sols2[1].formatted["A"], "[x]");
  EXPECT_EQ(sols2[1].formatted["B"], "[y]");
  EXPECT_EQ(sols2[2].formatted["A"], "[x, y]");
  EXPECT_EQ(sols2[2].formatted["B"], "[]");
}

TEST(VPrologCompilerTest, SolveConjunctionAndBuiltins) {
  CompilerTestHarness h;

  const std::string progSrc = R"(
    color(red).
    color(green).
    color(blue).
  )";

  Parser parser(progSrc);
  h.compiler.compileProgram(parser.parseProgram());

  // Conjunction: ?- color(C), C = green.
  CompiledQuery q1 = h.compiler.compileQuery("color(C), C = green");
  auto sols1       = h.compiler.solve(q1);
  ASSERT_EQ(sols1.size(), 1u);
  EXPECT_EQ(sols1[0].formatted["C"], "green");

  // Arithmetic: ?- X is 10 + 5 * 2.
  CompiledQuery q2 = h.compiler.compileQuery("X is 10 + 5 * 2");
  auto sols2       = h.compiler.solve(q2);
  ASSERT_EQ(sols2.size(), 1u);
  EXPECT_EQ(sols2[0].formatted["X"], "20");

  // Comparison: ?- 42 > 10.
  CompiledQuery q3 = h.compiler.compileQuery("42 > 10");
  EXPECT_TRUE(h.compiler.solveOnce(q3));

  // Negation: ?- \+ (1 = 2).
  CompiledQuery q4 = h.compiler.compileQuery(R"(\+ (1 = 2))");
  EXPECT_TRUE(h.compiler.solveOnce(q4));

  // Negation failure: ?- \+ (1 = 1).
  CompiledQuery q5 = h.compiler.compileQuery(R"(\+ (1 = 1))");
  EXPECT_FALSE(h.compiler.solveOnce(q5));
}

TEST(VPrologCompilerTest, IntrospectVortexStdLib) {
  CompilerTestHarness h;

  // Query all modules
  CompiledQuery qMods = h.compiler.compileQuery("vortex_module(Mod)");
  auto modSols        = h.compiler.solve(qMods);
  EXPECT_GE(modSols.size(), 7u);

  // Query math functions
  CompiledQuery qMath =
      h.compiler.compileQuery("vortex_function('std:math', Fn)");
  auto mathSols = h.compiler.solve(qMath);
  EXPECT_GE(mathSols.size(), 6u);
  bool foundAbs = false;
  for (const auto &s : mathSols) {
    if (s.formatted.at("Fn") == "abs") foundAbs = true;
  }
  EXPECT_TRUE(foundAbs);

  // Query 3-arity function path
  CompiledQuery qPath =
      h.compiler.compileQuery("vortex_function('std:string', Fn, Path)");
  auto pathSols = h.compiler.solve(qPath);
  EXPECT_GE(pathSols.size(), 3u);
  bool foundTrim = false;
  for (const auto &s : pathSols) {
    if (s.formatted.at("Fn") == "trim" &&
        s.formatted.at("Path") == "std:string/trim") {
      foundTrim = true;
    }
  }
  EXPECT_TRUE(foundTrim);
}

TEST(VPrologCompilerTest, IntrospectVortexInstructionsAndPipelines) {
  CompilerTestHarness h;

  // std:math/clamp is composed of a 2-instruction pipeline:
  // #CLAMP_MAX -> #CLAMP_MIN
  CompiledQuery qClamp = h.compiler.compileQuery(
      "vortex_instruction('std:math/clamp', Step, Label)");
  auto clampSols = h.compiler.solve(qClamp);
  ASSERT_EQ(clampSols.size(), 2u);
  EXPECT_EQ(clampSols[0].formatted.at("Step"), "0");
  EXPECT_EQ(clampSols[0].formatted.at("Label"), "#CLAMP_MAX");
  EXPECT_EQ(clampSols[1].formatted.at("Step"), "1");
  EXPECT_EQ(clampSols[1].formatted.at("Label"), "#CLAMP_MIN");

  // std:string/clean is composed of #STR_TRIM -> #STR_TO_LOWER
  CompiledQuery qClean = h.compiler.compileQuery(
      "vortex_instruction('std:string/clean', Step, Label)");
  auto cleanSols = h.compiler.solve(qClean);
  ASSERT_EQ(cleanSols.size(), 2u);
  EXPECT_EQ(cleanSols[0].formatted.at("Label"), "#STR_TRIM");
  EXPECT_EQ(cleanSols[1].formatted.at("Label"), "#STR_TO_LOWER");
}

TEST(VPrologCompilerTest, IntrospectVortexContractsAndParams) {
  CompilerTestHarness h;

  // std:math/div has a precondition: #REQUIRE_NON_ZERO
  CompiledQuery qDiv =
      h.compiler.compileQuery("vortex_contract('std:math/div', Type, Label)");
  auto divSols = h.compiler.solve(qDiv);
  ASSERT_GE(divSols.size(), 1u);
  EXPECT_EQ(divSols[0].formatted.at("Type"), "precondition");
  EXPECT_EQ(divSols[0].formatted.at("Label"), "#REQUIRE_NON_ZERO");

  // std:math/abs has a postcondition: #REQUIRE_NON_NEGATIVE
  CompiledQuery qAbs =
      h.compiler.compileQuery("vortex_contract('std:math/abs', Type, Label)");
  auto absSols = h.compiler.solve(qAbs);
  ASSERT_GE(absSols.size(), 1u);
  EXPECT_EQ(absSols[0].formatted.at("Type"), "postcondition");
  EXPECT_EQ(absSols[0].formatted.at("Label"), "#REQUIRE_NON_NEGATIVE");

  // std:math/max has input and output wings introspectable via vortex_param/4
  CompiledQuery qParams = h.compiler.compileQuery(
      "vortex_param('std:math/max', Wing, Slot, Target)");
  auto paramSols = h.compiler.solve(qParams);
  // max has 2 inputs and 1 output
  ASSERT_EQ(paramSols.size(), 3u);
  EXPECT_EQ(paramSols[0].formatted.at("Wing"), "input");
  EXPECT_EQ(paramSols[0].formatted.at("Slot"), "0");
  EXPECT_EQ(paramSols[1].formatted.at("Wing"), "input");
  EXPECT_EQ(paramSols[1].formatted.at("Slot"), "1");
  EXPECT_EQ(paramSols[2].formatted.at("Wing"), "output");
  EXPECT_EQ(paramSols[2].formatted.at("Slot"), "0");
}

} // namespace
