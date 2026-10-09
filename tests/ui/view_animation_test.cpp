// The animation layer between a placement's layouts
// (view-system-implementation-plan.md §5.3, package U6a). Time comes only
// from advance(), so every case runs on a clock the test moves by hand.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>
#include <vector>

#include <choreograph/Easing.h>

#include "common/ui/view/view_animation.hpp"

namespace {

// Allocation is counted only between start and stop on this thread, so the
// rest of the binary is unaffected by the replacement below.
thread_local bool countingAllocations    = false;
thread_local std::size_t allocationCount = 0;

} // namespace

void *operator new(const std::size_t size) {
  if (countingAllocations) {
    ++allocationCount;
  }
  if (void *const block = std::malloc(size == 0 ? 1 : size)) {
    return block;
  }
  throw std::bad_alloc{};
}
void operator delete(void *const block) noexcept { std::free(block); }
void operator delete(void *const block, std::size_t /*size*/) noexcept {
  std::free(block);
}

namespace {

using xanadu::MotionConfig;
using xanadu::view::AnimationTarget;
using xanadu::view::itemGhost;
using xanadu::view::LayoutSink;
using xanadu::view::MotionCause;
using xanadu::view::MotionHint;
using xanadu::view::PlacedEdge;
using xanadu::view::PlacedFrame;
using xanadu::view::PlacedItem;
using xanadu::view::SubjectId;
using xanadu::view::ViewAnimation;
using xanadu::view::ViewEpoch;
using Ms = ViewAnimation::Millis;

const MotionConfig kMotion{};

PlacedItem at(const SubjectId id, const float x, const float y = 0.0F) {
  return {.id = id, .centre = {x, y, 0.0F}, .width = 100.0F, .height = 40.0F};
}

/// What was drawn for @p id: nothing when it was not drawn.
std::optional<PlacedItem> drawn(const LayoutSink &out, const SubjectId id) {
  for (const auto &item : out.items()) {
    if (item.id == id) {
      return item;
    }
  }
  return std::nullopt;
}

std::size_t timesDrawn(const LayoutSink &out, const SubjectId id) {
  std::size_t n = 0;
  for (const auto &item : out.items()) {
    n += item.id == id ? 1U : 0U;
  }
  return n;
}

/// One placement over a hand-run clock.
struct Placement {
  ViewAnimation animation;
  LayoutSink layout, out;
  ViewEpoch epoch{1};

  explicit Placement(const MotionConfig &config = kMotion)
      : animation(config) {}

  Placement *show(std::initializer_list<PlacedItem> items,
                  const MotionCause cause = MotionCause::Move) {
    layout.clear();
    for (const auto &item : items) {
      layout.push(item);
    }
    animation.retarget({.layout = layout, .epoch = epoch, .cause = cause});
    return this;
  }
  const LayoutSink &advance(const double ms) {
    animation.advance(Ms{ms}, out);
    return out;
  }
  /// Shown and settled, so what follows starts from rest.
  Placement *rest(std::initializer_list<PlacedItem> items) {
    show(items);
    advance(1000.0);
    return this;
  }
};

const SubjectId kCellA = SubjectId::cell(1);
const SubjectId kCellB = SubjectId::cell(2);

TEST(ViewAnimationTest, ASubjectInBothLayoutsTweensOnTheClockItIsGiven) {
  Placement first, second;
  for (auto *p : {&first, &second}) {
    p->rest({at(kCellA, 0.0F)});
    p->show({at(kCellA, 100.0F)});
  }
  const auto half = drawn(first.advance(60.0), kCellA);
  ASSERT_TRUE(half);
  EXPECT_FLOAT_EQ(half->centre.x, 100.0F * choreograph::easeOutCubic(0.5F));
  EXPECT_EQ(drawn(second.advance(60.0), kCellA)->centre.x, half->centre.x);
  EXPECT_TRUE(first.animation.animating());

  EXPECT_FLOAT_EQ(drawn(first.advance(60.0), kCellA)->centre.x, 100.0F);
  EXPECT_FALSE(first.animation.animating());
}

TEST(ViewAnimationTest, ALayoutThatMovedNothingStartsNoMotion) {
  Placement p;
  p.rest({at(kCellA, 0.0F), at(kCellB, 200.0F)});
  p.show({at(kCellA, 0.0F), at(kCellB, 200.0F)});
  EXPECT_FALSE(p.animation.animating());
}

TEST(ViewAnimationTest, ANewSubjectFadesInAndAGoneOneFadesOutFromItsCopy) {
  Placement p;
  p.rest({at(kCellA, 0.0F)});
  p.show({at(kCellA, 0.0F), at(kCellB, 300.0F, 20.0F)});
  const auto in = drawn(p.advance(60.0), kCellB);
  ASSERT_TRUE(in);
  EXPECT_FLOAT_EQ(in->opacity, choreograph::easeOutCubic(0.5F));
  EXPECT_FLOAT_EQ(in->centre.x, 300.0F); // fades where it is, no travel
  EXPECT_FLOAT_EQ(drawn(p.advance(60.0), kCellB)->opacity, 1.0F);

  p.show({at(kCellA, 0.0F)});
  const auto out = drawn(p.advance(60.0), kCellB);
  ASSERT_TRUE(out);
  EXPECT_FLOAT_EQ(out->opacity, 1.0F - choreograph::easeOutCubic(0.5F));
  EXPECT_FLOAT_EQ(out->centre.x, 300.0F);
  EXPECT_FLOAT_EQ(out->centre.y, 20.0F);
  EXPECT_FALSE(drawn(p.advance(60.0), kCellB));
  EXPECT_FALSE(p.animation.animating());
}

// A toss renumbers the derived arena, so the old view cell's id matches
// nothing: it fades from the copy last drawn, mid-flight as it was, without
// the animation layer holding or asking any view space; the real cell keeps
// its id and travels.
TEST(ViewAnimationTest, ATossedCellFadesInPlaceFromWhereItWasLastDrawn) {
  Placement p;
  const auto oldCell = SubjectId::viewCell(7, 1);
  const auto newCell = SubjectId::viewCell(7, 2);
  p.rest({at(kCellA, 0.0F), at(oldCell, 50.0F)});
  p.show({at(kCellA, 0.0F), at(oldCell, 150.0F)});
  const float midway = drawn(p.advance(60.0), oldCell)->centre.x;
  ASSERT_GT(midway, 50.0F);
  ASSERT_LT(midway, 150.0F);

  p.epoch = 2;
  p.show({at(kCellA, 10.0F), at(newCell, 200.0F)}, MotionCause::Rebind);
  const auto &tossOut = kMotion.tossOut;
  const auto &tossIn  = kMotion.tossIn;
  const auto leaving  = drawn(p.advance(tossOut.durationMs / 2.0), oldCell);
  ASSERT_TRUE(leaving);
  EXPECT_FLOAT_EQ(leaving->centre.x, midway);
  EXPECT_NEAR(leaving->opacity, 1.0F - choreograph::easeInCubic(0.5F), 1e-5F);

  const auto arriving = drawn(p.out, newCell);
  ASSERT_TRUE(arriving);
  const float grown =
      choreograph::easeOutCubic(tossOut.durationMs / 2.0F / tossIn.durationMs);
  EXPECT_FLOAT_EQ(arriving->opacity, grown);
  EXPECT_NEAR(arriving->width,
              100.0F *
                  (kMotion.tossInScale + (1.0F - kMotion.tossInScale) * grown),
              1e-3F);
  EXPECT_GT(drawn(p.out, kCellA)->centre.x, 0.0F);

  EXPECT_FALSE(drawn(p.advance(tossOut.durationMs / 2.0), oldCell));
  p.advance(tossIn.durationMs);
  EXPECT_FLOAT_EQ(drawn(p.out, newCell)->width, 100.0F);
  EXPECT_FLOAT_EQ(drawn(p.out, newCell)->opacity, 1.0F);
  EXPECT_FLOAT_EQ(drawn(p.out, kCellA)->centre.x, 10.0F);
}

// Every repeat is taken the moment it arrives and starts from where the
// cell is drawn, so the last step ends one step's time after it arrives
// rather than after every earlier step has played out.
TEST(ViewAnimationTest, AHeldKeyRetargetsAndNeverQueues) {
  Placement p;
  p.rest({at(kCellA, 0.0F)});
  constexpr double kRepeatMs = 30.0;
  constexpr int kRepeats     = 5;
  float before               = 0.0F;
  for (int step = 1; step <= kRepeats; ++step) {
    p.show({at(kCellA, 100.0F * static_cast<float>(step))});
    const float now = drawn(p.advance(0.0), kCellA)->centre.x;
    EXPECT_FLOAT_EQ(now, before) << "step " << step << " jumped";
    before = drawn(p.advance(kRepeatMs), kCellA)->centre.x;
    EXPECT_GT(before, now) << "step " << step << " waited";
  }
  p.advance(kMotion.step.durationMs - kRepeatMs);
  EXPECT_FLOAT_EQ(drawn(p.out, kCellA)->centre.x, 100.0F * kRepeats);
  EXPECT_FALSE(p.animation.animating());
}

TEST(ViewAnimationTest, AHeldKeyNeverShowsAViewCellGoneByTheNextRepeat) {
  Placement p;
  p.rest({at(kCellA, 0.0F)});
  constexpr double kRepeatMs = 30.0;
  constexpr int kRepeats     = 6;
  p.show({at(kCellA, 100.0F)}); // the press itself; then the repeats
  p.advance(kRepeatMs);
  for (int step = 2; step <= kRepeats; ++step) {
    const auto passing =
        SubjectId::viewCell(static_cast<std::uint64_t>(step), 1);
    p.show({at(kCellA, 100.0F * static_cast<float>(step)),
            at(passing, 100.0F * static_cast<float>(step), 50.0F)});
    for (int frame = 0; frame < 3; ++frame) {
      const auto seen = drawn(p.advance(kRepeatMs / 3.0), passing);
      EXPECT_FLOAT_EQ(seen ? seen->opacity : 0.0F, 0.0F)
          << "repeat " << step << " showed a passing view cell";
    }
  }
  // One that outlives a repeat interval fades in after it.
  const auto stays = SubjectId::viewCell(99, 1);
  p.show({at(kCellA, 1000.0F), at(stays, 1000.0F, 50.0F)});
  EXPECT_FLOAT_EQ(drawn(p.advance(kRepeatMs), stays)->opacity, 0.0F);
  p.advance(kMotion.step.durationMs);
  EXPECT_FLOAT_EQ(drawn(p.out, stays)->opacity, 1.0F);
}

TEST(ViewAnimationTest, ReducedMotionCutsEveryChange) {
  MotionConfig reduced;
  reduced.reduced = true;
  Placement p{reduced};
  p.rest({at(kCellA, 0.0F), at(kCellB, 50.0F)});
  p.show({at(kCellA, 100.0F)});
  EXPECT_FALSE(p.animation.animating());
  const auto &out = p.advance(0.0);
  EXPECT_FLOAT_EQ(drawn(out, kCellA)->centre.x, 100.0F);
  EXPECT_FLOAT_EQ(drawn(out, kCellA)->opacity, 1.0F);
  EXPECT_FALSE(drawn(out, kCellB));

  // Turned on mid-motion, it cuts what is moving.
  Placement q;
  q.rest({at(kCellA, 0.0F), at(kCellB, 50.0F)});
  q.show({at(kCellA, 100.0F)});
  q.advance(30.0);
  q.animation.configure(reduced);
  EXPECT_FALSE(q.animation.animating());
  EXPECT_FLOAT_EQ(drawn(q.advance(0.0), kCellA)->centre.x, 100.0F);
  EXPECT_FALSE(drawn(q.out, kCellB));
}

// The base view's transition(): the page brought over starts at once and
// takes longest, the row waits and takes less (§10.3.3).
TEST(ViewAnimationTest, MotionHintsTimeEachSubject) {
  Placement p;
  const auto subject = SubjectId::page(1, 0);
  const auto row     = SubjectId::page(2, 0);
  p.rest({at(subject, 0.0F), at(row, 0.0F, 500.0F), at(kCellA, 0.0F, -500.0F)});
  p.layout.clear();
  p.layout.push(at(subject, 600.0F));
  p.layout.push(at(row, 400.0F, 500.0F));
  p.layout.push(at(kCellA, 120.0F, -500.0F));
  p.layout.push(
      MotionHint{.id = subject, .delayMs = 0.0F, .durationMs = 620.0F});
  p.layout.push(MotionHint{.id = row, .delayMs = 90.0F, .durationMs = 450.0F});
  p.animation.retarget({.layout = p.layout, .epoch = p.epoch});

  const auto &out = p.advance(90.0);
  EXPECT_FLOAT_EQ(drawn(out, row)->centre.x, 0.0F);
  EXPECT_NEAR(drawn(out, subject)->centre.x,
              600.0F * choreograph::easeInOutQuad(90.0F / 620.0F), 1e-3F);
  p.advance(225.0);
  EXPECT_NEAR(drawn(p.out, row)->centre.x,
              400.0F * choreograph::easeInOutQuad(0.5F), 1e-3F);
  // Unhinted: the default step, long since done.
  EXPECT_FLOAT_EQ(drawn(p.out, kCellA)->centre.x, 120.0F);
  p.advance(305.0);
  EXPECT_FLOAT_EQ(drawn(p.out, subject)->centre.x, 600.0F);
  EXPECT_FALSE(p.animation.animating());
}

TEST(ViewAnimationTest, AGhostOnlyFades) {
  Placement p;
  const auto ghost = SubjectId::marker(4);
  auto shown       = at(ghost, 0.0F);
  shown.flags      = itemGhost;
  shown.opacity    = 0.2F;
  p.rest({shown});
  shown.centre.x = 300.0F;
  shown.opacity  = 0.4F;
  p.show({shown});
  const auto half = drawn(p.advance(60.0), ghost);
  EXPECT_FLOAT_EQ(half->centre.x, 300.0F);
  EXPECT_GT(half->opacity, 0.2F);
  EXPECT_LT(half->opacity, 0.4F);
}

TEST(ViewAnimationTest, AViewSwitchCrossFadesWithoutTravel) {
  Placement p;
  p.rest({at(kCellA, 0.0F)});
  p.show({at(kCellA, 400.0F)}, MotionCause::ViewSwitch);
  const auto &out = p.advance(kMotion.viewSwitch.durationMs / 2.0);
  ASSERT_EQ(timesDrawn(out, kCellA), 2U);
  for (const auto &item : out.items()) {
    EXPECT_TRUE(item.centre.x == 0.0F || item.centre.x == 400.0F);
    EXPECT_NEAR(item.opacity, 0.5F, 1e-5F); // in-out is symmetric
  }
  p.advance(kMotion.viewSwitch.durationMs);
  ASSERT_EQ(timesDrawn(p.out, kCellA), 1U);
  EXPECT_FLOAT_EQ(drawn(p.out, kCellA)->centre.x, 400.0F);
}

// What comes out is flat: frames composed, labels renumbered to what was
// written, so the presenter reads poses with no frame in sight.
TEST(ViewAnimationTest, RecordsComeOutComposedWithLabelsRenumbered) {
  ViewAnimation animation;
  LayoutSink layout, out;
  const auto pack = SubjectId::viewCell(10, 1);
  const auto frame =
      layout.push(PlacedFrame{.id = pack, .centre = {100.0F, 0.0F, 0.0F}});
  auto part  = at(kCellA, 5.0F);
  part.frame = frame;
  layout.push(at(kCellB, -50.0F));
  layout.push(part);
  const auto label = layout.push(at(SubjectId::label(3), 0.0F, 30.0F));
  layout.push(PlacedEdge{.from  = kCellA,
                         .to    = kCellB,
                         .a     = {105.0F, 0.0F, 0.0F},
                         .b     = {-50.0F, 0.0F, 0.0F},
                         .label = label});
  animation.retarget({.layout = layout, .epoch = 1});
  animation.advance(Ms{1000.0}, out);

  const auto composed = drawn(out, kCellA);
  ASSERT_TRUE(composed);
  EXPECT_FLOAT_EQ(composed->centre.x, 105.0F);
  EXPECT_FALSE(composed->frame);
  ASSERT_EQ(out.frames().size(), 1U);
  EXPECT_FALSE(out.frames().front().parent);
  ASSERT_EQ(out.edges().size(), 1U);
  ASSERT_TRUE(out.edges().front().label);
  EXPECT_EQ(out.items()[*out.edges().front().label].id, SubjectId::label(3));
}

TEST(ViewAnimationTest, ASteadyPlacementAllocatesNothingToRetargetOrAdvance) {
  ViewAnimation animation;
  LayoutSink layout, out;
  constexpr int kCells = 200;
  const auto lay       = [&](const float shift) {
    layout.clear();
    for (int i = 0; i < kCells; ++i) {
      const auto id = SubjectId::cell(static_cast<std::uint64_t>(i));
      layout.push(at(id, static_cast<float>(i) * 10.0F + shift));
      layout.push(
          PlacedEdge{.from = id,
                           .to = SubjectId::cell(static_cast<std::uint64_t>(i + 1))});
    }
    layout.push(PlacedFrame{.id = SubjectId::viewCell(1, 1)});
  };
  // Reach size: every buffer has held a full layout in both of its roles.
  for (int warm = 0; warm < 3; ++warm) {
    lay(static_cast<float>(warm));
    animation.retarget({.layout = layout, .epoch = 1});
    animation.advance(Ms{16.0}, out);
  }
  allocationCount     = 0;
  countingAllocations = true;
  for (int frame = 0; frame < 20; ++frame) {
    if (frame % 3 == 0) {
      lay(static_cast<float>(frame) * 3.0F);
    }
    animation.retarget({.layout = layout, .epoch = 1});
    animation.advance(Ms{16.0}, out);
  }
  countingAllocations = false;
  EXPECT_EQ(allocationCount, 0U);
  EXPECT_EQ(out.items().size(), static_cast<std::size_t>(kCells));
}

} // namespace
