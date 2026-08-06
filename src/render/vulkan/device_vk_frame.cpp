/**
 * @file device_vk_frame.cpp
 * @brief Vulkan device implementation: frame recording, submission and capture.
 */
#include <gleditor/render/vulkan/device_vk.hpp> // IWYU pragma: associated

#include <gleditor/sdl_compat.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

#include <gleditor/sdl_wrap.hpp>

namespace render::vulkan {

namespace {

void check(const VkResult result, const char *what) {
  if (VK_SUCCESS != result) {
    throw std::runtime_error(
        std::format("Vulkan: {} failed with VkResult {}", what,
                    static_cast<int>(result)));
  }
}

/// Insert a layout transition for a single-layer colour image.
void transitionColour(const VkCommandBuffer commands, const VkImage image,
                      const VkImageLayout from, const VkImageLayout to,
                      const VkAccessFlags srcAccess,
                      const VkAccessFlags dstAccess,
                      const VkPipelineStageFlags srcStage,
                      const VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier barrier{};
  barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout           = from;
  barrier.newLayout           = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image               = image;
  barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barrier.srcAccessMask       = srcAccess;
  barrier.dstAccessMask       = dstAccess;
  vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0, nullptr,
                       1, &barrier);
}

} // namespace

bool DeviceVK::beginFrame() {
  if (!initialised) {
    return false;
  }

  auto &frame = frames[frameIndex];

  // Wait for the frame slot this submission will reuse.
  check(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
        "vkWaitForFences");

  const auto acquired =
      vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frame.imageAvailable,
                            VK_NULL_HANDLE, &acquiredImage);
  if (VK_ERROR_OUT_OF_DATE_KHR == acquired) {
    // The surface changed size behind our back; rebuild and skip this frame.
    int width  = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(targetWindow->window, &width, &height);
    recreateSwapchain(std::max(width, 1), std::max(height, 1));
    return false;
  }
  if (VK_SUCCESS != acquired && VK_SUBOPTIMAL_KHR != acquired) {
    check(acquired, "vkAcquireNextImageKHR");
  }

  check(vkResetFences(device, 1, &frame.inFlight), "vkResetFences");
  check(vkResetCommandBuffer(frame.commands, 0), "vkResetCommandBuffer");

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer");

  std::array<VkClearValue, 3> clears{};
  clears[0].color = VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}};
  // The picking attachment is an unsigned integer target, so it takes an
  // integer clear rather than the float one the colour target uses.
  clears[1].color        = VkClearColorValue{.uint32 = {0, 0, 0, 0}};
  clears[2].depthStencil = {1.0F, 0};

  VkRenderPassBeginInfo passInfo{};
  passInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  passInfo.renderPass      = renderPass;
  passInfo.framebuffer     = framebuffer;
  passInfo.renderArea      = {{0, 0}, swapchainExtent};
  passInfo.clearValueCount = clears.size();
  passInfo.pClearValues    = clears.data();
  vkCmdBeginRenderPass(frame.commands, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

  const VkViewport viewport{0.0F,
                            0.0F,
                            static_cast<float>(swapchainExtent.width),
                            static_cast<float>(swapchainExtent.height),
                            0.0F,
                            1.0F};
  const VkRect2D scissor{{0, 0}, swapchainExtent};
  vkCmdSetViewport(frame.commands, 0, 1, &viewport);
  vkCmdSetScissor(frame.commands, 0, 1, &scissor);

  frameActive = true;
  return true;
}

void DeviceVK::bindPipeline(const PipelineHandle pipeline) {
  const auto it = pipelines.find(pipeline.id);
  if (pipelines.end() == it) {
    throw std::invalid_argument("DeviceVK::bindPipeline: unknown pipeline");
  }
  boundPipeline = pipeline;
  if (frameActive) {
    vkCmdBindPipeline(frames[frameIndex].commands,
                      VK_PIPELINE_BIND_POINT_GRAPHICS, it->second.pipeline);
  }
}

void DeviceVK::bindGlyphTexture(const TextureHandle texture) {
  boundTexture = texture;

  const auto pipelineIt = pipelines.find(boundPipeline.id);
  const auto textureIt  = textures.find(texture.id);
  if (pipelines.end() == pipelineIt || textures.end() == textureIt) {
    return;
  }

  const auto highlightIt = buffers.find(highlightBuffer.id);
  if (buffers.end() == highlightIt) {
    return;
  }

  const VkDescriptorBufferInfo highlightInfo{highlightIt->second.buffer, 0,
                                             highlightIt->second.bytes};
  const VkDescriptorImageInfo imageInfo{glyphSampler, textureIt->second.view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

  const auto set = pipelineIt->second.sets[frameIndex];
  std::array<VkWriteDescriptorSet, 2> writes{};
  writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet          = set;
  writes[0].dstBinding      = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[0].pBufferInfo     = &highlightInfo;
  writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet          = set;
  writes[1].dstBinding      = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].pImageInfo      = &imageInfo;

  vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);

  if (frameActive) {
    vkCmdBindDescriptorSets(frames[frameIndex].commands,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineIt->second.layout, 0, 1, &set, 0, nullptr);
  }
}

void DeviceVK::setHighlights(const std::span<const HighlightRange> ranges) {
  const auto it = buffers.find(highlightBuffer.id);
  if (buffers.end() == it) {
    return;
  }
  const auto count = std::min<std::size_t>(ranges.size(), maxHighlightRanges);
  ensureIdleForMutation();
  std::memcpy(it->second.mapped, ranges.data(), count * sizeof(HighlightRange));
}

void DeviceVK::drawGlyphs(const DrawUniforms &uniforms,
                          const BufferHandle vertices,
                          const std::size_t vertexByteOffset,
                          const std::uint32_t instanceCount) {
  if (!frameActive || 0 == instanceCount) {
    return;
  }
  const auto pipelineIt = pipelines.find(boundPipeline.id);
  const auto bufferIt   = buffers.find(vertices.id);
  if (pipelines.end() == pipelineIt || buffers.end() == bufferIt) {
    throw std::invalid_argument("DeviceVK::drawGlyphs: unknown pipeline or buffer");
  }

  auto &frame = frames[frameIndex];

  // Vulkan's clip space has +Y pointing down, OpenGL's points up. Callers hand
  // over one conventional transform, so the backend that differs is the one
  // that adapts: negating the row of the transform that produces clip-space Y
  // -- premultiplying by diag(1, -1, 1, 1) -- puts the image the same way up as
  // the other backends, which is what makes their output directly comparable.
  // In the column-major layout that row is elements 1, 5, 9 and 13. Winding is
  // unaffected in practice because the glyph pipeline does not cull.
  DrawUniforms flipped = uniforms;
  for (std::size_t i = 1; i < flipped.mvp.size(); i += 4) {
    flipped.mvp[i] = -flipped.mvp[i];
  }

  vkCmdPushConstants(frame.commands, pipelineIt->second.layout,
                     VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(DrawUniforms),
                     &flipped);

  // Binding the vertex buffer at an offset is how the draw is aimed at one
  // page's rows, matching what the GL backend does with attribute pointers.
  const VkDeviceSize offset = vertexByteOffset;
  vkCmdBindVertexBuffers(frame.commands, 0, 1, &bufferIt->second.buffer,
                         &offset);
  vkCmdDraw(frame.commands, 4, instanceCount, 0, 0);
}

void DeviceVK::requestPickingTag(const int x, const int y) {
  if (!frameActive) {
    return;
  }
  if (x < 0 || y < 0 || x >= static_cast<int>(swapchainExtent.width) ||
      y >= static_cast<int>(swapchainExtent.height)) {
    return;
  }

  auto &frame = frames[frameIndex];
  if (frame.pickPending) {
    // This slot's previous result has not been collected. Dropping the request
    // keeps the frame moving; the caller asks again next frame.
    return;
  }

  // The copy itself cannot be recorded here: the render pass is still open and
  // the tag image only reaches TRANSFER_SRC layout when the pass ends. Note the
  // pixel now and emit the copy in endFrame().
  frame.pickX       = x;
  frame.pickY       = y;
  frame.pickPending = true;
}

std::optional<PickingResult> DeviceVK::takePickingTag() {
  // Frames are submitted in slot order, so the oldest outstanding pick is in
  // the slot that will be recorded next.
  for (std::uint32_t i = 0; i < framesInFlight; i++) {
    auto &frame = frames[(frameIndex + i) % framesInFlight];
    if (!frame.pickPending || !frame.pickSubmitted) {
      continue;
    }

    // A zero timeout makes this a poll rather than a stall: the copy landed
    // when the frame that carried it completed, which is what its fence says.
    if (VK_SUCCESS !=
        vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, 0)) {
      continue;
    }

    frame.pickPending   = false;
    frame.pickSubmitted = false;

    const auto bufferIt = buffers.find(frame.pickingBuffer.id);
    if (buffers.end() == bufferIt || nullptr == bufferIt->second.mapped) {
      continue;
    }
    const auto *values = static_cast<const std::uint32_t *>(bufferIt->second.mapped);
    return PickingResult{frame.pickX, frame.pickY,
                         PickingTag{values[0], values[1]}};
  }
  return std::nullopt;
}

void DeviceVK::endFrame() {
  if (!frameActive) {
    return;
  }
  frameActive = false;

  // Raise anything validation objected to while this frame was recorded, from
  // here rather than from inside the messenger, where unwinding through the
  // driver's stack would be undefined.
  diagnostics.raiseIfError("vulkan validation reported an error");

  auto &frame = frames[frameIndex];
  vkCmdEndRenderPass(frame.commands);

  if (frame.pickPending && !frame.pickSubmitted) {
    // The render pass left the tag image in TRANSFER_SRC layout, so the single
    // pixel can be copied out without an extra barrier. The copy rides along
    // with the frame and completes when its fence signals.
    const auto pickIt = buffers.find(frame.pickingBuffer.id);
    if (buffers.end() != pickIt) {
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset      = {frame.pickX, frame.pickY, 0};
      region.imageExtent      = {1, 1, 1};
      vkCmdCopyImageToBuffer(frame.commands, tagImage,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             pickIt->second.buffer, 1, &region);
      frame.pickSubmitted = true;
    } else {
      frame.pickPending = false;
    }
  }

  // Present by blitting the offscreen colour target onto the acquired
  // swapchain image, which also resolves any difference in their formats.
  const VkImage swapImage = swapchainImages[acquiredImage];
  transitionColour(frame.commands, swapImage, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1]  = {static_cast<std::int32_t>(swapchainExtent.width),
                         static_cast<std::int32_t>(swapchainExtent.height), 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.dstOffsets[1]  = blit.srcOffsets[1];
  vkCmdBlitImage(frame.commands, colourImage,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                 VK_FILTER_NEAREST);

  transitionColour(frame.commands, swapImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

  check(vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer");

  const VkPipelineStageFlags waitStage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{};
  submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount   = 1;
  submit.pWaitSemaphores      = &frame.imageAvailable;
  submit.pWaitDstStageMask    = &waitStage;
  submit.commandBufferCount   = 1;
  submit.pCommandBuffers      = &frame.commands;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores    = &frame.renderFinished;
  check(vkQueueSubmit(graphicsQueue, 1, &submit, frame.inFlight),
        "vkQueueSubmit");

  VkPresentInfoKHR present{};
  present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores    = &frame.renderFinished;
  present.swapchainCount     = 1;
  present.pSwapchains        = &swapchain;
  present.pImageIndices      = &acquiredImage;

  const auto presented = vkQueuePresentKHR(presentQueue, &present);
  if (VK_ERROR_OUT_OF_DATE_KHR == presented || VK_SUBOPTIMAL_KHR == presented) {
    swapchainOutOfDate = true;
  } else if (VK_SUCCESS != presented) {
    check(presented, "vkQueuePresentKHR");
  }

  framesSubmitted = true;
  frameIndex      = (frameIndex + 1) % framesInFlight;

  if (swapchainOutOfDate) {
    int width  = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(targetWindow->window, &width, &height);
    recreateSwapchain(std::max(width, 1), std::max(height, 1));
  }
}

FrameImage DeviceVK::captureColorTarget() {
  FrameImage image;
  image.width  = static_cast<int>(swapchainExtent.width);
  image.height = static_cast<int>(swapchainExtent.height);

  const auto pixels = static_cast<VkDeviceSize>(swapchainExtent.width) *
                      swapchainExtent.height;
  const auto bytes = pixels * 4;
  image.rgba.resize(bytes);

  // Called after endFrame(), so the colour target holds a finished image and
  // the render pass has already left it in TRANSFER_SRC layout. Waiting for
  // the queue is all the synchronisation the copy needs.
  vkDeviceWaitIdle(device);
  framesSubmitted = false;

  auto staging = allocateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  const auto commands = beginOneShot();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent      = {swapchainExtent.width, swapchainExtent.height, 1};
  vkCmdCopyImageToBuffer(commands, colourImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer,
                         1, &region);
  endOneShot(commands);

  // colourFormat is R8G8B8A8_UNORM, which is already the channel order
  // FrameImage promises, and Vulkan's framebuffer origin is top left like
  // FrameImage's.
  std::memcpy(image.rgba.data(), staging.mapped, bytes);

  auto disposable = staging;
  destroyBufferRecord(disposable);
  return image;
}

} // namespace render::vulkan
// vi: set sw=2 sts=2 ts=2 et:
