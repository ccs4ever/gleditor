/**
 * @file tests/lib/onion_skin_test.cpp
 * @brief Unit tests for 3D Multi-Document Onion Skinning layout and cycling.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <choreograph/Choreograph.h>
#include <glm/vec3.hpp>

#include <gleditor/animation.hpp>
#include <gleditor/state.hpp>

namespace {

struct OnionSkinLayer {
  glm::vec3 position;
  float opacity;
};

OnionSkinLayer computeLayer(const std::size_t docIdx,
                            const std::size_t activeIdx,
                            const std::size_t totalDocs) {
  if (totalDocs == 0) {
    return {glm::vec3(0.0F), 1.0F};
  }
  const auto active = activeIdx % totalDocs;
  const auto k      = (docIdx - active + totalDocs) % totalDocs;
  const float kF    = static_cast<float>(k);

  const glm::vec3 pos(kF * 18.0F, kF * 14.0F, -kF * 10.0F);
  const float op = (k == 0) ? 1.0F : std::max(0.20F, 1.0F - 0.20F * kF);
  return {pos, op};
}

std::size_t cycleIndex(const std::size_t currentIdx, const int delta,
                       const std::size_t totalDocs) {
  if (totalDocs == 0) {
    return 0;
  }
  const int n = static_cast<int>(totalDocs);
  int nextIdx = (static_cast<int>(currentIdx) + delta) % n;
  if (nextIdx < 0) {
    nextIdx += n;
  }
  return static_cast<std::size_t>(nextIdx);
}

void stepTimeline(ch::Timeline &timeline, const double seconds,
                  const double slice = 1.0 / 60.0) {
  for (double elapsed = 0.0; elapsed < seconds; elapsed += slice) {
    timeline.step(std::min(slice, seconds - elapsed));
  }
}

} // namespace

TEST(OnionSkinTest, ActiveFrontDocumentIsAtOriginWithFullOpacity) {
  constexpr std::size_t totalDocs = 5;
  for (std::size_t active = 0; active < totalDocs; ++active) {
    const auto layer = computeLayer(active, active, totalDocs);
    EXPECT_FLOAT_EQ(layer.position.x, 0.0F);
    EXPECT_FLOAT_EQ(layer.position.y, 0.0F);
    EXPECT_FLOAT_EQ(layer.position.z, 0.0F);
    EXPECT_FLOAT_EQ(layer.opacity, 1.0F);
  }
}

TEST(OnionSkinTest, BackgroundDocumentsCascadeInDepthAndStagger) {
  constexpr std::size_t totalDocs = 4;
  constexpr std::size_t active    = 0;

  // Layer 1 (doc 1): k = 1
  const auto l1 = computeLayer(1, active, totalDocs);
  EXPECT_FLOAT_EQ(l1.position.x, 18.0F);
  EXPECT_FLOAT_EQ(l1.position.y, 14.0F);
  EXPECT_FLOAT_EQ(l1.position.z, -10.0F);
  EXPECT_FLOAT_EQ(l1.opacity, 0.80F);

  // Layer 2 (doc 2): k = 2
  const auto l2 = computeLayer(2, active, totalDocs);
  EXPECT_FLOAT_EQ(l2.position.x, 36.0F);
  EXPECT_FLOAT_EQ(l2.position.y, 28.0F);
  EXPECT_FLOAT_EQ(l2.position.z, -20.0F);
  EXPECT_FLOAT_EQ(l2.opacity, 0.60F);

  // Layer 3 (doc 3): k = 3
  const auto l3 = computeLayer(3, active, totalDocs);
  EXPECT_FLOAT_EQ(l3.position.x, 54.0F);
  EXPECT_FLOAT_EQ(l3.position.y, 42.0F);
  EXPECT_FLOAT_EQ(l3.position.z, -30.0F);
  EXPECT_FLOAT_EQ(l3.opacity, 0.40F);
}

TEST(OnionSkinTest, OpacityDecayClampsAtFloorForDeepStacks) {
  constexpr std::size_t totalDocs = 8;
  constexpr std::size_t active    = 0;

  // Layer 4: k = 4 -> 1.0 - 0.80 = 0.20
  const auto l4 = computeLayer(4, active, totalDocs);
  EXPECT_FLOAT_EQ(l4.opacity, 0.20F);

  // Layer 5+: k = 5 -> clamped at 0.20 floor
  const auto l5 = computeLayer(5, active, totalDocs);
  EXPECT_FLOAT_EQ(l5.opacity, 0.20F);
  const auto l7 = computeLayer(7, active, totalDocs);
  EXPECT_FLOAT_EQ(l7.opacity, 0.20F);
}

TEST(OnionSkinTest, ScrollWheelCyclingWrapsForwardAndBackward) {
  constexpr std::size_t totalDocs = 3;

  // Forward cycle
  EXPECT_EQ(cycleIndex(0, 1, totalDocs), 1U);
  EXPECT_EQ(cycleIndex(1, 1, totalDocs), 2U);
  EXPECT_EQ(cycleIndex(2, 1, totalDocs), 0U); // Wraps to 0

  // Backward cycle
  EXPECT_EQ(cycleIndex(0, -1, totalDocs), 2U); // Wraps to 2
  EXPECT_EQ(cycleIndex(2, -1, totalDocs), 1U);
  EXPECT_EQ(cycleIndex(1, -1, totalDocs), 0U);
}

TEST(OnionSkinTest, BackToFrontDepthSortingOrder) {
  struct MockDoc {
    std::size_t id;
    float z;
  };

  std::vector<MockDoc> docs = {
      {0, 0.0F},   // Front
      {1, -10.0F}, // Mid
      {2, -30.0F}, // Deepest
      {3, -20.0F}, // Deep
  };

  std::stable_sort(
      docs.begin(), docs.end(),
      [](const MockDoc &a, const MockDoc &b) { return a.z < b.z; });

  // Lowest Z (furthest behind) must be first for alpha compositing
  EXPECT_EQ(docs[0].id, 2U);
  EXPECT_FLOAT_EQ(docs[0].z, -30.0F);
  EXPECT_EQ(docs[1].id, 3U);
  EXPECT_FLOAT_EQ(docs[1].z, -20.0F);
  EXPECT_EQ(docs[2].id, 1U);
  EXPECT_FLOAT_EQ(docs[2].z, -10.0F);
  EXPECT_EQ(docs[3].id, 0U);
  EXPECT_FLOAT_EQ(docs[3].z, 0.0F);
}

TEST(OnionSkinTest, TimelineOpacityRampInterpolation) {
  ch::Timeline timeline;
  ch::Output<float> opacity{1.0F};

  // Animate opacity from 1.0 down to 0.40
  timeline.apply(&opacity).then<ch::RampTo>(0.40F, gleditor::anim::docArrival,
                                            ch::EaseInOutQuad());

  EXPECT_FLOAT_EQ(opacity(), 1.0F);
  stepTimeline(timeline, gleditor::anim::docArrival / 2.0);
  EXPECT_LT(opacity(), 1.0F);
  EXPECT_GT(opacity(), 0.40F);

  stepTimeline(timeline, gleditor::anim::docArrival);
  EXPECT_NEAR(opacity(), 0.40F, 1e-4);
}

TEST(OnionSkinTest, AppStateWheelHandlerInterception) {
  AppState state;
  EXPECT_FALSE(state.wheelHandler);

  bool intercepted   = false;
  float receivedWy   = 0.0F;
  state.wheelHandler = [&](float /*wx*/, float wy,
                           std::uint16_t /*mods*/) -> bool {
    intercepted = true;
    receivedWy  = wy;
    return true;
  };

  EXPECT_TRUE(state.wheelHandler(0.0F, 1.0F, 0));
  EXPECT_TRUE(intercepted);
  EXPECT_FLOAT_EQ(receivedWy, 1.0F);
}
