#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

// Link-counting shape predicates over cells of an attached foreign store. Every
// store proxy carries an internal store-ref index link; it is bookkeeping, so
// `{}` must hold on a proxy of a cell with no user links (spec 4.8.1).
namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct Foreign {
  std::shared_ptr<Store> store;
  zigzag::Manifold manifold;
  DimRef a{zigzag::noCell};
  DimRef b{zigzag::noCell};
  CellRef none{zigzag::noCell};
  CellRef one{zigzag::noCell};
  CellRef two{zigzag::noCell};
};

Foreign makeForeign() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("vql_shape_wildcard_foreign_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  UserPermascroll::Config config;
  config.storageDir = directory / "permascroll";
  auto store =
      std::make_shared<Store>(std::make_shared<UserPermascroll>(config));
  auto version     = store->sliceGenesis(MicroversionId{});
  const auto axisA = store->makeDimension(version, "d.a");
  const auto axisB = store->makeDimension(axisA.version, "d.b");
  version          = axisB.version;
  const auto cell  = [&](const std::string_view text) {
    version = store->makeCell(version, text);
    return store->cellRefOf(version).value();
  };
  const auto none  = cell("none");
  const auto one   = cell("one");
  const auto two   = cell("two");
  const auto leaf  = cell("leaf");
  const auto leaf2 = cell("leaf two");
  version          = store->setLink(version, one, axisA.dim, false, leaf);
  version          = store->setLink(version, two, axisA.dim, false, leaf2);
  const auto leaf3 = cell("leaf three");
  version          = store->setLink(version, two, axisB.dim, false, leaf3);
  auto manifold    = store->rebuildManifold(version);
  return Foreign{.store    = std::move(store),
                 .manifold = std::move(manifold),
                 .a        = axisA.dim,
                 .b        = axisB.dim,
                 .none     = none,
                 .one      = one,
                 .two      = two};
}

struct Fixture {
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
  CellRef twoProxy{arena.proxyFor(space, foreign.two)};

  explicit Fixture(const bool bindAxes) {
    if (!bindAxes) return;
    arena.bindDimension(arena.ensureDimension("d.a"), space, foreign.a,
                        zigzag::DimensionBindingMode::Explicit);
    arena.bindDimension(arena.ensureDimension("d.b"), space, foreign.b,
                        zigzag::DimensionBindingMode::Explicit);
  }

  bool holds(const CellRef cell, const std::string_view predicate) {
    const auto result = executor.execute(
        "for $c in . where " + std::string{predicate} + " return $c", {cell});
    EXPECT_TRUE(result) << predicate << ": " << result.error().message;
    return result && !result->empty();
  }
};

struct Row {
  std::string_view predicate;
  bool none;
  bool one;
  bool two;
};

constexpr Row rows[] = {
    {"?{}", true, false, false},         {"?{!*}", true, false, false},
    {"?{*}", false, true, true},         {"?{*, !d.x}", false, true, true},
    {"?{!*, +d.a}", false, true, false},
};

void expectRows(const bool bound) {
  Fixture h(bound);
  for (const auto &row : rows) {
    SCOPED_TRACE(std::string{row.predicate} + (bound ? " bound" : " unbound"));
    EXPECT_EQ(h.holds(h.noneProxy, row.predicate), row.none);
    EXPECT_EQ(h.holds(h.oneProxy, row.predicate), row.one);
    EXPECT_EQ(h.holds(h.twoProxy, row.predicate), row.two);
  }
}

TEST(VQLShapeWildcardForeignTest, ProxyCellsAnswerByUserLinksWithBoundAxes) {
  expectRows(true);
}

TEST(VQLShapeWildcardForeignTest, ProxyCellsAnswerByUserLinksWithUnboundAxes) {
  expectRows(false);
}

TEST(VQLShapeWildcardForeignTest, MixedLocalAndForeignStreamKeepsItsOrder) {
  Fixture h(true);
  const auto localNone = h.arena.makeCell("local none");
  const auto localOne  = h.arena.makeCell("local one");
  zigzag::expectWritten(h.arena.link(localOne, h.arena.ensureDimension("d.a"),
                                     DimVector::POS, h.arena.makeCell("leaf")));
  const std::vector<CellRef> stream{h.oneProxy, localNone, h.noneProxy,
                                    localOne};
  const auto empty =
      h.executor.execute("for $c in . where ?{} return $c", stream);
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_EQ(*empty, (std::vector<CellRef>{localNone, h.noneProxy}));
  const auto linked =
      h.executor.execute("for $c in . where ?{*} return $c", stream);
  ASSERT_TRUE(linked) << linked.error().message;
  EXPECT_EQ(*linked, (std::vector<CellRef>{h.oneProxy, localOne}));
}

TEST(VQLShapeWildcardForeignTest, PredicateLeavesTheForeignStoreAlone) {
  Fixture h(true);
  const auto ops = h.foreign.store->opCount();
  EXPECT_TRUE(h.holds(h.noneProxy, "?{}"));
  EXPECT_EQ(h.foreign.store->opCount(), ops);
  EXPECT_FALSE(h.executor.programRetainedBecause());
}

} // namespace
} // namespace xanadu::vql
