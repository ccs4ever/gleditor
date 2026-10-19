#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "common/xanadu/shallow_alignment.hpp"

namespace {
using xanadu::preferShallowAlignment;
using xanadu::ShallowAlignmentError;
using xanadu::ShallowAlignmentParams;
using xanadu::SlopeFragment;

ShallowAlignmentParams params() {
  return {.minimumDelta       = -120,
          .maximumDelta       = 120,
          .minimumRun         = 12,
          .slopeSaturation    = 2,
          .movementScale      = 120,
          .movementWeight     = .03,
          .worstSlopeWeight   = .3,
          .minimumImprovement = .0001,
          .gatheringShare     = .2,
          .candidates         = 49,
          .maximumFragments   = 4096};
}

std::vector<SlopeFragment> neighbors() {
  return {{.link = 1, .x = 0, .y = 0},
          {.link = 1, .right = true, .translated = true, .x = 100, .y = 160},
          {.link = 2, .x = 0, .y = 120},
          {.link = 2, .right = true, .translated = true, .x = 100, .y = 160}};
}

TEST(ShallowAlignmentTest, SharedCompanionBalancesBothVisibleLinks) {
  const auto snapshot = neighbors();
  const auto result   = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(result);
  EXPECT_LT(result->cost, result->originalCost);
  EXPECT_LT(result->delta, -40);
  EXPECT_GT(result->delta, -120);
  // The first route was gratuitously extreme. The other may grow moderately
  // steeper: the solver must evaluate a compromise rather than level one link.
  EXPECT_LT(
      std::max(std::abs(160 + result->delta), std::abs(40 + result->delta)),
      160);
  EXPECT_EQ(result->occurrences, 4U);
  EXPECT_EQ(result->evaluations, params().candidates + 1);
  EXPECT_EQ(snapshot[0].y,
            0); // The snapshot and fixed reading point are untouched.
  EXPECT_EQ(snapshot[1].x, 100);
}

TEST(ShallowAlignmentTest, NarrowGutterPrefersCompromiseToAnExtremeRoute) {
  auto snapshot = neighbors();
  for (auto &fragment : snapshot) {
    if (fragment.right) fragment.x = 18;
  }
  const auto result = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(result);
  EXPECT_LT(result->cost, result->originalCost);
  // Approximate the tiny native KJV gutter, rather than validating only a
  // generous 100-pixel run where saturated averages are easier to balance.
  EXPECT_LE(
      std::max(std::abs(160 + result->delta), std::abs(40 + result->delta)),
      90);
  EXPECT_GE(result->delta, params().minimumDelta);
  EXPECT_LE(result->delta, params().maximumDelta);
}

TEST(ShallowAlignmentTest, DuplicateScaffoldingAndInputOrderDoNotChangeVotes) {
  auto snapshot       = neighbors();
  const auto original = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(original);
  for (int i = 0; i < 50; ++i) snapshot.push_back(snapshot.front());
  std::ranges::reverse(snapshot);
  const auto duplicate = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate->delta, original->delta);
  EXPECT_EQ(duplicate->cost, original->cost);
  EXPECT_EQ(duplicate->fragments, original->fragments);
  snapshot.back().x   = 3;
  const auto conflict = preferShallowAlignment(snapshot, params());
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error(), ShallowAlignmentError::ConflictingDuplicate);
}

TEST(ShallowAlignmentTest, PageFragmentsShareTheirOccurrenceVote) {
  auto snapshot       = neighbors();
  const auto original = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(original);
  // Multiple visible page fragments remain represented, without letting one
  // occurrence win by repeated scaffold references to the same counterpart.
  for (std::uint64_t page = 1; page < 40; ++page) {
    auto fragment     = snapshot.front();
    fragment.fragment = page;
    snapshot.push_back(fragment);
  }
  const auto fragmented = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(fragmented);
  EXPECT_EQ(fragmented->occurrences, 4U);
  EXPECT_EQ(fragmented->fragments, 43U);
  EXPECT_NEAR(fragmented->cost, original->cost, 1e-12);
  EXPECT_EQ(fragmented->delta, original->delta);
}

TEST(ShallowAlignmentTest, SparseOffscreenMembersCannotDominateVisibleLayout) {
  auto snapshot       = neighbors();
  const auto original = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(original);
  snapshot.push_back(
      {.link = 1, .occurrence = 1, .visible = false, .x = 0, .y = -100000});
  snapshot.push_back(
      {.link = 1, .occurrence = 2, .visible = false, .x = 0, .y = 100000});
  const auto sparse = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(sparse);
  EXPECT_EQ(sparse->delta, original->delta);
  EXPECT_EQ(sparse->cost, original->cost);
}

TEST(ShallowAlignmentTest, ContradictoryOrdersAndZeroRunRemainBounded) {
  const std::array<SlopeFragment, 4> snapshot{
      {{.link = 1, .x = 0, .y = -80},
       {.link = 1, .right = true, .translated = true, .x = 0, .y = 80},
       {.link = 2, .x = 0, .y = 80},
       {.link = 2, .right = true, .translated = true, .x = 0, .y = -80}}};
  auto bounded         = params();
  bounded.minimumDelta = -5;
  bounded.maximumDelta = 5;
  const auto result    = preferShallowAlignment(snapshot, bounded);
  ASSERT_TRUE(result);
  EXPECT_TRUE(std::isfinite(result->cost));
  EXPECT_GE(result->delta, -5);
  EXPECT_LE(result->delta, 5);
  EXPECT_LE(result->cost, result->originalCost);
  bounded.minimumDelta = bounded.maximumDelta = 0;
  const auto pinned = preferShallowAlignment(snapshot, bounded);
  ASSERT_TRUE(pinned);
  EXPECT_EQ(pinned->delta, 0);
}

TEST(ShallowAlignmentTest,
     NoOppositeSideOrNoUsefulImprovementKeepsArrangement) {
  const std::array<SlopeFragment, 2> unpaired{
      {{.link = 1, .x = 0, .y = 0},
       {.link = 2, .right = true, .translated = true, .x = 100, .y = 160}}};
  const auto result = preferShallowAlignment(unpaired, params());
  ASSERT_TRUE(result);
  EXPECT_EQ(result->delta, 0);
  EXPECT_EQ(result->evaluations, 1U);
  auto hysteresis               = params();
  hysteresis.minimumImprovement = 100;
  const auto refused = preferShallowAlignment(neighbors(), hysteresis);
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->delta, 0);
  EXPECT_EQ(refused->cost, refused->originalCost);
}

TEST(ShallowAlignmentTest, RefusesIncompleteWorkAndInvalidBounds) {
  auto limited             = params();
  limited.maximumFragments = 3;
  const auto overflow      = preferShallowAlignment(neighbors(), limited);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), ShallowAlignmentError::WorkLimit);
  limited              = params();
  limited.minimumDelta = 1; // Staying put must always remain feasible.
  const auto bounds    = preferShallowAlignment(neighbors(), limited);
  ASSERT_FALSE(bounds);
  EXPECT_EQ(bounds.error(), ShallowAlignmentError::InvalidParameters);
  auto invalid        = neighbors();
  invalid.front().y   = std::numeric_limits<double>::quiet_NaN();
  const auto geometry = preferShallowAlignment(invalid, params());
  ASSERT_FALSE(geometry);
  EXPECT_EQ(geometry.error(), ShallowAlignmentError::InvalidFragment);
}
TEST(ShallowAlignmentTest, RefusesOverflowRatherThanReturningNaNCost) {
  auto extreme = neighbors();
  for (auto &fragment : extreme) {
    fragment.x = std::numeric_limits<double>::max();
    fragment.y = std::numeric_limits<double>::max();
  }
  // Two distinct left occurrences force a finite-input centroid sum past
  // double's range. One occurrence on each side only subtracts identical
  // finite coordinates and is not an overflow fixture.
  auto extra       = extreme.front();
  extra.occurrence = 1;
  extreme.push_back(extra);
  const auto overflow = preferShallowAlignment(extreme, params());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), ShallowAlignmentError::NumericalOverflow);
}

TEST(ShallowAlignmentTest, DenseEndsetsUseAFragmentAndCandidateBudget) {
  std::vector<SlopeFragment> snapshot;
  for (std::uint64_t i = 0; i < 256; ++i) {
    snapshot.push_back(
        {.link = 1, .occurrence = i, .x = 0, .y = static_cast<double>(i) / 4});
    snapshot.push_back({.link       = 1,
                        .occurrence = i,
                        .right      = true,
                        .translated = true,
                        .x          = 100,
                        .y          = 120 - static_cast<double>(i) / 4});
  }
  const auto result = preferShallowAlignment(snapshot, params());
  ASSERT_TRUE(result);
  EXPECT_EQ(result->fragments, 512U);
  EXPECT_EQ(result->occurrences, 512U);
  EXPECT_EQ(result->evaluations, params().candidates + 1);
  EXPECT_TRUE(std::isfinite(result->cost));
}

} // namespace
