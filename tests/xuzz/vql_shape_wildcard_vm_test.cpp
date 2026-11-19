#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

// Shape predicates that count a cell's links: `{}`, `{*}` and the exact forms
// after `!*` (spec 4.8.1). The expectations follow the specification table; the
// pre-cutoff host interpreter, since deleted, treated `{}` and `{*}` as always
// true and `{!*, ...}` as always false, which the specification contradicts.
namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct Fixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{engine, vm};
  const DimRef a{coordinator.resolveDimension("d.a")};
  const DimRef b{coordinator.resolveDimension("d.b")};
  const DimRef t{coordinator.resolveDimension("d.t")};

  // No link at all.
  CellRef none{arena.makeCell("none")};
  // One link, positive on d.a.
  CellRef oneA{arena.makeCell("one-a")};
  // Two links on two dimensions.
  CellRef twoAB{arena.makeCell("two-ab")};
  // One link, negative on d.a.
  CellRef negA{arena.makeCell("neg-a")};
  // Only +d.t.
  CellRef onlyT{arena.makeCell("only-t")};
  // +d.t and +d.a.
  CellRef tAndA{arena.makeCell("t-and-a")};

  Fixture() {
    link(oneA, a, DimVector::POS);
    link(twoAB, a, DimVector::POS);
    link(twoAB, b, DimVector::POS);
    link(negA, a, DimVector::NEG);
    link(onlyT, t, DimVector::POS);
    link(tAndA, t, DimVector::POS);
    link(tAndA, a, DimVector::POS);
  }

  void link(const CellRef from, const DimRef dim, const DimVector direction) {
    zigzag::expectWritten(
        arena.link(from, dim, direction, arena.makeCell("leaf")));
  }

  std::vector<CellRef> all() const {
    return {none, oneA, twoAB, negA, onlyT, tAndA};
  }

  // The members of all() that satisfy the predicate, in order.
  std::vector<CellRef> select(const std::string_view predicate) {
    std::vector<CellRef> kept;
    for (const auto cell : all()) {
      const auto result = executor.execute(
          "for $c in . where " + std::string{predicate} + " return $c", {cell});
      EXPECT_TRUE(result) << predicate << ": " << result.error().message;
      if (result && !result->empty()) kept.push_back(cell);
    }
    return kept;
  }
};

TEST(VQLShapeWildcardVMTest, EmptyShapeHoldsOnlyOnACellWithNoLinks) {
  Fixture h;
  EXPECT_EQ(h.select("?{}"), (std::vector<CellRef>{h.none}));
  EXPECT_EQ(h.select("?{!*}"), (std::vector<CellRef>{h.none}));
}

TEST(VQLShapeWildcardVMTest, EverythingHoldsOnAnyCellWithALink) {
  Fixture h;
  EXPECT_EQ(h.select("?{*}"),
            (std::vector<CellRef>{h.oneA, h.twoAB, h.negA, h.onlyT, h.tAndA}));
}

TEST(VQLShapeWildcardVMTest, ExclusiveFormAllowsNothingBeyondItsRules) {
  Fixture h;
  EXPECT_EQ(h.select("?{!*, +d.t}"), (std::vector<CellRef>{h.onlyT}));
  EXPECT_EQ(h.select("?{!*, +d.a, +d.b}"), (std::vector<CellRef>{h.twoAB}));
  EXPECT_EQ(h.select("?{!*, ~d.a}"), (std::vector<CellRef>{h.oneA, h.negA}));
  EXPECT_EQ(h.select("?{!*, +d.a}"), (std::vector<CellRef>{h.oneA}));
}

TEST(VQLShapeWildcardVMTest, WildcardResetsEarlierRulesInSourceOrder) {
  Fixture h;
  // The presence of +d.t is still required, but the later `!*` forgets that d.t
  // is allowed, so no cell can satisfy both.
  EXPECT_TRUE(h.select("?{+d.t, !*}").empty());
  EXPECT_EQ(h.select("?{!*, *, +d.t}"),
            (std::vector<CellRef>{h.onlyT, h.tAndA}));
}

TEST(VQLShapeWildcardVMTest, ComplementFormNeedsALinkButNoneAlongTheDimension) {
  Fixture h;
  EXPECT_EQ(h.select("?{*, !d.a}"), (std::vector<CellRef>{h.onlyT}));
  EXPECT_EQ(h.select("?{*, !d.t}"),
            (std::vector<CellRef>{h.oneA, h.twoAB, h.negA}));
}

TEST(VQLShapeWildcardVMTest, PlainPresenceRulesStillCountOnlyTheirDimensions) {
  Fixture h;
  EXPECT_EQ(h.select("?{+d.a, +d.b}"), (std::vector<CellRef>{h.twoAB}));
  EXPECT_EQ(h.select("?{+d.a}"),
            (std::vector<CellRef>{h.oneA, h.twoAB, h.tAndA}));
}

TEST(VQLShapeWildcardVMTest, NegationFlipsEveryForm) {
  Fixture h;
  EXPECT_EQ(h.select("not ?{}"),
            (std::vector<CellRef>{h.oneA, h.twoAB, h.negA, h.onlyT, h.tAndA}));
  EXPECT_EQ(h.select("not ?{*}"), (std::vector<CellRef>{h.none}));
  EXPECT_EQ(h.select("not ?{!*, +d.t}"),
            (std::vector<CellRef>{h.none, h.oneA, h.twoAB, h.negA, h.tAndA}));
}

TEST(VQLShapeWildcardVMTest, BracketFilterAndNestedTargetUseTheSameTest) {
  Fixture h;
  const auto target = h.arena.makeCell("holder");
  h.link(target, h.t, DimVector::POS);
  zigzag::expectWritten(
      h.arena.link(h.core.home(), h.a, DimVector::POS, h.none));
  const auto bracket = h.executor.execute("##/d.a[{}]");
  ASSERT_TRUE(bracket) << bracket.error().message;
  // `none` is reached through d.a, so it gained a link and is no longer empty.
  EXPECT_TRUE(bracket->empty());
  const auto filtered = h.executor.execute("##/d.a[{*}]");
  ASSERT_TRUE(filtered) << filtered.error().message;
  EXPECT_EQ(*filtered, (std::vector<CellRef>{h.none}));
}

TEST(VQLShapeWildcardVMTest, ReadOnlyPredicateQueryReleasesItsProgram) {
  Fixture h;
  const auto cells = h.arena.cellCount();
  const auto result =
      h.executor.execute("for $c in . where ?{!*, +d.t} return $c", {h.onlyT});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{h.onlyT}));
  EXPECT_FALSE(h.executor.programRetainedBecause())
      << *h.executor.programRetainedBecause();
  EXPECT_EQ(h.arena.cellCount(), cells);
}

TEST(VQLShapeWildcardVMTest, PredicateLowersToTheProbeOpcode) {
  Fixture h;
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile("for $c in ## where ?{*} return $c");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  std::size_t probes = 0;
  for (const auto opcode : compiled.generatedOpcodes)
    probes +=
        h.vm.getOpcodeKind(opcode) == zigzag::vortex::OpcodeKind::VqlMaskProbe;
  EXPECT_EQ(probes, 1U);
}

TEST(VQLShapeWildcardVMTest, FormsThatStayUnsupportedStillNameTheForm) {
  Fixture h;
  const auto result = h.executor.execute("##/d.a[{$shape}]");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("variable, function"),
            std::string::npos)
      << result.error().message;
}

} // namespace
} // namespace xanadu::vql
