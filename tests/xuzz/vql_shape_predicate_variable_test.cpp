// Q-SHAPEPRED: a shape predicate or quantifier argument that names a variable,
// a function result or a function argument. The shape is resolved while the
// query is compiled, so each form lowers to exactly what its literal does and
// must answer over the same cells.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

/// Items a, b, c hang off home on d.items. a carries a tag, b a tag and a
/// keep, c a keep only; b's tag cell carries a d.sub link.
struct VQLShapePredicateVariableTest : testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  DimRef items = engine.resolveDimension("d.items");
  DimRef tag   = engine.resolveDimension("d.tag");
  DimRef keep  = engine.resolveDimension("d.keep");
  DimRef sub   = engine.resolveDimension("d.sub");
  CellRef a{arena.makeCell("a")};
  CellRef b{arena.makeCell("b")};
  CellRef c{arena.makeCell("c")};

  void SetUp() override {
    ASSERT_TRUE(arena.link(core.home(), items, DimVector::POS, a));
    ASSERT_TRUE(arena.link(a, items, DimVector::POS, b));
    ASSERT_TRUE(arena.link(b, items, DimVector::POS, c));
    ASSERT_TRUE(arena.link(a, tag, DimVector::POS, arena.makeCell("ta")));
    const auto tagB = arena.makeCell("tb");
    ASSERT_TRUE(arena.link(b, tag, DimVector::POS, tagB));
    ASSERT_TRUE(arena.link(tagB, sub, DimVector::POS, arena.makeCell("s")));
    ASSERT_TRUE(arena.link(b, keep, DimVector::POS, arena.makeCell("kb")));
    ASSERT_TRUE(arena.link(c, keep, DimVector::POS, arena.makeCell("kc")));
  }

  std::vector<CellRef> run(const std::string &query) {
    return engine.execute(query);
  }

  std::optional<bool> truthOf(const CellRef cell) {
    const auto value = core.render(cell);
    if (!value) return std::nullopt;
    if (const auto *flag = std::get_if<bool>(&*value)) return *flag;
    return std::nullopt;
  }

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::runtime_error &error) {
      return error.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }

  /// The variable form must answer exactly what the literal does, and the
  /// literal's answer is pinned so a form that is wrong for both cannot pass.
  void expectSame(const std::string &form, const std::string &literal,
                  const std::vector<CellRef> &expected) {
    SCOPED_TRACE(form);
    EXPECT_EQ(run(literal), expected) << "literal: " << literal;
    EXPECT_EQ(run(form), expected);
  }
};

TEST_F(VQLShapePredicateVariableTest, VariableRuleInAPathPredicate) {
  expectSame("let $s := { +d.tag } return ##/d.items[{ $s }]",
             "##/d.items[{ +d.tag }]", {a, b});
}

TEST_F(VQLShapePredicateVariableTest, VariableDerivedWithFurtherRules) {
  expectSame("let $s := { +d.tag } return ##/d.items[{ $s, +d.keep }]",
             "##/d.items[{ +d.tag, +d.keep }]", {b});
}

TEST_F(VQLShapePredicateVariableTest, VariableWildcardOrderIsKept) {
  expectSame("let $w := { !* } return ##/d.items[{ $w, ~d.items, +d.keep }]",
             "##/d.items[{ !*, ~d.items, +d.keep }]", {c});
  expectSame("let $s := { *, !d.tag } return ##/d.items[{ $s }]",
             "##/d.items[{ *, !d.tag }]", {c});
}

TEST_F(VQLShapePredicateVariableTest, FunctionResultInAPathPredicate) {
  expectSame("def tagged() := { +d.tag }; ##/d.items[{ tagged() }]",
             "##/d.items[{ +d.tag }]", {a, b});
}

TEST_F(VQLShapePredicateVariableTest, ShapeFunctionTakingAShapeArgument) {
  expectSame("def wider($s) := { $s, +d.keep }; "
             "let $t := { +d.tag } return ##/d.items[{ wider($t) }]",
             "##/d.items[{ +d.tag, +d.keep }]", {b});
}

TEST_F(VQLShapePredicateVariableTest, ShapeFunctionArgumentUsedAsAPredicate) {
  expectSame("def pick($m) := ##/d.items[{ $m }]; "
             "let $s := { +d.keep } return pick($s)",
             "##/d.items[{ +d.keep }]", {b, c});
  expectSame("def pick($m) := ##/d.items[{ $m }]; pick({ +d.keep })",
             "##/d.items[{ +d.keep }]", {b, c});
}

TEST_F(VQLShapePredicateVariableTest, ShapeArgumentUsedInAWhereOfADefinition) {
  EXPECT_EQ(run("def f($m) := { for $x in ##/d.items where $m return $x }; "
                "let $s := { +d.tag } return f($s)"),
            (std::vector<CellRef>{a, b}));
  EXPECT_EQ(run("def f($m) := ##/d.items[?$m]; f({ +d.keep })"),
            (std::vector<CellRef>{b, c}));
}

TEST_F(VQLShapePredicateVariableTest, NestedTargetNamesAShape) {
  expectSame("let $n := { +d.sub } return ##/d.items[{ +d.tag: { $n } }]",
             "##/d.items[{ +d.tag: { +d.sub } }]", {b});
  expectSame("let $n := { +d.sub } return ##/d.items[{ +d.tag: $n }]",
             "##/d.items[{ +d.tag: { +d.sub } }]", {b});
  expectSame("def inner() := { +d.sub }; ##/d.items[{ +d.tag: inner() }]",
             "##/d.items[{ +d.tag: { +d.sub } }]", {b});
}

TEST_F(VQLShapePredicateVariableTest, BareVariableAsABracketPredicate) {
  expectSame("let $s := { +d.tag } return ##/d.items[$s]",
             "##/d.items[{ +d.tag }]", {a, b});
  expectSame("def tagged() := { +d.tag }; ##/d.items[tagged()]",
             "##/d.items[{ +d.tag }]", {a, b});
}

TEST_F(VQLShapePredicateVariableTest, QuestionFormsInAWhereClause) {
  const std::string literal =
      "for $x in ##/d.items where ?{ +d.keep } return $x";
  EXPECT_EQ(run(literal), (std::vector<CellRef>{b, c}));
  EXPECT_EQ(run("let $s := { +d.keep }; for $x in ##/d.items where ?$s "
                "return $x"),
            (std::vector<CellRef>{b, c}));
  EXPECT_EQ(run("let $s := { +d.keep }; for $x in ##/d.items where ?{ $s } "
                "return $x"),
            (std::vector<CellRef>{b, c}));
  EXPECT_EQ(run("def k() := { +d.keep }; for $x in ##/d.items where ?k() "
                "return $x"),
            (std::vector<CellRef>{b, c}));
  EXPECT_EQ(run("def k() := { +d.keep }; for $x in ##/d.items where ?{ k() } "
                "return $x"),
            (std::vector<CellRef>{b, c}));
  EXPECT_EQ(run("let $s := { +d.keep }; for $x in ##/d.items where not ?$s "
                "return $x"),
            (std::vector<CellRef>{a}));
}

TEST_F(VQLShapePredicateVariableTest, QuantifierArgumentsThatNameAShape) {
  const auto truth = [&](const std::string &query) {
    const auto cells = run(query);
    EXPECT_EQ(cells.size(), 1u) << query;
    return cells.empty() ? std::optional<bool>{} : truthOf(cells.front());
  };
  // any/all/none over { +d.tag } (a, b of three items) and { +d.keep }.
  const struct {
    const char *name;
    bool tag;
    bool keep;
  } rows[] = {
      {"any", true, true}, {"all", false, false}, {"none", false, false}};
  for (const auto &row : rows) {
    const std::string name = row.name;
    SCOPED_TRACE(name);
    EXPECT_EQ(truth(name + "(##/d.items, { +d.tag })"), row.tag);
    for (const auto &form :
         {"let $s := { +d.tag } return " + name + "(##/d.items, $s)",
          "let $s := { +d.tag } return " + name + "(##/d.items, { $s })",
          "def t() := { +d.tag }; " + name + "(##/d.items, t())",
          "def t() := { +d.tag }; " + name + "(##/d.items, { t() })",
          "def q($m) := " + name + "(##/d.items, $m); q({ +d.tag })",
          "def q($m) := " + name +
              "(##/d.items, $m); "
              "let $s := { +d.tag } return q($s)"}) {
      SCOPED_TRACE(form);
      EXPECT_EQ(truth(form), row.tag);
    }
    EXPECT_EQ(
        truth("let $s := { +d.keep } return " + name + "(##/d.items, $s)"),
        row.keep);
  }
}

TEST_F(VQLShapePredicateVariableTest, QuantifierAnswersAreNotJustPresence) {
  // any is true where some item has a keep; all is false because a lacks it.
  const auto any = run("let $s := { +d.keep } return any(##/d.items, $s)");
  const auto all = run("let $s := { +d.keep } return all(##/d.items, $s)");
  ASSERT_EQ(any.size(), 1u);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(truthOf(any.front()), std::optional{true});
  EXPECT_EQ(truthOf(all.front()), std::optional{false});
}

TEST_F(VQLShapePredicateVariableTest, EmptyAndWildcardShapeVariables) {
  expectSame("let $e := { } return ##/d.items[$e]", "##/d.items[{}]", {});
  expectSame("let $e := { } return ##/d.items[{ $e }]", "##/d.items[{}]", {});
  expectSame("let $w := { * } return ##/d.items[$w]", "##/d.items[{*}]",
             {a, b, c});
  expectSame("let $w := { * } return ##/d.items[{ $w, !d.keep }]",
             "##/d.items[{ *, !d.keep }]", {a});
}

TEST_F(VQLShapePredicateVariableTest, ShapeVariableRebuiltBetweenQueries) {
  (void)run("let $s := { +d.tag } return $s");
  EXPECT_EQ(run("##/d.items[$s]"), (std::vector<CellRef>{a, b}));
  (void)run("let $s := { +d.keep } return $s");
  EXPECT_EQ(run("##/d.items[$s]"), (std::vector<CellRef>{b, c}));
  EXPECT_EQ(run("##/d.items[{ $s }]"), (std::vector<CellRef>{b, c}));
}

TEST_F(VQLShapePredicateVariableTest, UnboundVariableIsRefusedByName) {
  for (const std::string query :
       {"##/d.items[{ $missing }]", "##/d.items[{ +d.tag, $missing }]",
        "for $x in ##/d.items where ?{ $missing } return $x",
        "any(##/d.items, { $missing })", "##/d.items[{ +d.tag: $missing }]",
        "##/d.items[{ +d.tag: { $missing } }]",
        "def p($m) := ##/d.items[{ $m }]; p({ $missing })"}) {
    SCOPED_TRACE(query);
    EXPECT_THAT(refusalOf(query), testing::HasSubstr("is not bound"));
  }
}

TEST_F(VQLShapePredicateVariableTest, FormsThePredicateBatteryOnceRefused) {
  // These were refused as "variable, function, pivot, or virtual rules"; each
  // now names what is wrong with it. An undefined function is not a shape.
  EXPECT_THAT(refusalOf("##/d.items[{ +d.tag: $target }]"),
              testing::HasSubstr("is not bound"));
  EXPECT_THAT(refusalOf("##/d.items[{ $shape }]"),
              testing::HasSubstr("is not bound"));
  EXPECT_THAT(refusalOf("##/d.items[{ some(.) }]"),
              testing::HasSubstr("'some' is not defined"));
}

TEST_F(VQLShapePredicateVariableTest, BareUnboundVariableKeepsItsTruthiness) {
  // A bare `$x` as a predicate is a truth test first: unbound is empty and
  // false everywhere else in VQL, and a shape test does not change that.
  EXPECT_TRUE(run("##/d.items[$missing]").empty());
  EXPECT_TRUE(run("for $x in ##/d.items where ?$missing return $x").empty());
}

TEST_F(VQLShapePredicateVariableTest, NonShapeVariableIsRefusedByName) {
  EXPECT_THAT(refusalOf("let $n := 3 return ##/d.items[{ $n }]"),
              testing::AllOf(testing::HasSubstr("not statically known"),
                             testing::HasSubstr("$n")));
  EXPECT_THAT(refusalOf("let $n := 3 return ##/d.items[{ +d.tag: { $n } }]"),
              testing::HasSubstr("not statically known"));
  EXPECT_THAT(refusalOf("def n() := 3; ##/d.items[{ n() }]"),
              testing::HasSubstr("did not return a Shape value"));
}

TEST_F(VQLShapePredicateVariableTest, ClearedHandleRefusesByName) {
  const auto handle = run("let $s := { +d.tag } return $s");
  ASSERT_EQ(handle.size(), 1u);
  engine.clearVariables();
  engine.setVariable("s", handle);
  EXPECT_THAT(refusalOf("##/d.items[{ $s }]"),
              testing::HasSubstr("not statically known"));
  EXPECT_THAT(refusalOf("any(##/d.items, { $s })"),
              testing::HasSubstr("not statically known"));
}

TEST_F(VQLShapePredicateVariableTest, RunTimeChosenShapeRefusesAccurately) {
  // `$h` is bound once per item to a cell the compiler never recorded as a
  // shape, so no literal can be chosen while compiling; it refuses rather
  // than guess.
  EXPECT_THAT(refusalOf("for $h in ##/d.items return ##/d.items[{ $h }]"),
              testing::HasSubstr("not statically known"));
}

TEST_F(VQLShapePredicateVariableTest, FailedQueryLeavesNothingBehind) {
  const auto cells = arena.cellCount();
  const auto table = engine.shapeCount();
  EXPECT_ANY_THROW(
      run("let $s := { +d.tag } return ##/d.items[{ $s, $missing }]"));
  EXPECT_EQ(arena.cellCount(), cells);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
  EXPECT_EQ(engine.shapeCount(), table);
  EXPECT_TRUE(engine.getVariable("s").empty());
}

TEST_F(VQLShapePredicateVariableTest,
       RepeatedVariableQueriesDoNotGrowTheArena) {
  // A rebound variable leaves the cell it held behind, whatever it held. A
  // shape's rules are cells, so what it leaves is its handle, its witness and
  // five cells for its one rule (text, kind, negation, bidirectionality,
  // direction): a fixed footprint per round, never a growing table.
  constexpr std::size_t cellsOfOneRuleShape = 1 + 1 + 5;
  constexpr int rounds                      = 10;
  (void)run("let $n := 1 return ##/d.items[{ +d.tag }]");
  const auto scalarBefore = arena.cellCount();
  for (int round = 0; round < rounds; ++round)
    (void)run("let $n := 1 return ##/d.items[{ +d.tag }]");
  const auto scalarGrowth = arena.cellCount() - scalarBefore;

  (void)run("let $s := { +d.tag } return ##/d.items[$s]");
  const auto shapeBefore = arena.cellCount();
  for (int round = 0; round < rounds; ++round)
    (void)run("let $s := { +d.tag } return ##/d.items[$s]");
  EXPECT_LE(arena.cellCount() - shapeBefore,
            scalarGrowth + rounds * cellsOfOneRuleShape);
}

// Review round: a shape handle is marked, so no path reads it as a value.
TEST_F(VQLShapePredicateVariableTest,
       ClearedOrForeignHandleRefusesInBareForms) {
  const auto handle = run("let $s := { +d.tag } return $s");
  ASSERT_EQ(handle.size(), 1u);
  engine.clearVariables();
  engine.setVariable("s", handle);
  for (const std::string query :
       {"##/d.items[$s]", "##/d.items[not $s]", "any(##/d.items, $s)",
        "for $x in ##/d.items where ?$s return $x",
        "for $x in ##/d.items where not $s return $x",
        "def f($m) := ##/d.items[$m]; f($s)", "##/d.items[{ $s }]"}) {
    SCOPED_TRACE(query);
    EXPECT_THAT(refusalOf(query), testing::HasSubstr("not statically known"));
  }
  VQLEngine other{core};
  other.setVariable("q", handle);
  EXPECT_THAT([&] { (void)other.execute("##/d.items[$q]"); },
              testing::ThrowsMessage<std::runtime_error>(
                  testing::HasSubstr("not statically known")));
}

TEST_F(VQLShapePredicateVariableTest, PlainCellInAVariableIsStillAValue) {
  engine.setVariable("s", std::vector<CellRef>{a});
  EXPECT_EQ(run("##/d.items[$s]"), (std::vector<CellRef>{a, b, c}));
}

TEST_F(VQLShapePredicateVariableTest, CompoundBareParametersInADefinition) {
  EXPECT_EQ(run("def f($m) := ##/d.items[$m and $m]; f({ +d.tag })"),
            (std::vector<CellRef>{a, b}));
  EXPECT_EQ(run("def f($m) := ##/d.items[?$m and ?$m]; f({ +d.tag })"),
            (std::vector<CellRef>{a, b}));
  EXPECT_EQ(run("def f($m, $n) := ##/d.items[$m or $n]; "
                "f({ +d.tag }, { +d.keep })"),
            (std::vector<CellRef>{a, b, c}));
}

TEST_F(VQLShapePredicateVariableTest, ShapePassedThroughANonShapeDefinition) {
  EXPECT_EQ(run("def idf($m) := $m; let $s := { +d.tag } return "
                "##/d.items[idf($s)]"),
            (std::vector<CellRef>{a, b}));
  const auto none = run("def id($x) := $x; let $s := { +d.nothere } return "
                        "any(##/d.items, id($s))");
  ASSERT_EQ(none.size(), 1u);
  EXPECT_EQ(truthOf(none.front()), std::optional{false});
  const auto wrapped = run("def g($m) := $m; def f($m) := any(##/d.items, "
                           "g($m)); f({ +d.nothere })");
  ASSERT_EQ(wrapped.size(), 1u);
  EXPECT_EQ(truthOf(wrapped.front()), std::optional{false});
}

TEST_F(VQLShapePredicateVariableTest, ShapeHandlesAreNotComparable) {
  for (const std::string query :
       {"let $s := { +d.tag }; let $t := { +d.keep }; "
        "for $x in ##/d.items where $s = $t return $x",
        "let $s := { +d.tag }; for $x in ##/d.items where $s = $s return $x",
        "let $s := { +d.tag }; for $x in ##/d.items where $s != 1 return $x",
        "def f($m, $n) := $m = $n; f({ +d.tag }, { +d.keep })"}) {
    SCOPED_TRACE(query);
    EXPECT_THAT(refusalOf(query),
                testing::AllOf(testing::HasSubstr("shape value"),
                               testing::HasSubstr("operand")));
  }
}

TEST_F(VQLShapePredicateVariableTest, RecursionWithAPlainFlagStillCompiles) {
  EXPECT_EQ(run("def pure walk($f, $n: Int) := { if $n > 0 return "
                "walk($f, $n + -1) else return ##/d.items[$f]; }; walk(1, 2)"),
            (std::vector<CellRef>{a, b, c}));
}

TEST_F(VQLShapePredicateVariableTest, RecursionTakingAShapeRefusesByName) {
  const auto refusal =
      refusalOf("def pure walk($m, $n: Int) := { if $n > 0 return "
                "walk($m, $n + -1) else return ##/d.items[$m]; }; "
                "walk({ +d.tag }, 3)");
  EXPECT_THAT(refusal, testing::HasSubstr("shape"));
  EXPECT_THAT(refusal, testing::HasSubstr("walk"));
}

TEST_F(VQLShapePredicateVariableTest, LongForwardingChainCompiles) {
  std::string defs;
  constexpr int depth = 40;
  for (int level = 0; level < depth; ++level)
    defs += "def f" + std::to_string(level) + "($m) := " +
            (level + 1 == depth ? "##/d.items[$m]"
                                : "f" + std::to_string(level + 1) + "($m)") +
            "; ";
  EXPECT_EQ(run(defs + "f0({ +d.tag })"), (std::vector<CellRef>{a, b}));
}

TEST_F(VQLShapePredicateVariableTest, ScalarTargetVariableIsDiagnosedAsSuch) {
  EXPECT_THAT(refusalOf("let $n := 3 return ##/d.items[{ +d.tag: $n }]"),
              testing::HasSubstr("only a shape-valued variable"));
}

// Structural rule: a marked handle is read only where a shape is consumed.
TEST_F(VQLShapePredicateVariableTest, ShapeReadAsAValueRefusesByName) {
  const std::string s0 = "let $s := { +d.tag } ";
  for (const auto &query :
       {// lambda frames (D1)
        s0 + "return $s / \\($m) ##/d.items[$m]",
        s0 + "return $s / \\($m) any(##/d.items, $m)",
        s0 + "let $f := \\($m) ##/d.items[$m] return $s / $f",
        // builtins and wrapped arguments (D2)
        "def f($m) := ##/d.items[$m]; " + s0 + "return f(first($s))",
        "def f($m) := ##/d.items[$m]; " + s0 + "return f(string($s))",
        s0 + "let $m := first($s) return ##/d.items[$m]",
        s0 + "return ##/d.items[count($s) = 1]",
        s0 + "return ##/d.items[contains(., $s)]", s0 + "return string($s)",
        s0 + "return count($s)", s0 + "return value($s)",
        "def f($m) := count($m); " + s0 + "return f($s)",
        // operators and aliases (D3, D6)
        s0 + "return 1 + $s", s0 + "return $s + $s",
        s0 + "for $i in ##/d.items return $i + $s",
        s0 + "return ##/d.items / $s",
        // sources and conditions (D4)
        s0 + "for $v in $s return 1",
        s0 + "for $v in ##/d.items for $w in $s return 1",
        s0 + "let $x := 3 if $s return $x", s0 + "return any($s, .)",
        s0 + "return $s/d.tag"}) {
    SCOPED_TRACE(query);
    EXPECT_THAT(refusalOf(query),
                testing::HasSubstr("a shape value cannot be used as"));
  }
}

TEST_F(VQLShapePredicateVariableTest, ShapeConsumingFormsStillWork) {
  const std::string s0 = "let $s := { +d.tag } ";
  EXPECT_EQ(run(s0 + "return ##/d.items[$s]"), (std::vector<CellRef>{a, b}));
  EXPECT_EQ(run(s0 + "let $t := $s return ##/d.items[{ $t, +d.keep }]"),
            (std::vector<CellRef>{b}));
  EXPECT_EQ(run(s0 + "return $s").size(), 1u);
  EXPECT_EQ(run("def f($m) := ##/d.items[$m]; " + s0 + "return f($s)"),
            (std::vector<CellRef>{a, b}));
  EXPECT_EQ(run(s0 + "for $x in ##/d.items where $s return $x"),
            (std::vector<CellRef>{a, b}));
}

TEST_F(VQLShapePredicateVariableTest, FirstShapeQueryKeepsNoProgram) {
  // The shape dimensions are named when the engine is made, so the first shape
  // of a session costs what the next one does.
  const auto before = arena.cellCount();
  (void)run("let $s := { +d.tag } return ##/d.items[$s]");
  const auto first = arena.cellCount() - before;
  (void)run("let $s := { +d.tag } return ##/d.items[$s]");
  const auto second = arena.cellCount() - before - first;
  EXPECT_LE(first, second + 1);
}

TEST_F(VQLShapePredicateVariableTest, InlineLambdaMayConsumeAShape) {
  const auto answer = run("let $s := { +d.tag } return any(##/d.items, "
                          "\\($c) ##/d.items[$s])");
  ASSERT_EQ(answer.size(), 1u);
  EXPECT_EQ(truthOf(answer.front()), std::optional{true});
}

TEST_F(VQLShapePredicateVariableTest, ClosureCapturingAShapeRefusesByName) {
  for (const std::string query :
       {"let $s := { +d.tag } return any(##/d.items, \\($c) $s)",
        "let $s := { +d.tag } return ##/d.items / \\($i) $s"}) {
    SCOPED_TRACE(query);
    EXPECT_THAT(refusalOf(query),
                testing::HasSubstr("a shape value cannot be used as"));
  }
}

// D5: a variable is a bare number, so it carries its handle's epoch.
TEST_F(VQLShapePredicateVariableTest, HeldHandleWhoseCellWasReleasedRefuses) {
  const auto mark   = arena.mark();
  const auto handle = run("let $s := { +d.tag } return $s");
  ASSERT_EQ(handle.size(), 1u);
  engine.setVariable("held", handle);
  zigzag::expectMarkOrder(arena.release(mark));
  // The number is reused by a new handle that means something else.
  (void)run("let $s := { +d.keep } return $s");
  for (const std::string query :
       {"##/d.items[$held]", "def g($m) := ##/d.items[$m]; g($held)",
        "any(##/d.items, $held)"}) {
    SCOPED_TRACE(query);
    EXPECT_THAT(refusalOf(query), testing::HasSubstr("not statically known"));
  }
}

// Foreign-owner proxies carry a store-ref index link that is bookkeeping: `{}`
// holds on a proxy with no user links, and the variable form must agree.
struct Foreign {
  std::shared_ptr<Store> store;
  zigzag::Manifold manifold;
  DimRef a{zigzag::noCell};
  CellRef none{zigzag::noCell};
  CellRef one{zigzag::noCell};
};

Foreign makeForeign() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("vql_shape_predicate_variable_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  UserPermascroll::Config config;
  config.storageDir = directory / "permascroll";
  auto store =
      std::make_shared<Store>(std::make_shared<UserPermascroll>(config));
  auto version     = store->sliceGenesis(MicroversionId{});
  const auto axisA = store->makeDimension(version, "d.a");
  version          = axisA.version;
  const auto cell  = [&](const std::string_view text) {
    version = store->makeCell(version, text);
    return store->cellRefOf(version).value();
  };
  const auto none = cell("none");
  const auto one  = cell("one");
  const auto leaf = cell("leaf");
  version         = store->setLink(version, one, axisA.dim, false, leaf);
  auto manifold   = store->rebuildManifold(version);
  return Foreign{.store    = std::move(store),
                 .manifold = std::move(manifold),
                 .a        = axisA.dim,
                 .none     = none,
                 .one      = one};
}

struct ForeignFixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{engine, vm};
  Foreign foreign{makeForeign()};
  std::uint32_t space{arena.attach(zigzag::Space{.manifold = &foreign.manifold,
                                                 .store  = foreign.store.get(),
                                                 .reader = foreign.store.get(),
                                                 .label  = "foreign"})};
  CellRef noneProxy{arena.proxyFor(space, foreign.none)};
  CellRef oneProxy{arena.proxyFor(space, foreign.one)};

  ForeignFixture() {
    arena.bindDimension(arena.ensureDimension("d.a"), space, foreign.a,
                        zigzag::DimensionBindingMode::Explicit);
  }

  bool holds(const CellRef cell, const std::string &prelude,
             const std::string &predicate) {
    const auto result = executor.execute(
        prelude + "for $c in . where " + predicate + " return $c", {cell});
    EXPECT_TRUE(result) << predicate << ": " << result.error().message;
    return result && !result->empty();
  }
};

TEST(VQLShapePredicateForeignTest, VariableFormsSkipTheStoreRefLink) {
  ForeignFixture h;
  struct Row {
    const char *prelude;
    const char *predicate;
    bool none;
    bool one;
  };
  const Row rows[] = {
      {"let $e := {}; ", "?$e", true, false},
      {"let $e := {}; ", "?{ $e }", true, false},
      {"let $w := {*}; ", "?$w", false, true},
      {"let $w := {*}; ", "?{ $w, !d.x }", false, true},
      {"let $x := {!*}; ", "?{ $x, +d.a }", false, true},
      {"def e() := {}; ", "?e()", true, false},
  };
  for (const auto &row : rows) {
    SCOPED_TRACE(std::string{row.prelude} + row.predicate);
    EXPECT_EQ(h.holds(h.noneProxy, row.prelude, row.predicate), row.none);
    EXPECT_EQ(h.holds(h.oneProxy, row.prelude, row.predicate), row.one);
  }
  EXPECT_FALSE(h.executor.programRetainedBecause());
}

TEST(VQLShapePredicateForeignTest, ProgramReleaseAfterAVariablePredicate) {
  ForeignFixture h;
  constexpr int rounds = 3;
  (void)h.holds(h.oneProxy, "let $w := 1; ", "?{*}");
  const auto scalarBefore = h.arena.cellCount();
  for (int run = 0; run < rounds; ++run)
    EXPECT_TRUE(h.holds(h.oneProxy, "let $w := 1; ", "?{*}"));
  const auto scalarGrowth = h.arena.cellCount() - scalarBefore;

  (void)h.holds(h.oneProxy, "let $w := {*}; ", "?$w");
  const auto shapeBefore = h.arena.cellCount();
  for (int run = 0; run < rounds; ++run)
    EXPECT_TRUE(h.holds(h.oneProxy, "let $w := {*}; ", "?$w"));
  EXPECT_FALSE(h.executor.programRetainedBecause());
  // The shape leaves its own cells: handle, witness, and five for its one rule.
  constexpr std::size_t cellsOfOneRuleShape = 1 + 1 + 5;
  EXPECT_LE(h.arena.cellCount() - shapeBefore,
            scalarGrowth + rounds * cellsOfOneRuleShape);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

} // namespace
} // namespace xanadu::vql
