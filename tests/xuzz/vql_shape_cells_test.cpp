// V5, the data half: a shape value's rules are cells on the rank a view's are
// on, its targets are values captured when it was made, and each handle
// carries an epoch witness. The compile-time table still serves the static
// lowering, which these tests pin so the cells stay an addition.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/shape_cells.hpp"
#include "common/xanadu/vql/shape_limits.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::CellRef;
using zigzag::DimVector;

struct RuleView {
  std::string dimension;
  std::string kind;
  bool negated{};
  bool bidirectional{};
  std::int64_t sign{};
  std::optional<CellRef> target;
  bool voided{};
  bool late{};
  std::optional<CellRef> nested;
  std::optional<std::string> binder;
};

struct VQLShapeCellsTest : testing::Test {
  zigzag::ArenaManifold arena;
  VQLEngine engine{arena};
  zigzag::vortex::VortexVM vm{engine.core()};
  xanadu::vql::QueryExecutor executor{engine, vm};

  void SetUp() override {
    for (const char *name : {"d.keep", "d.drop", "d.tag", "d.n"})
      (void)engine.resolveDimension(name);
    ASSERT_TRUE(arena.link(engine.core().home(),
                           engine.resolveDimension("d.keep"), DimVector::POS,
                           arena.makeCell("neighbour")));
  }

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::exception &error) {
      return error.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }

  [[nodiscard]] xanadu::vql::ShapeDimensions dims() const {
    const auto found = xanadu::vql::findShapeDimensions(engine.core());
    EXPECT_TRUE(found.has_value());
    return found.value_or(xanadu::vql::ShapeDimensions{});
  }

  /// The handle a query returns.
  CellRef handleOf(const std::string &query) {
    const auto cells = engine.execute(query);
    EXPECT_EQ(cells.size(), 1u);
    return cells.empty() ? zigzag::noCell : cells.front();
  }

  std::optional<CellRef> neighbour(CellRef cell, zigzag::DimRef dim) const {
    return arena.linked(cell, dim, DimVector::POS);
  }

  std::vector<RuleView> rulesOf(CellRef shape) {
    const auto d = dims();
    std::vector<RuleView> rules;
    for (auto rule = neighbour(shape, d.rule); rule;
         rule      = neighbour(*rule, d.rule)) {
      RuleView view;
      view.dimension     = arena.textOf(*rule);
      view.kind          = arena.textOf(*neighbour(*rule, d.kind));
      view.negated       = *arena.asBool(*neighbour(*rule, d.neg));
      view.bidirectional = *arena.asBool(*neighbour(*rule, d.bidir));
      view.sign          = *arena.asInt64(*neighbour(*rule, d.dir));
      view.target        = neighbour(*rule, d.target);
      view.voided        = neighbour(*rule, d.voided) == rule;
      view.late          = neighbour(*rule, d.late) == rule;
      view.nested        = neighbour(*rule, d.nested);
      if (const auto to = neighbour(*rule, d.to))
        view.binder = arena.textOf(*to);
      rules.push_back(std::move(view));
    }
    return rules;
  }
};

TEST_F(VQLShapeCellsTest, RulesAreCellsInSourceOrderOnTheViewRank) {
  const auto handle =
      handleOf("let $s := { +d.keep, !d.drop, ~d.tag, -d.n, *, !* } return $s");
  const auto rules = rulesOf(handle);
  ASSERT_EQ(rules.size(), 6u);
  EXPECT_EQ(rules[0].dimension, "d.keep");
  EXPECT_EQ(rules[0].kind, "dim");
  EXPECT_FALSE(rules[0].negated);
  EXPECT_EQ(rules[0].sign, 1);
  EXPECT_TRUE(rules[1].negated);
  EXPECT_TRUE(rules[2].bidirectional);
  EXPECT_EQ(rules[3].sign, -1);
  EXPECT_EQ(rules[4].kind, "wildcard");
  EXPECT_FALSE(rules[4].negated);
  EXPECT_EQ(rules[5].kind, "wildcard");
  EXPECT_TRUE(rules[5].negated);
  // Same dimension a view's rules are on, and the same marker as ever.
  EXPECT_EQ(arena.textOf(dims().rule), "d.vql.rule");
  EXPECT_TRUE(xanadu::vql::carriesShapeMarker(arena, handle));
}

TEST_F(VQLShapeCellsTest, SourceOrderIsPartOfTheData) {
  const auto forward =
      rulesOf(handleOf("let $s := { +d.keep, +d.tag } return $s"));
  const auto backward =
      rulesOf(handleOf("let $s := { +d.tag, +d.keep } return $s"));
  ASSERT_EQ(forward.size(), 2u);
  ASSERT_EQ(backward.size(), 2u);
  EXPECT_EQ(forward[0].dimension, backward[1].dimension);
  EXPECT_EQ(forward[1].dimension, backward[0].dimension);
}

TEST_F(VQLShapeCellsTest, NestedShapeIsOneEdgeToAShapeCellWithItsOwnRank) {
  const auto handle =
      handleOf("let $s := { +d.keep: { +d.tag, !d.drop } } return $s");
  const auto rules = rulesOf(handle);
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_EQ(rules[0].kind, "nested");
  EXPECT_FALSE(rules[0].target.has_value());
  ASSERT_TRUE(rules[0].nested.has_value());
  const auto inner = rulesOf(*rules[0].nested);
  ASSERT_EQ(inner.size(), 2u);
  EXPECT_EQ(inner[0].dimension, "d.tag");
  EXPECT_TRUE(inner[1].negated);
  // A nested shape is not a handle, but it is witnessed like one.
  EXPECT_FALSE(xanadu::vql::carriesShapeMarker(arena, *rules[0].nested));
  EXPECT_TRUE(neighbour(*rules[0].nested, dims().epoch).has_value());
}

TEST_F(VQLShapeCellsTest, NestedThroughAShapeVariable) {
  const auto handle = handleOf(
      "let $inner := { +d.tag }; let $s := { +d.keep: $inner } return $s");
  const auto rules = rulesOf(handle);
  ASSERT_EQ(rules.size(), 1u);
  ASSERT_TRUE(rules[0].nested.has_value());
  EXPECT_EQ(rulesOf(*rules[0].nested).front().dimension, "d.tag");
}

TEST_F(VQLShapeCellsTest, LiteralTargetIsAFreshScalarPerShape) {
  const auto a =
      rulesOf(handleOf("let $s := { +d.tag: \"x\", +d.n: 3 } return $s"));
  // A shape that is equal to a held one is that one (RECLAIM R-E); two shapes
  // that merely hold the same value each hold a scalar of their own.
  const auto b = rulesOf(handleOf("let $t := { +d.keep: \"x\" } return $t"));
  ASSERT_TRUE(a[0].target && a[1].target && b[0].target);
  EXPECT_EQ(arena.textOf(*a[0].target), "x");
  EXPECT_EQ(arena.asInt64(*a[1].target), 3);
  EXPECT_NE(a[0].target, b[0].target);
}

TEST_F(VQLShapeCellsTest, PresenceOnlyRuleHasNoTargetAndNoMarker) {
  const auto rules = rulesOf(handleOf("let $s := { +d.keep } return $s"));
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_FALSE(rules[0].target);
  EXPECT_FALSE(rules[0].voided);
  EXPECT_FALSE(rules[0].nested);
  EXPECT_FALSE(rules[0].binder);
}

TEST_F(VQLShapeCellsTest, ComputedTargetIsCapturedByValueAtMint) {
  const auto handle =
      handleOf("let $n := 5; let $s := { +d.tag: $n + 1 } return $s");
  const auto first = rulesOf(handle);
  ASSERT_TRUE(first[0].target);
  EXPECT_EQ(arena.asInt64(*first[0].target), 6);
  // Rebinding what it read changes nothing the shape holds.
  engine.setVariable("n", arena.makeScalarCell(std::int64_t{100}));
  (void)engine.execute("let $n := 7 return $n");
  const auto later = rulesOf(handle);
  ASSERT_TRUE(later[0].target);
  EXPECT_EQ(later[0].target, first[0].target);
  EXPECT_EQ(arena.asInt64(*later[0].target), 6);
}

TEST_F(VQLShapeCellsTest, AShapeReturnedByAFunctionCapturesWhereItIsCalled) {
  const auto handle = handleOf(
      "def f() := { +d.tag: $n + 1 }; let $n := 2; let $s := f() return $s");
  const auto rules = rulesOf(handle);
  ASSERT_EQ(rules.size(), 1u);
  ASSERT_TRUE(rules[0].target);
  EXPECT_EQ(arena.asInt64(*rules[0].target), 3);
}

TEST_F(VQLShapeCellsTest, CapturedValueIsACopyNotTheCellItReadFrom) {
  const auto source = arena.makeScalarCell(std::int64_t{41});
  engine.setVariable("v", source);
  const auto rules = rulesOf(handleOf("let $s := { +d.tag: $v } return $s"));
  ASSERT_TRUE(rules[0].target);
  EXPECT_NE(rules[0].target, source);
  EXPECT_EQ(arena.asInt64(*rules[0].target), 41);
  // Writing the source later leaves the copy alone.
  ASSERT_TRUE(arena.setValueBits(source, xanadu::ValueKind::Int64, 42));
  EXPECT_EQ(arena.asInt64(*rules[0].target), 41);
}

TEST_F(VQLShapeCellsTest, SeveralValuesRefuseNamingOne) {
  const auto a = arena.makeScalarCell(std::int64_t{1});
  const auto b = arena.makeScalarCell(std::int64_t{2});
  engine.setVariable("many", std::vector<CellRef>{a, b});
  EXPECT_THAT(refusalOf("let $s := { +d.tag: $many } return $s"),
              testing::HasSubstr("one"));
  EXPECT_THAT(refusalOf("let $s := { +d.tag: $many } return $s"),
              testing::HasSubstr("several values"));
}

TEST_F(VQLShapeCellsTest, NoValueIsTheVoidMarkerNotAbsence) {
  const auto rules =
      rulesOf(handleOf("let $s := { +d.tag: ##/+d.missing } return $s"));
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_TRUE(rules[0].voided);
  EXPECT_FALSE(rules[0].target);
}

TEST_F(VQLShapeCellsTest, UnboundNameStaysABinder) {
  const auto rules = rulesOf(handleOf("let $s := { +d.tag: $t } return $s"));
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_EQ(rules[0].binder, "$t");
  EXPECT_FALSE(rules[0].target);
  EXPECT_FALSE(rules[0].voided);
}

TEST_F(VQLShapeCellsTest, ADefaultInAShapeRefusesByName) {
  EXPECT_THAT(refusalOf("let $s := { +d.tag: $t := 5 } return $s"),
              testing::AllOf(testing::HasSubstr("':=' default"),
                             testing::HasSubstr("d.tag")));
  EXPECT_THAT(refusalOf("##{ +d.tag: $t := 5 }"),
              testing::HasSubstr("':=' default"));
}

TEST_F(VQLShapeCellsTest, ASubjectRelativeTargetRefusesWhenAValueIsMade) {
  const auto expected = testing::HasSubstr("relative to the subject");
  EXPECT_THAT(refusalOf("let $s := { +d.tag: ./+d.keep[1] } return $s"),
              expected);
  EXPECT_THAT(refusalOf("def mk($m) := ##{ $m }; mk({ +d.tag: ./+d.keep[1] })"),
              expected);
  EXPECT_THAT(refusalOf("def f() := { +d.tag: ./+d.keep[1] }; "
                        "let $s := f() return $s"),
              expected);
}

TEST_F(VQLShapeCellsTest, TheInlineFormAtItsConsumerStillWorks) {
  const auto subject = arena.makeCell("subject");
  const auto partner = arena.makeCell("partner");
  ASSERT_TRUE(arena.link(subject, engine.resolveDimension("d.keep"),
                         DimVector::POS, partner));
  engine.setVariable("subject", subject);
  const auto result =
      engine.execute("apply_shape($subject, {*, +d.tag: ./+d.keep[1]})");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(
      arena.linked(subject, engine.resolveDimension("d.tag"), DimVector::POS),
      partner);
}

TEST_F(VQLShapeCellsTest, PathAndFunctorTargetsAreRefusedByName) {
  EXPECT_THAT(refusalOf("let $s := { +d.tag: /+d.keep } return $s"),
              testing::HasSubstr("rootless partial path"));
}

// A computed target made where the handle is one cell for many turns or calls
// (a loop, a def) is not captured: the cells hold none, the rule says so
// (d.vql.late), and this query's consumers read the expression where they
// stand, as they did before shapes were cells. Everything df0967b6 compiled
// still compiles.
TEST_F(VQLShapeCellsTest, AComputedTargetInALoopIsLateNotCaptured) {
  engine.setVariable(
      "items", std::vector<CellRef>{arena.makeCell("a"), arena.makeCell("b")});
  const auto handles =
      engine.execute("for $i in $items let $s := { +d.tag: $i } return $s");
  ASSERT_EQ(handles.size(), 2u);
  const auto rules = rulesOf(handles.front());
  ASSERT_EQ(rules.size(), 1u);
  EXPECT_TRUE(rules[0].late);
  EXPECT_FALSE(rules[0].target);
  EXPECT_FALSE(rules[0].voided);
  // A static consumer in the loop evaluates at the consumer.
  EXPECT_NO_THROW(
      (void)engine.execute("for $i in $items let $s := { *, +d.tag: $i } "
                           "return apply_shape($i, $s)"));
  // A literal target is the same cell every turn and nothing is late.
  const auto literal =
      engine.execute("for $i in $items let $s := { +d.tag: \"x\" } return $s");
  ASSERT_EQ(literal.size(), 2u);
  EXPECT_FALSE(rulesOf(literal.front())[0].late);
}

TEST_F(VQLShapeCellsTest, ALateHandleIsRefusedByNameInALaterQuery) {
  engine.setVariable("items", std::vector<CellRef>{arena.makeCell("a")});
  const auto handles =
      engine.execute("for $i in $items let $s := { +d.tag: $i } return $s");
  ASSERT_EQ(handles.size(), 1u);
  ASSERT_TRUE(rulesOf(handles.front())[0].late);
  // The session still holds the handle (it was a result); a later query that
  // names it has no scope to read `$i` in.
  engine.setVariable("s", handles.front());
  EXPECT_THAT(refusalOf("##{ $s }"),
              testing::HasSubstr("of another query, where its computed target "
                                 "is read at each use"));
}

// The forms below are also the ones that run a body more than once; each is
// accepted, and a handle made in one is late.
TEST_F(VQLShapeCellsTest, EveryRepeatedFormAcceptsAComputedTarget) {
  const std::string make = "def mk() := {+d.keep: $n + 1}; let $n := 1 ";
  // A def body, however it is reached.
  EXPECT_NO_THROW((void)engine.execute("def g($x) := {*, +d.tag: $x}; g(##)"));
  EXPECT_NO_THROW((void)engine.execute(make + "return mk()"));
  EXPECT_NO_THROW((void)engine.execute(make + "return count(mk())"));
  // let $s := mk() at the query body is captured, as it always was.
  const auto handle = handleOf(make + "let $s := mk() return $s");
  EXPECT_FALSE(rulesOf(handle)[0].late);
  EXPECT_EQ(arena.asInt64(*rulesOf(handle)[0].target), 2);
  for (const char *quantifier : {"any", "all", "none"})
    EXPECT_NO_THROW((void)engine.execute(make + "return " + quantifier +
                                         "(##, count(mk()) > 0)"))
        << quantifier;
  // Rest groups and element-wise contracts.
  EXPECT_NO_THROW((void)engine.execute(
      "def f(...$r: Stream<{d.tag := {+d.keep: $n + 1}}>): Any := $r; "
      "let $n := 1 return f(##)"));
  EXPECT_NO_THROW((void)engine.execute(
      "def f($x: Stream<{d.tag := {+d.keep: $n + 1}}>): Any := $x; "
      "let $n := 1 return f(##)"));
}

TEST_F(VQLShapeCellsTest, ASubjectRelativeTargetStaysRefusedInALoop) {
  engine.setVariable("items", std::vector<CellRef>{arena.makeCell("a")});
  EXPECT_THAT(refusalOf("for $i in $items let $s := { +d.tag: ./+d.keep[1] } "
                        "return $s"),
              testing::HasSubstr("relative to the subject"));
}

TEST_F(VQLShapeCellsTest, ACyclicRankEndsWhereItBegan) {
  const auto d = dims();
  // A rank that loops back on itself ends under the standard rank protection,
  // with no nested edge to bound it: once on a self-link, once ringing back
  // through its head.
  const auto selfLinked = handleOf("let $s := { +d.keep } return $s");
  ASSERT_TRUE(arena.link(*neighbour(selfLinked, d.rule), d.rule, DimVector::POS,
                         *neighbour(selfLinked, d.rule)));
  const auto ringed = handleOf("let $t := { +d.keep, +d.tag } return $t");
  ASSERT_TRUE(arena.link(*neighbour(*neighbour(ringed, d.rule), d.rule), d.rule,
                         DimVector::POS, ringed));
  for (const auto handle : {selfLinked, ringed}) {
    const auto late = xanadu::vql::hasLateRule(arena, d, handle);
    ASSERT_TRUE(late.has_value());
    EXPECT_FALSE(*late);
    EXPECT_FALSE(xanadu::vql::staleShapeReason(arena, d, handle));
  }
}

TEST_F(VQLShapeCellsTest, AShapeThatNestsItselfRefusesAtTheDepthCeiling) {
  const auto handle = handleOf("let $s := { +d.keep: { +d.tag } } return $s");
  const auto d      = dims();
  const auto inner  = *neighbour(*neighbour(handle, d.rule), d.nested);
  // The nested shape nests its parent: an edge, not a rank, so no start is
  // ever reached again and only the ceiling ends the walk.
  ASSERT_TRUE(
      arena.link(*neighbour(inner, d.rule), d.nested, DimVector::POS, handle));
  constexpr std::size_t ceiling = 8;
  const auto late = xanadu::vql::hasLateRule(arena, d, handle, ceiling);
  ASSERT_FALSE(late.has_value());
  EXPECT_THAT(late.error(), testing::HasSubstr("maximum nesting depth 8"));
  const auto stale = xanadu::vql::staleShapeReason(arena, d, handle, ceiling);
  ASSERT_TRUE(stale.has_value());
  EXPECT_THAT(*stale, testing::HasSubstr("maximum nesting depth 8"));
  const std::vector<CellRef> handles{handle};
  const auto refused =
      xanadu::vql::refreshShapeWitnesses(arena, d, handles, ceiling);
  ASSERT_TRUE(refused.has_value());
  EXPECT_THAT(*refused, testing::HasSubstr("maximum nesting depth 8"));
  // A consumer in a query is refused by name, not hung.
  engine.setShapeLimits({.maxRules = 64, .maxDepth = ceiling});
  EXPECT_THAT(refusalOf("##{ $s }"),
              testing::HasSubstr("maximum nesting depth 8"));
}

TEST_F(VQLShapeCellsTest, ANestedChainDeeperThanTheCeilingRefusesByName) {
  const auto d = dims();
  // A chain, not a cycle, built by hand: the compiler would refuse to write
  // one deeper than its ceiling, so only a reader with a smaller ceiling
  // meets it.
  const auto chain = [&](const std::size_t levels) {
    const CellRef head = arena.makeCell();
    CellRef tail       = head;
    for (std::size_t level = 0; level < levels; ++level) {
      const CellRef rule = arena.makeCell("d.tag");
      const CellRef next = arena.makeCell();
      EXPECT_TRUE(arena.link(tail, d.rule, DimVector::POS, rule));
      EXPECT_TRUE(arena.link(rule, d.nested, DimVector::POS, next));
      tail = next;
    }
    return head;
  };
  constexpr std::size_t ceiling = 3;
  // `ceiling` nested levels below the head is the deepest a reader admits.
  const auto atCeiling =
      xanadu::vql::hasLateRule(arena, d, chain(ceiling), ceiling);
  ASSERT_TRUE(atCeiling.has_value());
  EXPECT_FALSE(*atCeiling);
  const auto past =
      xanadu::vql::hasLateRule(arena, d, chain(ceiling + 1), ceiling);
  ASSERT_FALSE(past.has_value());
  EXPECT_THAT(past.error(), testing::HasSubstr("maximum nesting depth 3"));
}

// RECLAIM R-E: a plain shape written again is the handle the session holds.

TEST_F(VQLShapeCellsTest, APlainShapeWrittenAgainIsTheHandleHeld) {
  const auto first = handleOf("let $s := { +d.keep: 7, !d.tag, *, "
                              "{ d.n <=> d.drop }, +d.drop: { +d.tag } } "
                              "return $s");
  const auto cells = arena.cellCount();
  const auto again = handleOf("let $s := { +d.keep: 7, !d.tag, *, "
                              "{ d.n <=> d.drop }, +d.drop: { +d.tag } } "
                              "return $s");
  EXPECT_EQ(again, first);
  EXPECT_EQ(arena.cellCount(), cells);
  // Held by a variable of another name, it is still the one handle.
  EXPECT_EQ(handleOf("let $other := { +d.keep: 7, !d.tag, *, "
                     "{ d.n <=> d.drop }, +d.drop: { +d.tag } } return $other"),
            first);
}

TEST_F(VQLShapeCellsTest, ShapesThatDifferInAnyRuleFieldAreNotInterned) {
  const auto base = handleOf("let $s := { +d.keep: 7 } return $s");
  // Each under a name of its own, so the base stays held and a hit is possible.
  int variable = 0;
  for (const char *other :
       {"{ +d.keep: 8 }", "{ +d.keep: 7.0 }", "{ +d.keep: \"7\" }",
        "{ +d.keep: true }", "{ !d.keep: 7 }", "{ -d.keep: 7 }",
        "{ ~d.keep: 7 }", "{ +d.tag: 7 }", "{ +d.keep }",
        "{ +d.keep: 7, +d.keep: 7 }", "{ +d.keep: { +d.n: 7 } }"}) {
    SCOPED_TRACE(other);
    const auto name = "$v" + std::to_string(variable++);
    EXPECT_NE(handleOf("let " + name + " := " + other + " return " + name),
              base);
  }
  // Doubles are told by bits: zero and negative zero are two values.
  const auto zero = handleOf("let $z := { +d.n: 0.0 } return $z");
  EXPECT_NE(handleOf("let $m := { +d.n: -0.0 } return $m"), zero);
}

TEST_F(VQLShapeCellsTest, AnInternedShapeKeepsItsCapturedScalarByValue) {
  const auto first = handleOf("let $s := { +d.n: 7 } return $s");
  const auto again = handleOf("let $s := { +d.n: 7 } return $s");
  ASSERT_EQ(again, first);
  const auto rules = rulesOf(first);
  ASSERT_EQ(rules.size(), 1u);
  ASSERT_TRUE(rules.front().target.has_value());
  EXPECT_EQ(arena.asInt64(*rules.front().target), 7);
  // Nothing writes a shape's cells after it is made, so a query that applies
  // it leaves them as they were.
  (void)engine.execute("let $t := { +d.n: 7 } return ##{ $t }");
  EXPECT_EQ(arena.asInt64(*rulesOf(first).front().target), 7);
}

TEST_F(VQLShapeCellsTest, ADroppedShapeIsNotResurrected) {
  const auto first = handleOf("let $s := { +d.keep } return $s");
  (void)engine.execute("let $s := 1 return $s");
  ASSERT_EQ(engine.shapeCount(), 0u);
  // The table no longer names it, so its compile-time record is gone and a
  // hit would hand out a handle the compiler refuses as unrecorded.
  const auto again = handleOf("let $s := { +d.keep } return $s");
  EXPECT_NE(again, first);
  EXPECT_NO_THROW((void)engine.execute("return ##{ $s }"));
}

TEST_F(VQLShapeCellsTest, AHeldShapeWithAStaleWitnessIsNotInterned) {
  const auto first   = handleOf("let $s := { +d.keep } return $s");
  const auto witness = *neighbour(first, dims().epoch);
  ASSERT_TRUE(arena.setValueBits(witness, xanadu::ValueKind::Int64, 12345));
  // Writing the shape again must not hand back a cell whose number may be
  // another shape's now; it makes its own, with a witness that holds.
  const auto again = handleOf("let $t := { +d.keep } return $t");
  EXPECT_NE(again, first);
  EXPECT_FALSE(xanadu::vql::staleShapeReason(arena, dims(), again));
}

TEST_F(VQLShapeCellsTest, AComputedTargetIsNeverInterned) {
  // The program writes the target when it runs, so two shapes with the same
  // text may hold different values and the text does not say which.
  const auto first = handleOf("let $n := 1; let $s := { +d.n: $n + 1 } "
                              "return $s");
  const auto again = handleOf("let $n := 1; let $s := { +d.n: $n + 1 } "
                              "return $s");
  EXPECT_NE(again, first);
  const auto other = handleOf("let $n := 5; let $s := { +d.n: $n + 1 } "
                              "return $s");
  EXPECT_EQ(arena.asInt64(*rulesOf(other).front().target), 6);
  EXPECT_EQ(arena.asInt64(*rulesOf(first).front().target), 2);
}

TEST_F(VQLShapeCellsTest, ALateShapeIsNeverInterned) {
  const std::string make = "def mk() := {+d.keep: $n + 1}; let $n := 1 ";
  const auto first       = engine.execute(make + "return mk()");
  const auto again       = engine.execute(make + "return mk()");
  ASSERT_EQ(first.size(), 1u);
  ASSERT_EQ(again.size(), 1u);
  EXPECT_NE(again.front(), first.front());
  engine.setVariable("items", std::vector<CellRef>{arena.makeCell("a")});
  const auto looped =
      engine.execute("for $i in $items let $s := { +d.tag: $i } return $s");
  const auto loopedAgain =
      engine.execute("for $i in $items let $s := { +d.tag: $i } return $s");
  ASSERT_EQ(looped.size(), 1u);
  ASSERT_EQ(loopedAgain.size(), 1u);
  EXPECT_NE(looped.front(), loopedAgain.front());
}

TEST_F(VQLShapeCellsTest, AShapeNamingAnotherIsNotInterned) {
  // A name is read from the scope the shape is made in, so equal text can
  // stand for different rules.
  (void)engine.execute("let $a := { +d.keep } return $a");
  const auto first = handleOf("let $b := { $a, +d.tag } return $b");
  (void)engine.execute("let $a := { +d.drop } return $a");
  const auto again = handleOf("let $b := { $a, +d.tag } return $b");
  EXPECT_NE(again, first);
  EXPECT_EQ(rulesOf(again).front().dimension, "d.drop");
}

TEST_F(VQLShapeCellsTest, EveryHandleCarriesAnEpochWitness) {
  const auto handle  = handleOf("let $s := { +d.keep } return $s");
  const auto witness = neighbour(handle, dims().epoch);
  ASSERT_TRUE(witness.has_value());
  EXPECT_EQ(arena.asInt64(*witness),
            static_cast<std::int64_t>(*arena.epochOfCell(handle)));
  EXPECT_FALSE(xanadu::vql::staleShapeReason(arena, dims(), handle));
}

TEST_F(VQLShapeCellsTest, AHandleWhoseWitnessNamesAnotherEpochRefuses) {
  const auto handle  = handleOf("let $s := { +d.keep } return $s");
  const auto witness = *neighbour(handle, dims().epoch);
  ASSERT_TRUE(arena.setValueBits(witness, xanadu::ValueKind::Int64, 12345));
  EXPECT_THAT(refusalOf("##{ $s }"),
              testing::HasSubstr("released and its number reused"));
}

TEST_F(VQLShapeCellsTest, EveryConsumerOfAShapeVariableChecksTheWitness) {
  const auto handle  = handleOf("let $s := { +d.keep } return $s");
  const auto witness = *neighbour(handle, dims().epoch);
  ASSERT_TRUE(arena.setValueBits(witness, xanadu::ValueKind::Int64, 12345));
  for (const char *query : {"##{ $s }", "##[$s]", "project(##, $s)",
                            "apply_shape(##, $s)", "##[{ $s, +d.tag }]"})
    EXPECT_THAT(refusalOf(query),
                testing::HasSubstr("released and its number reused"))
        << query;
}

TEST_F(VQLShapeCellsTest, ANestedShapeWithAStaleWitnessRefusesTheOuter) {
  const auto handle  = handleOf("let $s := { +d.keep: { +d.tag } } return $s");
  const auto inner   = *rulesOf(handle).front().nested;
  const auto witness = *neighbour(inner, dims().epoch);
  ASSERT_TRUE(arena.setValueBits(witness, xanadu::ValueKind::Int64, 12345));
  EXPECT_THAT(refusalOf("##{ $s }"), testing::HasSubstr("nested in a handle"));
}

TEST_F(VQLShapeCellsTest, AReleasedHandleIsUnrecordedNotSilentlyReused) {
  const auto handle = handleOf("let $s := { +d.keep } return $s");
  engine.clearVariables();
  // The session forgot it; the handle's cell outlives that, and a reader
  // with only the cell is told so by name rather than given another shape.
  engine.setVariable("s", handle);
  EXPECT_THAT(refusalOf("##{ $s }"), testing::HasSubstr("no record of"));
}

TEST_F(VQLShapeCellsTest, RuleCellsOutliveTheQueryAndTheNextOne) {
  const auto handle =
      handleOf("let $s := { +d.keep: { +d.tag }, +d.n: 9 } return $s");
  (void)engine.execute("let $other := { +d.drop } return $other");
  const auto rules = rulesOf(handle);
  ASSERT_EQ(rules.size(), 2u);
  EXPECT_EQ(rules[0].kind, "nested");
  EXPECT_EQ(arena.asInt64(*rules[1].target), 9);
  EXPECT_FALSE(xanadu::vql::staleShapeReason(arena, dims(), handle));
  EXPECT_EQ(engine.getVariable("s"), std::vector<CellRef>{handle});
  // And the session still resolves it.
  EXPECT_NO_THROW((void)engine.execute("##[$s]"));
}

TEST_F(VQLShapeCellsTest,
       ClearingTheSessionForgetsTheHandlesButNotTheArenaCells) {
  const auto handle = handleOf("let $s := { +d.keep } return $s");
  ASSERT_EQ(engine.shapeCount(), 1u);
  engine.clearVariables();
  EXPECT_EQ(engine.shapeCount(), 0u);
  EXPECT_EQ(rulesOf(handle).size(), 1u);
  EXPECT_THAT(refusalOf("##{ $s }"), testing::Not(testing::IsEmpty()));
  // A shape made after the clear is whole.
  const auto again = handleOf("let $s := { +d.tag } return $s");
  EXPECT_EQ(rulesOf(again).front().dimension, "d.tag");
}

TEST_F(VQLShapeCellsTest, TheRuleCountCeilingIsTheEnginesNotAConstant) {
  engine.setShapeLimits({.maxRules = 3, .maxDepth = 8});
  EXPECT_NO_THROW(
      (void)engine.execute("let $s := { +d.keep, +d.tag, +d.n } return $s"));
  EXPECT_THAT(
      refusalOf("let $s := { +d.keep, +d.tag, +d.n, +d.drop } return $s"),
      testing::HasSubstr("maximum size 3 rules"));
  engine.setShapeLimits({.maxRules = 5000, .maxDepth = 8});
  EXPECT_NO_THROW((void)engine.execute(
      "let $s := { +d.keep, +d.tag, +d.n, +d.drop } return $s"));
}

TEST_F(VQLShapeCellsTest, TheDepthCeilingIsTheEnginesNotAConstant) {
  engine.setShapeLimits({.maxRules = 64, .maxDepth = 1});
  EXPECT_NO_THROW(
      (void)engine.execute("let $s := { +d.keep: { +d.tag } } return $s"));
  EXPECT_THAT(
      refusalOf("let $s := { +d.keep: { +d.tag: { +d.n } } } return $s"),
      testing::HasSubstr("maximum nesting depth 1"));
}

TEST_F(VQLShapeCellsTest, ThePredicateCeilingsFollowTheSameLimits) {
  engine.setShapeLimits({.maxRules = 2, .maxDepth = 1});
  EXPECT_THAT(refusalOf("##[{ +d.keep, +d.tag, +d.n }]"),
              testing::HasSubstr("maximum"));
  EXPECT_THAT(refusalOf("##[{ +d.keep: { +d.tag: { +d.n } } }]"),
              testing::HasSubstr("nesting depth 1"));
}

TEST(VQLShapeSettings, TheCeilingsAreSettingsSeededWithTheDefaults) {
  const auto specs =
      xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Settings);
  const auto defaultOf =
      [&](std::string_view name) -> std::optional<std::int64_t> {
    for (const auto &spec : specs)
      if (spec.name == name && !spec.schemas.empty() &&
          !spec.schemas.front().defaultValues.empty())
        if (const auto *value = std::get_if<std::int64_t>(
                &spec.schemas.front().defaultValues.front()))
          return *value;
    return std::nullopt;
  };
  EXPECT_EQ(defaultOf(xanadu::settings::kVqlShapeMaxRules),
            static_cast<std::int64_t>(xanadu::vql::defaultShapeMaxRules));
  EXPECT_EQ(defaultOf(xanadu::settings::kVqlShapeMaxDepth),
            static_cast<std::int64_t>(xanadu::vql::defaultShapeMaxDepth));
}

TEST(VQLShapeSettings, AHostHandsItsConfiguredLimitsToItsEngine) {
  zigzag::vortex::VortexHostConfig config;
  config.shapeLimits = {.maxRules = 7, .maxDepth = 3};
  zigzag::Manifold base;
  zigzag::vortex::VortexHost host(&base);
  host.setConfig(config);
  EXPECT_EQ(host.vqlEngine().shapeLimits(), config.shapeLimits);
}

// The static path is unchanged by the cells. Each hash is FNV-1a over the
// program's disassembly with the cell numbers and addresses erased (they move
// when a shape mints cells; the opcodes, labels and operand shapes must not),
// taken from the compiler at df0967b6 before shape values were cells. A line
// count could stay put while an operand changed; a hash cannot.
namespace {
std::string withoutCellNumbers(const std::string_view text) {
  std::string out;
  for (std::size_t at = 0; at < text.size();) {
    if (text.substr(at, 3) == "[0x") {
      at = text.find(']', at) + 1;
    } else if (std::isdigit(static_cast<unsigned char>(text[at]))) {
      while (at < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[at])))
        ++at;
      out += 'N';
    } else {
      out += text[at++];
    }
  }
  return out;
}

std::uint64_t fnv1a(const std::string_view text) {
  constexpr std::uint64_t offsetBasis = 0xcbf29ce484222325ULL;
  constexpr std::uint64_t prime       = 0x100000001b3ULL;
  std::uint64_t hash                  = offsetBasis;
  for (const char c : text)
    hash = (hash ^ static_cast<unsigned char>(c)) * prime;
  return hash;
}
} // namespace

TEST(VQLShapeStaticPrograms, AreUnchangedByTheCells) {
  struct Pin {
    const char *query;
    std::uint64_t hash;
  };
  const std::vector<Pin> pins = {
      {"##{ +d.keep }", 0x46cbf4ae8509fb6cULL},
      {"##{ +d.keep, !d.drop }", 0x59285a096f2b7f90ULL},
      {"let $s := { +d.keep } return ##{ $s }", 0xdb83b43c358abab2ULL},
      {"apply_shape(##, {*, +d.tag: \"x\"})", 0xba457276be09d120ULL},
      {"##[{ +d.keep }]", 0x5ab3999c493f3d49ULL},
      {"project(##, { +d.keep, !d.drop })", 0xd0ee9b0368cd0aeeULL},
      {"let $s := { +d.keep, !d.drop } return ##[$s]", 0x4d3da1446c8ef7abULL},
      {"##[{ +d.keep: { +d.tag } }]", 0xc897280d9747c68cULL},
      {"let $a := { +d.keep }; let $b := { $a, +d.tag } return ##{ $b }",
       0x5dbef0d0f47de7cfULL},
  };
  for (const auto &pin : pins) {
    SCOPED_TRACE(pin.query);
    // A fresh engine each, as the hashes were taken.
    zigzag::ArenaManifold arena;
    VQLEngine engine{arena};
    zigzag::vortex::VortexVM vm{engine.core()};
    xanadu::vql::QueryExecutor executor{engine, vm};
    for (const char *name : {"d.keep", "d.drop", "d.tag", "d.n"})
      (void)engine.resolveDimension(name);
    ASSERT_TRUE(executor.execute(pin.query).has_value());
    EXPECT_EQ(fnv1a(withoutCellNumbers(executor.disassembly())), pin.hash);
  }
}

} // namespace
