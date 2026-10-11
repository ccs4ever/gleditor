#include <gtest/gtest.h>

#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include "mocks/device.hpp"

namespace {
class RegionTableDevice : public MockRenderDevice {
public:
  using RenderDevice::beginRegions;
  using RenderDevice::endRegions;
  using RenderDevice::regionOf;
};
TEST(RenderRegions, ClampsWritingWithoutChangingProjection) {
  RegionTableDevice device;
  device.beginRegions(100, 80);
  const auto id      = device.defineRegion({.viewLeft   = -20,
                                            .viewBottom = -10,
                                            .viewWidth  = 140,
                                            .viewHeight = 100,
                                            .clipLeft   = -5,
                                            .clipBottom = 20,
                                            .clipWidth  = 120,
                                            .clipHeight = 90});
  const auto &region = device.regionOf(id);
  EXPECT_EQ(region.viewLeft, -20);
  EXPECT_EQ(region.viewWidth, 140);
  EXPECT_EQ(region.clipLeft, 0);
  EXPECT_EQ(region.clipBottom, 20);
  EXPECT_EQ(region.clipWidth, 100);
  EXPECT_EQ(region.clipHeight, 60);
  EXPECT_EQ(device.regionOf(std::nullopt).clipHeight, 80);
  device.endRegions();
  EXPECT_THROW(device.setRegion(id), std::invalid_argument);
  EXPECT_THROW(device.defineRegion({}), std::logic_error);
}
TEST(RenderRegions, BatchesKeepRegionsAndRestoreImmediateScope) {
  RegionTableDevice device;
  device.beginRegions(100, 80);
  const auto id = device.defineRegion(
      {.viewWidth = 100, .viewHeight = 80, .clipWidth = 50, .clipHeight = 80});
  device.setRegion(id);
  std::array<render::GlyphBatch, 2> batches{};
  batches[1].region = id;
  std::vector<std::optional<render::RegionId>> seen;
  EXPECT_CALL(device,
              drawGlyphs(testing::_, testing::_, testing::_, testing::_))
      .WillRepeatedly([&](const auto &, auto, auto, auto) {
        seen.push_back(device.currentRegion());
      });
  device.drawGlyphBatches(batches);
  EXPECT_EQ(seen,
            (std::vector<std::optional<render::RegionId>>{std::nullopt, id}));
  EXPECT_EQ(device.currentRegion(), id);
  {
    const render::RegionScope overlay(device, std::nullopt);
    EXPECT_FALSE(device.currentRegion());
  }
  EXPECT_EQ(device.currentRegion(), id);
}
TEST(RenderRegions, RefusesInvalidViewportAndDepth) {
  RegionTableDevice device;
  device.beginRegions(100, 80);
  EXPECT_THROW(device.defineRegion({.viewWidth = 0, .viewHeight = 80}),
               std::invalid_argument);
  EXPECT_THROW(device.defineRegion({.viewWidth  = 100,
                                    .viewHeight = 80,
                                    .nearDepth  = .8F,
                                    .farDepth   = .2F}),
               std::invalid_argument);
  EXPECT_THROW(device.defineRegion(
                   {.viewWidth  = 100,
                    .viewHeight = 80,
                    .nearDepth  = std::numeric_limits<float>::quiet_NaN()}),
               std::invalid_argument);
}

class RenderRegionDeviceTest : public testing::TestWithParam<render::Backend> {
};
TEST_P(RenderRegionDeviceTest, ClipsColorAndPickingThenRestoresWholeTarget) {
  const auto backend = GetParam();
  if (!render::backendCompiledIn(backend)) GTEST_SKIP();
  AutoSDL sdl(SDL_INIT_VIDEO);
  render::configureBackendWindowAttributes(backend);
  AutoSDLWindow window("render region", 128, 96,
                       render::backendWindowFlags(backend) | SDL_WINDOW_HIDDEN);
  auto device = render::createDevice(backend);
  device->initialize(window);
  if (backend != render::Backend::Vulkan) device->setPresentEnabled(false);
  device->setStrictDiagnostics(true);
  RenderState state(device.get());
  const auto shaders = gleditor::assetPath("shaders");
  render::PipelineDesc desc;
  desc.name           = "glyph";
  desc.vertexSource   = render::readShaderBody(shaders + "/glyph.vert.glsl");
  desc.fragmentSource = render::readShaderBody(shaders + "/glyph.frag.glsl");
  desc.spirvDir       = shaders + "/vulkan";
  desc.layout         = Doc::vertexLayout();
  gleditor::Canvas canvas(device.get(), "Sans 12", 1);
  canvas.createPipeline(desc);
  canvas.setIdentity(7, 3);
  canvas.addRect(0, 0, 128, 96, 0xff0000ff);
  canvas.commit();
  const auto projection = glm::ortho(0.F, 128.F, 0.F, 96.F, -1.F, 1.F);
  std::optional<render::PickingResult> inside, outside;
  for (int frame = 0; frame < 4; ++frame) {
    while (const auto result = device->takePickingTag()) {
      if (result->requestId == 1) inside = result;
      if (result->requestId == 2) outside = result;
    }
    ASSERT_TRUE(device->beginFrame());
    const auto id = device->defineRegion({.viewWidth  = 128,
                                          .viewHeight = 96,
                                          .clipLeft   = 20,
                                          .clipBottom = 10,
                                          .clipWidth  = 70,
                                          .clipHeight = 40});
    {
      const render::RegionScope content(*device, id);
      canvas.draw(state, projection);
    }
    if (frame == 0) ASSERT_TRUE(device->requestPickingTag(30, 66, 1));
    if (frame == 1) ASSERT_TRUE(device->requestPickingTag(30, 20, 2));
    device->endFrame();
    device->waitIdle();
  }
  ASSERT_TRUE(inside);
  ASSERT_TRUE(outside);
  EXPECT_FALSE(inside->tag.empty());
  EXPECT_EQ(inside->tag.docIndex, 7U);
  EXPECT_TRUE(outside->tag.empty());
  auto image = device->captureColorTarget();
  ASSERT_EQ(image.width, 128);
  ASSERT_EQ(image.height, 96);
  for (int y = 0; y < 96; ++y) {
    for (int x = 0; x < 128; ++x) {
      const auto offset = static_cast<std::size_t>((y * 128 + x) * 4);
      EXPECT_EQ(image.rgba[offset],
                (x >= 20 && x < 90 && y >= 46 && y < 86) ? 255 : 0)
          << x << "," << y;
    }
  }
  ASSERT_TRUE(device->beginFrame());
  canvas.draw(state, projection);
  device->endFrame();
  image = device->captureColorTarget();
  EXPECT_EQ(image.rgba[0], 255);
  // Mixed recorded regions must survive worker recording and ignore the
  // immediate region. A near inset remains in front of later far draws.
  const std::array<Doc::VBORow, 3> rows{
      {{.pos        = {64.F, 48.F},
        .foreground = Doc::VBORow::ink(0xff0000ff, 0, true),
        .atlas      = 0,
        .quad       = Doc::VBORow::box(0, 128, 96, render::tagKindPage),
        .paper      = 0},
       {.pos        = {64.F, 48.F},
        .foreground = Doc::VBORow::ink(0x0000ffff, 0, true),
        .atlas      = 0,
        .quad       = Doc::VBORow::box(0, 128, 96, render::tagKindPage),
        .paper      = 0},
       {.pos        = {64.F, 48.F},
        .foreground = Doc::VBORow::ink(0x00ff00ff, 0, true),
        .atlas      = 0,
        .quad       = Doc::VBORow::box(0, 128, 96, render::tagKindPage),
        .paper      = 0}}};
  const auto vertices =
      device->createBuffer(render::BufferKind::Vertex, sizeof(rows));
  device->updateBuffer(vertices, 0, std::as_bytes(std::span(rows)));
  const auto pipeline = device->createPipeline(desc);
  ASSERT_TRUE(device->beginFrame());
  const auto left  = device->defineRegion({.viewWidth  = 128,
                                           .viewHeight = 96,
                                           .clipWidth  = 64,
                                           .clipHeight = 96,
                                           .nearDepth  = .8F,
                                           .farDepth   = 1.F});
  const auto right = device->defineRegion({.viewWidth  = 128,
                                           .viewHeight = 96,
                                           .clipLeft   = 64,
                                           .clipWidth  = 64,
                                           .clipHeight = 96,
                                           .nearDepth  = .8F,
                                           .farDepth   = 1.F});
  const auto inset = device->defineRegion({.viewWidth  = 128,
                                           .viewHeight = 96,
                                           .clipLeft   = 40,
                                           .clipBottom = 20,
                                           .clipWidth  = 48,
                                           .clipHeight = 40,
                                           .nearDepth  = 0.F,
                                           .farDepth   = .1F});
  render::DrawUniforms uniforms;
  std::copy_n(&projection[0][0], 16, uniforms.mvp.begin());
  std::vector<render::GlyphBatch> batches{
      {.uniforms         = uniforms,
       .vertices         = vertices,
       .vertexByteOffset = sizeof(Doc::VBORow) * 2,
       .instanceCount    = 1,
       .region           = inset}};
  for (std::size_t i = 0; i < 256; ++i) {
    batches.push_back({.uniforms         = uniforms,
                       .vertices         = vertices,
                       .vertexByteOffset = (i % 2) * sizeof(Doc::VBORow),
                       .instanceCount    = 1,
                       .region           = i % 2 ? right : left});
  }
  device->bindPipeline(pipeline);
  device->bindAtlasTexture(state.glyphCache.textureHandle());
  device->setRegion(inset);
  device->drawGlyphBatches(batches);
  EXPECT_EQ(device->currentRegion(), inset);
  device->endFrame();
  image            = device->captureColorTarget();
  const auto pixel = [&](const int x, const int y, const int channel) {
    return image.rgba[static_cast<std::size_t>((y * 128 + x) * 4 + channel)];
  };
  EXPECT_EQ(pixel(10, 10, 0), 255);
  EXPECT_EQ(pixel(110, 10, 2), 255);
  EXPECT_EQ(pixel(60, 60, 1), 255);
  device->waitIdle();
  device->destroyBuffer(vertices);
  const auto diagnostics = device->takeDiagnostics();
  for (const auto &message : diagnostics)
    EXPECT_NE(message.severity, render::DiagnosticSeverity::Error);
}
INSTANTIATE_TEST_SUITE_P(AllBackends, RenderRegionDeviceTest,
                         testing::Values(render::Backend::OpenGL,
                                         render::Backend::OpenGLES,
                                         render::Backend::Vulkan));
} // namespace
