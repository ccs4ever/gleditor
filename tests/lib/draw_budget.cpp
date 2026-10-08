/**
 * @file draw_budget.cpp
 * @brief Tests for the two decisions taken before a page is drawn: whether it
 *        is in view at all, and whether it is large enough to be worth glyphs.
 *
 * Worth testing directly rather than through a rendered frame: a sign error in
 * either makes pages vanish or turn to bars, and a frame that is missing a page
 * it should have drawn looks exactly like a frame of a document that is shorter
 * than you thought.
 */
#include <gtest/gtest.h>

#include <gleditor/draw_budget.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace {

/// A camera looking down -Z from z = distance, matching the editor's.
glm::mat4 viewProjection(const float distance, const float fovDegrees = 45.0F) {
  const auto projection = glm::perspective(glm::radians(fovDegrees),
                                           800.0F / 600.0F, 0.1F, 10000.0F);
  const auto camera =
      glm::lookAt(glm::vec3(0.0F, 0.0F, distance), glm::vec3(0.0F, 0.0F, 0.0F),
                  glm::vec3(0.0F, 1.0F, 0.0F));
  return projection * camera;
}

/// Half extents of a page-sized box, in the same units the tests place it in.
constexpr float halfW = 40.0F;
constexpr float halfH = 30.0F;
constexpr float depth = 0.1F;

} // namespace

TEST(Frustum, aBoxUnderTheCameraIsInView) {
  EXPECT_FALSE(outsideFrustum(viewProjection(200.0F), halfW, halfH, depth));
}

TEST(Frustum, aBoxFarToOneSideIsRejected) {
  const auto base = viewProjection(200.0F);
  for (const float offset : {-4000.0F, 4000.0F}) {
    const auto mvp =
        base * glm::translate(glm::mat4(1.0F), glm::vec3(offset, 0.0F, 0.0F));
    EXPECT_TRUE(outsideFrustum(mvp, halfW, halfH, depth)) << "x " << offset;
  }
  for (const float offset : {-4000.0F, 4000.0F}) {
    const auto mvp =
        base * glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, offset, 0.0F));
    EXPECT_TRUE(outsideFrustum(mvp, halfW, halfH, depth)) << "y " << offset;
  }
}

// The conservative direction. A box crossing the edge of the screen has corners
// outside one plane and corners inside it, and drawing it is the only right
// answer -- rejecting it would clip the page at the window edge.
TEST(Frustum, aBoxStraddlingAnEdgeIsKept) {
  const auto base = viewProjection(200.0F);
  // Far enough out that the box's centre is off screen but its near edge is
  // not.
  for (const float offset : {-95.0F, 95.0F}) {
    const auto mvp =
        base * glm::translate(glm::mat4(1.0F), glm::vec3(offset, 0.0F, 0.0F));
    EXPECT_FALSE(outsideFrustum(mvp, halfW, halfH, depth)) << "x " << offset;
  }
}

// A box behind the camera has corners with negative w, where dividing through
// would flip the comparison and could keep something invisible or, worse, cull
// something visible in front of it.
TEST(Frustum, aBoxBehindTheCameraIsNotMistakenForOneInFront) {
  const auto base = viewProjection(200.0F);
  const auto behind =
      base * glm::translate(glm::mat4(1.0F), glm::vec3(3000.0F, 0.0F, 400.0F));
  EXPECT_TRUE(outsideFrustum(behind, halfW, halfH, depth));
}

// The whole point of culling a long document: only a few of its pages can be on
// screen, and the rest have to be rejected. Pages are stacked 100 apart.
TEST(Frustum, mostPagesOfALongDocumentAreRejected) {
  const auto base = viewProjection(200.0F);
  int kept        = 0;
  for (int page = 0; page < 1000; page++) {
    const auto mvp =
        base *
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -100.0F * page, 0.0F));
    if (!outsideFrustum(mvp, halfW, halfH, depth)) {
      kept++;
    }
  }
  EXPECT_GT(kept, 0) << "culling everything would draw an empty frame";
  EXPECT_LT(kept, 10) << "kept " << kept << " of 1000 pages";
}

namespace {

/// A box turned edge-on to the camera, so its own y runs along the view axis
/// and its corners lie at two depths.
glm::mat4 edgeOnAt(const float z) {
  return glm::rotate(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, z)),
                     glm::radians(90.0F), glm::vec3(1.0F, 0.0F, 0.0F));
}

} // namespace

TEST(InsideFrustum, aBoxWhollyInViewIsInside) {
  EXPECT_TRUE(insideFrustum(viewProjection(200.0F), halfW, halfH));
  // Turned to the view, too, so long as both of its depths are in range.
  EXPECT_TRUE(
      insideFrustum(viewProjection(200.0F) * edgeOnAt(0.0F), halfW, halfH));
}

// At 200 units the view reaches about 110 to each side and 83 up and down;
// each offset puts one edge of the box over one of the four side planes.
TEST(InsideFrustum, aBoxStraddlingASidePlaneIsNotInside) {
  const auto base = viewProjection(200.0F);
  for (const glm::vec3 offset :
       {glm::vec3(-95.0F, 0.0F, 0.0F), glm::vec3(95.0F, 0.0F, 0.0F),
        glm::vec3(0.0F, -70.0F, 0.0F), glm::vec3(0.0F, 70.0F, 0.0F)}) {
    const auto mvp = base * glm::translate(glm::mat4(1.0F), offset);
    EXPECT_FALSE(insideFrustum(mvp, halfW, halfH))
        << "offset " << offset.x << ", " << offset.y;
    // Straddling, so not outside either: the two tests are not complements.
    EXPECT_FALSE(outsideFrustum(mvp, halfW, halfH, depth));
  }
}

// The near plane is 0.1 in front of the camera at z = 200. A sliver centred on
// it, narrow enough to be inside the side planes that close, is cut by it.
TEST(InsideFrustum, aBoxStraddlingTheNearPlaneIsNotInside) {
  const auto base         = viewProjection(200.0F);
  constexpr float sliverW = 0.01F;
  constexpr float sliverH = 0.05F;
  EXPECT_FALSE(insideFrustum(base * edgeOnAt(199.9F), sliverW, sliverH));
  // The same sliver a little further away is wholly inside.
  EXPECT_TRUE(insideFrustum(base * edgeOnAt(199.5F), sliverW, sliverH));
}

// The far plane is 10,000 in front of the camera, at z = -9800.
TEST(InsideFrustum, aBoxStraddlingTheFarPlaneIsNotInside) {
  const auto base = viewProjection(200.0F);
  EXPECT_FALSE(insideFrustum(base * edgeOnAt(-9800.0F), 10.0F, 100.0F));
  EXPECT_TRUE(insideFrustum(base * edgeOnAt(-9600.0F), 10.0F, 100.0F));
}

TEST(InsideFrustum, aBoxWhollyOutsideIsNotInside) {
  const auto base = viewProjection(200.0F);
  EXPECT_FALSE(insideFrustum(
      base * glm::translate(glm::mat4(1.0F), glm::vec3(4000.0F, 0.0F, 0.0F)),
      halfW, halfH));
  EXPECT_FALSE(insideFrustum(
      base * glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -20000.0F)),
      halfW, halfH));
}

// Behind the camera every corner has a negative w, which would flip every
// comparison after a divide; the test must not read that as inside.
TEST(InsideFrustum, aBoxBehindTheCameraIsNotInside) {
  const auto base = viewProjection(200.0F);
  EXPECT_FALSE(insideFrustum(
      base * glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 400.0F)),
      halfW, halfH));
}

TEST(ScreenScale, movingTheCameraBackShrinksThings) {
  const auto near = screenScaleAt(viewProjection(200.0F), 800.0F);
  const auto far  = screenScaleAt(viewProjection(2000.0F), 800.0F);
  EXPECT_GT(near, far);
  // Ten times the distance is a tenth the size, under a perspective projection.
  EXPECT_NEAR(near / far, 10.0F, 0.1F);
}

TEST(ScreenScale, aWiderDrawableGivesEachUnitMorePixels) {
  const auto mvp = viewProjection(200.0F);
  EXPECT_NEAR(screenScaleAt(mvp, 1600.0F), 2.0F * screenScaleAt(mvp, 800.0F),
              1e-3F);
}

// Nothing sensible can be said about the size of something on the camera plane,
// and dividing by its w would be a divide by zero. Reading it as very close
// means the fallback is full detail, which is never wrong to look at.
TEST(ScreenScale, somethingOnTheCameraPlaneReadsAsVeryClose) {
  // The box sits exactly where the camera is, so its centre has a clip w of
  // zero and the perspective divide has nothing to say.
  const auto mvp =
      viewProjection(200.0F) *
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 200.0F));
  EXPECT_GT(screenScaleAt(mvp, 800.0F), 1e6F);
}
