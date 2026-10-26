#include <gtest/gtest.h>

#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/sdl_wrap.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

namespace {
class DocumentPaletteDeviceTest
    : public testing::TestWithParam<render::Backend> {};

TEST_P(DocumentPaletteDeviceTest,
       DarkPaperPreservesHighlightsAndPickingWithoutLeakingIntoNextDraw) {
  const auto backend = GetParam();
  if (!render::backendCompiledIn(backend)) GTEST_SKIP();
  AutoSDL sdl(SDL_INIT_VIDEO);
  render::configureBackendWindowAttributes(backend);
  AutoSDLWindow window("document palette", 128, 96,
                       render::backendWindowFlags(backend) | SDL_WINDOW_HIDDEN);
  auto device = render::createDevice(backend);
  device->initialize(window);
  if (backend != render::Backend::Vulkan) device->setPresentEnabled(false);
  device->setStrictDiagnostics(true);
  const auto shaders = gleditor::assetPath("shaders");
  render::PipelineDesc desc;
  desc.name           = "glyph";
  desc.vertexSource   = render::readShaderBody(shaders + "/glyph.vert.glsl");
  desc.fragmentSource = render::readShaderBody(shaders + "/glyph.frag.glsl");
  desc.spirvDir       = shaders + "/vulkan";
  desc.layout         = Doc::vertexLayout();
  const auto pipeline = device->createPipeline(desc);

  // An exact coverage mask separates full ink and blank glyph background;
  // the test must measure both, without depending on a system font's pixels.
  const auto atlas =
      device->createTextureArray(128, 1, render::TextureFormat::R8, 1);
  std::vector<std::byte> coverage(128 * 128);
  for (int y = 0; y < 128; ++y)
    for (int x = 0; x < 6; ++x)
      coverage[static_cast<std::size_t>(y * 128 + x)] = std::byte{255};
  device->updateTextureLayer(atlas, 0, 0, 0, 128, 128, coverage);
  const auto identity = render::packTagIdentity(0, 7, 3);
  const std::array<Doc::VBORow, 5> rows{{
      {.pos        = {64, 48},
       .foreground = Doc::VBORow::fill(0xFFFFFFFFU, Doc::VBORow::onPaper),
       .quad       = Doc::VBORow::box(0, 128, 96, render::tagKindPage),
       .paper      = Doc::VBORow::paperAt(0xFFFFFFFFU, 0)},
      {.pos        = {20, 48},
       .foreground = Doc::VBORow::ink(0, Doc::VBORow::onText, false),
       .quad       = Doc::VBORow::box(0, 12, 96, render::tagKindGlyph),
       .paper      = Doc::VBORow::paperAt(0xFFFFFFFFU, 1)},
      {.pos        = {44, 48},
       .foreground = Doc::VBORow::ink(0, Doc::VBORow::onText, false),
       .quad       = Doc::VBORow::box(0, 12, 96, render::tagKindGlyph),
       .paper      = Doc::VBORow::paperAt(0xFFFFFFFFU, 2)},
      {.pos        = {68, 48},
       .foreground = Doc::VBORow::fill(0x808080FFU, Doc::VBORow::onText),
       .quad       = Doc::VBORow::box(0, 12, 96, render::tagKindGlyph)},
      {.pos        = {92, 48},
       .foreground = Doc::VBORow::fill(0x2266CCFFU, Doc::VBORow::onText),
       .quad       = Doc::VBORow::box(0, 12, 96, render::tagKindOverlay)},
  }};
  const auto vertices =
      device->createBuffer(render::BufferKind::Vertex, sizeof(rows));
  device->updateBuffer(vertices, 0, std::as_bytes(std::span(rows)));
  const render::HighlightRange highlight{
      .identity     = identity | (render::tagKindGlyph
                              << (render::tagDocBits + render::tagPageBits)),
      .firstCluster = 2,
      .lastCluster  = 2,
      .colour       = 0x22D3EEFFU};
  render::DrawUniforms uniforms{.identity       = identity,
                                .paperColour    = 0x181C24FFU,
                                .inkColour      = 0xE8EDF4FFU,
                                .paletteEnabled = 1};
  const auto projection = glm::ortho(0.F, 128.F, 0.F, 96.F, -1.F, 1.F);
  std::copy_n(&projection[0][0], 16, uniforms.mvp.begin());
  std::optional<render::PickingResult> darkPick, ordinaryPick;
  render::FrameImage dark, ordinary;
  for (int frame = 0; frame < 4; ++frame) {
    while (const auto result = device->takePickingTag()) {
      if (result->requestId == 1) darkPick = result;
      if (result->requestId == 2) ordinaryPick = result;
    }
    ASSERT_TRUE(device->beginFrame());
    device->bindPipeline(pipeline);
    device->bindAtlasTexture(atlas);
    device->setHighlights(std::span(&highlight, 1));
    uniforms.paletteEnabled = frame == 0 ? 1 : 0;
    device->drawGlyphs(uniforms, vertices, 0, rows.size());
    if (frame < 2) ASSERT_TRUE(device->requestPickingTag(20, 48, frame + 1));
    device->endFrame();
    device->waitIdle();
    if (frame == 0) dark = device->captureColorTarget();
    if (frame == 1) ordinary = device->captureColorTarget();
  }
  ASSERT_TRUE(darkPick);
  ASSERT_TRUE(ordinaryPick);
  EXPECT_EQ(darkPick->tag, ordinaryPick->tag);
  EXPECT_EQ(darkPick->tag.clusterIndex, 1U);
  const auto pixel = [](const render::FrameImage &image, const int x) {
    const auto offset = static_cast<std::size_t>((48 * image.width + x) * 4);
    return std::array<unsigned int, 3>{
        image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2]};
  };
  EXPECT_EQ(pixel(dark, 8), (std::array<unsigned int, 3>{24, 28, 36}));
  EXPECT_EQ(pixel(dark, 17), (std::array<unsigned int, 3>{232, 237, 244}));
  EXPECT_EQ(pixel(dark, 23), pixel(dark, 8));
  EXPECT_EQ(pixel(dark, 41), (std::array<unsigned int, 3>{0, 0, 0}));
  EXPECT_EQ(pixel(dark, 47), (std::array<unsigned int, 3>{34, 211, 238}));
  const auto coarse = pixel(dark, 68);
  for (int channel = 0; channel < 3; ++channel) {
    const auto ink   = pixel(dark, 17)[channel];
    const auto paper = pixel(dark, 8)[channel];
    EXPECT_NEAR(coarse[channel],
                ink * (127.F / 255.F) + paper * (128.F / 255.F), 1.F);
  }
  EXPECT_EQ(pixel(dark, 92), (std::array<unsigned int, 3>{34, 102, 204}));
  EXPECT_EQ(pixel(ordinary, 8), (std::array<unsigned int, 3>{255, 255, 255}));
  EXPECT_EQ(pixel(ordinary, 17), (std::array<unsigned int, 3>{0, 0, 0}));
  EXPECT_EQ(pixel(ordinary, 23), pixel(ordinary, 8));
  EXPECT_EQ(pixel(ordinary, 47), pixel(dark, 47));
  EXPECT_EQ(pixel(ordinary, 92), pixel(dark, 92));
  device->destroyBuffer(vertices);
  device->destroyTexture(atlas);
  for (const auto &message : device->takeDiagnostics())
    EXPECT_NE(message.severity, render::DiagnosticSeverity::Error);
}

INSTANTIATE_TEST_SUITE_P(AllBackends, DocumentPaletteDeviceTest,
                         testing::Values(render::Backend::OpenGL,
                                         render::Backend::OpenGLES,
                                         render::Backend::Vulkan));
} // namespace
