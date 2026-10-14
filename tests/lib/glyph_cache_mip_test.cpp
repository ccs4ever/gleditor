#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <gleditor/text/font.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <memory>
#include <string_view>

namespace {

class GlyphCacheMipOrderTest : public testing::TestWithParam<render::Backend> {
};

TEST_P(GlyphCacheMipOrderTest,
       SameGlyphPixelsDoNotDependOnEarlierCacheRequests) {
  const auto backend = GetParam();
  if (!render::backendCompiledIn(backend)) GTEST_SKIP();

  AutoSDL sdl(SDL_INIT_VIDEO);
  render::configureBackendWindowAttributes(backend);
  AutoSDLWindow window("glyph cache mip order", 128, 96,
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

  RenderState referenceState(device.get());
  RenderState prefixedState(device.get());
  auto referenceCanvas =
      std::make_unique<gleditor::Canvas>(device.get(), "Serif 24", 1);
  auto prefixedCanvas =
      std::make_unique<gleditor::Canvas>(device.get(), "Serif 24", 1);
  referenceCanvas->createPipeline(desc);
  prefixedCanvas->createPipeline(desc);

  const auto font = gleditor::text::FontManager::instance().getFont("Serif 24");
  ASSERT_TRUE(font);
  const auto referenceGlyph = referenceState.glyphCache.put("A", font);
  ASSERT_TRUE(referenceGlyph);
  // Request an unused glyph first in the second atlas. The later A is still
  // the first drawn glyph and has the same bitmap and geometry, while its
  // atlas origin may occupy another phase of the global mip grid.
  const auto prefixGlyph = prefixedState.glyphCache.put("m", font);
  ASSERT_TRUE(prefixGlyph);
  const auto prefixedGlyph = prefixedState.glyphCache.put("A", font);
  ASSERT_TRUE(prefixedGlyph);
  ASSERT_GT(referenceGlyph->ink, 0.0F);
  ASSERT_GT(prefixedGlyph->ink, 0.0F);
  ASSERT_EQ(referenceGlyph->dims.width, prefixedGlyph->dims.width);
  ASSERT_EQ(referenceGlyph->dims.height, prefixedGlyph->dims.height);
  ASSERT_EQ(referenceGlyph->ink, prefixedGlyph->ink);
  ASSERT_NE(referenceGlyph->texCoords.topLeft.x,
            prefixedGlyph->texCoords.topLeft.x)
      << "the two cache request orders must place A at distinct atlas texels";

  constexpr std::string_view sample = "A";
  const auto referenceMetrics       = referenceCanvas->addText(
      referenceState, 32.0F, 64.0F, sample, 0x000000FFU, 0xFFFFFFFFU);
  const auto prefixedMetrics = prefixedCanvas->addText(
      prefixedState, 32.0F, 64.0F, sample, 0x000000FFU, 0xFFFFFFFFU);
  ASSERT_EQ(referenceMetrics.width, prefixedMetrics.width);
  ASSERT_EQ(referenceMetrics.height, prefixedMetrics.height);
  referenceCanvas->commit();
  prefixedCanvas->commit();
  referenceState.glyphCache.flush();
  prefixedState.glyphCache.flush();

  // At 0.375 screen pixels per layout pixel this glyph samples the atlas mip
  // chain. Both draws use the same transform, Canvas input, colors and glyph;
  // only the unused request that preceded A differs.
  const glm::mat4 projection =
      glm::ortho(0.0F, 128.0F, 0.0F, 96.0F, -1.0F, 1.0F);
  const glm::mat4 transform =
      glm::translate(glm::mat4(1.0F), glm::vec3(8.0F, 16.0F, 0.0F)) *
      glm::scale(glm::mat4(1.0F), glm::vec3(0.375F, 0.375F, 1.0F));

  ASSERT_TRUE(device->beginFrame());
  device->endFrame();
  device->waitIdle();
  const auto emptyImage = device->captureColorTarget();

  ASSERT_TRUE(device->beginFrame());
  referenceCanvas->draw(referenceState, projection * transform);
  device->endFrame();
  device->waitIdle();
  const auto referenceImage = device->captureColorTarget();

  ASSERT_TRUE(device->beginFrame());
  prefixedCanvas->draw(prefixedState, projection * transform);
  device->endFrame();
  device->waitIdle();
  const auto prefixedImage = device->captureColorTarget();
  ASSERT_EQ(referenceImage.width, 128);
  ASSERT_EQ(referenceImage.height, 96);
  ASSERT_EQ(prefixedImage.width, referenceImage.width);
  ASSERT_EQ(prefixedImage.height, referenceImage.height);
  ASSERT_EQ(prefixedImage.rgba.size(), referenceImage.rgba.size());

  const auto changedRgbPixels = [](const auto &image, const auto &empty) {
    std::size_t changed = 0;
    for (std::size_t pixel = 0; pixel < image.rgba.size(); pixel += 4) {
      changed += (image.rgba[pixel] != empty.rgba[pixel] ||
                  image.rgba[pixel + 1] != empty.rgba[pixel + 1] ||
                  image.rgba[pixel + 2] != empty.rgba[pixel + 2])
                     ? 1U
                     : 0U;
    }
    return changed;
  };
  ASSERT_EQ(emptyImage.rgba.size(), referenceImage.rgba.size());
  EXPECT_GT(changedRgbPixels(referenceImage, emptyImage), 0U)
      << "the reference glyph draw changed no color pixels";
  EXPECT_GT(changedRgbPixels(prefixedImage, emptyImage), 0U)
      << "the prefixed glyph draw changed no color pixels";

  std::size_t changedPixels = 0;
  unsigned int maximumDelta = 0;
  for (std::size_t pixel = 0; pixel < referenceImage.rgba.size(); pixel += 4) {
    bool changed = false;
    for (std::size_t channel = 0; channel < 4; ++channel) {
      const auto a = referenceImage.rgba[pixel + channel];
      const auto b = prefixedImage.rgba[pixel + channel];
      changed |= a != b;
      maximumDelta = std::max(maximumDelta,
                              static_cast<unsigned int>(a > b ? a - b : b - a));
    }
    changedPixels += changed ? 1U : 0U;
  }
  EXPECT_EQ(changedPixels, 0U)
      << "same minified A changed after an unused glyph request; atlas origins "
         "were ("
      << referenceGlyph->texCoords.topLeft.x << ','
      << referenceGlyph->texCoords.topLeft.y << ") and ("
      << prefixedGlyph->texCoords.topLeft.x << ','
      << prefixedGlyph->texCoords.topLeft.y << "); max channel delta "
      << maximumDelta;

  EXPECT_TRUE(device->takeDiagnostics().empty());
}

INSTANTIATE_TEST_SUITE_P(AllBackends, GlyphCacheMipOrderTest,
                         testing::Values(render::Backend::OpenGL,
                                         render::Backend::OpenGLES,
                                         render::Backend::Vulkan));

} // namespace
