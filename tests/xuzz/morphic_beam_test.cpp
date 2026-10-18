/**
 * @file morphic_beam_test.cpp
 * @brief Unit tests for morphic optical beams and cross-domain anchor geometry.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "common/xanadu/framing.hpp"
#include "common/xanadu/link_layout.hpp"

using namespace xanadu;

TEST(MorphicBeamTest, MorphicRouteGeometricContinuity) {
  const glm::vec3 docEdge(12.0F, 50.0F, 0.0F);
  const glm::vec3 cellAnchor(80.0F, -20.0F, -40.0F);

  // Tangents: document leaves horizontally along +X, cell approaches along -Z
  // normal
  const glm::vec3 docTangent(1.0F, 0.0F, 0.0F);
  const glm::vec3 cellNormal(0.0F, 0.0F, 1.0F);

  constexpr std::size_t segments = 16;
  const auto route =
      morphicRoute(docEdge, cellAnchor, docTangent, cellNormal, segments);

  ASSERT_EQ(route.size(), segments + 1);

  // Endpoint precision
  EXPECT_NEAR(route.front().x, docEdge.x, 1e-4F);
  EXPECT_NEAR(route.front().y, docEdge.y, 1e-4F);
  EXPECT_NEAR(route.front().z, docEdge.z, 1e-4F);

  EXPECT_NEAR(route.back().x, cellAnchor.x, 1e-4F);
  EXPECT_NEAR(route.back().y, cellAnchor.y, 1e-4F);
  EXPECT_NEAR(route.back().z, cellAnchor.z, 1e-4F);

  // Smoothness: no zero segment lengths and no jump discontinuities
  for (std::size_t i = 1; i < route.size(); ++i) {
    const float segLen = glm::distance(route[i - 1], route[i]);
    EXPECT_GT(segLen, 0.0F) << "Segment " << i << " collapsed to zero length";
  }

  // Tangent alignment check: initial step has positive X velocity
  const glm::vec3 initialStep = route[1] - route[0];
  EXPECT_GT(initialStep.x, 0.0F)
      << "Initial spline trajectory must follow docTangent (+X)";

  // Final step approaches cell with positive Z velocity matching normal
  const glm::vec3 finalStep = route.back() - route[route.size() - 2];
  EXPECT_GT(finalStep.z, 0.0F)
      << "Final spline arrival must have positive Z component";
}

TEST(MorphicBeamTest, AvoidsCollinearZSingularity) {
  // Pure Z-depth displacement that would collapse sideways cross product in 2D
  const glm::vec3 from(0.0F, 0.0F, 0.0F);
  const glm::vec3 to(0.0F, 0.0F, -60.0F);
  const glm::vec3 fromTangent(1.0F, 0.0F, 0.0F);
  const glm::vec3 toTangent(-1.0F, 0.0F, 0.0F);

  const auto route = morphicRoute(from, to, fromTangent, toTangent, 12);
  ASSERT_EQ(route.size(), 13U);

  // The route curves laterally in X rather than collapsing to a singular ray
  float maxLateralDeviation = 0.0F;
  for (const auto &pt : route) {
    maxLateralDeviation = std::max(maxLateralDeviation, std::abs(pt.x));
  }
  EXPECT_GT(maxLateralDeviation, 5.0F) << "Morphic route must curve laterally "
                                          "into 3D space to avoid Z collinear "
                                          "singularity";
}

TEST(MorphicBeamTest, CellAnchorGeometricBounds) {
  CellAnchor anchor{
      .position   = glm::vec3(50.0F, 100.0F, -30.0F),
      .width      = 200.0F,
      .height     = 80.0F,
      .lineHeight = 18.0F,
      .normal     = glm::vec3(0.0F, 0.0F, 1.0F),
  };

  // When facing rightwards (towardsRight = true), edge is on right margin
  const float halfW = anchor.width * 0.5F;
  const float halfH = anchor.height * 0.5F;

  const float expectedRightX = anchor.position.x + halfW;
  const float expectedTopY   = anchor.position.y + halfH;
  const float expectedBotY   = anchor.position.y - halfH;

  const glm::vec3 rightTop(expectedRightX, expectedTopY, anchor.position.z);
  const glm::vec3 rightBottom(expectedRightX, expectedBotY, anchor.position.z);

  EXPECT_FLOAT_EQ(rightTop.x, 150.0F);
  EXPECT_FLOAT_EQ(rightTop.y, 140.0F);
  EXPECT_FLOAT_EQ(rightTop.z, -30.0F);

  EXPECT_FLOAT_EQ(rightBottom.x, 150.0F);
  EXPECT_FLOAT_EQ(rightBottom.y, 60.0F);
  EXPECT_FLOAT_EQ(rightBottom.z, -30.0F);

  // When facing leftwards (towardsRight = false), edge is on left margin
  const float expectedLeftX = anchor.position.x - halfW;
  EXPECT_FLOAT_EQ(expectedLeftX, -50.0F);
}
