/**
 * @file vql_watch_test.cpp
 * @brief Unit tests for VQL Watch statements, delta metadata, and termination.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::DimVector;

TEST(VQLWatchTest, StreamingAndDeltaMetadata) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef item1 = arena.makeCell("first");
  zigzag::CellRef item2 = arena.makeCell("second");

  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));

  // watch streaming over target path with virtual d.delta
  auto deltas = engine.execute("watch ##/d.items as $item {\n"
                               "  $item/+d.delta\n"
                               "}");

  ASSERT_EQ(deltas.size(), 2u);
  EXPECT_EQ(arena.textOf(deltas[0]), "modified");
  EXPECT_EQ(arena.textOf(deltas[1]), "modified");
}

TEST(VQLWatchTest, SinceAndUntilTermination) {
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

  // watch with since @88 catch-up simulation and until $t = "done" termination
  auto results =
      engine.execute("watch ##/d.tasks as $t since @88 until $t = \"done\" {\n"
                     "  $t\n"
                     "}");

  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(arena.textOf(results[0]), "init");
  EXPECT_EQ(arena.textOf(results[1]), "processing");
  EXPECT_EQ(arena.textOf(results[2]), "done");
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
