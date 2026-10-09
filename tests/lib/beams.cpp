/**
 * @file beams.cpp
 * @brief The record and the geometry a beam is drawn from.
 *
 * A beam is the second thing this library draws, and the first that is not an
 * axis-aligned quad. As with Doc::VBORow, the shader unpacks a layout written
 * out again in GLSL and nothing links the two, so the parts that can drift --
 * the field offsets, the vertex-index convention, the kind the picking tag
 * carries -- are written out here as the shader does them and checked.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gmock/gmock.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

#include "headless_device.hpp"
#include "mocks/device.hpp"

using gleditor::Beams;
using testing::_;
using testing::NiceMock;
using testing::Return;

namespace {

/// A device that keeps what was written to its one buffer, so a test can read
/// back the rows a commit produced.
class RecordingDevice : public NiceMock<MockRenderDevice> {
public:
  std::vector<std::byte> contents;

  RecordingDevice() {
    ON_CALL(*this, createBuffer)
        .WillByDefault(
            [this](const render::BufferKind, const std::size_t bytes) {
              contents.assign(bytes, std::byte{});
              return render::BufferHandle{1};
            });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault(
            [this](const render::BufferHandle, const std::size_t bytes) {
              contents.resize(bytes, std::byte{});
              return render::BufferHandle{1};
            });
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](const render::BufferHandle,
                              const std::size_t offset,
                              const std::span<const std::byte> data) {
          if (offset + data.size() > contents.size()) {
            contents.resize(offset + data.size(), std::byte{});
          }
          std::memcpy(contents.data() + offset, data.data(), data.size());
        });
  }

  /// The row at @p byteOffset, as the vertex stage would read it.
  [[nodiscard]] Beams::Row rowAt(const std::size_t byteOffset) const {
    Beams::Row row{};
    std::memcpy(&row, contents.data() + byteOffset, sizeof(row));
    return row;
  }
};

/// The camera ray through @p point, from the combined matrix alone, written
/// out as viewRayThrough() in assets/shaders/beam.vert.glsl does it.
glm::vec3 viewRayThrough(const glm::mat4 &mvp, const glm::vec3 &point) {
  const glm::vec4 clip = mvp * glm::vec4(point, 1.0F);
  const glm::vec3 rowX{mvp[0][0], mvp[1][0], mvp[2][0]};
  const glm::vec3 rowY{mvp[0][1], mvp[1][1], mvp[2][1]};
  const glm::vec3 rowW{mvp[0][3], mvp[1][3], mvp[2][3]};
  return glm::cross((clip.w * rowX) - (clip.x * rowW),
                    (clip.w * rowY) - (clip.y * rowW));
}

/// What the vertex stage builds from a beam, a vertex index and the matrix it
/// is drawn with, written out as assets/shaders/beam.vert.glsl does it.
glm::vec3 cornerOf(const Beams::Row &row, const int corner,
                   const glm::mat4 &mvp) {
  const glm::vec3 from{row.from[0], row.from[1], row.from[2]};
  const glm::vec3 to{row.to[0], row.to[1], row.to[2]};
  const float along  = (0 != (corner & 2)) ? 1.0F : 0.0F;
  const float across = (0 != (corner & 1)) ? 1.0F : -1.0F;

  const auto run = glm::normalize(to - from);
  auto sideways  = glm::cross(
      run, glm::normalize(viewRayThrough(mvp, glm::mix(from, to, 0.5F))));
  if (glm::dot(sideways, sideways) < 1e-6F) {
    const glm::vec3 other = std::abs(run.z) < 0.9F
                                ? glm::vec3(0.0F, 0.0F, 1.0F)
                                : glm::vec3(1.0F, 0.0F, 0.0F);
    sideways              = glm::cross(run, other);
  }
  const auto offset = glm::normalize(sideways) * (row.width * 0.5F) * across;
  return glm::mix(from, to, along) + offset;
}

/// A camera at @p eye looking at the origin, as the renderer builds one.
glm::mat4 lookingAtOrigin(const glm::vec3 &eye) {
  return glm::perspective(glm::radians(45.0F), 4.0F / 3.0F, 0.1F, 1000.0F) *
         glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
}

/// Head on: on the +z axis, looking down -z at the plane the pages lie in.
const glm::mat4 headOn = lookingAtOrigin({0.0F, 0.0F, 50.0F});

/**
 * @brief Four cameras round the origin, none of them end on to a world axis:
 *        in front and to the side, behind and below, above, and low to the
 *        right. Azimuth from +z towards +x, then elevation, in degrees.
 */
constexpr std::array<std::array<float, 2>, 4> oblique{
    {{30.0F, 20.0F}, {150.0F, -25.0F}, {250.0F, 60.0F}, {300.0F, 5.0F}}};

glm::vec3 orbit(const std::array<float, 2> &angles, const float radius) {
  const float azimuth   = glm::radians(angles[0]);
  const float elevation = glm::radians(angles[1]);
  return {radius * std::cos(elevation) * std::sin(azimuth),
          radius * std::sin(elevation),
          radius * std::cos(elevation) * std::cos(azimuth)};
}

/// Where @p point lands, in pixels of a 640 by 480 target.
glm::vec2 onScreen(const glm::mat4 &mvp, const glm::vec3 &point) {
  const glm::vec4 clip = mvp * glm::vec4(point, 1.0F);
  return {((clip.x / clip.w) * 0.5F + 0.5F) * 640.0F,
          ((clip.y / clip.w) * 0.5F + 0.5F) * 480.0F};
}

/// Area of the drawn ribbon on screen, in square pixels.
float screenArea(const Beams::Row &row, const glm::mat4 &mvp) {
  std::array<glm::vec2, 4> c{};
  for (int corner = 0; corner < 4; ++corner) {
    c[static_cast<std::size_t>(corner)] =
        onScreen(mvp, cornerOf(row, corner, mvp));
  }
  // The strip's quad is 0, 1, 3, 2 going round.
  const std::array<glm::vec2, 4> ring{c[0], c[1], c[3], c[2]};
  float twice = 0.0F;
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const auto &a = ring[i];
    const auto &b = ring[(i + 1) % ring.size()];
    twice += (a.x * b.y) - (b.x * a.y);
  }
  return std::abs(twice) * 0.5F;
}

class BeamsTest : public testing::Test {
protected:
  std::unique_ptr<RecordingDevice> device;

  void SetUp() override { device = std::make_unique<RecordingDevice>(); }
};

} // namespace

TEST_F(BeamsTest, theRecordIsWhatTheShaderDeclares) {
  using Row = Beams::Row;
  EXPECT_EQ(sizeof(Row), 44U);

  const auto layout = Beams::layout();
  EXPECT_EQ(layout.stride, sizeof(Row));
  ASSERT_EQ(layout.attributes.size(), 6U);
  EXPECT_EQ(layout.attributes[0].offset, offsetof(Row, from));
  EXPECT_EQ(layout.attributes[1].offset, offsetof(Row, width));
  EXPECT_EQ(layout.attributes[2].offset, offsetof(Row, to));
  EXPECT_EQ(layout.attributes[3].offset, offsetof(Row, colour));
  EXPECT_EQ(layout.attributes[4].offset, offsetof(Row, tag));
  EXPECT_EQ(layout.attributes[5].offset, offsetof(Row, along));
  for (std::uint32_t i = 0; i < layout.attributes.size(); i++) {
    EXPECT_EQ(layout.attributes[i].location, i);
  }
  // Three floats then one, twice over, then a pair: a compiler that padded
  // between them would leave the shader reading the wrong words with no
  // complaint from anybody.
  EXPECT_EQ(offsetof(Row, width), 12U);
  EXPECT_EQ(offsetof(Row, to), 16U);
  EXPECT_EQ(offsetof(Row, colour), 28U);
  EXPECT_EQ(offsetof(Row, tag), 32U);
  EXPECT_EQ(offsetof(Row, along), 36U);
}

// A beam added on its own runs the whole route it belongs to. The fade along
// a beam's length is the only thing saying which way the link points, and a
// beam that arrived claiming to be the middle third of something would draw
// that fade wrong.
TEST_F(BeamsTest, aBeamAddedOnItsOwnCoversTheWholeRoute) {
  Beams beams(device.get(), 4);
  beams.add({0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 1.0F, 0xFFFFFFFF, 0);
  beams.commit();

  const auto row = device->rowAt(0);
  EXPECT_FLOAT_EQ(row.along[0], 0.0F);
  EXPECT_FLOAT_EQ(row.along[1], 1.0F);
}

// A route that bends is still one beam as far as the fade is concerned: the
// segments carry their own share of the whole, by arc length, and the shares
// join up. Splitting the fade per segment instead restarted it at every
// corner, which said "this way" three times over for one link.
TEST_F(BeamsTest, aPathSharesOneFadeOutAmongItsSegments) {
  Beams beams(device.get(), 8);
  // Ten units, then thirty: the first segment is a quarter of the route and
  // has to be given a quarter of the fade, not half of it.
  const std::array<glm::vec3, 3> route{glm::vec3{0.0F, 0.0F, 0.0F},
                                       glm::vec3{10.0F, 0.0F, 0.0F},
                                       glm::vec3{40.0F, 0.0F, 0.0F}};
  beams.addPath(route, 1.0F, 0xABCDEF12, 5);
  beams.commit();
  ASSERT_EQ(beams.committed(), 2U);

  const auto first  = device->rowAt(0);
  const auto second = device->rowAt(sizeof(Beams::Row));
  EXPECT_FLOAT_EQ(first.along[0], 0.0F);
  EXPECT_FLOAT_EQ(first.along[1], 0.25F);
  // No gap and no overlap where they meet, which is what makes the fade
  // continuous across the joint.
  EXPECT_FLOAT_EQ(second.along[0], first.along[1]);
  EXPECT_FLOAT_EQ(second.along[1], 1.0F);

  // And the rest of the record is the same beam either side of the corner.
  EXPECT_EQ(first.colour, 0xABCDEF12U);
  EXPECT_EQ(second.colour, 0xABCDEF12U);
  EXPECT_EQ(second.tag, 5U);
  EXPECT_FLOAT_EQ(first.to[0], second.from[0]);
}

// Degenerate routes add nothing rather than dividing by a length of zero.
TEST_F(BeamsTest, aPathThatGoesNowhereAddsNothing) {
  Beams beams(device.get(), 4);
  const std::array<glm::vec3, 1> alone{glm::vec3{1.0F, 2.0F, 3.0F}};
  beams.addPath(alone, 1.0F, 0xFFFFFFFF, 0);
  EXPECT_EQ(beams.pending(), 0U);

  const std::array<glm::vec3, 3> stuck{glm::vec3{1.0F, 2.0F, 3.0F},
                                       glm::vec3{1.0F, 2.0F, 3.0F},
                                       glm::vec3{1.0F, 2.0F, 3.0F}};
  beams.addPath(stuck, 1.0F, 0xFFFFFFFF, 0);
  EXPECT_EQ(beams.pending(), 0U);
}

TEST_F(BeamsTest, addedBeamsReachTheBufferInOrder) {
  Beams beams(device.get(), 8);
  EXPECT_EQ(beams.pending(), 0U);
  EXPECT_EQ(beams.committed(), 0U);

  beams.add({1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}, 0.5F, 0xFF0000FF, 7);
  beams.add({-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 2.0F, 0x00FF00FF, 9);
  EXPECT_EQ(beams.pending(), 2U);
  // Nothing is on the device until it is committed, which is what lets a
  // caller rebuild a batch part way through a frame's worth of decisions.
  EXPECT_EQ(beams.committed(), 0U);

  beams.commit();
  EXPECT_EQ(beams.committed(), 2U);

  const auto first = device->rowAt(0);
  EXPECT_FLOAT_EQ(first.from[0], 1.0F);
  EXPECT_FLOAT_EQ(first.to[2], 6.0F);
  EXPECT_FLOAT_EQ(first.width, 0.5F);
  EXPECT_EQ(first.colour, 0xFF0000FFU);
  EXPECT_EQ(first.tag, 7U);

  const auto second = device->rowAt(sizeof(Beams::Row));
  EXPECT_FLOAT_EQ(second.from[0], -1.0F);
  EXPECT_EQ(second.tag, 9U);
}

// A batch is rebuilt rather than edited, so the storage has to survive being
// filled, emptied and filled again -- and a shorter batch must not leave the
// tail of the last one being drawn.
TEST_F(BeamsTest, rebuildingReplacesWhatWasThereBefore) {
  Beams beams(device.get(), 8);
  for (int i = 0; i < 5; i++) {
    beams.add({0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 1.0F, 0xFFFFFFFF,
              static_cast<std::uint32_t>(i));
  }
  beams.commit();
  EXPECT_EQ(beams.committed(), 5U);

  beams.clear();
  EXPECT_EQ(beams.pending(), 0U);
  beams.add({0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, 1.0F, 0xFFFFFFFF, 42);
  beams.commit();
  EXPECT_EQ(beams.committed(), 1U);

  beams.clear();
  beams.commit();
  EXPECT_EQ(beams.committed(), 0U);
}

TEST_F(BeamsTest, moreBeamsThanTheStorageHoldsGrowIt) {
  Beams beams(device.get(), 4);
  for (int i = 0; i < 200; i++) {
    beams.add({0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, 1.0F, 0xFFFFFFFF,
              static_cast<std::uint32_t>(i));
  }
  beams.commit();
  EXPECT_EQ(beams.committed(), 200U);
  EXPECT_GE(device->contents.size(), 200U * sizeof(Beams::Row));
  EXPECT_EQ(device->rowAt(199 * sizeof(Beams::Row)).tag, 199U);
}

// The four corners the vertex index picks have to come out as a rectangle
// centred on the segment, in the order a triangle strip wants: 0 and 1 at one
// end, 2 and 3 at the other. Swapping the two bits would draw a bow tie.
TEST_F(BeamsTest, theFourCornersAreARectangleAlongTheRun) {
  const Beams::Row row{{-5.0F, 0.0F, 0.0F}, 2.0F, {5.0F, 0.0F, 0.0F}, 0, 0};

  const auto nearLeft  = cornerOf(row, 0, headOn);
  const auto nearRight = cornerOf(row, 1, headOn);
  const auto farLeft   = cornerOf(row, 2, headOn);
  const auto farRight  = cornerOf(row, 3, headOn);

  // The first two sit at the near end and the last two at the far end.
  EXPECT_FLOAT_EQ(nearLeft.x, -5.0F);
  EXPECT_FLOAT_EQ(nearRight.x, -5.0F);
  EXPECT_FLOAT_EQ(farLeft.x, 5.0F);
  EXPECT_FLOAT_EQ(farRight.x, 5.0F);

  // Half the width to each side of the run, and the two ends offset the same
  // way, so the quad has parallel sides.
  EXPECT_FLOAT_EQ(glm::distance(nearLeft, nearRight), row.width);
  EXPECT_FLOAT_EQ(glm::distance(farLeft, farRight), row.width);
  EXPECT_FLOAT_EQ(nearLeft.y, farLeft.y);
  EXPECT_FLOAT_EQ(nearRight.y, farRight.y);
}

// Head on, a beam centred in the view lies in the plane of the pages, as every
// beam did under the old rule (run x z): turning to face the camera changes
// nothing for a reader looking straight at the pages.
TEST_F(BeamsTest, headOnABeamLiesInThePlaneOfThePages) {
  for (const auto &to :
       {glm::vec3{5.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 5.0F, 0.0F},
        glm::vec3{3.0F, 4.0F, 0.0F}}) {
    const Beams::Row row{{-to.x, -to.y, 0.0F}, 1.5F, {to.x, to.y, 0.0F}, 0, 0};
    for (int corner = 0; corner < 4; ++corner) {
      EXPECT_NEAR(cornerOf(row, corner, headOn).z, 0.0F, 1e-5F);
    }
  }
}

// The width is measured across the beam whichever way it runs and from
// wherever it is seen, which is what says the offset is perpendicular to the
// run rather than along an axis.
TEST_F(BeamsTest, theWidthIsAcrossTheBeamAtAnyAngle) {
  for (const auto &angles : oblique) {
    const auto mvp = lookingAtOrigin(orbit(angles, 60.0F));
    for (const auto &to :
         {glm::vec3{5.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 5.0F, 0.0F},
          glm::vec3{0.0F, 0.0F, 5.0F}, glm::vec3{-2.0F, 7.0F, 3.0F}}) {
      const Beams::Row row{{0.0F, 0.0F, 0.0F}, 1.5F, {to.x, to.y, to.z}, 0, 0};
      const auto left  = cornerOf(row, 0, mvp);
      const auto right = cornerOf(row, 1, mvp);
      EXPECT_NEAR(glm::distance(left, right), row.width, 1e-4F);
      EXPECT_NEAR(glm::dot(right - left, glm::normalize(to)), 0.0F, 1e-4F);
    }
  }
}

// The ribbon turns about its run to face the camera: across it is
// perpendicular to the camera ray, so its full width is seen.
TEST_F(BeamsTest, theRibbonFacesTheCamera) {
  for (const auto &angles : oblique) {
    const auto eye = orbit(angles, 60.0F);
    const auto mvp = lookingAtOrigin(eye);
    const Beams::Row row{{0.0F, 0.0F, -5.0F}, 1.0F, {0.0F, 0.0F, 5.0F}, 0, 0};
    const auto across = cornerOf(row, 1, mvp) - cornerOf(row, 0, mvp);
    // The midpoint is the origin, so the ray to it runs from the eye.
    EXPECT_NEAR(glm::dot(glm::normalize(across), glm::normalize(eye)), 0.0F,
                1e-4F);
  }
}

// F3: under the old rule a beam along z had no width at any angle, and one
// along x or y vanished edge on. Now a beam along each world axis covers the
// screen from every one of the four cameras.
TEST_F(BeamsTest, aBeamAlongEachAxisHasAreaFromEveryAngle) {
  for (const auto &axis :
       {glm::vec3{1.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 1.0F, 0.0F},
        glm::vec3{0.0F, 0.0F, 1.0F}}) {
    const auto from = axis * -20.0F;
    const auto to   = axis * 20.0F;
    const Beams::Row row{
        {from.x, from.y, from.z}, 2.0F, {to.x, to.y, to.z}, 0, 0};
    for (const auto &angles : oblique) {
      EXPECT_GT(screenArea(row, lookingAtOrigin(orbit(angles, 150.0F))), 50.0F)
          << "axis " << axis.x << axis.y << axis.z << " from " << angles[0]
          << "," << angles[1];
    }
  }
}

// Exactly end on, a ribbon is a line whichever way it turns: the fallback
// keeps the arithmetic finite, and nothing is drawn. A cap for that case is
// outside this rule (design/view-system-implementation-plan.md, R3).
TEST_F(BeamsTest, exactlyEndOnABeamIsALine) {
  const Beams::Row row{{0.0F, 0.0F, -20.0F}, 2.0F, {0.0F, 0.0F, 20.0F}, 0, 0};
  for (int corner = 0; corner < 4; ++corner) {
    const auto at = cornerOf(row, corner, headOn);
    EXPECT_TRUE(std::isfinite(at.x) && std::isfinite(at.y));
  }
  EXPECT_NEAR(screenArea(row, headOn), 0.0F, 1e-2F);
}

// Vulkan rewrites the matrix it draws with -- y negated, z remapped -- and the
// ray is taken from that matrix. Only the sign of across changes, so the same
// four corners are drawn, each pair swapped.
TEST_F(BeamsTest, vulkansRewrittenMatrixDrawsTheSameRibbon) {
  const auto mvp      = lookingAtOrigin(orbit(oblique[0], 60.0F));
  glm::mat4 rewritten = mvp;
  for (int column = 0; column < 4; ++column) {
    rewritten[column][1] = -mvp[column][1];
    rewritten[column][2] = (mvp[column][3] - mvp[column][2]) * 0.5F;
  }
  const Beams::Row row{{1.0F, -2.0F, -4.0F}, 1.0F, {-3.0F, 2.0F, 6.0F}, 0, 0};
  for (int corner = 0; corner < 4; ++corner) {
    const auto a = cornerOf(row, corner, mvp);
    const auto b = cornerOf(row, corner ^ 1, rewritten);
    EXPECT_NEAR(glm::distance(a, b), 0.0F, 1e-4F) << "corner " << corner;
  }
}

// Beams are picked, which is how a link is traversed by clicking it, so the
// kind a beam reports has to survive the identity word's four bits and come
// back as a beam rather than as a page or a glyph.
TEST_F(BeamsTest, aPickedBeamComesBackAsABeam) {
  constexpr std::uint32_t kindShift = render::tagDocBits + render::tagPageBits;
  EXPECT_LT(render::tagKindBeam, 1U << render::tagKindBits);

  const auto base      = render::packTagIdentity(0, 3, 11);
  const auto assembled = base | (render::tagKindBeam << kindShift);
  const auto tag       = render::unpackPickingTag(assembled, 88, 0);
  EXPECT_EQ(tag.kind, render::tagKindBeam);
  EXPECT_EQ(tag.docIndex, 3U);
  EXPECT_EQ(tag.pageIndex, 11U);
  // The beam's own tag rides in the cluster word, so a caller can tell which of
  // its beams was clicked.
  EXPECT_EQ(tag.clusterIndex, 88U);
  // And it is not any of the kinds a quad can claim, so nothing can confuse the
  // two.
  EXPECT_NE(tag.kind, render::tagKindGlyph);
  EXPECT_NE(tag.kind, render::tagKindPage);
  EXPECT_NE(tag.kind, render::tagKindOverlay);
}

// Without a pipeline there is nothing to draw with, which happens when the
// shaders could not be read. A frame that hits that must still be a frame.
TEST_F(BeamsTest, drawingWithoutAPipelineIsQuiet) {
  Beams beams(device.get(), 4);
  EXPECT_FALSE(beams.ready());
  beams.add({0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 1.0F, 0xFFFFFFFF, 1);
  beams.commit();
  EXPECT_CALL(*device, drawGlyphs(_, _, _, _)).Times(0);
}

namespace {

class BeamsDrawnTest : public testing::TestWithParam<render::Backend> {};

/// Pixels where the red beam clearly dominates the black clear colour.
int litPixels(const render::FrameImage &image) {
  int count = 0;
  for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    if (image.rgba[i] - std::max(image.rgba[i + 1], image.rgba[i + 2]) > 40) {
      ++count;
    }
  }
  return count;
}

} // namespace

// The gate for F3 on the device, not only in the arithmetic above: a beam
// along each world axis is drawn from each of the four cameras.
TEST_P(BeamsDrawnTest, aBeamAlongEachAxisIsDrawnFromEveryAngle) {
  auto opened = headless::open(GetParam(), 640, 480);
  if (!opened) {
    GTEST_SKIP() << opened.error();
  }
  auto &device = *(*opened)->device;
  {
    RenderState state(&device);
    const auto shaders = gleditor::assetPath("shaders");
    for (const auto &axis :
         {glm::vec3{1.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 1.0F, 0.0F},
          glm::vec3{0.0F, 0.0F, 1.0F}}) {
      Beams beam(&device);
      beam.createPipeline(shaders, shaders + "/vulkan", true);
      beam.add(axis * -30.0F, axis * 30.0F, 3.0F, 0xFF2020FFU, 1);
      beam.commit();
      for (const auto &angles : oblique) {
        render::FrameImage image;
        // Twice, so the capture follows a frame already drawn.
        for (int frame = 0; frame < 2; ++frame) {
          ASSERT_TRUE(device.beginFrame());
          device.setHighlights({});
          beam.draw(state, lookingAtOrigin(orbit(angles, 150.0F)), 1.0F, 0);
          device.endFrame();
          device.waitIdle();
          image = device.captureColorTarget();
        }
        EXPECT_GT(litPixels(image), 100)
            << render::backendName(GetParam()) << " axis " << axis.x << axis.y
            << axis.z << " from " << angles[0] << "," << angles[1];
      }
      device.waitIdle();
    }
    EXPECT_TRUE(device.takeDiagnostics().empty());
  }
}

INSTANTIATE_TEST_SUITE_P(
    Backends, BeamsDrawnTest, testing::ValuesIn(headless::compiledBackends()),
    [](const testing::TestParamInfo<render::Backend> &info) {
      return render::backendName(info.param);
    });
