/**
 * @file tests/animation.cpp
 * @brief The animation behaviour that a rendered frame cannot show.
 *
 * A screenshot only ever shows the settled result, which is the whole point of
 * how the render loop defines "settled" -- so the shape of a fade, and the
 * bookkeeping around a document that is on its way out, have to be checked
 * here instead. Nothing in this file needs a graphics device: what is under
 * test is the timeline and the arithmetic hanging off it.
 */
#include <gtest/gtest.h>

#include <choreograph/Choreograph.h>
#include <gleditor/animation.hpp>
#include <gleditor/toast.hpp>

#include <chrono>

using Clock = ToastOverlay::Clock;

namespace {

/// Step a timeline in small slices, the way a render loop does, rather than in
/// one jump: an easing evaluated once at its end would pass a test that a
/// motion which never moved in between would also pass.
void run(ch::Timeline &timeline, const double seconds,
         const double slice = 1.0 / 60.0) {
  for (double elapsed = 0.0; elapsed < seconds; elapsed += slice) {
    timeline.step(std::min(slice, seconds - elapsed));
  }
}

} // namespace

TEST(Animation, RampReachesItsTargetAndStops) {
  ch::Timeline timeline;
  ch::Output<float> value{0.0F};
  timeline.apply(&value).then<ch::RampTo>(1.0F, gleditor::anim::docArrival,
                                          ch::EaseOutCubic());

  EXPECT_FALSE(timeline.empty()) << "a motion was applied, so one is in flight";
  run(timeline, gleditor::anim::docArrival / 2.0);
  EXPECT_GT(value(), 0.0F);
  EXPECT_LT(value(), 1.0F) << "halfway through, the ramp is still going";

  run(timeline, gleditor::anim::docArrival);
  EXPECT_FLOAT_EQ(1.0F, value());
  EXPECT_TRUE(timeline.empty())
      << "a finished motion has to leave the timeline, because an empty "
         "timeline is what tells the render loop the frame has settled";
}

TEST(Animation, EaseOutDecelerates) {
  // The arrival eases out, which is what makes it read as landing somewhere.
  // Stated as a property rather than as sampled constants: past the halfway
  // point of the duration, an ease-out has already covered most of the
  // distance, where a linear ramp would be at exactly half.
  ch::Timeline timeline;
  ch::Output<float> eased{0.0F};
  ch::Output<float> linear{0.0F};
  timeline.apply(&eased).then<ch::RampTo>(1.0F, 1.0, ch::EaseOutCubic());
  timeline.apply(&linear).then<ch::RampTo>(1.0F, 1.0, ch::EaseNone());

  run(timeline, 0.5);
  EXPECT_GT(eased(), linear())
      << "an ease-out is ahead of linear in the first half";
  EXPECT_GT(eased(), 0.8F) << "and well past halfway by the midpoint";
}

TEST(Animation, InterruptingAMotionContinuesFromWhereItGot) {
  // Closing a document retargets the survivors mid-flight. Restarting from the
  // old target would make them jump, which is the thing to rule out.
  ch::Timeline timeline;
  ch::Output<float> value{0.0F};
  timeline.apply(&value).then<ch::RampTo>(100.0F, 1.0, ch::EaseNone());
  run(timeline, 0.5);
  const float midway = value();
  ASSERT_GT(midway, 0.0F);
  ASSERT_LT(midway, 100.0F);

  timeline.apply(&value).then<ch::RampTo>(0.0F, 1.0, ch::EaseNone());
  timeline.step(0.0);
  EXPECT_FLOAT_EQ(midway, value())
      << "retargeting starts from the current value, not from a reset one";
}

TEST(ToastFade, RisesFromNothingAndReturnsToIt) {
  const auto posted  = Clock::now();
  const auto expires = posted + ToastOverlay::lifetime;

  EXPECT_FLOAT_EQ(0.0F, ToastOverlay::fadeFactor(posted, expires, posted))
      << "a toast starts invisible rather than popping in";
  EXPECT_FLOAT_EQ(0.0F, ToastOverlay::fadeFactor(posted, expires, expires))
      << "and ends invisible rather than vanishing at full brightness";
}

TEST(ToastFade, IsFullyOpaqueInTheMiddle) {
  const auto posted  = Clock::now();
  const auto expires = posted + ToastOverlay::lifetime;
  const auto middle  = posted + (ToastOverlay::lifetime / 2);
  EXPECT_FLOAT_EQ(1.0F, ToastOverlay::fadeFactor(posted, expires, middle));
}

TEST(ToastFade, RampsMonotonicallyAtBothEnds) {
  using Seconds      = std::chrono::duration<double>;
  const auto posted  = Clock::now();
  const auto expires = posted + ToastOverlay::lifetime;
  const auto at      = [&](const double offset) {
    return ToastOverlay::fadeFactor(
        posted, expires,
        posted + std::chrono::duration_cast<Clock::duration>(Seconds(offset)));
  };

  const double fade = gleditor::anim::toastFade;
  float previous    = 0.0F;
  for (int i = 1; i <= 10; i++) {
    const float now = at(fade * i / 10.0);
    EXPECT_GE(now, previous) << "the fade in never goes backwards, at step "
                             << i;
    previous = now;
  }
  EXPECT_FLOAT_EQ(1.0F, at(fade));

  const double life = Seconds(ToastOverlay::lifetime).count();
  previous          = 1.0F;
  for (int i = 0; i <= 10; i++) {
    const float now = at(life - (fade * (10 - i) / 10.0));
    EXPECT_LE(now, previous)
        << "the fade out never goes backwards, at step " << i;
    previous = now;
  }
}

TEST(ToastFade, StaysInRangeForALifetimeShorterThanTwoFades) {
  // A toast dropped early to make room for a newer one, and one whose whole
  // life is shorter than the two fades it would like, both have to produce an
  // alpha a blend can use.
  const auto posted = Clock::now();
  const auto brief =
      posted + std::chrono::duration_cast<Clock::duration>(
                   std::chrono::duration<double>(gleditor::anim::toastFade / 4));
  for (int i = 0; i <= 8; i++) {
    const auto when = posted + ((brief - posted) * i / 8);
    const float alpha = ToastOverlay::fadeFactor(posted, brief, when);
    EXPECT_GE(alpha, 0.0F);
    EXPECT_LE(alpha, 1.0F);
  }
  EXPECT_FLOAT_EQ(0.0F, ToastOverlay::fadeFactor(posted, brief,
                                                 brief + std::chrono::seconds(1)))
      << "past its expiry a toast contributes nothing";
}
// vi: set sw=2 sts=2 ts=2 et:
