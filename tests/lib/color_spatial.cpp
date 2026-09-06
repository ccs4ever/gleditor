#include <gtest/gtest.h>

#include <gleditor/color.hpp>
#include <gleditor/spatial.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

TEST(ColorTest, packAndUnpackRgbaRoundtrip) {
  const float r = 0.25F;
  const float g = 0.50F;
  const float b = 0.75F;
  const float a = 1.0F;

  const std::uint32_t packed = gleditor::color::packRgba(r, g, b, a);
  const gleditor::color::Color4 unpacked = gleditor::color::unpackRgba(packed);

  EXPECT_NEAR(unpacked.r, r, 0.01F);
  EXPECT_NEAR(unpacked.g, g, 0.01F);
  EXPECT_NEAR(unpacked.b, b, 0.01F);
  EXPECT_NEAR(unpacked.a, a, 0.01F);
}

TEST(ColorTest, parseAndFormatHexColor) {
  const auto c1 = gleditor::color::parseHexColor("#FF8000");
  ASSERT_TRUE(c1.has_value());
  EXPECT_NEAR(c1->r, 1.0F, 0.01F);
  EXPECT_NEAR(c1->g, 0.5F, 0.01F);
  EXPECT_NEAR(c1->b, 0.0F, 0.01F);

  // Without leading '#'
  const auto c2 = gleditor::color::parseHexColor("00FF00");
  ASSERT_TRUE(c2.has_value());
  EXPECT_NEAR(c2->r, 0.0F, 0.01F);
  EXPECT_NEAR(c2->g, 1.0F, 0.01F);
  EXPECT_NEAR(c2->b, 0.0F, 0.01F);

  // 3-digit hex string (#RGB) with allow3Digit=true
  const auto c3 = gleditor::color::parseHexColor("#F80", /*allow3Digit=*/true);
  ASSERT_TRUE(c3.has_value());
  EXPECT_NEAR(c3->r, 1.0F, 0.01F);
  EXPECT_NEAR(c3->g, 0.533F, 0.01F);
  EXPECT_NEAR(c3->b, 0.0F, 0.01F);

  // 3-digit hex without allow3Digit flag is rejected by default
  EXPECT_FALSE(gleditor::color::parseHexColor("#F80").has_value());

  // Invalid hex
  EXPECT_FALSE(gleditor::color::parseHexColor("invalid").has_value());
  EXPECT_FALSE(gleditor::color::parseHexColor("#12").has_value());
  EXPECT_FALSE(gleditor::color::parseHexColor("#1234").has_value());
  EXPECT_FALSE(gleditor::color::parseHexColor("#12345").has_value());

  // Formatting
  EXPECT_EQ(gleditor::color::formatHexColor(*c1), "#ff8000");
}

TEST(SpatialTest, evaluateQuadraticBezier) {
  const glm::vec3 p0(0.0F, 0.0F, 0.0F);
  const glm::vec3 p1(0.0F, 10.0F, 0.0F);
  const glm::vec3 p2(10.0F, 10.0F, 0.0F);

  const auto atStart =
      gleditor::spatial::evaluateQuadraticBezier(p0, p1, p2, 0.0F);
  EXPECT_NEAR(atStart.x, 0.0F, 0.001F);
  EXPECT_NEAR(atStart.y, 0.0F, 0.001F);

  const auto atEnd =
      gleditor::spatial::evaluateQuadraticBezier(p0, p1, p2, 1.0F);
  EXPECT_NEAR(atEnd.x, 10.0F, 0.001F);
  EXPECT_NEAR(atEnd.y, 10.0F, 0.001F);

  // At t = 0.5, B(0.5) = 0.25*p0 + 0.5*p1 + 0.25*p2 = (2.5, 7.5, 0.0)
  const auto atMid =
      gleditor::spatial::evaluateQuadraticBezier(p0, p1, p2, 0.5F);
  EXPECT_NEAR(atMid.x, 2.5F, 0.001F);
  EXPECT_NEAR(atMid.y, 7.5F, 0.001F);
}

TEST(ColorTest, toHexAndFromHex) {
  const std::string text = "Hello, Xanadu!";
  const std::string hex  = gleditor::color::toHex(text);
  EXPECT_EQ(gleditor::color::fromHex(hex), text);

  EXPECT_THROW(static_cast<void>(gleditor::color::fromHex("odd")),
               std::runtime_error);
  EXPECT_THROW(static_cast<void>(gleditor::color::fromHex("zz")),
               std::runtime_error);
}

TEST(SpatialTest, onScreenFrustumTesting) {
  const glm::mat4 proj =
      glm::perspective(glm::radians(60.0F), 4.0F / 3.0F, 0.1F, 100.0F);
  const glm::mat4 view =
      glm::lookAt(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
  const glm::mat4 vp = proj * view;

  // Origin is directly in front of camera
  EXPECT_TRUE(gleditor::spatial::onScreen(vp, glm::vec3(0, 0, 0)));

  // Point behind camera (Z > 10)
  EXPECT_FALSE(gleditor::spatial::onScreen(vp, glm::vec3(0, 0, 20)));

  // Point way out to the side
  EXPECT_FALSE(gleditor::spatial::onScreen(vp, glm::vec3(100, 0, 0)));
}

TEST(SpatialTest, framingDistanceAndFov) {
  const float width  = 20.0F;
  const float height = 15.0F;
  const float fovY   = 60.0F;
  const float aspect = 16.0F / 9.0F;

  const float dist =
      gleditor::spatial::framingDistance(width, height, fovY, aspect);
  EXPECT_GT(dist, 0.0F);

  const float computedFov =
      gleditor::spatial::framingFov(width, height, dist, aspect);
  EXPECT_NEAR(computedFov, fovY, 0.1F);
}
