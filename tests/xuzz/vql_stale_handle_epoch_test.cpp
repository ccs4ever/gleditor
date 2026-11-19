/**
 * @file vql_stale_handle_epoch_test.cpp
 * @brief The session's handle table is keyed by a cell number. A handle whose
 * cell was released must not hand its entry to a different cell that reuses
 * the number, and a live handle keeps its entry across releases above it.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::vql::ViewHandle;

xanadu::vql::VQLEngine::ShapeTable tableOf(const zigzag::CellRef handle,
                                           const zigzag::CellRef source) {
  xanadu::vql::VQLEngine::ShapeTable table;
  table.emplace(handle, ViewHandle{.source = source, .view = nullptr});
  return table;
}

TEST(VqlStaleHandleEpochTest, ReleasedHandleNumberReusedByANewCellIsDropped) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto m      = arena.mark();
  const auto handle = arena.makeCell("handle");
  engine.publishShapes(tableOf(handle, source), {});
  ASSERT_EQ(engine.shapeCount(), 1u);
  ASSERT_TRUE(arena.release(m));
  const auto other = arena.makeCell("someone else");
  ASSERT_EQ(other, handle);

  // The carry-over a query does: publish what the session already held.
  engine.publishShapes(engine.compilerShapes(), {});
  EXPECT_FALSE(engine.compilerShapes().contains(other));
  EXPECT_EQ(engine.shapeCount(), 0u);
}

TEST(VqlStaleHandleEpochTest, PruneNamesTheKeysItDroppedAndTheReason) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto m      = arena.mark();
  const auto handle = arena.makeCell("handle");
  engine.publishShapes(tableOf(handle, source), {});
  ASSERT_TRUE(arena.release(m));
  ASSERT_EQ(arena.makeCell("someone else"), handle);

  EXPECT_EQ(engine.pruneStaleShapes(), std::vector<zigzag::CellRef>{handle});
  EXPECT_EQ(engine.shapeCount(), 0u);
  EXPECT_FALSE(xanadu::vql::VQLEngine::staleHandleReason.empty());
}

TEST(VqlStaleHandleEpochTest, AQueryDoesNotLetAReusedNumberInheritAnEntry) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto m      = arena.mark();
  const auto handle = arena.makeCell("handle");
  engine.publishShapes(tableOf(handle, source), {});
  ASSERT_TRUE(arena.release(m));
  const auto other = arena.makeCell("someone else");
  ASSERT_EQ(other, handle);
  engine.setVariable("item", other);

  engine.execute("$item");
  EXPECT_FALSE(engine.compilerShapes().contains(other));
}

TEST(VqlStaleHandleEpochTest, LiveHandleKeepsItsEntryAcrossReleases) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto handle = arena.makeCell("handle");
  engine.publishShapes(tableOf(handle, source), {});
  for (int round = 0; round < 3; ++round) {
    const auto m = arena.mark();
    arena.makeCell("scratch");
    ASSERT_TRUE(arena.release(m));
    engine.publishShapes(engine.compilerShapes(), {});
    EXPECT_TRUE(engine.pruneStaleShapes().empty());
  }
  engine.setVariable("item", handle);
  engine.execute("$item");
  EXPECT_TRUE(engine.compilerShapes().contains(handle));
}

} // namespace
