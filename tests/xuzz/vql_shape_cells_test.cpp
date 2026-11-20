// V5, the data half: a shape value's rules are cells on the rank a view's are
// on, its targets are values captured when it was made, and each handle
// carries an epoch witness. The compile-time table still serves the static
// lowering, which these tests pin so the cells stay an addition.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
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
      view.nested        = neighbour(*rule, d.nested);
      if (const auto to = neighbour(*rule, d.to))
        view.binder = arena.textOf(*to);
      rules.push_back(std::move(view));
    }
    return rules;
  }

  std::size_t programSize(const std::string &query) {
    EXPECT_TRUE(executor.execute(query).has_value());
    const std::string text{executor.disassembly()};
    return static_cast<std::size_t>(std::ranges::count(text, '\n'));
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
  const auto b =
      rulesOf(handleOf("let $s := { +d.tag: \"x\", +d.n: 3 } return $s"));
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

TEST_F(VQLShapeCellsTest, AComputedTargetInALoopRefusesByName) {
  engine.setVariable(
      "items", std::vector<CellRef>{arena.makeCell("a"), arena.makeCell("b")});
  EXPECT_THAT(
      refusalOf("for $i in ##/+d.keep let $s := { +d.tag: $i } return $s"),
      testing::HasSubstr("inside a function body or a loop"));
  // A literal target is the same cell every turn and nothing is written.
  EXPECT_EQ(
      engine.execute("for $i in $items let $s := { +d.tag: \"x\" } return $s")
          .size(),
      2u);
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

// The static path is unchanged by the cells: these counts were taken from the
// compiler at df0967b6, before shape values were written as cells, so a count
// that moves means a shape started costing the graph something.
TEST_F(VQLShapeCellsTest, StaticProgramsAreUnchangedByTheCells) {
  struct Pin {
    const char *query;
    std::size_t lines;
  };
  const std::vector<Pin> pins = {
      {"##{ +d.keep }", 11},
      {"##{ +d.keep, !d.drop }", 11},
      {"let $s := { +d.keep } return ##{ $s }", 19},
      {"apply_shape(##, {*, +d.tag: \"x\"})", 27},
      {"##[{ +d.keep }]", 27},
      {"project(##, { +d.keep, !d.drop })", 16},
      {"let $s := { +d.keep, !d.drop } return ##[$s]", 44},
      {"##[{ +d.keep: { +d.tag } }]", 40},
      {"let $a := { +d.keep }; let $b := { $a, +d.tag } return ##{ $b }", 21},
  };
  for (const auto &pin : pins) {
    SCOPED_TRACE(pin.query);
    EXPECT_EQ(programSize(pin.query), pin.lines);
  }
}

} // namespace
