/**
 * @file device_capabilities.cpp
 * @brief Tests for what a device says it can do, and for the batched draw path
 *        that capability exists to serve.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gleditor/render/device.hpp>
#include <gleditor/render/gl/device_gl.hpp>
#include <gleditor/render/types.hpp>

#include "mocks/device.hpp"

#ifdef GLEDITOR_ENABLE_VULKAN
#include <gleditor/render/vulkan/device_vk.hpp>
#endif

#include <array>
#include <vector>

using testing::InSequence;

// A device that says nothing about itself must be taken to be the simplest
// kind. Defaulting the other way would have callers assume threading a backend
// cannot do.
TEST(DeviceCapabilities, defaultsAreConservative) {
  const render::DeviceCapabilities caps;
  EXPECT_FALSE(caps.parallelCommandRecording);
  EXPECT_EQ(1U, caps.recordingThreads);
}

// Not a statement about speed: a GL context is current on one thread, so there
// is no supported way for a second one to record part of a frame.
TEST(DeviceCapabilities, theGlFamilyCannotRecordInParallel) {
  for (const auto backend :
       {render::Backend::OpenGL, render::Backend::OpenGLES}) {
    const render::gl::DeviceGL device(backend);
    const auto caps = device.capabilities();
    EXPECT_FALSE(caps.parallelCommandRecording) << render::backendName(backend);
    EXPECT_EQ(1U, caps.recordingThreads) << render::backendName(backend);
  }
}

#ifdef GLEDITOR_ENABLE_VULKAN
// The claim the whole feature rests on. Reported from the pool the device
// actually allocated, so a machine that gives back one core reports one thread
// and no parallel recording rather than promising something it will not do.
TEST(DeviceCapabilities, vulkanReportsItsRecordingThreads) {
  const render::vulkan::DeviceVK device;
  const auto caps = device.capabilities();
  EXPECT_GE(caps.recordingThreads, 1U);
  EXPECT_EQ(caps.parallelCommandRecording, caps.recordingThreads > 1);
}
#endif

namespace {

render::GlyphBatch batchAt(const std::uint32_t instances,
                           const std::size_t offset) {
  render::GlyphBatch batch;
  batch.vertices         = render::BufferHandle{7};
  batch.vertexByteOffset = offset;
  batch.instanceCount    = instances;
  return batch;
}

} // namespace

// The default implementation is what every backend but Vulkan uses, so the
// batched call has to mean exactly the same thing as the one-at-a-time calls
// it replaced -- same draws, same order.
TEST(GlyphBatches, theDefaultIssuesOneDrawPerBatchInOrder) {
  MockRenderDevice device;
  const std::array batches = {batchAt(3, 0), batchAt(5, 64), batchAt(1, 128)};

  {
    const InSequence ordered;
    for (const auto &batch : batches) {
      EXPECT_CALL(device,
                  drawGlyphs(testing::_, batch.vertices, batch.vertexByteOffset,
                             batch.instanceCount));
    }
  }

  device.drawGlyphBatches(batches);
}

TEST(GlyphBatches, anEmptyRunDrawsNothing) {
  MockRenderDevice device;
  EXPECT_CALL(device,
              drawGlyphs(testing::_, testing::_, testing::_, testing::_))
      .Times(0);
  device.drawGlyphBatches(std::vector<render::GlyphBatch>{});
}

// The transform is per batch, and mixing them up would draw every page at one
// page's position -- a failure that still fills the screen with text.
TEST(GlyphBatches, eachBatchKeepsItsOwnTransform) {
  MockRenderDevice device;
  std::array batches          = {batchAt(1, 0), batchAt(1, 64)};
  batches[0].uniforms.mvp[12] = 11.0F;
  batches[1].uniforms.mvp[12] = 22.0F;

  std::vector<float> seen;
  EXPECT_CALL(device,
              drawGlyphs(testing::_, testing::_, testing::_, testing::_))
      .WillRepeatedly(
          [&seen](const render::DrawUniforms &uniforms, render::BufferHandle,
                  std::size_t,
                  std::uint32_t) { seen.push_back(uniforms.mvp[12]); });

  device.drawGlyphBatches(batches);
  EXPECT_THAT(seen, testing::ElementsAre(11.0F, 22.0F));
}
