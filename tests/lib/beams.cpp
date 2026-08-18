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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <gmock/gmock.h>

#include <glm/ext/vector_float3.hpp>
#include <glm/geometric.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/render/types.hpp>

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

/// What the vertex stage builds from a beam and a vertex index, written out as
/// assets/shaders/beam.vert.glsl does it.
glm::vec3 cornerOf(const Beams::Row &row, const int corner) {
  const glm::vec3 from{row.from[0], row.from[1], row.from[2]};
  const glm::vec3 to{row.to[0], row.to[1], row.to[2]};
  const float along  = (0 != (corner & 2)) ? 1.0F : 0.0F;
  const float across = (0 != (corner & 1)) ? 1.0F : -1.0F;

  const auto run      = to - from;
  const auto sideways = glm::cross(run, glm::vec3(0.0F, 0.0F, 1.0F));
  const auto offset   = glm::normalize(sideways) * (row.width * 0.5F) * across;
  return glm::mix(from, to, along) + offset;
}

class BeamsTest : public testing::Test {
protected:
  std::unique_ptr<RecordingDevice> device;

  void SetUp() override { device = std::make_unique<RecordingDevice>(); }
};

} // namespace

TEST_F(BeamsTest, theRecordIsWhatTheShaderDeclares) {
  using Row = Beams::Row;
  EXPECT_EQ(sizeof(Row), 36U);

  const auto layout = Beams::layout();
  EXPECT_EQ(layout.stride, sizeof(Row));
  ASSERT_EQ(layout.attributes.size(), 5U);
  EXPECT_EQ(layout.attributes[0].offset, offsetof(Row, from));
  EXPECT_EQ(layout.attributes[1].offset, offsetof(Row, width));
  EXPECT_EQ(layout.attributes[2].offset, offsetof(Row, to));
  EXPECT_EQ(layout.attributes[3].offset, offsetof(Row, colour));
  EXPECT_EQ(layout.attributes[4].offset, offsetof(Row, tag));
  for (std::uint32_t i = 0; i < layout.attributes.size(); i++) {
    EXPECT_EQ(layout.attributes[i].location, i);
  }
  // Three floats then one, twice over: a compiler that padded between them
  // would leave the shader reading the wrong words with no complaint from
  // anybody.
  EXPECT_EQ(offsetof(Row, width), 12U);
  EXPECT_EQ(offsetof(Row, to), 16U);
  EXPECT_EQ(offsetof(Row, colour), 28U);
  EXPECT_EQ(offsetof(Row, tag), 32U);
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
  const Beams::Row row{{0.0F, 0.0F, 0.0F}, 2.0F, {10.0F, 0.0F, 0.0F}, 0, 0};

  const auto nearLeft  = cornerOf(row, 0);
  const auto nearRight = cornerOf(row, 1);
  const auto farLeft   = cornerOf(row, 2);
  const auto farRight  = cornerOf(row, 3);

  // The first two sit at the near end and the last two at the far end.
  EXPECT_FLOAT_EQ(nearLeft.x, 0.0F);
  EXPECT_FLOAT_EQ(nearRight.x, 0.0F);
  EXPECT_FLOAT_EQ(farLeft.x, 10.0F);
  EXPECT_FLOAT_EQ(farRight.x, 10.0F);

  // Half the width to each side of the run, and the two ends offset the same
  // way, so the quad has parallel sides.
  EXPECT_FLOAT_EQ(glm::distance(nearLeft, nearRight), row.width);
  EXPECT_FLOAT_EQ(glm::distance(farLeft, farRight), row.width);
  EXPECT_FLOAT_EQ(nearLeft.y, farLeft.y);
  EXPECT_FLOAT_EQ(nearRight.y, farRight.y);

  // And it stays in the plane the pages lie in rather than turning to face the
  // camera: every corner keeps the Z of the end it came from.
  for (const auto &corner : {nearLeft, nearRight, farLeft, farRight}) {
    EXPECT_FLOAT_EQ(corner.z, 0.0F);
  }
}

// The width is measured across the beam whichever way it runs, which is what
// says the offset is perpendicular rather than along an axis.
TEST_F(BeamsTest, theWidthIsAcrossTheBeamAtAnyAngle) {
  for (const auto &to :
       {glm::vec3{5.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 5.0F, 0.0F},
        glm::vec3{3.0F, 4.0F, 0.0F}, glm::vec3{-2.0F, 7.0F, 0.0F}}) {
    const Beams::Row row{{0.0F, 0.0F, 0.0F}, 1.5F, {to.x, to.y, to.z}, 0, 0};
    const auto left  = cornerOf(row, 0);
    const auto right = cornerOf(row, 1);
    EXPECT_FLOAT_EQ(glm::distance(left, right), row.width);
    // Perpendicular: the offset between the corners has no component along the
    // run.
    EXPECT_NEAR(glm::dot(right - left, glm::normalize(to)), 0.0F, 1e-5F);
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
