#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {
TEST(VQLValueTest, SliceReadsAndReplacementWritesHaveDistinctEffects) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto cell = arena.makeCell("abcdef");
  engine.setVariable("item", cell);
  const auto slice = engine.execute("$item.[1, 2]");
  ASSERT_EQ(slice.size(), 1u);
  EXPECT_EQ(arena.textOf(slice.front()), "bc");
  EXPECT_EQ(arena.textOf(cell), "abcdef");
  const auto written = engine.execute("$item/value(\"XY\", 1, 2)");
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(written.front(), cell);
  EXPECT_EQ(arena.textOf(cell), "aXYdef");
  engine.execute("$item/value(\"complete\")");
  EXPECT_EQ(arena.textOf(cell), "complete");
}

TEST(VQLValueTest, InvalidOffsetsRefuseBeforeMutation) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto cell = arena.makeCell("untouched");
  engine.setVariable("item", cell);
  EXPECT_THROW(engine.execute("$item/value(\"bad\", \"offset\")"),
               std::runtime_error);
  EXPECT_EQ(arena.textOf(cell), "untouched");
}
} // namespace
