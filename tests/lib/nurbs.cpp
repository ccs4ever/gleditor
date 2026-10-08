#include <cmath>
#include <gleditor/nurbs.hpp>
#include <glm/geometric.hpp>
#include <gtest/gtest.h>
#include <limits>

TEST(NurbsTest, RationalQuadraticDescribesACircularArc) {
  const std::array points{glm::vec3(1, 0, 0), glm::vec3(1, 1, 0),
                          glm::vec3(0, 1, 0)};
  const std::array weights{1.F, std::sqrt(.5F), 1.F};
  const std::array knots{0.F, 0.F, 0.F, 1.F, 1.F, 1.F};
  const gleditor::NurbsPath path(points, weights, knots, 2);
  const auto samples = gleditor::sampleNurbs(path, 64);
  for (unsigned i = 0; i < samples.count; ++i) {
    const auto &p = samples.points[i];
    EXPECT_NEAR(glm::dot(p.position, p.position), 1, 1e-5);
    EXPECT_NEAR(glm::dot(p.position, p.tangent), 0, 1e-5);
    if (i) EXPECT_GT(p.along, samples.points[i - 1].along);
  }
  EXPECT_NEAR(samples.length, std::acos(-1.F) / 2, 1e-4);
  EXPECT_EQ(samples.points.front().along, 0);
  EXPECT_EQ(samples.points[samples.count - 1].along, 1);
}

TEST(NurbsTest, InteriorKnotRetainsLocalControlAndContinuousTangent) {
  std::array points{glm::vec3(0), glm::vec3(1, 1, 0), glm::vec3(2, 1, 0),
                    glm::vec3(3, 0, 0), glm::vec3(4, 0, 0)};
  const std::array weights{1.F, 1.F, 1.F, 1.F, 1.F};
  const std::array knots{0.F, 0.F, 0.F, 0.F, .5F, 1.F, 1.F, 1.F, 1.F};
  const gleditor::NurbsPath path(points, weights, knots, 3);
  const auto before = path.position(.1F);
  EXPECT_LT(glm::distance(path.tangent(.5F - 1e-5F), path.tangent(.5F + 1e-5F)),
            .001F);
  points.back().y = 10;
  EXPECT_EQ(path.position(.1F), before);
  EXPECT_EQ(path.position(0), points.front());
  EXPECT_EQ(path.position(1), points.back());
  EXPECT_EQ(gleditor::sampleNurbs(path, 10000).count, 257U);
}

TEST(NurbsTest, RefusesInvalidWeightsKnotsAndParameters) {
  const std::array points{glm::vec3(0), glm::vec3(1), glm::vec3(2)};
  std::array weights{1.F, 1.F, 1.F};
  std::array knots{0.F, 0.F, 0.F, 1.F, 1.F, 1.F};
  weights[1] = 0;
  EXPECT_THROW(gleditor::NurbsPath(points, weights, knots, 2),
               std::invalid_argument);
  weights[1] = 1;
  knots[2]   = .5F;
  EXPECT_THROW(gleditor::NurbsPath(points, weights, knots, 2),
               std::invalid_argument);
  knots[2] = 0;
  const gleditor::NurbsPath path(points, weights, knots, 2);
  EXPECT_THROW(static_cast<void>(path.position(-.1F)), std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(path.position(std::numeric_limits<float>::quiet_NaN())),
      std::invalid_argument);
}
