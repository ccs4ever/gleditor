#include <gtest/gtest.h>

#include <filesystem>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::DimVector;

class VQLSemanticsRepairTest : public ::testing::Test {
protected:
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};

  bool answer(std::string_view query) {
    auto result = engine.execute(query);
    EXPECT_EQ(result.size(), 1u);
    return !result.empty() && core.isTruthy(result.front()).value_or(false);
  }

  bool matches(std::string_view predicate) {
    return !engine
                .execute("let $root := ## where " + std::string(predicate) +
                         " return $root")
                .empty();
  }
};

TEST_F(VQLSemanticsRepairTest, UnsupportedSourcesNeverReturnAmbientHome) {
  for (const auto query :
       {"local(\"missing\")", "topic(\"missing\")", "btpk(\"missing\")",
        "system(\"settings\")", "##topic(\"missing\")", "##@(1)", "@1"}) {
    EXPECT_THROW(engine.execute(query), std::runtime_error) << query;
  }
  const auto current = engine.execute("current()");
  ASSERT_EQ(current.size(), 1u);
  EXPECT_EQ(current.front(), core.home());
  EXPECT_TRUE(engine.execute("##absent").empty());
}

TEST_F(VQLSemanticsRepairTest, CellIdentitiesCannotAliasByIntegerNarrowing) {
  const auto real = arena.makeCell("real");
  xanadu::vql::PathExpression path;
  path.anchor.kind   = xanadu::vql::AnchorKind::LiteralCellId;
  path.anchor.cellId = (std::uint64_t{1} << 32) + real;
  EXPECT_THROW(engine.evaluatePath(path), std::runtime_error);
  path.anchor.cellId = 0;
  EXPECT_THROW(engine.evaluatePath(path), std::runtime_error);
}

TEST_F(VQLSemanticsRepairTest, WatchAndDeltaRefuseBeforeManufacturingChanges) {
  const auto subject = arena.makeCell("subject");
  engine.setVariable("s", subject);
  const auto delta       = engine.resolveDimension("d.delta");
  const auto countBefore = arena.cellCount();
  EXPECT_THROW(engine.execute("watch $s as $event { $event }"),
               std::runtime_error);
  EXPECT_THROW(engine.execute("$s/delta()"), std::runtime_error);
  EXPECT_EQ(arena.cellCount(), countBefore);
  EXPECT_FALSE(arena.linked(subject, delta, DimVector::POS));
}

TEST_F(VQLSemanticsRepairTest,
       ComparisonsAreCartesianExistentialIncludingInequality) {
  const auto a = arena.makeCell("a");
  const auto b = arena.makeCell("b");
  engine.setVariable("left", std::vector<zigzag::CellRef>{a, b});
  engine.setVariable("right", std::vector<zigzag::CellRef>{a});
  engine.setVariable("empty", std::vector<zigzag::CellRef>{});
  EXPECT_TRUE(matches("$left = $right"));
  EXPECT_TRUE(matches("$left != $right"));
  EXPECT_FALSE(matches("not ($left = $right)"));
  EXPECT_TRUE(matches("$right != $left"));
  EXPECT_FALSE(matches("$empty = \"\""));
  EXPECT_FALSE(matches("$empty != $left"));
  EXPECT_FALSE(matches("$left != $empty"));
  EXPECT_FALSE(matches("$empty = $empty"));
}

TEST_F(VQLSemanticsRepairTest, ScalarComparisonCoercionBoundariesRemainStable) {
  EXPECT_TRUE(matches(R"("01" = 1)"));
  EXPECT_TRUE(matches(R"("1e0" = 1)"));
  EXPECT_TRUE(matches(R"(1 = 1.0000000005)"));
  EXPECT_FALSE(matches(R"(1 = 1.000000001)"));
  EXPECT_FALSE(matches(R"(1 != 1.0000000005)"));
  EXPECT_TRUE(matches(R"(1 != 1.000000001)"));

  EXPECT_FALSE(matches(R"("" = 0)"));
  EXPECT_TRUE(matches(R"("" != 0)"));
  EXPECT_TRUE(matches(R"("x" < "y")"));
  EXPECT_FALSE(matches(R"("x" = 1)"));

  // Both integers round to the same IEEE-754 double in the current policy.
  EXPECT_TRUE(matches("9007199254740992 = 9007199254740993"));
  EXPECT_FALSE(matches(R"("nan" = "nan")"));
  EXPECT_FALSE(matches(R"("nan" != "nan")"));
  EXPECT_FALSE(matches(R"("inf" < 1)"));
}

TEST_F(VQLSemanticsRepairTest, QuantifiersUseEveryExplicitDomainMember) {
  const auto a = arena.makeScalarCell(std::int64_t{1});
  const auto b = arena.makeScalarCell(std::int64_t{2});
  engine.setVariable("items", std::vector<zigzag::CellRef>{a, b});
  engine.setVariable("empty", std::vector<zigzag::CellRef>{});
  engine.setVariable(".", core.home());
  EXPECT_TRUE(answer("any($items, . = 2)"));
  EXPECT_FALSE(answer("all($items, . = 2)"));
  EXPECT_TRUE(answer("all($items, . > 0)"));
  EXPECT_TRUE(answer("none($items, . = 3)"));
  EXPECT_FALSE(answer("none($items, . = 2)"));
  EXPECT_FALSE(answer("any($empty, true)"));
  EXPECT_TRUE(answer("all($empty, false)"));
  EXPECT_TRUE(answer("none($empty, true)"));
  EXPECT_TRUE(matches("all($items, . > 0)"));
  EXPECT_THROW(engine.execute("all($items)"), std::runtime_error);
  EXPECT_EQ(engine.getVariable("."), std::vector<zigzag::CellRef>{core.home()});
}

TEST_F(VQLSemanticsRepairTest, QuantifiersApplyLambdasAndBoundPathFunctors) {
  const auto a    = arena.makeScalarCell(std::int64_t{1});
  const auto b    = arena.makeScalarCell(std::int64_t{2});
  const auto flag = engine.resolveDimension("d.flag");
  ASSERT_TRUE(arena.link(b, flag, DimVector::POS, arena.makeCell("present")));
  engine.setVariable("items", std::vector<zigzag::CellRef>{a, b});
  EXPECT_TRUE(answer(R"(any($items, \($x) ($x = 2)))"));
  EXPECT_FALSE(answer(R"(all($items, \($x) ($x = 2)))"));
  EXPECT_TRUE(answer(R"(none($items, \($x) ($x = 3)))"));
  EXPECT_TRUE(
      answer("let $has_flag := /+d.flag return any($items, $has_flag)"));
  EXPECT_FALSE(
      answer("let $has_flag := /+d.flag return all($items, $has_flag)"));
  EXPECT_FALSE(
      answer("let $has_flag := /+d.flag return none($items, $has_flag)"));
  EXPECT_TRUE(matches("any($items, ./+d.flag)"));
}

TEST_F(VQLSemanticsRepairTest, CardinalityAndIdentityDoNotConfuseEqualContent) {
  const auto first  = arena.makeCell("same");
  const auto second = arena.makeCell("same");
  engine.setVariable("items", std::vector<zigzag::CellRef>{first, second});
  engine.setVariable("empty", std::vector<zigzag::CellRef>{});
  engine.setVariable("a", first);
  engine.setVariable("b", second);
  EXPECT_TRUE(matches("$a = $b"));
  EXPECT_FALSE(answer("same_cell($a, $b)"));
  EXPECT_TRUE(answer("same_cell($a, $a)"));
  EXPECT_EQ(engine.execute("first($items)"),
            std::vector<zigzag::CellRef>{first});
  EXPECT_TRUE(engine.execute("optional_one($empty)").empty());
  EXPECT_TRUE(engine.execute("first($empty)").empty());
  EXPECT_TRUE(engine.execute("##/d.missing/value(\"x\", 0, 1)").empty());
  EXPECT_THROW(engine.execute("one($empty)"), std::runtime_error);
  EXPECT_THROW(engine.execute("optional_one($items)"), std::runtime_error);
  EXPECT_THROW(engine.execute("same_cell($items, $a)"), std::runtime_error);
  const auto count = engine.execute("count($items)");
  ASSERT_EQ(count.size(), 1u);
  EXPECT_EQ(arena.asInt64(count.front()), 2);
}

TEST_F(VQLSemanticsRepairTest, ProjectCopiesTopologyWithoutChangingSources) {
  const auto subject = arena.makeCell("subject");
  const auto child   = arena.makeCell("child");
  const auto hidden  = arena.makeCell("hidden");
  const auto keep    = engine.resolveDimension("d.keep");
  const auto omit    = engine.resolveDimension("d.omit");
  ASSERT_TRUE(arena.link(subject, keep, DimVector::POS, child));
  ASSERT_TRUE(arena.link(child, keep, DimVector::POS, subject));
  ASSERT_TRUE(arena.link(subject, omit, DimVector::POS, hidden));
  engine.setVariable("s", subject);
  const auto result = engine.execute("project($s, {+d.keep})");
  ASSERT_EQ(result.size(), 1u);
  const auto view = result.front();
  EXPECT_NE(view, subject);
  const auto copiedChild = arena.linked(view, keep, DimVector::POS);
  ASSERT_TRUE(copiedChild);
  EXPECT_NE(*copiedChild, child);
  EXPECT_EQ(arena.linked(*copiedChild, keep, DimVector::POS), view);
  EXPECT_EQ(arena.linked(*copiedChild, keep, DimVector::NEG), view);
  EXPECT_FALSE(arena.linked(view, omit, DimVector::POS));
  EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), child);
  EXPECT_EQ(arena.linked(child, keep, DimVector::NEG), subject);
  EXPECT_EQ(arena.linked(subject, omit, DimVector::POS), hidden);
  EXPECT_EQ(arena.textOf(view), "subject");
  EXPECT_EQ(arena.contentOf(view).front(), arena.contentOf(subject).front());
  EXPECT_THROW(engine.execute("$s{+d.keep}"), std::runtime_error);
  EXPECT_EQ(arena.linked(subject, omit, DimVector::POS), hidden);
}

TEST_F(VQLSemanticsRepairTest, ProjectPreservesForeignStoreQuoteBindings) {
  const auto directory =
      std::filesystem::temp_directory_path() / "vql_projection_quote_test";
  std::filesystem::remove_all(directory);
  {
    xanadu::UserPermascroll::Config config;
    config.storageDir  = directory;
    auto scroll        = std::make_shared<xanadu::UserPermascroll>(config);
    auto store         = std::make_shared<xanadu::Store>(scroll);
    const auto genesis = store->sliceGenesis(xanadu::MicroversionId{});
    const auto dim     = store->makeDimension(genesis, "d.text");
    const auto cell    = store->makeCell(dim.version, "foreign content");
    const auto linked = store->setLink(cell, store->homeCell().value(), dim.dim,
                                       false, store->cellRefOf(cell).value());
    xanadu::MultiStoreCoordinator coordinator;
    coordinator.addStore("source", "primary", store, linked);
    VQLEngine loadedEngine(coordinator);
    const auto original = loadedEngine.execute("##source/+d.text");
    ASSERT_EQ(original.size(), 1u);
    const auto copied = loadedEngine.execute("project(##source/+d.text, {})");
    ASSERT_EQ(copied.size(), 1u);
    EXPECT_NE(copied.front(), original.front());
    EXPECT_EQ(coordinator.arena().textOf(copied.front()), "foreign content");
    const auto sourceQuote =
        coordinator.arena().quotedContent(original.front());
    const auto copiedQuote = coordinator.arena().quotedContent(copied.front());
    ASSERT_TRUE(sourceQuote);
    ASSERT_TRUE(copiedQuote);
    EXPECT_EQ(copiedQuote->store, sourceQuote->store);
    EXPECT_EQ(copiedQuote->spans, sourceQuote->spans);
  }
  std::filesystem::remove_all(directory);
}

TEST_F(VQLSemanticsRepairTest,
       ShapeMasksRespectRuleOrderAndRefuseUnsupportedForms) {
  const auto subject = arena.makeCell("subject");
  const auto foo     = engine.resolveDimension("d.foo");
  const auto bar     = engine.resolveDimension("d.bar");
  const auto a       = arena.makeCell("a");
  const auto b       = arena.makeCell("b");
  ASSERT_TRUE(arena.link(subject, foo, DimVector::POS, a));
  ASSERT_TRUE(arena.link(subject, bar, DimVector::POS, b));
  engine.setVariable("s", subject);
  const auto copied = engine.execute("project($s, {+d.foo, !*, +d.bar})");
  ASSERT_EQ(copied.size(), 1u);
  EXPECT_FALSE(arena.linked(copied.front(), foo, DimVector::POS));
  EXPECT_TRUE(arena.linked(copied.front(), bar, DimVector::POS));
  EXPECT_THROW(engine.execute("apply_shape($s, {+d.foo, d.foo <=> d.bar})"),
               std::runtime_error);
  EXPECT_EQ(arena.linked(subject, foo, DimVector::POS), a);
  EXPECT_EQ(arena.linked(subject, bar, DimVector::POS), b);
  engine.setVariable("target", a);
  engine.execute("apply_shape($s, {+d.foo: $target, !*})");
  EXPECT_FALSE(arena.linked(subject, foo, DimVector::POS));
  EXPECT_FALSE(arena.linked(a, foo, DimVector::NEG));
  EXPECT_THROW(engine.execute("##/d.absent@(88)"), std::runtime_error);
  EXPECT_THROW(engine.execute("##/d.absent/d.foo <=> d.bar"),
               std::runtime_error);
}

TEST_F(VQLSemanticsRepairTest, ApplyShapeExplicitlyMasksAndAssigns) {
  const auto subject     = arena.makeCell("subject");
  const auto kept        = arena.makeCell("kept");
  const auto omitted     = arena.makeCell("omitted");
  const auto replacement = arena.makeCell("replacement");
  const auto keep        = engine.resolveDimension("d.keep");
  const auto omit        = engine.resolveDimension("d.omit");
  ASSERT_TRUE(arena.link(subject, keep, DimVector::POS, kept));
  ASSERT_TRUE(arena.link(subject, omit, DimVector::POS, omitted));
  engine.setVariable("s", subject);
  engine.setVariable("target", replacement);
  EXPECT_EQ(engine.execute("apply_shape($s, {+d.keep: $target})"),
            std::vector<zigzag::CellRef>{subject});
  EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), replacement);
  EXPECT_EQ(arena.linked(replacement, keep, DimVector::NEG), subject);
  EXPECT_FALSE(arena.linked(kept, keep, DimVector::NEG));
  EXPECT_FALSE(arena.linked(subject, omit, DimVector::POS));
  EXPECT_FALSE(arena.linked(omitted, omit, DimVector::NEG));
}

TEST_F(VQLSemanticsRepairTest,
       ShapeAssignmentsRejectReciprocalCollisionsBeforeMasking) {
  const auto first    = arena.makeCell("first");
  const auto second   = arena.makeCell("second");
  const auto target   = arena.makeCell("shared");
  const auto original = arena.makeCell("original");
  const auto edge     = engine.resolveDimension("d.edge");
  const auto keep     = engine.resolveDimension("d.keep");
  ASSERT_TRUE(arena.link(first, keep, DimVector::POS, original));
  engine.setVariable("subjects", std::vector<zigzag::CellRef>{first, second});
  engine.setVariable("target", target);
  for (const auto query : {"apply_shape($subjects, {+d.edge: $target})",
                           "apply_shape($subjects, {-d.edge: $target})"}) {
    EXPECT_THROW(engine.execute(query), std::runtime_error);
    EXPECT_EQ(arena.linked(first, keep, DimVector::POS), original);
    EXPECT_EQ(arena.linked(original, keep, DimVector::NEG), first);
    EXPECT_FALSE(arena.linked(first, edge, DimVector::POS));
    EXPECT_FALSE(arena.linked(first, edge, DimVector::NEG));
    EXPECT_FALSE(arena.linked(second, edge, DimVector::POS));
    EXPECT_FALSE(arena.linked(second, edge, DimVector::NEG));
    EXPECT_FALSE(arena.linked(target, edge, DimVector::POS));
    EXPECT_FALSE(arena.linked(target, edge, DimVector::NEG));
  }
  engine.setVariable("subjects", std::vector<zigzag::CellRef>{first, first});
  EXPECT_NO_THROW(
      engine.execute("apply_shape($subjects, {*, +d.edge: $target})"));
  EXPECT_EQ(arena.linked(first, edge, DimVector::POS), target);
  EXPECT_EQ(arena.linked(target, edge, DimVector::NEG), first);
}

TEST_F(VQLSemanticsRepairTest,
       NestedShapeAssignmentRefusesBeforeTargetsOrMasksAreEvaluated) {
  const auto subject   = arena.makeCell("subject");
  const auto original  = arena.makeCell("original");
  const auto keep      = engine.resolveDimension("d.keep");
  const auto outer     = engine.resolveDimension("d.outer");
  const auto allocated = engine.resolveDimension("d.allocated");
  ASSERT_TRUE(arena.link(subject, keep, DimVector::POS, original));
  engine.setVariable("s", subject);
  for (const auto query :
       {"apply_shape($s, {+d.outer: {+d.inner: \"expected\"}})",
        "apply_shape($s, {+d.first: $s/+d.allocated%, "
        "+d.outer: count({+d.inner: \"expected\"})})"}) {
    EXPECT_THROW(engine.execute(query), std::runtime_error);
    EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), original);
    EXPECT_EQ(arena.linked(original, keep, DimVector::NEG), subject);
    EXPECT_FALSE(arena.linked(subject, outer, DimVector::POS));
    EXPECT_FALSE(arena.linked(subject, allocated, DimVector::POS));
  }
  EXPECT_THROW(engine.execute("def unsupported() := {+d.inner: \"expected\"}; "
                              "apply_shape($s, {+d.outer: unsupported()})"),
               std::runtime_error);
  EXPECT_EQ(arena.linked(subject, keep, DimVector::POS), original);
  EXPECT_FALSE(arena.linked(subject, outer, DimVector::POS));
  EXPECT_NO_THROW(
      engine.execute("apply_shape($s, {*, +d.outer: project($s, {})})"));
}

TEST_F(VQLSemanticsRepairTest,
       LinkTargetsAreIdentitiesAndNumericSentinelsRefuse) {
  const auto subject  = arena.makeCell("subject");
  const auto neighbor = arena.makeCell("neighbor");
  const auto dim      = engine.resolveDimension("d.edge");
  ASSERT_TRUE(arena.link(subject, dim, DimVector::POS, neighbor));
  engine.setVariable("s", subject);
  const auto before = arena.cellCount();
  EXPECT_THROW(engine.execute("$s/link(+d.edge, 0)"), std::runtime_error);
  EXPECT_THROW(engine.execute("$s/link(+d.edge, -1)"), std::runtime_error);
  EXPECT_EQ(arena.cellCount(), before);
  EXPECT_EQ(arena.linked(subject, dim, DimVector::POS), neighbor);
  EXPECT_THROW(engine.execute("$s/new(+d.edge)"), std::runtime_error);
  EXPECT_THROW(engine.execute("$s/break(+d.edge)"), std::runtime_error);
}

TEST_F(VQLSemanticsRepairTest,
       CloneChainHasLeftmostAuthorityAndRejectsRankCollisions) {
  const auto a     = arena.makeCell("authority");
  const auto b     = arena.makeCell("b");
  const auto c     = arena.makeCell("c");
  const auto other = arena.makeCell("other authority");
  engine.setVariable("a", a);
  engine.setVariable("b", b);
  engine.setVariable("c", c);
  engine.setVariable("other", other);
  EXPECT_EQ(engine.execute("$a><$b><$c"), std::vector<zigzag::CellRef>{a});
  EXPECT_EQ(arena.linked(a, core.dims().clone, DimVector::POS), b);
  EXPECT_EQ(arena.linked(b, core.dims().clone, DimVector::POS), c);
  EXPECT_EQ(arena.cloneMaster(c, core.dims().clone), a);
  EXPECT_EQ(core.render(c), core.render(a));
  EXPECT_THROW(engine.execute("$other><$b"), std::runtime_error);
  EXPECT_FALSE(arena.linked(other, core.dims().clone, DimVector::POS));
  EXPECT_EQ(arena.cloneMaster(c, core.dims().clone), a);
  EXPECT_THROW(engine.execute("$a><$a"), std::runtime_error);
}

TEST_F(VQLSemanticsRepairTest, EmptyWalkDoesNotInvokeFunctionsOrLambdas) {
  engine.execute("def f($x) := ##/+d.unexpected%;");
  const auto dimension = engine.resolveDimension("d.unexpected");
  const auto missing   = engine.resolveDimension("d.missing");
  (void)missing;
  const auto before = arena.cellCount();
  EXPECT_TRUE(engine.execute("##/+d.missing/f()").empty());
  EXPECT_TRUE(
      engine.execute("##/+d.missing / \\($x) ##/+d.unexpected%").empty());
  EXPECT_TRUE(engine.execute("$absent/f()").empty());
  EXPECT_TRUE(engine.execute("$absent/##").empty());
  EXPECT_EQ(arena.cellCount(), before);
  EXPECT_FALSE(arena.linked(core.home(), dimension, DimVector::POS));
}

TEST_F(VQLSemanticsRepairTest, EmptyFunctorWalkDoesNotInvokeItsLaterFunction) {
  engine.execute("def f($x) := ##/+d.unexpected%;");
  engine.resolveDimension("d.unexpected");
  engine.resolveDimension("d.missing");
  engine.execute("let $walk := /+d.missing/f()");
  const auto before = arena.cellCount();
  EXPECT_TRUE(engine.execute("##/$walk").empty());
  EXPECT_EQ(arena.cellCount(), before);
}

TEST_F(VQLSemanticsRepairTest, TopLevelCallsStillStartWithoutContext) {
  EXPECT_EQ(engine.execute("def f() := ##; f()"),
            std::vector<zigzag::CellRef>{core.home()});
  const auto count = engine.execute("count($absent)");
  ASSERT_EQ(count.size(), 1u);
  EXPECT_EQ(core.render(count.front()),
            zigzag::vortex::CellValue{std::int64_t{0}});
}

TEST_F(VQLSemanticsRepairTest, EmptyFinalStatementClearsEarlierScriptResults) {
  EXPECT_TRUE(engine.execute("##; ##/+d.missing").empty());
  EXPECT_TRUE(engine.execute("##; let $x := $absent return $x").empty());
  EXPECT_TRUE(engine.execute("def f($x: Int) := $x; ##; f(\"bad\")").empty());
  EXPECT_EQ(engine.execute("$absent; ##"),
            std::vector<zigzag::CellRef>{core.home()});
}

TEST_F(VQLSemanticsRepairTest, ExplicitArgumentsUseCallerCellBindings) {
  const auto caller      = arena.makeCell("caller");
  const auto replacement = arena.makeCell("replacement");
  engine.setVariable("x", caller);
  engine.setVariable("replacement", replacement);
  EXPECT_EQ(
      engine.execute("def second($x, $y) := $y; second($replacement, $x)"),
      std::vector<zigzag::CellRef>{caller});
  EXPECT_EQ(engine.getVariable("x"), std::vector<zigzag::CellRef>{caller});
}

TEST_F(VQLSemanticsRepairTest, ExplicitArgumentsCaptureCallerFunctors) {
  const auto subject  = arena.makeCell("subject");
  const auto expected = arena.makeCell("expected");
  ASSERT_TRUE(arena.link(subject, engine.resolveDimension("d.expected"),
                         DimVector::POS, expected));
  engine.setVariable("subject", subject);
  EXPECT_EQ(engine.execute(R"(
    def via($f, $g, $c) := $c/$g;
    let $f := /+d.expected;
    let $replacement := /+d.other;
    via($replacement, $f, $subject)
  )"),
            std::vector<zigzag::CellRef>{expected});
}

TEST_F(VQLSemanticsRepairTest, DefaultsSeeEarlierCalleeParameters) {
  const auto replacement = arena.makeCell("replacement");
  engine.setVariable("replacement", replacement);
  EXPECT_EQ(engine.execute("def f($x, $y := $x) := $y; f($replacement)"),
            std::vector<zigzag::CellRef>{replacement});
  EXPECT_TRUE(engine.execute("def need($x: Cell) := $x; need()").empty());
}

TEST_F(VQLSemanticsRepairTest, ExcessArityFailsBeforeEvaluatingArguments) {
  const auto before = arena.cellCount();
  EXPECT_THROW(engine.execute("def f($x) := $x; f(##/+d.created%, \"extra\")"),
               std::runtime_error);
  EXPECT_EQ(arena.cellCount(), before);
  const auto a = arena.makeCell("a");
  const auto b = arena.makeCell("b");
  engine.setVariable("a", a);
  engine.setVariable("b", b);
  EXPECT_EQ(engine.execute("def rest($xs...) := $xs; rest($a, $b)"),
            (std::vector<zigzag::CellRef>{a, b}));
}

TEST_F(VQLSemanticsRepairTest,
       FunctionFailuresRestoreCallerBindingsAndFunctors) {
  const auto caller      = arena.makeCell("caller");
  const auto replacement = arena.makeCell("replacement");
  const auto target      = arena.makeCell("target");
  ASSERT_TRUE(arena.link(caller, engine.resolveDimension("d.original"),
                         DimVector::POS, target));
  engine.setVariable("x", caller);
  engine.setVariable("replacement", replacement);
  engine.execute("let $f := /+d.original");
  for (
      const auto query :
      {R"(def bad($x, $y := one($absent)) := $x; bad($replacement))",
       R"(def bad($x, $y: Unknown) := $x; bad($replacement, $replacement))",
       R"(def bad($x) := { let $f := /+d.other; return one($absent); }; bad($replacement))",
       R"(def bad($x): Int := $x; bad($replacement))"}) {
    EXPECT_THROW(engine.execute(query), std::runtime_error) << query;
    EXPECT_EQ(engine.getVariable("x"), std::vector<zigzag::CellRef>{caller});
    EXPECT_EQ(engine.execute("$x/$f"), std::vector<zigzag::CellRef>{target});
  }
}

TEST_F(VQLSemanticsRepairTest, LambdaFailureRestoresCallerBindingsAndFunctors) {
  const auto caller      = arena.makeCell("caller");
  const auto replacement = arena.makeCell("replacement");
  const auto target      = arena.makeCell("target");
  ASSERT_TRUE(arena.link(caller, engine.resolveDimension("d.original"),
                         DimVector::POS, target));
  engine.setVariable("x", caller);
  engine.setVariable("replacement", replacement);
  engine.execute("let $f := /+d.original");
  EXPECT_THROW(engine.execute(R"(
    $replacement / \($x) {
      let $f := /+d.other;
      return one($absent);
    }
  )"),
               std::runtime_error);
  EXPECT_EQ(engine.getVariable("x"), std::vector<zigzag::CellRef>{caller});
  EXPECT_EQ(engine.execute("$x/$f"), std::vector<zigzag::CellRef>{target});
}

TEST_F(VQLSemanticsRepairTest, PerspectiveFailuresRestoreAmbientSelection) {
  const auto subject = arena.makeCell("subject");
  engine.setVariable("p", subject);
  engine.execute(R"(
    def bad($p): Int := "invalid";
    with perspective("alice") { $p/+d.comment => [ "Alice note" ] }
    with perspective("bob") { $p/+d.comment => [ "Bob note" ] }
  )");
  ASSERT_TRUE(engine.execute("$p/+d.comment").empty());

  for (
      const auto query : {
          R"(with perspective("alice") { watch ## {} })",
          R"(with perspective("bob") { with perspective("alice") { bad($p) } })",
          R"($p/@alice:bad())",
      }) {
    SCOPED_TRACE(query);
    EXPECT_THROW(engine.execute(query), std::runtime_error);
    EXPECT_TRUE(engine.execute("$p/+d.comment").empty());
  }

  const auto nested = engine.execute(R"(
    with perspective("bob") {
      with perspective("alice") { $p/+d.comment }
      $p/+d.comment
    }
  )");
  ASSERT_EQ(nested.size(), 1u);
  EXPECT_EQ(arena.textOf(nested.front()), "Bob note");
  engine.execute(R"($p/+d.comment => [ "Canonical note" ])");
  const auto canonical = engine.execute("$p/+d.comment");
  ASSERT_EQ(canonical.size(), 1u);
  EXPECT_EQ(arena.textOf(canonical.front()), "Canonical note");
  const auto alice = engine.execute("$p/@alice:+d.comment");
  ASSERT_EQ(alice.size(), 1u);
  EXPECT_EQ(arena.textOf(alice.front()), "Alice note");
}

TEST_F(VQLSemanticsRepairTest, UnknownFunctionsRefuseInEveryEvaluationContext) {
  for (const auto query : {
           "definitely_missing_function()",
           R"(contains("abc", definitely_missing_function()))",
           R"(def f() := definitely_missing_function(); f())",
       }) {
    SCOPED_TRACE(query);
    const auto before = arena.cellCount();
    try {
      engine.execute(query);
      FAIL() << "unknown function fabricated a result";
    } catch (const std::runtime_error &error) {
      EXPECT_NE(
          std::string_view(error.what()).find("definitely_missing_function"),
          std::string_view::npos);
      EXPECT_NE(std::string_view(error.what()).find("0 argument(s)"),
                std::string_view::npos);
    }
    EXPECT_EQ(arena.cellCount(), before);
  }
}

TEST_F(VQLSemanticsRepairTest,
       UnsupportedBuiltinArityRefusesBeforeArgumentsRun) {
  for (const auto query : {
           "contains()",
           R"(contains("abc"))",
           R"(contains("abc", "a", ##/+d.unexpected%))",
           R"(contains("abc", contains("a")))",
           R"(contains("abc", contains("a", "a", ##/+d.unexpected%)))",
           "count($absent, ##/+d.unexpected%)",
           "contains(\"abc\", count($absent, ##/+d.unexpected%))",
           "##/value(0, 0, 1, ##/+d.unexpected%)",
           "one()",
       }) {
    SCOPED_TRACE(query);
    const auto before = arena.cellCount();
    EXPECT_THROW(engine.execute(query), std::runtime_error);
    EXPECT_EQ(arena.cellCount(), before);
  }
  EXPECT_TRUE(answer(R"(contains("abc", "b"))"));
  EXPECT_FALSE(answer(R"(contains("abc", "z"))"));
  // Missing operands supply no text to a substring comparison.
  EXPECT_FALSE(answer(R"(contains("abc", $absent))"));
}

TEST_F(VQLSemanticsRepairTest, InvalidFunctionAssignmentPreservesSubject) {
  const auto subject  = arena.makeCell("subject");
  const auto original = arena.makeCell("original");
  const auto edge     = engine.resolveDimension("d.edge");
  const auto target   = engine.resolveDimension("d.target");
  ASSERT_TRUE(arena.link(subject, edge, DimVector::POS, original));
  engine.setVariable("s", subject);
  for (
      const auto query : {
          R"(apply_shape($s, {+d.target: definitely_missing_function()}))",
          R"(def f() := definitely_missing_function(); apply_shape($s, {+d.target: f()}))",
          R"(apply_shape($s, {+d.target: contains("a", "a", "extra")}))",
      }) {
    SCOPED_TRACE(query);
    const auto before = arena.cellCount();
    EXPECT_THROW(engine.execute(query), std::runtime_error);
    EXPECT_EQ(arena.cellCount(), before);
    EXPECT_EQ(arena.textOf(subject), "subject");
    EXPECT_EQ(arena.linked(subject, edge, DimVector::POS), original);
    EXPECT_EQ(arena.linked(original, edge, DimVector::NEG), subject);
    EXPECT_FALSE(arena.linked(subject, target, DimVector::POS));
  }
}

TEST_F(VQLSemanticsRepairTest, EmptyMacroStepsNeverInvokeDownstreamBodies) {
  engine.execute("def f($x) := ##/+d.unexpected%;");
  engine.resolveDimension("d.missing");
  const auto unexpected = engine.resolveDimension("d.unexpected");
  const auto before     = arena.cellCount();
  for (const auto query : {
           "##/(/+d.missing/f())",
           "##/(/(/+d.missing)/f())",
           "##/(/+d.missing/f())+",
           "##/(/+d.missing/##/f())",
           R"(##/(/+d.missing/\($x) ##/+d.unexpected%))",
       }) {
    SCOPED_TRACE(query);
    EXPECT_TRUE(engine.execute(query).empty());
    EXPECT_EQ(arena.cellCount(), before);
    EXPECT_FALSE(arena.linked(core.home(), unexpected, DimVector::POS));
  }
  for (const auto query : {"##/(/+d.missing/f())?", "##/(/+d.missing/f())*"}) {
    SCOPED_TRACE(query);
    EXPECT_EQ(engine.execute(query), std::vector<zigzag::CellRef>{core.home()});
    EXPECT_EQ(arena.cellCount(), before);
  }
}

TEST_F(VQLSemanticsRepairTest, EmptyWalkOnlyRunsArgumentFreeBuiltinCount) {
  engine.resolveDimension("d.missing");
  const auto unexpected = engine.resolveDimension("d.unexpected");
  const auto counted    = engine.execute("##/d.missing/count()");
  ASSERT_EQ(counted.size(), 1U);
  EXPECT_EQ(arena.asInt64(counted.front()), 0);
  const auto before = arena.cellCount();
  EXPECT_TRUE(engine.execute("##/d.missing/count(##/+d.unexpected%)").empty());
  EXPECT_EQ(arena.cellCount(), before);
  EXPECT_FALSE(arena.linked(core.home(), unexpected, DimVector::POS));
  engine.execute("def count() := ##/+d.unexpected%;");
  EXPECT_TRUE(engine.execute("##/d.missing/count()").empty());
  EXPECT_EQ(arena.cellCount(), before);
  EXPECT_FALSE(arena.linked(core.home(), unexpected, DimVector::POS));
}

TEST_F(VQLSemanticsRepairTest, EmptyFieldWeaveStopsBeforeCallingLaterFunction) {
  engine.execute(R"(def f() := ##/+d.unexpected%"unexpected";)");
  const auto unexpected = engine.resolveDimension("d.unexpected");

  EXPECT_TRUE(
      engine.execute(R"(for $h in ##/missing return /+d.missing/f())").empty());
  EXPECT_FALSE(arena.linked(core.home(), unexpected, DimVector::POS));
}

} // namespace
