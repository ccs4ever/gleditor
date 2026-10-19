#include "common/xanadu/anchor_motion.hpp"
#include <gtest/gtest.h>

namespace {
xanadu::AnchorFocus focus(std::uint32_t occurrence) {
  static const xanadu::AnchorFocus base{.site = {.range = {10, 20}}};
  auto result       = base;
  result.occurrence = occurrence;
  return result;
}
float value(const xanadu::AnchorMotion &motion, std::uint32_t occurrence) {
  for (const auto &track : motion.tracks())
    if (track.focus && track.focus->occurrence == occurrence)
      return track.value;
  return 0;
}
} // namespace
TEST(AnchorMotionTest, FocusExtendsRetractsAndReversesFromCurrentProgress) {
  xanadu::AnchorMotion motion;
  motion.update(focus(0), 0, .25F, false);
  EXPECT_TRUE(motion.moving());
  motion.update(focus(0), .125, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), .5F);
  motion.update(focus(1), .125, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), .5F);
  motion.update(focus(1), .25, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), .25F);
  EXPECT_FLOAT_EQ(value(motion, 1), .5F);
  motion.update(focus(0), .25, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), .25F);
  motion.update(focus(0), .5, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), 1);
  EXPECT_FLOAT_EQ(value(motion, 1), 0);
  EXPECT_FALSE(motion.moving());
  motion.update({}, .5, .25F, false);
  motion.update({}, .75, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), 0);
  EXPECT_FALSE(motion.moving());
}
TEST(AnchorMotionTest, ReducedMotionAndZeroDurationCutAndDoNotReplay) {
  for (bool reduced : {true, false}) {
    xanadu::AnchorMotion motion;
    const float seconds = reduced ? .25F : 0;
    motion.update(focus(0), 0, seconds, reduced);
    EXPECT_FLOAT_EQ(value(motion, 0), 1);
    EXPECT_FALSE(motion.moving());
    motion.update(focus(0), 100, seconds, reduced);
    EXPECT_FLOAT_EQ(value(motion, 0), 1);
    motion.update({}, 101, seconds, reduced);
    EXPECT_FLOAT_EQ(value(motion, 0), 0);
    EXPECT_FALSE(motion.moving());
  }
}
TEST(AnchorMotionTest, IdentityIncludesSideMemberOccurrenceAndSite) {
  auto first = focus(0), second = first;
  second.member = 1;
  EXPECT_NE(first, second);
  second      = first;
  second.side = xanadu::LinkSide::Right;
  EXPECT_NE(first, second);
  second                = first;
  second.site.range.end = 21;
  EXPECT_NE(first, second);
}

TEST(AnchorMotionTest, LiveReducedMotionToggleCannotReplaySettledReach) {
  xanadu::AnchorMotion motion;
  motion.update(focus(0), 0, .25F, false);
  motion.update(focus(0), .05, .25F, true);
  EXPECT_FLOAT_EQ(value(motion, 0), 1);
  motion.update(focus(0), .06, .25F, false);
  EXPECT_FLOAT_EQ(value(motion, 0), 1);
  EXPECT_FALSE(motion.moving());
}
