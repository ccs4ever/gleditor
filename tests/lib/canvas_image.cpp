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
#include <gleditor/doc.hpp>
#include <gleditor/image_cache.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <limits>
#include <stdexcept>

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
  std::map<std::uint32_t, std::size_t> writtenBytes;
  std::uint32_t nextBufferId{1};
  std::uint32_t nextPipelineId{1};

  RecordingDevice() {
    ON_CALL(*this, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(*this, createTextureArray)
        .WillByDefault(testing::Return(render::TextureHandle{1}));
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
          writtenBytes[handle.id] =
              std::max(writtenBytes[handle.id], offset + data.size());
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

  [[nodiscard]] Doc::VBORow glyphRowAt(const std::size_t index = 0) const {
    Doc::VBORow row{};
    std::memcpy(&row, buffers.at(1).data() + index * sizeof(row), sizeof(row));
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

TEST_F(CanvasImageTest, NestedClipsIntersectAndPopRestoresTheParent) {
  canvas->pushClip({0, 0, 20, 20});
  canvas->pushClip({5, 6, 8, 7});
  canvas->addRect(-10, -10, 100, 100, 0xFF0000FFU);
  canvas->popClip();
  canvas->addRect(-10, -10, 100, 100, 0x00FF00FFU);
  canvas->commit();
  const auto inner = device->glyphRowAt();
  EXPECT_FLOAT_EQ(inner.pos[0], 9.0F);
  EXPECT_FLOAT_EQ(inner.pos[1], 9.5F);
  EXPECT_EQ(inner.quad >> 20U, 8U);
  EXPECT_EQ((inner.quad >> 8U) & 4095U, 7U);
  const auto outer = device->glyphRowAt(1);
  EXPECT_FLOAT_EQ(outer.pos[0], 10.0F);
  EXPECT_FLOAT_EQ(outer.pos[1], 10.0F);
  EXPECT_EQ(outer.quad >> 20U, 20U);
}

TEST_F(CanvasImageTest, DisjointClipDrawsNothingAndClearResetsTheStack) {
  canvas->pushClip({0, 0, 10, 10});
  canvas->pushClip({20, 20, 5, 5});
  canvas->addRect(0, 0, 100, 100, 0xFFFFFFFFU);
  canvas->addImage(0, 0, 100, 100, fourColourResource());
  canvas->commit();
  EXPECT_TRUE(canvas->empty());
  canvas->clear();
  EXPECT_THROW(canvas->popClip(), std::logic_error);
  canvas->addRect(0, 0, 100, 100, 0xFFFFFFFFU);
  canvas->commit();
  EXPECT_FALSE(canvas->empty());
}

TEST_F(CanvasImageTest, InvalidClipsAreRefusedWithoutChangingTheStack) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(canvas->pushClip({nan, 0, 10, 10}), std::invalid_argument);
  EXPECT_THROW(canvas->pushClip({0, 0, -1, 10}), std::invalid_argument);
  EXPECT_THROW(canvas->popClip(), std::logic_error);
}

TEST_F(CanvasImageTest, ImageClippingInterpolatesUvOnAllFourEdges) {
  canvas->pushClip({35, 30, 25, 20});
  canvas->addImage(10, 20, 100, 50, fourColourResource());
  canvas->commit();
  const auto row = device->imageRowAt(imageBuffer(), 0);
  EXPECT_FLOAT_EQ(row.pos[0], 47.5F);
  EXPECT_FLOAT_EQ(row.pos[1], 40.0F);
  EXPECT_FLOAT_EQ(row.size[0], 25.0F);
  EXPECT_FLOAT_EQ(row.size[1], 20.0F);
  EXPECT_FLOAT_EQ(row.uv[0], 0.375F);
  EXPECT_FLOAT_EQ(row.uv[1], 0.6F);
  EXPECT_FLOAT_EQ(row.uv[2], 0.5F);
  EXPECT_FLOAT_EQ(row.uv[3], 0.8F);
}

TEST_F(CanvasImageTest,
       RetainedGlyphClippingPreservesAtlasSamplingAndIdentity) {
  RenderState state(device.get());
  const auto font  = gleditor::text::FontManager::instance().getFont("Sans 12");
  const auto glyph = state.glyphCache.put("W", font);
  ASSERT_TRUE(glyph);
  const float width  = static_cast<float>(static_cast<int>(glyph->dims.width));
  const float height = static_cast<float>(static_cast<int>(glyph->dims.height));
  ASSERT_GT(width, 5.0F);
  ASSERT_GT(height, 5.0F);
  const auto fitted = gleditor::text::fit("W", font, {});
  canvas->setTag(render::tagKindOverlay, 42);
  canvas->pushClip({3.1F, 2.1F, 3.8F, height - 4.2F});
  gleditor::text::ShapingStatsScope capture;
  canvas->addText(state, {0, 0, width, height}, fitted, 0xFFFFFFFFU, 0);
  canvas->commit();
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  const auto row = device->glyphRowAt();
  EXPECT_FLOAT_EQ(row.pos[0], 5.0F);
  EXPECT_EQ(row.quad >> 20U, 2U);
  EXPECT_EQ(row.atlas >> 16U,
            static_cast<unsigned>(glyph->texCoords.topLeft.x) + 4U);
  EXPECT_EQ(row.atlas & 65535U,
            static_cast<unsigned>(glyph->texCoords.topLeft.y) + 3U);
  EXPECT_EQ(row.paper & 65535U, 42U);
  canvas->popClip();
  EXPECT_THROW(canvas->popClip(), std::logic_error);
}

TEST_F(CanvasImageTest, BoxedTextFitsCachesAndRestoresTheEnclosingClip) {
  RenderState state(device.get());
  gleditor::text::ShapingCache cache;
  const gleditor::ui::Rect box{10, 10, 100, 40};
  const auto first =
      canvas->addText(state, box, "A long label needing an ellipsis",
                      0xFFFFFFFFU, 0, {}, &cache);
  EXPECT_TRUE(first.fitted.truncated);
  EXPECT_EQ(first.fitted.lines, 1U);
  EXPECT_LE(first.fitted.widthPx, box.width);
  EXPECT_FLOAT_EQ(first.box.left, box.left);
  canvas->clear();
  gleditor::text::ShapingStatsScope capture;
  std::ignore = canvas->addText(state, box, "A long label needing an ellipsis",
                                0xFFFFFFFFU, 0, {}, &cache);
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  EXPECT_THROW(canvas->popClip(), std::logic_error);
}

TEST_F(CanvasImageTest, EmptyBoxNeverUsesTheUnboundedFitConvention) {
  RenderState state(device.get());
  gleditor::text::ShapingStatsScope capture;
  const auto result =
      canvas->addText(state, {0, 0, 0, 40}, "hidden", 0xFFFFFFFFU, 0);
  EXPECT_TRUE(result.fitted.truncated);
  EXPECT_TRUE(result.fitted.shaping.glyphs.empty());
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  canvas->commit();
  EXPECT_TRUE(canvas->empty());
}

TEST_F(CanvasImageTest, ClipModeKeepsAndCropsAPartialCluster) {
  RenderState state(device.get());
  const auto result =
      canvas->addText(state, {0, 0, 4, 40}, "Wider text", 0xFFFFFFFFU, 0,
                      {.overflow = gleditor::text::Overflow::Clip});
  EXPECT_TRUE(result.fitted.truncated);
  EXPECT_EQ(result.fitted.visibleBytes, 0U);
  ASSERT_EQ(result.fitted.shaping.glyphs.size(), 1U);
  EXPECT_EQ(result.fitted.shaping.glyphs.front().chr, "W");
  canvas->commit();
  EXPECT_FALSE(canvas->empty());
  const auto row = device->glyphRowAt();
  EXPECT_EQ(row.quad >> 20U, 4U);
  EXPECT_FLOAT_EQ(row.pos[0], 2.0F);
}

TEST_F(CanvasImageTest, ExplicitBoxDrawsOneBoundedLine) {
  RenderState state(device.get());
  const auto result = canvas->addText(
      state, {10, 100 - 40, 80, 40},
      "A very long label\nwith an explicit newline and more text", 0xFFFFFFFFU,
      0);
  const auto font = gleditor::text::FontManager::instance().getFont("Sans 12");
  const gleditor::TextMetrics metrics{
      static_cast<float>(result.fitted.shaping.textWidthPx),
      static_cast<float>(result.fitted.shaping.textHeightPx)};
  EXPECT_LE(metrics.width, 80.0F);
  EXPECT_EQ(metrics.height, std::ceil(font->metrics().lineHeight));
  canvas->commit();
  ASSERT_GT(device->writtenBytes[1], 0U);
  for (std::size_t i = 0; i < device->writtenBytes[1] / sizeof(Doc::VBORow);
       ++i) {
    const auto row     = device->glyphRowAt(i);
    const float width  = static_cast<float>(row.quad >> 20U);
    const float height = static_cast<float>((row.quad >> 8U) & 4095U);
    EXPECT_GE(row.pos[0] - width * 0.5F, 10.0F);
    EXPECT_LE(row.pos[0] + width * 0.5F, 90.0F);
    EXPECT_LE(row.pos[1] + height * 0.5F, 100.0F);
    EXPECT_GE(row.pos[1] - height * 0.5F, 100.0F - metrics.height);
  }
}

TEST_F(CanvasImageTest, NarrowerFitConstraintAlsoNarrowsThePhysicalClip) {
  RenderState state(device.get());
  const auto result = canvas->addText(
      state, {0, 0, 100, 40}, "Wider text", 0xFFFFFFFFU, 0,
      {.maxWidthPx = 4.0F, .overflow = gleditor::text::Overflow::Clip});
  EXPECT_FLOAT_EQ(result.box.width, 4.0F);
  canvas->commit();
  ASSERT_FALSE(canvas->empty());
  const auto row = device->glyphRowAt();
  EXPECT_EQ(row.quad >> 20U, 4U);
  EXPECT_FLOAT_EQ(row.pos[0], 2.0F);
}
