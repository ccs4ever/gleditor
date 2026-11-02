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

TEST(VQLWatchTest, RefusesSimulatedDeltaMetadata) {
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

} // namespace
