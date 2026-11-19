/**
 * @file vql_stale_handle_epoch_test.cpp
 * @brief The session's handle table is keyed by a cell number. A handle whose
 * cell was released must not hand its entry to a different cell that reuses
 * the number, and a live handle keeps its entry across releases above it.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::vql::ComputedViewRules;

// The table's one remaining kind keyed by a cell that outlives its program is a
// view's computed rules (keyed by the view cell); the epoch bookkeeping is the
// same for any entry, so the tests use that kind. The second argument is kept
// so each case still reads as "handle over source".
xanadu::vql::VQLEngine::ShapeTable tableOf(const zigzag::CellRef handle,
                                           const zigzag::CellRef /*source*/) {
  xanadu::vql::VQLEngine::ShapeTable table;
  table.emplace(handle, ComputedViewRules{});
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

TEST(VqlStaleHandleEpochTest, NestedMarksKeepTheOuterHandleAndDropTheInnerOne) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto outer  = arena.mark();
  const auto h1     = arena.makeCell("h1");
  const auto inner  = arena.mark();
  const auto h2     = arena.makeCell("h2");
  auto table        = tableOf(h1, source);
  table.merge(tableOf(h2, source));
  engine.publishShapes(std::move(table), {});
  ASSERT_EQ(engine.shapeCount(), 2u);
  ASSERT_TRUE(arena.release(inner));
  ASSERT_EQ(arena.makeCell("someone else"), h2);

  engine.publishShapes(engine.compilerShapes(), {});
  EXPECT_TRUE(engine.compilerShapes().contains(h1));
  EXPECT_FALSE(engine.compilerShapes().contains(h2));
  ASSERT_TRUE(arena.release(outer));
}

TEST(VqlStaleHandleEpochTest, ClearVariablesForgetsTheRecordedEpochs) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto m      = arena.mark();
  const auto h1     = arena.makeCell("h1");
  engine.publishShapes(tableOf(h1, source), {});
  ASSERT_TRUE(arena.release(m));
  engine.clearVariables();
  const auto h2 = arena.makeCell("h2");
  ASSERT_EQ(h2, h1);

  engine.publishShapes(tableOf(h2, source), {});
  EXPECT_TRUE(engine.compilerShapes().contains(h2));
}

TEST(VqlStaleHandleEpochTest, AnEntryWhoseKeyIsNotACellIsRefusedNotKept) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto source = arena.makeCell("source");
  const auto absent = zigzag::ArenaManifold::refOf(1u << 20);
  ASSERT_FALSE(arena.contains(absent));
  engine.publishShapes(tableOf(absent, source), {});
  EXPECT_EQ(engine.shapeCount(), 0u);
  EXPECT_FALSE(xanadu::vql::VQLEngine::notACellReason.empty());
}

} // namespace
