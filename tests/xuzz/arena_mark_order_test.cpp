/**
 * @file arena_mark_order_test.cpp
 * @brief Marks are a stack: releasing or discarding one under a later
 * outstanding mark is refused by name, and changes nothing (coordinator plan
 * section 5, "Out-of-order mark release"). Before the refusal existed,
 * releasing the outer mark first made the later release regrow the cells.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <tuple>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::MarkRefusal;

TEST(ArenaMarkOrderTest, ReleasingOuterBeforeInnerIsRefusedAndChangesNothing) {
  ArenaManifold arena;
  const auto outer    = arena.mark();
  const auto baseline = arena.cellCount();
  std::ignore         = arena.makeCell();
  const auto inner    = arena.mark();
  std::ignore         = arena.makeCell();

  const auto refused = arena.release(outer);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason, MarkRefusal::Reason::LaterMarkOutstanding);
  EXPECT_EQ(refused.error().requested, outer.serial);
  EXPECT_EQ(refused.error().outstanding, inner.serial);
  EXPECT_EQ(arena.outstandingMarks(), 2U);
  EXPECT_EQ(arena.cellCount(), baseline + 2U);

  ASSERT_TRUE(arena.release(inner));
  EXPECT_EQ(arena.cellCount(), baseline + 1U);
  ASSERT_TRUE(arena.release(outer));
  EXPECT_EQ(arena.cellCount(), baseline);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(ArenaMarkOrderTest, TheRefusalNamesBothMarks) {
  ArenaManifold arena;
  const auto outer = arena.mark();
  const auto inner = arena.mark();
  const auto text  = toString(arena.release(outer).error());
  EXPECT_THAT(text, testing::HasSubstr(std::to_string(outer.serial)));
  EXPECT_THAT(text, testing::HasSubstr(std::to_string(inner.serial)));
  EXPECT_THAT(text, testing::HasSubstr("later mark"));
  EXPECT_TRUE(arena.release(inner));
  EXPECT_TRUE(arena.release(outer));
}

TEST(ArenaMarkOrderTest, DiscardingOuterBeforeInnerIsRefused) {
  ArenaManifold arena;
  const auto outer   = arena.mark();
  const auto inner   = arena.mark();
  const auto refused = arena.discard(outer);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason, MarkRefusal::Reason::LaterMarkOutstanding);
  EXPECT_EQ(arena.outstandingMarks(), 2U);
  EXPECT_TRUE(arena.discard(inner));
  EXPECT_TRUE(arena.discard(outer));
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(ArenaMarkOrderTest, ASettledMarkIsNotOutstanding) {
  ArenaManifold arena;
  const auto outer = arena.mark();
  const auto inner = arena.mark();
  ASSERT_TRUE(arena.release(inner));
  const auto again = arena.release(inner);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().reason, MarkRefusal::Reason::NotOutstanding);
  EXPECT_EQ(again.error().outstanding, outer.serial);
  EXPECT_EQ(arena.outstandingMarks(), 1U);
  EXPECT_TRUE(arena.release(outer));
  const auto none = arena.release(outer);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().outstanding, std::nullopt);
}

} // namespace
