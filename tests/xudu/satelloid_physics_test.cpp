/**
 * @file satelloid_physics_test.cpp
 * @brief Unit tests for the 3-Way Tension Solver extension with Flying Cell
 * Satelloids, tenuous Bezier parent tethers, and focus ring dynamics.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "common/xanadu/universal_link_endpoint.hpp"
#include "xudu/core/tension_layout.hpp"
#include "xudu/satelloid.hpp"
#include "xudu/tenuous_tether.hpp"

namespace xudu {
namespace {

using ::testing::DoubleNear;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::IsTrue;
using ::testing::Lt;

TEST(SatelloidPhysicsTest, CollinearConvergenceWithin300ms) {
  TensionParams params;
  params.kSatelloidAlign = 200.0F;
  params.kTether         = 10.0F;
  params.kDamping        = 18.0F;
  params.satelloidMass   = 0.5F;
  params.satelloidGap    = 10.0F;

  TensionLayoutEngine engine(params);

  // Near document pinned at origin (reading reference)
  TensionBody docBody;
  docBody.docIndex        = 1;
  docBody.targetKind      = LinkTargetKind::Document;
  docBody.position        = glm::vec3(0.0F, 0.0F, 0.0F);
  docBody.restingPosition = glm::vec3(0.0F, 0.0F, 0.0F);
  docBody.width           = 50.0F;
  docBody.height          = 70.0F;
  docBody.isForeground    = true;
  docBody.pinned          = true;

  // Cell satelloid initially resting in background lattice at Z = -40px
  const glm::vec3 nativePos(80.0F, -15.0F, -40.0F);
  TensionBody cellBody;
  cellBody.targetId        = 42;
  cellBody.targetKind      = LinkTargetKind::ZigzagCell;
  cellBody.position        = nativePos;
  cellBody.restingPosition = nativePos;
  cellBody.width           = 24.0F;
  cellBody.height          = 14.0F;
  cellBody.isForeground    = true;
  cellBody.isFlying        = true;
  cellBody.mass            = params.satelloidMass;
  cellBody.pinned          = false;

  engine.setBody(docBody);
  engine.setBody(cellBody);

  // Active constraint: link to text line at Y = 25.0px on document
  TensionConstraint constraint;
  constraint.fromDoc     = 1;
  constraint.fromKind    = LinkTargetKind::Document;
  constraint.toTarget    = 42;
  constraint.toKind      = LinkTargetKind::ZigzagCell;
  constraint.nearAnchorY = 25.0F;
  constraint.farAnchorY  = 0.0F; // Cell center
  constraint.targetGap   = params.satelloidGap;
  constraint.prominence  = 1.0F;
  constraint.active      = true;

  engine.addConstraint(constraint);

  // Target coordinates for collinear reading:
  // X_target = doc.x + 0.5 * (doc.w + cell.w) + gap = 0 + 0.5 * (50 + 24) + 10
  // = 47.0px Y_target = doc.y + (nearY - farY) = 0 + (25 - 0) = 25.0px Z_target
  // = doc.z = 0.0px (reading plane)
  const float expectedTargetX = 47.0F;
  const float expectedTargetY = 25.0F;
  const float expectedTargetZ = 0.0F;

  // 18 RK4 steps at dt = 0.016s represents 288ms (<= 300ms)
  constexpr float dt = 0.016F;
  for (int step = 0; step < 18; ++step) {
    engine.step(dt);
  }

  const auto solved = engine.findCellBody(42);
  ASSERT_TRUE((solved).has_value());

  // Satelloid must settle collinear to text line within +/- 0.5px
  EXPECT_NEAR(solved->position.y, expectedTargetY, 0.5F)
      << "Satelloid Y must settle collinear to active text line within +/- "
         "0.5px";
  EXPECT_NEAR(solved->position.z, expectedTargetZ, 0.5F)
      << "Satelloid Z must glide forward to reading plane Z=0 within +/- 0.5px";
  EXPECT_NEAR(solved->position.x, expectedTargetX, 0.5F)
      << "Satelloid X must position adjacent to right margin within +/- 0.5px";
}

TEST(SatelloidPhysicsTest, TetherRestorationPullsBackToLattice) {
  TensionParams params;
  params.kSatelloidAlign  = 36.0F;
  params.kTether          = 10.0F;
  params.kDamping         = 7.5F;
  params.satelloidMass    = 0.5F;
  params.kTier            = 12.0F;
  params.backgroundDepthZ = -40.0F;

  TensionLayoutEngine engine(params);

  const glm::vec3 nativePos(70.0F, 10.0F, -40.0F);

  // Cell satelloid initially displaced into foreground Z = 0
  TensionBody cellBody;
  cellBody.targetId        = 99;
  cellBody.targetKind      = LinkTargetKind::ZigzagCell;
  cellBody.position        = glm::vec3(45.0F, 20.0F, 0.0F);
  cellBody.restingPosition = nativePos;
  cellBody.width           = 24.0F;
  cellBody.height          = 14.0F;
  cellBody.isForeground    = false; // Inactive, no longer foreground reading
  cellBody.isFlying        = false;
  cellBody.mass            = params.satelloidMass;
  cellBody.pinned          = false;

  engine.setBody(cellBody);

  // No active constraint -- tether restoring force must pull back to native
  // position
  constexpr float dt = 0.016F;
  for (int step = 0; step < 200; ++step) {
    engine.step(dt);
  }

  const auto restored = engine.findCellBody(99);
  ASSERT_TRUE((restored).has_value());

  // Satelloid must return back towards background lattice coordinate
  EXPECT_NEAR(restored->position.z, nativePos.z, 0.5F);
  EXPECT_NEAR(restored->position.x, nativePos.x, 0.5F);
  EXPECT_NEAR(restored->position.y, nativePos.y, 0.5F);
}

TEST(SatelloidPhysicsTest, TenuousTetherBezierGeometricContinuity) {
  const glm::vec3 nativePos(80.0F, -10.0F, -40.0F);
  const glm::vec3 satelloidPos(45.0F, 25.0F, 0.0F);

  const glm::vec3 ctrl =
      TenuousTetherOverlay::computeControlPoint(nativePos, satelloidPos, 15.0F);

  // Control point must arch backwards deeper into -Z than both endpoints
  EXPECT_LT(ctrl.z, nativePos.z);
  EXPECT_LT(ctrl.z, satelloidPos.z);
  EXPECT_FLOAT_EQ(ctrl.z, -55.0F); // min(-40, 0) - 15 = -55

  constexpr int segments = 16;
  std::vector<glm::vec3> arc(segments + 1);
  for (int i = 0; i <= segments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments);
    arc[i] =
        TenuousTetherOverlay::evaluateBezier(nativePos, ctrl, satelloidPos, t);
  }

  // Endpoints match
  EXPECT_FLOAT_EQ(arc.front().x, nativePos.x);
  EXPECT_FLOAT_EQ(arc.front().y, nativePos.y);
  EXPECT_FLOAT_EQ(arc.front().z, nativePos.z);

  EXPECT_FLOAT_EQ(arc.back().x, satelloidPos.x);
  EXPECT_FLOAT_EQ(arc.back().y, satelloidPos.y);
  EXPECT_FLOAT_EQ(arc.back().z, satelloidPos.z);

  // Smooth continuity with non-zero segment lengths
  for (std::size_t i = 1; i < arc.size(); ++i) {
    const float segLen = glm::distance(arc[i - 1], arc[i]);
    EXPECT_GT(segLen, 0.0F) << "Segment " << i << " must have non-zero length";
  }
}

TEST(SatelloidPhysicsTest, CellSatelloidFocusRingAndAlphaDynamics) {
  CellSatelloid sat;
  sat.cellRef     = 101;
  sat.originPos   = glm::vec3(50.0F, 50.0F, -40.0F);
  sat.currentPos  = glm::vec3(45.0F, 25.0F, 0.0F);
  sat.targetPos   = glm::vec3(45.0F, 25.0F, 0.0F);
  sat.width       = 24.0F;
  sat.height      = 14.0F;
  sat.text        = "Sample cell prose";
  sat.dimName     = "d.category";
  sat.accentColor = 0x10B981FF; // Emerald
  sat.active      = true;
  sat.targetAlpha = 1.0F;
  sat.alpha       = 0.0F;

  EXPECT_EQ(sat.cellRef, 101);
  EXPECT_EQ(sat.dimName, "d.category");
  EXPECT_EQ(sat.accentColor, 0x10B981FF);
  EXPECT_FLOAT_EQ(sat.pulseAlpha, 0.0F);
  EXPECT_FLOAT_EQ(sat.pulseRadius, 0.0F);

  // Trigger pulse animation
  sat.triggerPulse();
  EXPECT_FLOAT_EQ(sat.pulseAlpha, 1.0F);
  EXPECT_FLOAT_EQ(sat.pulseRadius, 6.0F);

  // Step dynamics forward
  constexpr float dt = 0.016F;
  sat.updateDynamics(dt);

  // Pulse expands and decays
  EXPECT_GT(sat.pulseRadius, 6.0F);
  EXPECT_LT(sat.pulseAlpha, 1.0F);
  EXPECT_GT(sat.pulseAlpha, 0.0F);
  EXPECT_GT(sat.alpha, 0.0F);

  // After multiple steps (~1s), pulse alpha decays and card alpha reaches
  // target
  for (int step = 0; step < 60; ++step) {
    sat.updateDynamics(dt);
  }

  EXPECT_LT(sat.pulseAlpha, 0.01F);
  EXPECT_NEAR(sat.alpha, 1.0F, 0.01F);

  // Deactivate satelloid
  sat.active      = false;
  sat.targetAlpha = 0.0F;
  for (int step = 0; step < 60; ++step) {
    sat.updateDynamics(dt);
  }
  EXPECT_LT(sat.alpha, 0.01F);
}

TEST(SatelloidPhysicsTest, EquilibriumAnalyticalSolverAlignsSatelloids) {
  TensionParams params;
  params.satelloidGap = 12.0F;
  TensionLayoutEngine engine(params);

  TensionBody doc;
  doc.docIndex     = 5;
  doc.targetKind   = LinkTargetKind::Document;
  doc.position     = glm::vec3(0.0F, 100.0F, 0.0F);
  doc.width        = 60.0F;
  doc.height       = 80.0F;
  doc.isForeground = true;

  TensionBody cell;
  cell.targetId        = 77;
  cell.targetKind      = LinkTargetKind::ZigzagCell;
  cell.restingPosition = glm::vec3(120.0F, 0.0F, -40.0F);
  cell.width           = 30.0F;
  cell.height          = 20.0F;
  cell.isForeground    = true;
  cell.isFlying        = true;

  engine.setBody(doc);
  engine.setBody(cell);

  TensionConstraint link;
  link.fromDoc     = 5;
  link.fromKind    = LinkTargetKind::Document;
  link.toTarget    = 77;
  link.toKind      = LinkTargetKind::ZigzagCell;
  link.nearAnchorY = 30.0F; // 30px offset from doc center
  link.farAnchorY  = 0.0F;
  link.targetGap   = 12.0F;
  link.active      = true;

  engine.addConstraint(link);
  engine.solveEquilibrium();

  const auto resCell = engine.findCellBody(77);
  ASSERT_TRUE((resCell).has_value());

  // In equilibrium:
  // X = doc.x + 0.5 * (60 + 30) + 12 = 0 + 45 + 12 = 57.0px
  // Y = doc.y + (30 - 0) = 100 + 30 = 130.0px
  // Z = 0.0px
  EXPECT_FLOAT_EQ(resCell->position.x, 57.0F);
  EXPECT_FLOAT_EQ(resCell->position.y, 130.0F);
  EXPECT_FLOAT_EQ(resCell->position.z, 0.0F);
}

} // namespace
} // namespace xudu
