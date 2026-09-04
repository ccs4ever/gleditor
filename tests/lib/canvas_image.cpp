/**
 * @file canvas_image.cpp
 * @brief Canvas's image path: the second pipeline addImage() writes into.
 *
 * Before this, addImage() wrote into the same stream as addRect()/addText(),
 * sampling the glyph atlas at a texel offset derived from the quad's own
 * size -- which is why an image could only ever be drawn at its native pixel
 * size, and why it was never anything but the glyph atlas on screen. This
 * checks the record the image pipeline actually reads, the same way
 * tests/lib/beams.cpp checks Beams::Row against beam.vert.glsl.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include <gmock/gmock.h>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/image_cache.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

#include "mocks/device.hpp"

using gleditor::Canvas;
using gleditor::ImageResource;
using testing::NiceMock;

namespace {

/// Mirrors the private ImageRow declared in canvas.cpp. Field order and size
/// are the contract with assets/shaders/image.vert.glsl; canvas.cpp's own
/// static_assert on sizeof(ImageRow) is what keeps this test's copy honest.
struct ImageRowMirror {
  std::array<float, 2> pos;
  std::array<float, 2> size;
  std::array<float, 4> uv;
  std::uint32_t layer;
  std::uint32_t tint;
  std::uint32_t index;
};

/// A device that keeps what was written to each of its buffers, keyed by
/// handle -- unlike tests/lib/beams.cpp's RecordingDevice, Canvas now owns
/// two buffer pools (glyph-shaped rows and image rows), and a test that
/// conflated them would not catch one pipeline's writes clobbering the
/// other's.
class RecordingDevice : public NiceMock<MockRenderDevice> {
public:
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::uint32_t nextBufferId{1};
  std::uint32_t nextPipelineId{1};

  RecordingDevice() {
    ON_CALL(*this, createBuffer)
        .WillByDefault(
            [this](const render::BufferKind, const std::size_t bytes) {
              const auto id = nextBufferId++;
              buffers[id].assign(bytes, std::byte{});
              return render::BufferHandle{id};
            });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault(
            [this](const render::BufferHandle handle, const std::size_t bytes) {
              buffers[handle.id].resize(bytes, std::byte{});
              return handle;
            });
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](const render::BufferHandle handle,
                              const std::size_t offset,
                              const std::span<const std::byte> data) {
          auto &buf = buffers[handle.id];
          if (offset + data.size() > buf.size()) {
            buf.resize(offset + data.size(), std::byte{});
          }
          std::memcpy(buf.data() + offset, data.data(), data.size());
        });
    ON_CALL(*this, createPipeline)
        .WillByDefault([this](const render::PipelineDesc &) {
          return render::PipelineHandle{nextPipelineId++};
        });
  }

  /// The image row at @p byteOffset within whichever buffer @p handle names.
  [[nodiscard]] ImageRowMirror imageRowAt(const render::BufferHandle handle,
                                          const std::size_t byteOffset) const {
    ImageRowMirror row{};
    std::memcpy(&row, buffers.at(handle.id).data() + byteOffset, sizeof(row));
    return row;
  }
};

ImageResource fourColourResource() {
  ImageResource image;
  image.width   = 8;
  image.height  = 8;
  image.layer   = 3;
  image.u0      = 0.25F;
  image.v0      = 0.5F;
  image.u1      = 0.75F;
  image.v1      = 1.0F;
  image.texture = render::TextureHandle{42};
  return image;
}

class CanvasImageTest : public testing::Test {
protected:
  std::unique_ptr<RecordingDevice> device;
  std::unique_ptr<Canvas> canvas;

  void SetUp() override {
    device = std::make_unique<RecordingDevice>();
    // BufferPool's constructor calls createBuffer eagerly, and the glyph
    // pool is declared before the image pool, so buffer id 1 is always the
    // glyph stream and 2 the image one.
    canvas = std::make_unique<Canvas>(device.get(), "Sans 12");
  }

  [[nodiscard]] static constexpr render::BufferHandle imageBuffer() {
    return render::BufferHandle{2};
  }
};

} // namespace

// A quad with no extent, or an ImageResource that failed to load (no atlas
// layer, no texture), must not reach either stream: this is what lets a
// caller try to draw whatever it has without checking validity itself.
TEST_F(CanvasImageTest, degenerateOrInvalidImagesAddNothing) {
  const ImageResource valid = fourColourResource();
  canvas->addImage(0.0F, 0.0F, 0.0F, 10.0F, valid);
  canvas->addImage(0.0F, 0.0F, 10.0F, 0.0F, valid);
  canvas->addImage(0.0F, 0.0F, 10.0F, 10.0F, ImageResource{});

  canvas->commit();
  EXPECT_TRUE(canvas->empty());
}

// The resource's UV rect is carried through verbatim rather than derived from
// the quad's own size -- the fix that lets an image be scaled or cropped
// instead of only ever drawn at native pixel size.
TEST_F(CanvasImageTest, anImageRowCarriesTheResourcesUvRectVerbatim) {
  const ImageResource image = fourColourResource();
  canvas->addImage(10.0F, 20.0F, 100.0F, 50.0F, image, 0x11223344U);
  canvas->commit();

  const auto row = device->imageRowAt(imageBuffer(), 0);
  EXPECT_FLOAT_EQ(row.pos[0], 10.0F + 50.0F);
  EXPECT_FLOAT_EQ(row.pos[1], 20.0F + 25.0F);
  EXPECT_FLOAT_EQ(row.size[0], 100.0F);
  EXPECT_FLOAT_EQ(row.size[1], 50.0F);
  EXPECT_FLOAT_EQ(row.uv[0], image.u0);
  EXPECT_FLOAT_EQ(row.uv[1], image.v0);
  EXPECT_FLOAT_EQ(row.uv[2], image.u1);
  EXPECT_FLOAT_EQ(row.uv[3], image.v1);
  EXPECT_EQ(row.layer, static_cast<std::uint32_t>(image.layer));
  EXPECT_EQ(row.tint, 0x11223344U);
}

// The picking index travels with the image the same way a glyph's cluster
// index does, so a program using one canvas for several images can tell them
// apart when one is clicked.
TEST_F(CanvasImageTest, thePickingIndexIsWhateverSetTagLastSaid) {
  const ImageResource image = fourColourResource();
  canvas->setTag(render::tagKindOverlay, 7);
  canvas->addImage(0.0F, 0.0F, 10.0F, 10.0F, image);
  canvas->commit();

  EXPECT_EQ(device->imageRowAt(imageBuffer(), 0).index, 7U);
}

// addRect()/addText() must keep writing the glyph-shaped stream and nothing
// of addImage()'s must land there: the two pipelines have different vertex
// layouts, and a row of one shape read as the other is silent corruption, not
// a crash.
TEST_F(CanvasImageTest, addRectStaysOnTheGlyphStreamAlone) {
  canvas->addRect(0.0F, 0.0F, 10.0F, 10.0F, 0xFF0000FFU);
  canvas->commit();
  EXPECT_FALSE(canvas->empty());

  // The image buffer exists from construction (the pool reserves its initial
  // capacity eagerly), but addRect alone must never write to it: nothing
  // committed to the image stream, and it stays all zero.
  const auto &imageContents = device->buffers.at(imageBuffer().id);
  EXPECT_TRUE(std::ranges::all_of(
      imageContents, [](const std::byte b) { return std::byte{0} == b; }));
}

// Committing an image and then clearing without adding another must leave
// nothing committed -- the same rebuild-from-scratch contract Canvas's glyph
// stream already has.
TEST_F(CanvasImageTest, clearingDropsPreviouslyCommittedImages) {
  const ImageResource image = fourColourResource();
  canvas->addImage(0.0F, 0.0F, 10.0F, 10.0F, image);
  canvas->commit();
  EXPECT_FALSE(canvas->empty());

  canvas->clear();
  canvas->commit();
  EXPECT_TRUE(canvas->empty());
}

// draw() must bind the image's own atlas, not the glyph cache's texture,
// when there are committed images -- the fix for the bug where every image
// quad sampled whichever glyph happened to occupy its atlas layer.
TEST_F(CanvasImageTest, drawBindsTheImagesOwnAtlas) {
  render::PipelineDesc documentDesc;
  documentDesc.vertexSource   = "void main() { gl_Position = vec4(0.0); }";
  documentDesc.fragmentSource = "void main() { outColor = vec4(0.0); }";
  canvas->createPipeline(documentDesc, false);

  const ImageResource image = fourColourResource();
  canvas->addImage(0.0F, 0.0F, 10.0F, 10.0F, image);
  canvas->commit();

  RenderState state(device.get());
  EXPECT_CALL(*device, bindAtlasTexture(image.texture)).Times(1);
  canvas->draw(state, glm::mat4(1.0F));
}
