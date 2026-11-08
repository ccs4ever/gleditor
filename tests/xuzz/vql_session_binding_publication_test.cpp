#include <gtest/gtest.h>

#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct SessionBindingHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  const DimRef values{coordinator.resolveDimension("d.values")};
  const CellRef one{arena.makeScalarCell(std::int64_t{1})};
  const CellRef two{arena.makeScalarCell(std::int64_t{2})};

  SessionBindingHarness() {
    zigzag::expectWritten(arena.link(core.home(), values, DimVector::POS, one));
    zigzag::expectWritten(arena.link(one, values, DimVector::POS, two));
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(std::string_view query) {
    zigzag::vortex::VortexVM vm{core};
    QueryExecutor executor(engine, vm);
    return executor.execute(query);
  }
};

TEST(VQLSessionBindingPublicationTest,
     PublishesOrderedCellsAndKeepsAnEmptyBindingDistinctFromUnknown) {
  SessionBindingHarness h;
  const auto defined = h.run("let $saved := ##/d.values; return $saved");
  ASSERT_TRUE(defined) << defined.error().message;
  ASSERT_EQ(*defined, (std::vector<CellRef>{h.one, h.two}));
  EXPECT_EQ(h.engine.getVariable("saved"),
            (std::vector<CellRef>{h.one, h.two}));

  const auto empty = h.run("let $empty := ##/d.missing; return $empty");
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());
  EXPECT_TRUE(h.engine.variables().contains("empty"));
  EXPECT_TRUE(h.engine.getVariable("empty").empty());
  zigzag::expectWritten(h.arena.setValueBits(h.one, ValueKind::Int64,
                                             static_cast<std::uint64_t>(9)));
  const auto reused = h.run("return $saved");
  ASSERT_TRUE(reused) << reused.error().message;
  EXPECT_EQ(*reused, (std::vector<CellRef>{h.one, h.two}));
  EXPECT_EQ(h.arena.asInt64(h.one), std::optional<std::int64_t>{9});
}

TEST(VQLSessionBindingPublicationTest, PreservesWideIntegerAndDoubleBindings) {
  SessionBindingHarness h;
  const auto result =
      h.run("let $wide := 9007199254740993; let $fraction := 1.25; "
            "return $wide, $fraction");
  ASSERT_TRUE(result) << result.error().message;
  const auto wide     = h.engine.getVariable("wide");
  const auto fraction = h.engine.getVariable("fraction");
  ASSERT_EQ(wide.size(), 1U);
  ASSERT_EQ(fraction.size(), 1U);
  EXPECT_EQ(h.arena.asInt64(wide.front()),
            std::optional<std::int64_t>{9007199254740993});
  EXPECT_EQ(h.arena.asDouble(fraction.front()), std::optional<double>{1.25});
}

TEST(VQLSessionBindingPublicationTest,
     CapturedLambdaCanRunInANewVmForTheSameArena) {
  SessionBindingHarness h;
  h.engine.setVariable("input", h.one);
  const auto created =
      h.run(R"(let $saved := ##/d.values; let $f := \($x) $saved; return $f)");
  ASSERT_TRUE(created) << created.error().message;
  ASSERT_TRUE(h.engine.variables().contains("f"));
  const auto applied = h.run("return $input/$f");
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(*applied, (std::vector<CellRef>{h.one, h.two}));
}

TEST(VQLSessionBindingPublicationTest,
     RecursiveClosureKeepsItsCreatorFrameAndCanBeRebound) {
  SessionBindingHarness h;
  constexpr std::string_view makeClosure = R"(
    def pure make($depth: Int) := {
      let $owned := string(9007199254740993);
      if $depth > 0 return (make($depth + -1))
      else return (\($item) $owned);
    };
    let $f := make(2); return $f
  )";
  const auto created                     = h.run(makeClosure);
  ASSERT_TRUE(created) << created.error().message;
  h.engine.setVariable("input", h.core.home());
  const auto applied = h.run("return $input/$f");
  ASSERT_TRUE(applied) << applied.error().message;
  ASSERT_EQ(applied->size(), 1U);
  EXPECT_EQ(h.arena.textOf(applied->front()), "9007199254740993");

  const auto replacement = h.arena.makeCell("replacement");
  h.engine.setVariable("saved", replacement);
  const auto rebound = h.run(R"(let $f := \($x) $saved; return $f)");
  ASSERT_TRUE(rebound) << rebound.error().message;
  const auto appliedReplacement = h.run("return $input/$f");
  ASSERT_TRUE(appliedReplacement) << appliedReplacement.error().message;
  EXPECT_EQ(*appliedReplacement, (std::vector<CellRef>{replacement}));
}

TEST(VQLSessionBindingPublicationTest, DoesNotPublishFunctionLocalLets) {
  SessionBindingHarness h;
  const auto result = h.run(
      "def pure localCell(): Cell := { let $inside := ##; return $inside; }; "
      "localCell()");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(h.engine.variables().contains("inside"));
}

TEST(VQLSessionBindingPublicationTest,
     RebindingStaticFunctorAndCellClearsShadow) {
  SessionBindingHarness h;
  h.engine.setVariable("route", h.one);
  const auto toFunctor = h.run("let $route := /+d.values; return ##");
  ASSERT_TRUE(toFunctor) << toFunctor.error().message;
  EXPECT_FALSE(h.engine.variables().contains("route"));
  EXPECT_TRUE(h.engine.compilerFunctors().contains("route"));
  const auto applied = h.run("return ##/d.values/$route");
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_EQ(*applied, (std::vector<CellRef>{h.two}));

  const auto changedFunctor = h.run("let $route := /+d.missing; return ##");
  ASSERT_TRUE(changedFunctor) << changedFunctor.error().message;
  const auto empty = h.run("return ##/d.values/$route");
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());

  const auto toCells = h.run("let $route := ##/d.values; return $route");
  ASSERT_TRUE(toCells) << toCells.error().message;
  EXPECT_FALSE(h.engine.compilerFunctors().contains("route"));
  EXPECT_EQ(h.engine.getVariable("route"),
            (std::vector<CellRef>{h.one, h.two}));
}

TEST(VQLSessionBindingPublicationTest, FailedQueryDoesNotPublishItsLet) {
  SessionBindingHarness h;
  const auto failed =
      h.run("def pure broken($value): Int := \"wrong type\"; "
            "let $uncommitted := ##/d.values; return broken(1)");
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_FALSE(h.engine.variables().contains("uncommitted"));
}

} // namespace
} // namespace xanadu::vql
