/**
 * @file vql_watch_test.cpp
 * @brief Unit tests for VQL Watch statements, delta metadata, and termination.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::DimVector;

TEST(VQLWatchTest, RefusesSimulatedDeltaMetadata) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef item1 = arena.makeCell("first");
  zigzag::CellRef item2 = arena.makeCell("second");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));

  // Snapshot watch execution does not invent a delta cell for existing data.
  auto deltas = engine.execute("watch ##/d.items as $item {\n"
                               "  $item/+d.delta\n"
                               "}");

  ASSERT_EQ(deltas.size(), 2u);
  EXPECT_EQ(deltas[0], item1);
  EXPECT_EQ(deltas[1], item2);
}

TEST(VQLWatchTest, RefusesSimulatedDeltaMetadataLeavesSliceUntouched) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef item1 = arena.makeCell("first");
  zigzag::CellRef item2 = arena.makeCell("second");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));

  EXPECT_THROW(engine.execute("watch ##/d.items as $item {\n"
                              "  $item/+d.delta\n"
                              "}"),
               std::runtime_error);
  const auto dDelta = engine.resolveDimension("d.delta");
  EXPECT_FALSE(arena.linked(item1, dDelta, DimVector::POS));
  EXPECT_FALSE(arena.linked(item2, dDelta, DimVector::POS));
  EXPECT_EQ(engine.execute("##/d.items"),
            (std::vector<zigzag::CellRef>{item1, item2}));
}

TEST(VQLWatchTest, PollReportsMembershipChangesAndCanBeCancelled) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  const auto dim = engine.resolveDimension("d.items");
  const auto a   = arena.makeCell("a");
  const auto b   = arena.makeCell("b");
  ASSERT_TRUE(arena.link(core.home(), dim, DimVector::POS, a));
  ASSERT_TRUE(arena.link(a, dim, DimVector::POS, b));

  xanadu::vql::Parser parser("##/d.items");
  const auto query = parser.parseQuery();
  const auto id = engine.subscribeWatch(std::get<PathExpression>(query.expr));
  const auto initial = engine.pollWatch(id);
  ASSERT_EQ(initial.size(), 2u);
  EXPECT_EQ(initial[0].cell, a);
  EXPECT_EQ(initial[0].change, VQLEngine::WatchChange::Added);
  EXPECT_TRUE(engine.pollWatch(id).empty());

  ASSERT_TRUE(arena.setValueBits(a, xanadu::ValueKind::Int64, 41));
  const auto valueChanged = engine.pollWatch(id, [&](const auto &event) {
    if (event.cell == a)
      ASSERT_TRUE(arena.setValueBits(a, xanadu::ValueKind::Int64, 42));
  });
  ASSERT_EQ(valueChanged.size(), 1u);
  EXPECT_EQ(valueChanged.front().cell, a);
  EXPECT_EQ(valueChanged.front().change, VQLEngine::WatchChange::Modified);
  ASSERT_TRUE(valueChanged.front().afterValue.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*valueChanged.front().afterValue), 41);
  EXPECT_TRUE(engine.pollWatch(id).empty());

  const auto c = arena.makeCell("c");
  ASSERT_TRUE(arena.link(b, dim, DimVector::POS, c));
  const auto changed = engine.pollWatch(id);
  ASSERT_EQ(changed.size(), 2u);
  EXPECT_EQ(changed[0].cell, b);
  EXPECT_EQ(changed[0].change, VQLEngine::WatchChange::Modified);
  EXPECT_TRUE(changed[0].topologyChanged);
  EXPECT_EQ(changed[1].cell, c);
  EXPECT_EQ(changed[1].change, VQLEngine::WatchChange::Added);

  const auto dExtra = engine.resolveDimension("d.extra");
  const auto extra  = arena.makeCell("extra");
  ASSERT_TRUE(arena.link(a, dExtra, DimVector::POS, extra));
  const auto topologyChanged = engine.pollWatch(id);
  ASSERT_EQ(topologyChanged.size(), 1u);
  EXPECT_EQ(topologyChanged.front().cell, a);
  EXPECT_TRUE(topologyChanged.front().topologyChanged);
  EXPECT_TRUE(engine.cancelWatch(id));
  EXPECT_FALSE(engine.cancelWatch(id));
}

TEST(VQLWatchTest, LocalMountDispatchAddsNoArenaCells) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  MountDeclaration mount{.storeAnchor = StoreAnchor{.method = "current"},
                         .dimName     = "d.cluster",
                         .direction   = DimVector::POS};
  const auto initialCellCount = arena.cellCount();
  engine.executeMount(mount);
  EXPECT_EQ(arena.cellCount(), initialCellCount);

  const auto result = engine.execute("##/+d.cluster");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result.front(), core.home());

  EXPECT_TRUE(engine.execute("unmount +d.cluster; ##/+d.cluster").empty());
  const auto remounted =
      engine.execute("mount current() as +d.cluster; ##/+d.cluster");
  ASSERT_EQ(remounted.size(), 1u);
  EXPECT_EQ(remounted.front(), core.home());
}

TEST(VQLWatchTest, RemoteMountDoesNotSilentlyResolveToLocalHome) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  EXPECT_THROW(engine.execute("mount topic(\"cluster\") as +d.remote; ##"),
               std::runtime_error);
}

TEST(VQLWatchTest, MountAliasIsPrivateToItsEngineSession) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine mounted(core);
  VQLEngine isolated(core);
  const MountDeclaration mount{.storeAnchor = StoreAnchor{.method = "current"},
                               .dimName     = "d.private",
                               .direction   = DimVector::POS};

  mounted.executeMount(mount);
  EXPECT_TRUE(isolated.execute("##/+d.private").empty());
  const auto mountedResult = mounted.execute("##/+d.private");
  ASSERT_EQ(mountedResult.size(), 1u);
  EXPECT_EQ(mountedResult.front(), core.home());
}

TEST(VQLWatchTest, CallbackMayCancelAndRegisterSubscriptionsSafely) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  const auto dim  = engine.resolveDimension("d.items");
  const auto item = arena.makeCell("item");
  ASSERT_TRUE(arena.link(core.home(), dim, DimVector::POS, item));

  xanadu::vql::Parser parser("##/d.items");
  const auto query = parser.parseQuery();
  const auto path  = std::get<PathExpression>(query.expr);
  const auto id    = engine.subscribeWatch(path);
  std::vector<VQLEngine::WatchId> added;
  const auto events = engine.pollWatch(id, [&](const auto &) {
    EXPECT_TRUE(engine.cancelWatch(id));
    while (added.size() < 128) added.push_back(engine.subscribeWatch(path));
  });

  ASSERT_EQ(events.size(), 1u);
  EXPECT_FALSE(engine.cancelWatch(id));
  ASSERT_EQ(added.size(), 128u);
  for (const auto addedId : added) EXPECT_TRUE(engine.cancelWatch(addedId));
}

TEST(VQLWatchTest, SinceRequiresStoreBackedHistory) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dTasks = engine.resolveDimension("d.tasks");
  zigzag::CellRef t1    = arena.makeCell("init");
  zigzag::CellRef t2    = arena.makeCell("processing");
  zigzag::CellRef t3    = arena.makeCell("done");
  zigzag::CellRef t4    = arena.makeCell("post_termination");

  EXPECT_TRUE(arena.link(core.home(), dTasks, DimVector::POS, t1));
  EXPECT_TRUE(arena.link(t1, dTasks, DimVector::POS, t2));
  EXPECT_TRUE(arena.link(t2, dTasks, DimVector::POS, t3));
  EXPECT_TRUE(arena.link(t3, dTasks, DimVector::POS, t4));

  EXPECT_THROW(
      engine.execute("watch ##/d.tasks as $t since @88 until $t = \"done\" {\n"
                     "  $t\n"
                     "}"),
      std::runtime_error);
}

TEST(VQLWatchTest, RefusesSimulatedCatchUp) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dTasks = engine.resolveDimension("d.tasks");
  zigzag::CellRef t1    = arena.makeCell("init");
  zigzag::CellRef t2    = arena.makeCell("processing");
  zigzag::CellRef t3    = arena.makeCell("done");
  zigzag::CellRef t4    = arena.makeCell("post_termination");

  EXPECT_TRUE(arena.link(core.home(), dTasks, DimVector::POS, t1));
  EXPECT_TRUE(arena.link(t1, dTasks, DimVector::POS, t2));
  EXPECT_TRUE(arena.link(t2, dTasks, DimVector::POS, t3));
  EXPECT_TRUE(arena.link(t3, dTasks, DimVector::POS, t4));

  EXPECT_THROW(
      engine.execute("watch ##/d.tasks as $t since @88 until $t = \"done\" {\n"
                     "  $t\n"
                     "}"),
      std::runtime_error);
  EXPECT_EQ(engine.execute("##/d.tasks"),
            (std::vector<zigzag::CellRef>{t1, t2, t3, t4}));
}

TEST(VQLWatchTest, ScriptReportsUnsupportedWatch) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef i1    = arena.makeCell("itemA");
  zigzag::CellRef i2    = arena.makeCell("itemB");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, i1));
  EXPECT_TRUE(arena.link(i1, dItems, DimVector::POS, i2));

  std::string script = "for $x in ##/d.items return $x;\n"
                       "watch ##/d.items as $it until $it = \"itemB\" {\n"
                       "  $it\n"
                       "};";

  EXPECT_THROW(engine.execute(script), std::runtime_error);
  EXPECT_EQ(engine.execute("##/d.items"),
            (std::vector<zigzag::CellRef>{i1, i2}));
}

TEST(VQLWatchTest, StreamingAndDeltaMetadata) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef item1 = arena.makeCell("first");
  zigzag::CellRef item2 = arena.makeCell("second");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));

  // Snapshot watch execution does not invent a delta cell for existing data.
  auto deltas = engine.execute("watch ##/d.items as $item {\n"
                               "  $item/+d.delta\n"
                               "}");

  ASSERT_EQ(deltas.size(), 2u);
  EXPECT_EQ(deltas[0], item1);
  EXPECT_EQ(deltas[1], item2);
}

TEST(VQLWatchTest, MultiStatementScriptWithForAndWatch) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef i1    = arena.makeCell("itemA");
  zigzag::CellRef i2    = arena.makeCell("itemB");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, i1));
  EXPECT_TRUE(arena.link(i1, dItems, DimVector::POS, i2));

  std::string script = "for $x in ##/d.items return $x;\n"
                       "watch ##/d.items as $it until $it = \"itemB\" {\n"
                       "  $it\n"
                       "};";

  auto results = engine.execute(script);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(arena.textOf(results[0]), "itemA");
  EXPECT_EQ(arena.textOf(results[1]), "itemB");
}

} // namespace
