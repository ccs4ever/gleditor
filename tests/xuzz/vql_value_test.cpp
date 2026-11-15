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

TEST(VQLValueTest, InvalidLengthsAndOutOfRangeOffsetsRefuseBeforeMutation) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto cell = arena.makeCell("untouched");
  engine.setVariable("item", cell);
  for (const auto *query :
       {"$item/value(\"bad\", 0, \"invalid\")",
        "$item/value(\"bad\", \"1trailing\")", "$item/value(\"bad\", 1e100)"}) {
    EXPECT_THROW(engine.execute(query), std::runtime_error) << query;
    EXPECT_EQ(arena.textOf(cell), "untouched") << query;
  }
  const auto result = engine.execute("$item/value(\"X\", 1, 2)");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result.front(), cell);
  EXPECT_EQ(arena.textOf(cell), "uXouched");
}

TEST(VQLValueTest, InvalidPreparedOffsetRollsBackArgumentEffects) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto cell   = arena.makeCell("untouched");
  const auto offset = arena.makeCell("original");
  engine.setVariable("item", cell);
  engine.setVariable("offset", offset);
  EXPECT_THROW(
      engine.execute("$item/value(\"bad\", $offset/value(\"invalid\"))"),
      std::runtime_error);
  EXPECT_EQ(arena.textOf(cell), "untouched");
  EXPECT_EQ(arena.textOf(offset), "original");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VQLValueTest, MaximalLengthClampsWithoutSignedOverflow) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto cell = arena.makeCell("untouched");
  engine.setVariable("item", cell);
  const auto read = engine.execute("$item.[1, 9223372036854775807]");
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(arena.textOf(read.front()), "ntouched");
  EXPECT_EQ(arena.textOf(cell), "untouched");
  engine.execute("$item/value(\"X\", 1, 9223372036854775807)");
  EXPECT_EQ(arena.textOf(cell), "uX");
  engine.execute("$item/value(\"untouched\")");
  engine.execute("$item/value(\"Y\", 1)");
  EXPECT_EQ(arena.textOf(cell), "uY");
}

TEST(VQLValueTest, ExcessArityRefusesBeforePreparingAnyArguments) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  const auto cell     = arena.makeCell("untouched");
  const auto argument = arena.makeCell("original");
  engine.setVariable("item", cell);
  engine.setVariable("argument", argument);
  const auto before = arena.cellCount();
  EXPECT_THROW(engine.execute("$item/value($argument/value(\"changed\"), "
                              "0, 1, 2)"),
               std::runtime_error);
  EXPECT_EQ(arena.textOf(cell), "untouched");
  EXPECT_EQ(arena.textOf(argument), "original");
  EXPECT_EQ(arena.cellCount(), before);
}
} // namespace
