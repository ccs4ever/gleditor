/**
 * @file spatial_unproject_test.cpp
 * @brief Tests that unprojection is the exact inverse of the projection the
 *        renderer uses, and that the ray tests built on it answer correctly.
 *
 * A pick that lands a few pixels off is indistinguishable, in a frame, from a
 * reader who missed; the round trips here are what tell the two apart.
 */
#include <gtest/gtest.h>

#include <gleditor/render/constants.hpp>
#include <gleditor/spatial.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

using gleditor::spatial::distanceToSegment;
using gleditor::spatial::intersectPlane;
using gleditor::spatial::intersectQuad;
using gleditor::spatial::projectToScreen;
using gleditor::spatial::projectToViewport;
using gleditor::spatial::Ray;
using gleditor::spatial::unprojectToRay;
using gleditor::spatial::Viewport;
using gleditor::spatial::windowToPixels;

/// A hundredth of a pixel, the tolerance the rendering plan sets.
constexpr float kPixelTolerance = 0.01F;

struct Pose {
  glm::vec3 eye;
  glm::vec3 target;
  glm::vec3 up;
};

const std::array<Pose, 4> kPoses{{
    {{0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
    {{120.0F, -40.0F, 300.0F}, {100.0F, -60.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
    {{-50.0F, 80.0F, -20.0F}, {10.0F, 0.0F, -200.0F}, {0.0F, 1.0F, 0.0F}},
    // Rolled and looking along x, so no axis of the camera is a world axis.
    {{5.0F, 5.0F, 5.0F}, {400.0F, 30.0F, -10.0F}, {0.0F, 0.6F, 0.8F}},
}};
constexpr std::array<float, 3> kFovs{30.0F, 45.0F, 90.0F};
constexpr std::array<float, 3> kAspects{0.5F, 1.0F, 16.0F / 9.0F};

/// The renderer's own construction (src/renderer.cpp): perspective * lookAt.
glm::mat4 worldToClip(const Pose &pose, const float fovDegrees,
                      const float aspect) {
  return glm::perspective(glm::radians(fovDegrees), aspect,
                          render::kDefaultNearClipZ, render::kDefaultFarClipZ) *
         glm::lookAt(pose.eye, pose.target, pose.up);
}

glm::vec3 forwardOf(const Pose &pose) {
  return glm::normalize(pose.target - pose.eye);
}

void expectPixel(const glm::vec2 actual, const glm::vec2 expected) {
  EXPECT_NEAR(actual.x, expected.x, kPixelTolerance);
  EXPECT_NEAR(actual.y, expected.y, kPixelTolerance);
}

/// Pixel -> ray -> a plane facing the camera at @p distance -> pixel.
void expectRoundTrip(const glm::mat4 &transform, const Viewport &viewport,
                     const Pose &pose, const float distance) {
  const auto forward = forwardOf(pose);
  for (int row = 0; row <= 4; ++row) {
    for (int column = 0; column <= 4; ++column) {
      const glm::vec2 pixel{
          viewport.left + (viewport.width * static_cast<float>(column) / 4.0F),
          viewport.bottom + (viewport.height * static_cast<float>(row) / 4.0F)};
      const auto ray = unprojectToRay(transform, pixel, viewport);
      ASSERT_TRUE(ray.has_value());
      const auto hit =
          intersectPlane(*ray, pose.eye + (distance * forward), -forward);
      ASSERT_TRUE(hit.has_value());
      const auto back = projectToViewport(transform, *hit, viewport);
      ASSERT_TRUE(back.has_value());
      expectPixel(glm::vec2(*back), pixel);
    }
  }
}

TEST(SpatialUnproject, roundTripsOverPosesFieldsOfViewAndAspects) {
  for (const auto &pose : kPoses) {
    for (const float fov : kFovs) {
      for (const float aspect : kAspects) {
        SCOPED_TRACE(testing::Message() << "fov " << fov << " aspect " << aspect
                                        << " eye " << pose.eye.x);
        const Viewport viewport{0.0F, 0.0F, 600.0F * aspect, 600.0F};
        const auto transform = worldToClip(pose, fov, aspect);
        // Not nearer: a hundredth of a pixel one unit from a camera three
        // hundred units from the origin is finer than a float world point
        // can be stored there, whatever the unprojection does.
        expectRoundTrip(transform, viewport, pose, 20.0F);
        expectRoundTrip(transform, viewport, pose, 250.0F);
      }
    }
  }
}

TEST(SpatialUnproject, roundTripsNearTheFarPlane) {
  // Where a single-precision inverse would have lost the digits.
  const auto &pose     = kPoses[1];
  const auto transform = worldToClip(pose, 45.0F, 16.0F / 9.0F);
  expectRoundTrip(transform, {0.0F, 0.0F, 1920.0F, 1080.0F}, pose, 9000.0F);
}

TEST(SpatialUnproject, theRayThroughTheCentreIsTheCameraForward) {
  for (const auto &pose : kPoses) {
    const auto transform = worldToClip(pose, 45.0F, 4.0F / 3.0F);
    const Viewport viewport{0.0F, 0.0F, 800.0F, 600.0F};
    const auto ray = unprojectToRay(transform, {400.0F, 300.0F}, viewport);
    ASSERT_TRUE(ray.has_value());
    const auto forward = forwardOf(pose);
    EXPECT_NEAR(glm::dot(ray->direction, forward), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::length(ray->direction), 1.0F, 1e-5F);
    // It starts on the near plane, on the camera's axis.
    const auto fromEye = ray->origin - pose.eye;
    EXPECT_NEAR(glm::dot(fromEye, forward), render::kDefaultNearClipZ, 1e-4F);
    EXPECT_NEAR(glm::length(glm::cross(fromEye, forward)), 0.0F, 1e-4F);
  }
}

TEST(SpatialUnproject, aPointBehindTheCameraProjectsToNothing) {
  const auto &pose     = kPoses[0];
  const auto transform = worldToClip(pose, 45.0F, 1.0F);
  const Viewport viewport{0.0F, 0.0F, 512.0F, 512.0F};
  EXPECT_FALSE(projectToViewport(transform, {0.0F, 0.0F, 20.0F}, viewport));
  // On the camera plane itself, too: the divide is undefined there.
  EXPECT_FALSE(projectToViewport(transform, pose.eye, viewport));
  EXPECT_TRUE(projectToViewport(transform, {0.0F, 0.0F, 0.0F}, viewport));
}

TEST(SpatialUnproject, aSingularMatrixUnprojectsToNothing) {
  const Viewport viewport{0.0F, 0.0F, 512.0F, 512.0F};
  EXPECT_FALSE(unprojectToRay(glm::mat4(0.0F), {10.0F, 10.0F}, viewport));
  auto flattened = worldToClip(kPoses[0], 45.0F, 1.0F);
  flattened[2]   = glm::vec4(0.0F); // drops world z: rank three
  EXPECT_FALSE(unprojectToRay(flattened, {10.0F, 10.0F}, viewport));
}

TEST(SpatialUnproject, anEmptyViewportUnprojectsToNothing) {
  const auto transform = worldToClip(kPoses[0], 45.0F, 1.0F);
  EXPECT_FALSE(
      unprojectToRay(transform, {0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 512.0F}));
}

TEST(SpatialUnproject, depthRunsFromMinusOneAtNearToOneAtFar) {
  const auto &pose     = kPoses[0];
  const auto transform = worldToClip(pose, 45.0F, 1.0F);
  const Viewport viewport{0.0F, 0.0F, 100.0F, 100.0F};
  const auto forward = forwardOf(pose);
  const auto nearest = projectToViewport(
      transform, pose.eye + (render::kDefaultNearClipZ * forward), viewport);
  const auto farthest = projectToViewport(
      transform, pose.eye + (render::kDefaultFarClipZ * forward), viewport);
  ASSERT_TRUE(nearest && farthest);
  EXPECT_NEAR(nearest->z, -1.0F, 1e-4F);
  EXPECT_NEAR(farthest->z, 1.0F, 1e-3F);
}

TEST(SpatialUnproject,
     aSubViewportMatchesTheWholeTargetUnderAnOffCentreCamera) {
  // A pane's camera is built for the pane's rectangle. Seen from the whole
  // target, the same camera is an off-centre frustum; the same physical pixel
  // must give the same world ray either way.
  constexpr float targetWidth  = 1600.0F;
  constexpr float targetHeight = 900.0F;
  const Viewport pane{300.0F, 200.0F, 640.0F, 480.0F};
  const Viewport target{0.0F, 0.0F, targetWidth, targetHeight};
  const float near = render::kDefaultNearClipZ;
  const float far  = render::kDefaultFarClipZ;
  const float fov  = glm::radians(50.0F);

  for (const auto &pose : kPoses) {
    const auto view   = glm::lookAt(pose.eye, pose.target, pose.up);
    const float top   = near * std::tan(fov / 2.0F);
    const float right = top * (pane.width / pane.height);
    const auto paneProjection =
        glm::perspective(fov, pane.width / pane.height, near, far);
    // Near-plane units per pixel, the same in both directions.
    const float perPixelX      = 2.0F * right / pane.width;
    const float perPixelY      = 2.0F * top / pane.height;
    const auto wholeProjection = glm::frustum(
        -right - (pane.left * perPixelX),
        -right + ((targetWidth - pane.left) * perPixelX),
        -top - (pane.bottom * perPixelY),
        -top + ((targetHeight - pane.bottom) * perPixelY), near, far);

    for (const glm::vec2 pixel :
         {glm::vec2{300.0F, 200.0F}, glm::vec2{620.0F, 440.0F},
          glm::vec2{939.5F, 679.5F}}) {
      const auto inPane = unprojectToRay(paneProjection * view, pixel, pane);
      const auto inTarget =
          unprojectToRay(wholeProjection * view, pixel, target);
      ASSERT_TRUE(inPane && inTarget);
      EXPECT_NEAR(glm::dot(inPane->direction, inTarget->direction), 1.0F,
                  1e-6F);
      EXPECT_NEAR(glm::distance(inPane->origin, inTarget->origin), 0.0F, 1e-4F);
      // And a point projected through the pane lands on that pixel.
      const auto hit  = inPane->origin + (37.0F * inPane->direction);
      const auto back = projectToViewport(paneProjection * view, hit, pane);
      ASSERT_TRUE(back.has_value());
      expectPixel(glm::vec2(*back), pixel);
    }
  }
}

TEST(SpatialUnproject, windowCoordinatesBecomeBottomUpPixels) {
  constexpr float windowHeight = 600.0F;
  for (const float scale : {1.0F, 1.5F, 2.0F}) {
    SCOPED_TRACE(scale);
    expectPixel(windowToPixels(0.0F, 0.0F, windowHeight, scale),
                {0.0F, windowHeight * scale});
    expectPixel(windowToPixels(0.0F, windowHeight, windowHeight, scale),
                {0.0F, 0.0F});
    expectPixel(windowToPixels(100.0F, 150.0F, windowHeight, scale),
                {100.0F * scale, 450.0F * scale});
  }
  static_assert(windowToPixels(2.0F, 1.0F, 3.0F, 2.0F).y == 4.0F);
}

TEST(SpatialUnproject, aPointerUnprojectsToWhatItPointsAtAtEveryContentScale) {
  constexpr float windowWidth  = 800.0F;
  constexpr float windowHeight = 600.0F;
  const auto &pose             = kPoses[1];
  const glm::vec3 world{110.0F, -55.0F, 0.0F};
  for (const float scale : {1.0F, 1.5F, 2.0F}) {
    SCOPED_TRACE(scale);
    const Viewport drawable{0.0F, 0.0F, windowWidth * scale,
                            windowHeight * scale};
    const auto transform = worldToClip(pose, 45.0F, windowWidth / windowHeight);
    const auto pixel     = projectToViewport(transform, world, drawable);
    ASSERT_TRUE(pixel.has_value());
    // Where a pointer over that point is reported: window units, top-down.
    const glm::vec2 pointer{pixel->x / scale,
                            windowHeight - (pixel->y / scale)};
    const auto ray = unprojectToRay(
        transform, windowToPixels(pointer.x, pointer.y, windowHeight, scale),
        drawable);
    ASSERT_TRUE(ray.has_value());
    const auto hit = intersectPlane(*ray, {0.0F, 0.0F, 0.0F}, {0, 0, 1});
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(glm::distance(*hit, world), 0.0F, 1e-3F);
  }
}

TEST(SpatialUnproject, agreesWithProjectToScreenInFrontOfTheCamera) {
  const auto &pose     = kPoses[2];
  const auto transform = worldToClip(pose, 60.0F, 16.0F / 9.0F);
  const Viewport viewport{0.0F, 0.0F, 1280.0F, 720.0F};
  const auto forward = forwardOf(pose);
  for (const float distance : {0.5F, 10.0F, 400.0F, 8000.0F}) {
    for (const glm::vec3 sideways :
         {glm::vec3{0.0F}, glm::vec3{30.0F, 0.0F, 0.0F},
          glm::vec3{-500.0F, 200.0F, 0.0F}}) {
      const auto world     = pose.eye + (distance * forward) + sideways;
      const auto projected = projectToViewport(transform, world, viewport);
      if (!projected) {
        continue;
      }
      expectPixel(
          glm::vec2(*projected),
          projectToScreen(transform, world, viewport.width, viewport.height));
    }
  }
}

TEST(SpatialRay, intersectPlaneRefusesParallelAndBehind) {
  const Ray ray{{0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, -1.0F}};
  const auto hit = intersectPlane(ray, {0.0F, 0.0F, 2.0F}, {0.0F, 0.0F, 3.0F});
  ASSERT_TRUE(hit.has_value());
  EXPECT_NEAR(hit->z, 2.0F, 1e-6F);
  EXPECT_FALSE(intersectPlane(ray, {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}));
  EXPECT_FALSE(intersectPlane(ray, {0.0F, 0.0F, 20.0F}, {0.0F, 0.0F, 1.0F}));
  EXPECT_FALSE(intersectPlane(ray, {0.0F, 0.0F, 0.0F}, glm::vec3{0.0F}));
}

TEST(SpatialRay, intersectQuadAnswersInThePlanesOwnCoordinates) {
  // A plane turned to face +x, moved and scaled by two: its own units are
  // twice the world's.
  auto planeToWorld = glm::translate(glm::mat4(1.0F), {50.0F, 4.0F, -6.0F});
  planeToWorld =
      glm::rotate(planeToWorld, std::numbers::pi_v<float> / 2.0F, {0, 1, 0});
  planeToWorld = glm::scale(planeToWorld, {2.0F, 2.0F, 2.0F});

  const Ray hits{{0.0F, 10.0F, -12.0F}, {1.0F, 0.0F, 0.0F}};
  const auto local = intersectQuad(hits, planeToWorld, 5.0F, 5.0F);
  ASSERT_TRUE(local.has_value());
  // World (50, 10, -12) is 6 up and 6 along the plane's x (which, turned, is
  // world -z): three of the plane's own units each way.
  EXPECT_NEAR(local->x, 3.0F, 1e-5F);
  EXPECT_NEAR(local->y, 3.0F, 1e-5F);

  EXPECT_FALSE(intersectQuad(hits, planeToWorld, 2.0F, 5.0F)); // misses wide
  const Ray away{{0.0F, 10.0F, -12.0F}, {-1.0F, 0.0F, 0.0F}};
  EXPECT_FALSE(intersectQuad(away, planeToWorld, 5.0F, 5.0F)); // behind
  const Ray along{{0.0F, 4.0F, -6.0F}, {0.0F, 0.0F, 1.0F}};
  EXPECT_FALSE(intersectQuad(along, planeToWorld, 5.0F, 5.0F));   // parallel
  EXPECT_FALSE(intersectQuad(hits, glm::mat4(0.0F), 5.0F, 5.0F)); // singular
}

/// The distance by brute force, to check the closed form against.
float sampledDistance(const Ray &ray, const glm::vec3 &from,
                      const glm::vec3 &to) {
  float best          = std::numeric_limits<float>::infinity();
  constexpr int steps = 400;
  for (int i = 0; i <= steps; ++i) {
    const auto onSpan = from + ((to - from) * (static_cast<float>(i) / steps));
    // The nearest point of the ray to a point is a closed form of its own.
    const float along =
        std::max(0.0F, glm::dot(onSpan - ray.origin, ray.direction));
    best = std::min(
        best, glm::distance(onSpan, ray.origin + (along * ray.direction)));
  }
  return best;
}

TEST(SpatialRay, distanceToSegmentMatchesBruteForce) {
  const Ray ray{{0.0F, 0.0F, 0.0F},
                glm::normalize(glm::vec3{1.0F, 0.2F, 0.0F})};
  struct Segment {
    glm::vec3 from, to;
  };
  const std::array<Segment, 6> segments{{
      {{5.0F, -3.0F, 0.0F}, {5.0F, 3.0F, 0.0F}},   // crosses the ray
      {{5.0F, -3.0F, 2.0F}, {5.0F, 3.0F, 2.0F}},   // passes two above it
      {{0.0F, 4.0F, 0.0F}, {10.0F, 6.0F, 0.0F}},   // parallel to it
      {{-6.0F, -1.0F, 0.0F}, {-2.0F, 1.0F, 1.0F}}, // behind its origin
      {{8.0F, 8.0F, 8.0F}, {8.0F, 8.0F, 8.0F}},    // a point
      {{3.0F, 2.0F, -1.0F}, {20.0F, 9.0F, 4.0F}},  // skew
  }};
  for (const auto &segment : segments) {
    EXPECT_NEAR(distanceToSegment(ray, segment.from, segment.to),
                sampledDistance(ray, segment.from, segment.to), 0.02F);
  }
  EXPECT_NEAR(distanceToSegment(ray, segments[0].from, segments[0].to), 0.0F,
              1e-5F);
  EXPECT_NEAR(distanceToSegment(ray, segments[1].from, segments[1].to), 2.0F,
              1e-5F);
}

} // namespace
