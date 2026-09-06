/**
 * @file tension_layout_test.cpp
 * @brief Unit tests for the 3-Way Tension Layout Engine and Tenuous Parent Tethers.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <glm/geometric.hpp>

#include "xudu/core/tension_layout.hpp"
#include "xudu/tenuous_tether.hpp"

namespace xudu {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsTrue;
using ::testing::Lt;

TEST(TensionLayoutTest, RK4DampedConvergence) {
  TensionParams params;
  params.kDamping = 12.0F;
  params.kPlane   = 25.0F;
  params.settleVelocityThreshold = 0.01F;

  TensionLayoutEngine engine(params);

  TensionBody body;
  body.docIndex   = 0;
  body.position   = glm::vec3(0.0F, 0.0F, 20.0F); // Displaced from Z = 0
  body.velocity   = glm::vec3(10.0F, -5.0F, 15.0F);
  body.mass       = 1.0F;
  body.isForeground = true;

  engine.setBody(body);
  EXPECT_FALSE(engine.isSettled());

  constexpr float dt = 0.016F;
  for (int i = 0; i < 250; ++i) {
    engine.step(dt);
  }

  EXPECT_TRUE(engine.isSettled());
  const auto *settledBody = engine.findBody(0);
  ASSERT_NE(settledBody, nullptr);
  EXPECT_THAT(std::abs(settledBody->position.z), Lt(0.1F));
  EXPECT_THAT(glm::length(settledBody->velocity), Lt(0.02F));
}

TEST(TensionLayoutTest, CoulombRepulsionPreventsOverlap) {
  TensionParams params;
  params.kRepel   = 6000.0F;
  params.kDamping = 10.0F;
  params.defaultGap = 8.0F;

  TensionLayoutEngine engine(params);

  // Two bodies initialized overlapping in X
  TensionBody b1;
  b1.docIndex = 1;
  b1.position = glm::vec3(0.0F, 0.0F, 0.0F);
  b1.width    = 40.0F;
  b1.height   = 60.0F;

  TensionBody b2;
  b2.docIndex = 2;
  b2.position = glm::vec3(5.0F, 0.0F, 0.0F); // Heavily overlapping!
  b2.width    = 40.0F;
  b2.height   = 60.0F;

  engine.setBody(b1);
  engine.setBody(b2);

  const float initialDist = std::abs(b2.position.x - b1.position.x);
  EXPECT_THAT(initialDist, Lt(48.0F));

  constexpr float dt = 0.016F;
  for (int i = 0; i < 250; ++i) {
    engine.step(dt);
  }

  const auto *res1 = engine.findBody(1);
  const auto *res2 = engine.findBody(2);
  ASSERT_NE(res1, nullptr);
  ASSERT_NE(res2, nullptr);

  const float finalDist = std::abs(res2->position.x - res1->position.x);
  // Repulsion must have separated them significantly wider than initial
  EXPECT_THAT(finalDist, Gt(initialDist));
  // Separated to safe non-overlapping distance
  EXPECT_THAT(finalDist, Ge(40.0F));
}

TEST(TensionLayoutTest, CollinearAlignmentSpring) {
  TensionParams params;
  params.kAlign   = 35.0F;
  params.kDamping = 12.0F;
  params.kPlane   = 20.0F;
  params.defaultGap = 8.0F;

  TensionLayoutEngine engine(params);

  // Near document pinned at origin
  TensionBody nearDoc;
  nearDoc.docIndex = 10;
  nearDoc.position = glm::vec3(0.0F, 0.0F, 0.0F);
  nearDoc.width    = 50.0F;
  nearDoc.height   = 70.0F;
  nearDoc.pinned   = true;

  // Far document initially in background plane at Z = -40
  TensionBody farDoc;
  farDoc.docIndex     = 20;
  farDoc.position     = glm::vec3(120.0F, 50.0F, -40.0F);
  farDoc.width        = 50.0F;
  farDoc.height       = 70.0F;
  farDoc.isForeground = true; // Becoming foreground subject of alignment
  farDoc.isFlying     = true;

  engine.setBody(nearDoc);
  engine.setBody(farDoc);

  // Link constraint: near anchor at Y = +10, far anchor at Y = -10
  // deltaAnchorY = 10 - (-10) = +20 => target far Y = 0 + 20 = 20
  // target far X = 0 + 0.5 * (50 + 50) + 8 = 58
  TensionConstraint link;
  link.fromDoc      = 10;
  link.toDoc        = 20;
  link.nearAnchorY  = 10.0F;
  link.farAnchorY   = -10.0F;
  link.targetGap    = 8.0F;
  link.prominence   = 1.0F;
  link.active       = true;

  engine.addConstraint(link);

  constexpr float dt = 0.016F;
  for (int i = 0; i < 300; ++i) {
    engine.step(dt);
  }

  const auto *resFar = engine.findBody(20);
  ASSERT_NE(resFar, nullptr);

  // Verify collinear side-by-side alignment:
  // X brought to target ~58
  EXPECT_THAT(std::abs(resFar->position.x - 58.0F), Lt(3.5F));
  // Y brought to collinear anchor level ~20
  EXPECT_THAT(std::abs(resFar->position.y - 20.0F), Lt(3.5F));
  // Z pulled forward to reading plane ~0
  EXPECT_THAT(std::abs(resFar->position.z - 0.0F), Lt(3.5F));
}

TEST(TensionLayoutTest, AnalyticalEquilibriumSolver) {
  TensionLayoutEngine engine;

  TensionBody b1;
  b1.docIndex     = 1;
  b1.width        = 40.0F;
  b1.height       = 60.0F;
  b1.isForeground = true;

  TensionBody b2;
  b2.docIndex     = 2;
  b2.width        = 50.0F;
  b2.height       = 60.0F;
  b2.isForeground = true;

  TensionBody b3;
  b3.docIndex     = 3;
  b3.width        = 40.0F;
  b3.height       = 60.0F;
  b3.isForeground = false; // Background corpus

  engine.setBody(b1);
  engine.setBody(b2);
  engine.setBody(b3);

  TensionConstraint c;
  c.fromDoc     = 1;
  c.toDoc       = 2;
  c.nearAnchorY = 15.0F;
  c.farAnchorY  = 5.0F;
  engine.addConstraint(c);

  engine.solveEquilibrium();

  const auto *r1 = engine.findBody(1);
  const auto *r2 = engine.findBody(2);
  const auto *r3 = engine.findBody(3);

  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  ASSERT_NE(r3, nullptr);

  EXPECT_THAT(r1->position.x, Eq(0.0F));
  EXPECT_THAT(r1->position.z, Eq(0.0F));

  // r2 X = 0.5 * 40 + 8 + 0.5 * 50 = 20 + 8 + 25 = 53
  EXPECT_THAT(r2->position.x, Eq(53.0F));
  EXPECT_THAT(r2->position.y, Eq(10.0F)); // 15 - 5 = 10
  EXPECT_THAT(r2->position.z, Eq(0.0F));

  // r3 should be resting in background
  EXPECT_THAT(r3->position.z, Eq(-40.0F));
}

TEST(TensionLayoutTest, TenuousTetherBezierProperties) {
  const glm::vec3 origin(0.0F, 0.0F, -40.0F);
  const glm::vec3 flying(50.0F, 10.0F, 0.0F);

  const glm::vec3 ctrl =
      TenuousTetherOverlay::computeControlPoint(origin, flying, 15.0F);

  // Control point Z must be deeper into -Z than both origin and flying
  EXPECT_THAT(ctrl.z, Lt(origin.z));
  EXPECT_THAT(ctrl.z, Lt(flying.z));
  EXPECT_THAT(ctrl.z, Eq(-55.0F)); // min(-40, 0) - 15 = -55

  // Bezier endpoints
  const glm::vec3 pStart =
      TenuousTetherOverlay::evaluateBezier(origin, ctrl, flying, 0.0F);
  const glm::vec3 pEnd =
      TenuousTetherOverlay::evaluateBezier(origin, ctrl, flying, 1.0F);
  const glm::vec3 pMid =
      TenuousTetherOverlay::evaluateBezier(origin, ctrl, flying, 0.5F);

  EXPECT_THAT(pStart.x, Eq(origin.x));
  EXPECT_THAT(pStart.y, Eq(origin.y));
  EXPECT_THAT(pStart.z, Eq(origin.z));

  EXPECT_THAT(pEnd.x, Eq(flying.x));
  EXPECT_THAT(pEnd.y, Eq(flying.y));
  EXPECT_THAT(pEnd.z, Eq(flying.z));

  // Midpoint is arched into depth
  EXPECT_THAT(pMid.x, Eq(25.0F));
  EXPECT_THAT(pMid.y, Eq(5.0F));
  EXPECT_THAT(pMid.z, Lt(-20.0F)); // Arched backwards
}

} // namespace
} // namespace xudu
