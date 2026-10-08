#include <gtest/gtest.h>

#ifdef GLEDITOR_ENABLE_VULKAN
#include <array>
#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render/vulkan/device_vk.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <memory>
#include <vector>

namespace {
class VulkanResourceLifetimeTest : public testing::Test {
protected:
  std::unique_ptr<AutoSDL> sdl;
  std::unique_ptr<AutoSDLWindow> window;
  std::unique_ptr<render::vulkan::DeviceVK> device;
  std::unique_ptr<RenderState> state;
  render::PipelineDesc desc;
  const glm::mat4 projection = glm::ortho(0.F, 192.F, 0.F, 64.F);
  void SetUp() override {
    sdl = std::make_unique<AutoSDL>(SDL_INIT_VIDEO);
    try {
      window = std::make_unique<AutoSDLWindow>(
          "resource lifetime", 192, 64,
          render::backendWindowFlags(render::Backend::Vulkan) |
              SDL_WINDOW_HIDDEN);
    } catch (const std::exception &error) {
      GTEST_SKIP() << "Hidden Vulkan window unavailable: " << error.what();
    }
    device = std::make_unique<render::vulkan::DeviceVK>();
    device->initialize(*window);
    device->setStrictDiagnostics(true);
    state               = std::make_unique<RenderState>(device.get());
    const auto shaders  = gleditor::assetPath("shaders");
    desc.name           = "glyph";
    desc.vertexSource   = render::readShaderBody(shaders + "/glyph.vert.glsl");
    desc.fragmentSource = render::readShaderBody(shaders + "/glyph.frag.glsl");
    desc.spirvDir       = shaders + "/vulkan";
    desc.layout         = Doc::vertexLayout();
  }
  std::unique_ptr<gleditor::Canvas> canvas() {
    auto result =
        std::make_unique<gleditor::Canvas>(device.get(), "Sans 12", 1);
    result->createPipeline(desc);
    return result;
  }
  static void expectPixel(const render::FrameImage &image, unsigned x,
                          std::array<std::uint8_t, 4> expected) {
    ASSERT_EQ(image.width, 192);
    ASSERT_EQ(image.height, 64);
    const auto offset = (32U * 192U + x) * 4U;
    ASSERT_LT(offset + 3U, image.rgba.size());
    for (unsigned channel = 0; channel < 4; ++channel)
      EXPECT_EQ(image.rgba[offset + channel], expected[channel]);
  }
};
TEST_F(VulkanResourceLifetimeTest,
       EarlierCanvasesRenderAfterMultiplePoolExtensions) {
  std::vector<std::unique_ptr<gleditor::Canvas>> canvases;
  // Cross the former device-wide ceiling while preserving earlier bindings.
  for (int i = 0; i < 32; ++i) canvases.push_back(canvas());
  canvases.front()->addRect(8, 8, 40, 48, 0xff0000ff);
  canvases.front()->commit();
  ASSERT_TRUE(device->beginFrame());
  canvases.front()->draw(*state, projection);
  device->endFrame();
  for (int i = 32; i < 65; ++i) canvases.push_back(canvas());
  canvases[32]->addRect(72, 8, 40, 48, 0x00ff00ff);
  canvases[32]->commit();
  canvases.back()->addRect(136, 8, 40, 48, 0x0000ffff);
  canvases.back()->commit();
  RecordProperty("pipelines", 130);
  for (int frame = 0; frame < 3; ++frame) {
    ASSERT_TRUE(device->beginFrame());
    for (const auto index : {0U, 32U, 64U})
      canvases[index]->draw(*state, projection);
    device->endFrame();
    const auto image = device->captureColorTarget();
    expectPixel(image, 24, {255, 0, 0, 255});
    expectPixel(image, 88, {0, 255, 0, 255});
    expectPixel(image, 152, {0, 0, 255, 255});
  }
  device->waitIdle();
  EXPECT_TRUE(device->takeDiagnostics().empty());
}
TEST_F(VulkanResourceLifetimeTest,
       DestroyedAndResizedBuffersOutliveRecordedDraws) {
  auto removed = canvas(), resized = canvas();
  removed->addRect(8, 8, 40, 48, 0xff0000ff);
  removed->commit();
  resized->addRect(72, 8, 40, 48, 0x0000ffff);
  resized->commit();
  ASSERT_TRUE(device->beginFrame());
  removed->draw(*state, projection);
  resized->draw(*state, projection);
  removed.reset();
  resized->clear();
  // Force new vertex storage after the old buffer has already been recorded.
  for (int i = 0; i < 256; ++i) resized->addRect(72, 8, 40, 48, 0x00ff00ff);
  resized->commit();
  device->waitIdle(); // This cannot retire the unsubmitted frame.
  device->endFrame();
  const auto original = device->captureColorTarget();
  expectPixel(original, 24, {255, 0, 0, 255});
  expectPixel(original, 88, {0, 0, 255, 255});
  device->waitIdle();
  ASSERT_TRUE(device->beginFrame());
  resized->draw(*state, projection);
  device->endFrame();
  expectPixel(device->captureColorTarget(), 88, {0, 255, 0, 255});
  EXPECT_TRUE(device->takeDiagnostics().empty());
}
TEST_F(VulkanResourceLifetimeTest, RepeatedFrameReuseRetiresReplacedBuffers) {
  for (int frame = 0; frame < 8; ++frame) {
    auto drawing = canvas();
    drawing->addRect(8, 8, 40, 48, 0xff0000ff);
    drawing->commit();
    ASSERT_TRUE(device->beginFrame());
    drawing->draw(*state, projection);
    drawing->clear();
    for (int i = 0; i < 256; ++i) drawing->addRect(8, 8, 40, 48, 0x00ff00ff);
    drawing->commit();
    drawing.reset();
    device->endFrame();
  }
  // Capture only after repeated slot reuse: readback must not provide the
  // synchronization that ordinary frame recycling is responsible for.
  expectPixel(device->captureColorTarget(), 24, {255, 0, 0, 255});
  EXPECT_TRUE(device->takeDiagnostics().empty());
}
} // namespace
#endif
