#include <algorithm>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct ForeignRank {
  std::shared_ptr<Store> store =
      std::make_shared<Store>(std::make_shared<UserPermascroll>());
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  std::vector<CellRef> foreign;
  CellRef anchor;
  zigzag::DimRef dimension;
  zigzag::DimRef foreignDimension;
  std::uint32_t space;

  ForeignRank(std::size_t count, bool ring = false) {
    auto version     = store->sliceGenesis({});
    const auto axis  = store->makeDimension(version, "d.items");
    version          = axis.version;
    foreignDimension = axis.dim;
    auto previous    = *store->homeCell();
    for (std::size_t i = 0; i < count; ++i) {
      version         = store->makeCell(version, std::to_string(i));
      const auto cell = store->cellRefOf(version).value();
      version =
          store->setLink(version, previous, axis.dim, DimVector::POS, cell);
      previous = cell;
      foreign.push_back(cell);
    }
    if (ring)
      version = store->setLink(version, previous, axis.dim, DimVector::POS,
                               *store->homeCell());
    coordinator.addStore("foreign", "primary", store, version);
    anchor    = *coordinator.resolveNamedStore("foreign");
    space     = arena.foreignOf(anchor)->space;
    dimension = coordinator.resolveDimension("d.items");
  }

  void expectForeign(std::span<const CellRef> actual,
                     std::span<const CellRef> expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      const auto ref = arena.foreignOf(actual[i]);
      ASSERT_TRUE(ref);
      EXPECT_EQ(ref->space, space);
      EXPECT_EQ(ref->index, expected[i]);
    }
  }
};

TEST(VQLForeignStreamTest, LongRanksMintNeighborsAndRetainCanonicalIdentity) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    ForeignRank h(256);
    EXPECT_FALSE(h.arena.findExistingProxy(h.space, h.foreign.back()));
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute(
        "def identity($x: Int): Int := $x; ./d.items/identity()", {h.anchor});
    ASSERT_TRUE(result) << result.error().message;
    h.expectForeign(*result, h.foreign);
    const auto again = executor.execute("./d.items", {h.anchor});
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(*again, *result);
    for (const auto query : {"##foreign/d.items", "##/d.items", "#/d.items"}) {
      const auto anchored = executor.execute(query);
      ASSERT_TRUE(anchored) << anchored.error().message;
      EXPECT_EQ(*anchored, *result) << query;
    }
    const auto missingStore = executor.execute("##missing/d.items");
    ASSERT_TRUE(missingStore) << missingStore.error().message;
    EXPECT_TRUE(missingStore->empty());
    const auto backwards = executor.execute("./-d.items", {result->back()});
    ASSERT_TRUE(backwards) << backwards.error().message;
    auto reverse = h.foreign;
    reverse.pop_back();
    std::ranges::reverse(reverse);
    reverse.push_back(*h.store->homeCell());
    h.expectForeign(*backwards, reverse);
    const auto whole = executor.execute("./-d.items::rank", {result->back()});
    ASSERT_TRUE(whole) << whole.error().message;
    auto wholeExpected = h.foreign;
    std::ranges::reverse(wholeExpected);
    wholeExpected.push_back(*h.store->homeCell());
    h.expectForeign(*whole, wholeExpected);
    const auto absent = executor.execute("./d.absent", {h.anchor});
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_TRUE(absent->empty());
  }
}

TEST(VQLForeignStreamTest, ForeignCyclesTerminateAndLocalEdgesOverride) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    ForeignRank h(3, true);
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute("./~d.items", {h.anchor});
    ASSERT_TRUE(result) << result.error().message;
    auto expected = h.foreign;
    expected.insert(expected.end(), h.foreign.rbegin(), h.foreign.rend());
    h.expectForeign(*result, expected);
    const auto local = h.arena.makeCell("local");
    zigzag::expectWritten(
        h.arena.link(h.anchor, h.dimension, DimVector::POS, local));
    const auto overlay = executor.execute("./d.items", {h.anchor});
    ASSERT_TRUE(overlay) << overlay.error().message;
    EXPECT_EQ(*overlay, (std::vector<CellRef>{local}));
  }
}

TEST(VQLForeignStreamTest,
     ReverseWholeRankUsesCanonicalForwardOrderWithOverlay) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    ForeignRank h(3);
    const auto middle = h.arena.proxyFor(h.space, h.foreign[1]);
    const auto local  = h.arena.makeCell("overlay");
    zigzag::expectWritten(
        h.arena.link(middle, h.dimension, DimVector::NEG, local));
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    const auto result = executor.execute("./-d.items::rank", {h.anchor});
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    const auto first = h.arena.foreignOf(result->front());
    ASSERT_TRUE(first);
    EXPECT_EQ(first->index, h.foreign[0]);
    EXPECT_EQ(result->back(), h.anchor);

    const auto localComponent = executor.execute("./-d.items", {middle});
    ASSERT_TRUE(localComponent) << localComponent.error().message;
    EXPECT_EQ(*localComponent, (std::vector<CellRef>{local}));
    EXPECT_EQ(h.arena.linked(local, h.dimension, DimVector::POS), middle);
  }
}

TEST(VQLForeignStreamTest,
     QueryDereferenceResolvesForeignMasterAndOverlayCycles) {
  for (const auto mode : {QueryExecutionMode::Vortex}) {
    ForeignRank h(3);
    // Bind the same foreign rank as d.clone without materializing its
    // neighbors.
    h.arena.bindDimension(h.core.dims().clone, h.space, h.foreignDimension);
    const auto tail = h.arena.proxyFor(h.space, h.foreign.back());
    h.engine.setVariable("x", tail);
    QueryExecutor executor(h.engine, h.vm, {.mode = mode});
    h.arena.setExecutionLimits(100'000, 0, 100'000);
    const auto exhausted = executor.execute("$x>");
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().category,
              QueryExecutionError::Category::Budget);
    EXPECT_FALSE(h.arena.findExistingProxy(h.space, h.foreign.front()));
    h.arena.setExecutionLimits(100'000, 100'000, 100'000);
    const auto master = executor.execute("$x>");
    ASSERT_TRUE(master) << master.error().message;
    EXPECT_EQ(*master, (std::vector<CellRef>{h.anchor}));
    const auto first  = h.arena.proxyFor(h.space, h.foreign.front());
    const auto second = h.arena.proxyFor(h.space, h.foreign[1]);
    // The late-bound proxy rank must first replace the authored representative
    // with its matching proxy. The local closure then forms a reciprocal
    // two-cell cycle and detaches the former tail neighbor.
    zigzag::expectWritten(
        h.arena.link(h.anchor, h.core.dims().clone, DimVector::POS, first));
    zigzag::expectWritten(
        h.arena.link(first, h.core.dims().clone, DimVector::NEG, second));
    EXPECT_EQ(h.arena.linked(first, h.core.dims().clone, DimVector::NEG),
              second);
    EXPECT_EQ(h.arena.linked(second, h.core.dims().clone, DimVector::POS),
              first);
    EXPECT_FALSE(h.arena.linked(tail, h.core.dims().clone, DimVector::NEG));
    const auto lasso = executor.execute("$x>");
    ASSERT_TRUE(lasso) << lasso.error().message;
    EXPECT_EQ(*lasso, (std::vector<CellRef>{tail}));
  }
}

TEST(VQLForeignStreamTest, ChoicePointRollsBackProxyMintAndIteratorPosition) {
  ForeignRank h(3);
  const auto iterator = h.arena.makeCell();
  const auto item     = h.arena.makeCell();
  const auto present  = h.arena.makeCell();
  const auto begin    = h.vm.mintOpcode(OpcodeKind::RankBegin);
  const auto next     = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const auto halt     = h.vm.mintOpcode(OpcodeKind::Halt);
  zigzag::expectWritten(
      h.arena.link(begin, h.core.dims().spin, DimVector::POS, next));
  zigzag::expectWritten(
      h.arena.link(next, h.core.dims().spin, DimVector::POS, halt));
  h.core.bindInput(begin, h.anchor);
  h.core.bindInput(
      begin, h.arena.makeScalarCell(static_cast<std::int64_t>(h.dimension)));
  h.core.bindInput(begin, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(begin, iterator);
  h.core.bindInput(next, iterator);
  h.core.bindOutput(next, item);
  h.core.bindOutput(next, present);
  const auto cursor = h.vm.spawnCursor(begin, "foreign-choice");
  EXPECT_FALSE(h.vm.run(cursor, 2).success);
  ASSERT_TRUE(h.vm.resultCell(item));
  EXPECT_EQ(h.arena.foreignOf(*h.vm.resultCell(item))->index, h.foreign[0]);
  h.vm.pushChoicePoint(cursor, next);
  h.vm.setCursorOpcode(cursor, next);
  h.arena.setExecutionLimits(100'000, 0, 100'000);
  const auto exhausted = h.vm.run(cursor, 1);
  EXPECT_FALSE(exhausted.success);
  EXPECT_NE(exhausted.errorMessage.find("work budget exhausted"),
            std::string::npos);
  EXPECT_FALSE(h.arena.findExistingProxy(h.space, h.foreign[1]));
  h.arena.setExecutionLimits(100'000, 100'000, 100'000);
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  const auto second = h.vm.resultCell(item);
  ASSERT_TRUE(second);
  EXPECT_EQ(h.arena.foreignOf(*second)->index, h.foreign[1]);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_FALSE(h.arena.findExistingProxy(h.space, h.foreign[1]));
  EXPECT_FALSE(h.vm.run(cursor, 1).success);
  ASSERT_TRUE(h.vm.resultCell(item));
  EXPECT_EQ(h.arena.foreignOf(*h.vm.resultCell(item))->index, h.foreign[1]);
}
} // namespace
} // namespace xanadu::vql
