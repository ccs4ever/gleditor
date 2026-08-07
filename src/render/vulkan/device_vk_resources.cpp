/**
 * @file device_vk_resources.cpp
 * @brief Vulkan device implementation: buffers, textures, pipeline and frames.
 */
#include <gleditor/render/vulkan/device_vk.hpp> // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace render::vulkan {

namespace {

void check(const VkResult result, const char *what) {
  if (VK_SUCCESS != result) {
    throw std::runtime_error(
        std::format("Vulkan: {} failed with VkResult {}", what,
                    static_cast<int>(result)));
  }
}

VkBufferUsageFlags bufferUsage(const BufferKind kind) {
  switch (kind) {
  case BufferKind::Vertex:
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  case BufferKind::Index:
    return VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  case BufferKind::Uniform:
    return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  case BufferKind::Readback:
    return VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  }
  throw std::invalid_argument("DeviceVK: unknown buffer kind");
}

VkFormat attributeFormat(const AttributeType type, const int components) {
  if (AttributeType::UnsignedInt == type) {
    switch (components) {
    case 1:
      return VK_FORMAT_R32_UINT;
    case 2:
      return VK_FORMAT_R32G32_UINT;
    case 3:
      return VK_FORMAT_R32G32B32_UINT;
    case 4:
      return VK_FORMAT_R32G32B32A32_UINT;
    default:
      break;
    }
  } else {
    switch (components) {
    case 1:
      return VK_FORMAT_R32_SFLOAT;
    case 2:
      return VK_FORMAT_R32G32_SFLOAT;
    case 3:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case 4:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      break;
    }
  }
  throw std::invalid_argument("DeviceVK: unsupported attribute component count");
}

} // namespace

// -- memory and buffers -------------------------------------------------------

std::uint32_t DeviceVK::findMemoryType(const std::uint32_t typeBits,
                                       const VkMemoryPropertyFlags props) const {
  for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
    if (0 != (typeBits & (1U << i)) &&
        props == (memoryProperties.memoryTypes[i].propertyFlags & props)) {
      return i;
    }
  }
  throw std::runtime_error("Vulkan: no memory type with the required properties");
}

DeviceVK::BufferRecord
DeviceVK::allocateBuffer(const VkDeviceSize bytes, const VkBufferUsageFlags usage,
                         const VkMemoryPropertyFlags props) const {
  BufferRecord record{};
  record.bytes = bytes;
  record.usage = usage;

  VkBufferCreateInfo info{};
  info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size        = bytes;
  info.usage       = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(device, &info, nullptr, &record.buffer),
        "vkCreateBuffer");

  VkMemoryRequirements reqs{};
  vkGetBufferMemoryRequirements(device, record.buffer, &reqs);

  VkMemoryAllocateInfo alloc{};
  alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize  = reqs.size;
  alloc.memoryTypeIndex = findMemoryType(reqs.memoryTypeBits, props);
  check(vkAllocateMemory(device, &alloc, nullptr, &record.memory),
        "vkAllocateMemory (buffer)");
  check(vkBindBufferMemory(device, record.buffer, record.memory, 0),
        "vkBindBufferMemory");

  if (0 != (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
    check(vkMapMemory(device, record.memory, 0, VK_WHOLE_SIZE, 0,
                      &record.mapped),
          "vkMapMemory");
  }
  return record;
}

void DeviceVK::destroyBufferRecord(BufferRecord &record) const {
  if (nullptr != record.mapped) {
    vkUnmapMemory(device, record.memory);
    record.mapped = nullptr;
  }
  if (VK_NULL_HANDLE != record.buffer) {
    vkDestroyBuffer(device, record.buffer, nullptr);
    record.buffer = VK_NULL_HANDLE;
  }
  if (VK_NULL_HANDLE != record.memory) {
    vkFreeMemory(device, record.memory, nullptr);
    record.memory = VK_NULL_HANDLE;
  }
}

BufferHandle DeviceVK::createBuffer(const BufferKind kind,
                                    const std::size_t bytes) {
  // Host-visible coherent memory keeps updates to a memcpy. The data here is
  // rewritten as documents load rather than being static, so the staging copy a
  // device-local buffer would need buys nothing.
  auto record = allocateBuffer(
      std::max<VkDeviceSize>(bytes, 1), bufferUsage(kind),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  const BufferHandle handle{nextHandleId++};
  buffers.emplace(handle.id, record);
  return handle;
}

void DeviceVK::destroyBuffer(const BufferHandle buffer) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    return;
  }
  ensureIdleForMutation();
  destroyBufferRecord(it->second);
  buffers.erase(it);
}

void DeviceVK::updateBuffer(const BufferHandle buffer, const std::size_t offset,
                            const std::span<const std::byte> data) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    throw std::invalid_argument("DeviceVK::updateBuffer: unknown buffer");
  }
  if (offset + data.size() > it->second.bytes) {
    throw std::out_of_range("DeviceVK::updateBuffer: write past end of buffer");
  }
  if (data.empty()) {
    return;
  }
  ensureIdleForMutation();
  std::memcpy(static_cast<std::byte *>(it->second.mapped) + offset, data.data(),
              data.size());
}

BufferHandle DeviceVK::growBuffer(const BufferHandle buffer,
                                  const std::size_t bytes) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    throw std::invalid_argument("DeviceVK::growBuffer: unknown buffer");
  }
  if (bytes <= it->second.bytes) {
    return buffer;
  }
  ensureIdleForMutation();

  auto grown = allocateBuffer(bytes, it->second.usage,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  // Both buffers are mapped, so carrying the contents forward is a plain copy
  // rather than a queue operation.
  std::memcpy(grown.mapped, it->second.mapped, it->second.bytes);
  destroyBufferRecord(it->second);
  it->second = grown;
  return buffer;
}

// -- one-shot transfers -------------------------------------------------------

VkCommandBuffer DeviceVK::beginOneShot() const {
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool        = commandPool;
  alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;

  VkCommandBuffer commands = VK_NULL_HANDLE;
  check(vkAllocateCommandBuffers(device, &alloc, &commands),
        "vkAllocateCommandBuffers (one shot)");

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
  return commands;
}

void DeviceVK::endOneShot(const VkCommandBuffer commands) const {
  check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

  VkSubmitInfo submit{};
  submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers    = &commands;
  check(vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE),
        "vkQueueSubmit (one shot)");
  check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle (one shot)");
  vkFreeCommandBuffers(device, commandPool, 1, &commands);
}

// -- textures -----------------------------------------------------------------

TextureHandle DeviceVK::createTextureArray(const int size, const int layers,
                                           const TextureFormat format,
                                           const int levels) {
  if (TextureFormat::R8 != format) {
    throw std::invalid_argument("DeviceVK: unsupported texture format");
  }

  TextureRecord record{};
  record.size   = size;
  record.layers = layers;
  record.levels = std::max(1, levels);

  VkImageCreateInfo info{};
  info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType     = VK_IMAGE_TYPE_2D;
  info.format        = VK_FORMAT_R8_UNORM;
  info.extent        = {static_cast<std::uint32_t>(size),
                        static_cast<std::uint32_t>(size), 1};
  info.mipLevels     = static_cast<std::uint32_t>(record.levels);
  info.arrayLayers   = static_cast<std::uint32_t>(layers);
  info.samples       = VK_SAMPLE_COUNT_1_BIT;
  info.tiling        = VK_IMAGE_TILING_OPTIMAL;
  // TRANSFER_SRC as well as DST: the mip chain is built by blitting each level
  // from the one above it, so every level is read as well as written.
  info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
  info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  check(vkCreateImage(device, &info, nullptr, &record.image),
        "vkCreateImage (atlas)");

  VkMemoryRequirements reqs{};
  vkGetImageMemoryRequirements(device, record.image, &reqs);
  VkMemoryAllocateInfo alloc{};
  alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize  = reqs.size;
  alloc.memoryTypeIndex =
      findMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(vkAllocateMemory(device, &alloc, nullptr, &record.memory),
        "vkAllocateMemory (atlas)");
  check(vkBindImageMemory(device, record.image, record.memory, 0),
        "vkBindImageMemory (atlas)");

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image            = record.image;
  viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  viewInfo.format           = VK_FORMAT_R8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                               static_cast<std::uint32_t>(record.levels), 0,
                               static_cast<std::uint32_t>(layers)};
  check(vkCreateImageView(device, &viewInfo, nullptr, &record.view),
        "vkCreateImageView (atlas)");

  // Move the whole array to the layout the shader samples from once, up front;
  // uploads transition individual layers in and out of it as needed.
  const auto commands = beginOneShot();
  VkImageMemoryBarrier barrier{};
  barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image            = record.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                              static_cast<std::uint32_t>(record.levels), 0,
                              static_cast<std::uint32_t>(layers)};
  barrier.srcAccessMask    = 0;
  barrier.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
  endOneShot(commands);

  const TextureHandle handle{nextHandleId++};
  textures.emplace(handle.id, record);
  return handle;
}

void DeviceVK::generateMipmaps(const TextureHandle texture) {
  const auto it = textures.find(texture.id);
  if (textures.end() == it || 1 >= it->second.levels) {
    return;
  }
  const auto &record = it->second;
  const auto layers  = static_cast<std::uint32_t>(record.layers);

  // Vulkan has no glGenerateMipmap: each level is blitted from the one above
  // it, and every step needs the source moved to TRANSFER_SRC and the
  // destination to TRANSFER_DST first. Linear filtering during the blit is
  // what does the averaging, so the format has to advertise it.
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R8_UNORM,
                                      &props);
  if (0 == (props.optimalTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
    // Without it the blit would fall back to nearest, which is not a mip chain
    // so much as a subsample of one. Leaving the levels as they are and saying
    // so beats quietly producing a worse image than no mipmaps at all.
    diagnostics.record(DiagnosticSeverity::Warning,
                       "the driver cannot filter R8 while blitting, so the "
                       "glyph atlas keeps a single mip level");
    return;
  }

  ensureIdleForMutation();
  const auto commands = beginOneShot();

  VkImageMemoryBarrier barrier{};
  barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image               = record.image;

  const auto transition = [&](const std::uint32_t level,
                              const VkImageLayout from, const VkImageLayout to,
                              const VkAccessFlags srcAccess,
                              const VkAccessFlags dstAccess,
                              const VkPipelineStageFlags srcStage,
                              const VkPipelineStageFlags dstStage) {
    barrier.oldLayout        = from;
    barrier.newLayout        = to;
    barrier.srcAccessMask    = srcAccess;
    barrier.dstAccessMask    = dstAccess;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, layers};
    vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
  };

  // Level zero holds the uploaded glyphs and is being sampled; the rest hold
  // whatever the last generation left and are about to be overwritten.
  transition(0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);

  auto extent = record.size;
  for (std::uint32_t level = 1; level < static_cast<std::uint32_t>(record.levels);
       level++) {
    const auto next = std::max(1, extent / 2);

    transition(level, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, layers};
    blit.srcOffsets[1]  = {extent, extent, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
    blit.dstOffsets[1]  = {next, next, 1};
    vkCmdBlitImage(commands, record.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, record.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    // This level becomes the next one's source, so it has to be readable
    // before the following blit rather than after the loop.
    transition(level, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    extent = next;
  }

  // Every level is now TRANSFER_SRC; hand the lot back to the shader.
  barrier.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                              static_cast<std::uint32_t>(record.levels), 0,
                              layers};
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  endOneShot(commands);
}

void DeviceVK::destroyTexture(const TextureHandle texture) {
  const auto it = textures.find(texture.id);
  if (textures.end() == it) {
    return;
  }
  ensureIdleForMutation();
  vkDestroyImageView(device, it->second.view, nullptr);
  vkDestroyImage(device, it->second.image, nullptr);
  vkFreeMemory(device, it->second.memory, nullptr);
  textures.erase(it);
}

void DeviceVK::updateTextureLayer(const TextureHandle texture, const int layer,
                                  const int xOffset, const int yOffset,
                                  const int width, const int height,
                                  const std::span<const std::byte> data) {
  if (0 == width || 0 == height) {
    return;
  }
  const auto it = textures.find(texture.id);
  if (textures.end() == it) {
    throw std::invalid_argument("DeviceVK::updateTextureLayer: unknown texture");
  }
  const auto expected =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (data.size() < expected) {
    throw std::invalid_argument(
        "DeviceVK::updateTextureLayer: data shorter than the given rectangle");
  }

  ensureIdleForMutation();

  auto staging = allocateBuffer(expected, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  std::memcpy(staging.mapped, data.data(), expected);

  const auto commands = beginOneShot();

  VkImageMemoryBarrier barrier{};
  barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image               = it->second.image;
  barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                 static_cast<std::uint32_t>(layer), 1};

  barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &barrier);

  VkBufferImageCopy region{};
  region.bufferOffset      = 0;
  region.bufferRowLength   = 0; // tightly packed
  region.bufferImageHeight = 0;
  region.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                              static_cast<std::uint32_t>(layer), 1};
  region.imageOffset       = {xOffset, yOffset, 0};
  region.imageExtent       = {static_cast<std::uint32_t>(width),
                              static_cast<std::uint32_t>(height), 1};
  vkCmdCopyBufferToImage(commands, staging.buffer, it->second.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  endOneShot(commands);

  auto disposable = staging;
  destroyBufferRecord(disposable);
}

// -- pipeline -----------------------------------------------------------------

std::vector<std::uint32_t> DeviceVK::readSpirv(const std::string &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream.is_open()) {
    throw std::runtime_error(
        "Vulkan: cannot open SPIR-V module " + path +
        ". Run `make shaders` to compile the portable shaders.");
  }
  const auto size = static_cast<std::size_t>(stream.tellg());
  if (0 != size % sizeof(std::uint32_t)) {
    throw std::runtime_error("Vulkan: SPIR-V module has a bad size: " + path);
  }
  std::vector<std::uint32_t> code(size / sizeof(std::uint32_t));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(code.data()),
              static_cast<std::streamsize>(size));
  return code;
}

VkShaderModule
DeviceVK::createShaderModule(const std::vector<std::uint32_t> &code) const {
  VkShaderModuleCreateInfo info{};
  info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = code.size() * sizeof(std::uint32_t);
  info.pCode    = code.data();

  VkShaderModule module = VK_NULL_HANDLE;
  check(vkCreateShaderModule(device, &info, nullptr, &module),
        "vkCreateShaderModule");
  return module;
}

PipelineHandle DeviceVK::createPipeline(const PipelineDesc &desc) {
  PipelineRecord record{};

  // Descriptor layout: highlights, glyph atlas. Both are read by the fragment
  // stage -- selection depends on where inside a quad a fragment sits, which
  // only exists per fragment. The transform is a push constant instead, so a
  // per-draw change costs no descriptor traffic and a recorded frame can hold a
  // different one for every draw.
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};

  VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
  setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  setLayoutInfo.bindingCount = bindings.size();
  setLayoutInfo.pBindings    = bindings.data();
  check(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr,
                                    &record.setLayout),
        "vkCreateDescriptorSetLayout");

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pushRange.offset     = 0;
  pushRange.size       = sizeof(DrawUniforms);

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount         = 1;
  layoutInfo.pSetLayouts            = &record.setLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges    = &pushRange;
  check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &record.layout),
        "vkCreatePipelineLayout");

  const auto vertModule =
      createShaderModule(readSpirv(desc.spirvDir + "/glyph.vert.spv"));
  const auto fragModule =
      createShaderModule(readSpirv(desc.spirvDir + "/glyph.frag.spv"));

  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName  = "main";
  stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName  = "main";

  // One binding, stepping once per instance: the quad's four vertices carry no
  // data and are synthesised from gl_VertexIndex.
  VkVertexInputBindingDescription binding{};
  binding.binding   = 0;
  binding.stride    = desc.layout.stride;
  binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

  std::vector<VkVertexInputAttributeDescription> attributes;
  attributes.reserve(desc.layout.attributes.size());
  for (const auto &attribute : desc.layout.attributes) {
    attributes.push_back(VkVertexInputAttributeDescription{
        attribute.location, 0,
        attributeFormat(attribute.type, attribute.components),
        attribute.offset});
  }

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions    = &binding;
  vertexInput.vertexAttributeDescriptionCount =
      static_cast<std::uint32_t>(attributes.size());
  vertexInput.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount  = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // The strip's two triangles wind opposite ways, so culling would drop half of
  // every glyph.
  raster.cullMode  = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0F;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
  depthStencil.depthWriteEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
  depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

  // One blend state per colour attachment; the picking target takes the same
  // straight write the colour target does.
  std::array<VkPipelineColorBlendAttachmentState, 2> blendAttachments{};
  for (auto &attachment : blendAttachments) {
    attachment.blendEnable = VK_FALSE;
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }

  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = blendAttachments.size();
  blend.pAttachments    = blendAttachments.data();

  // Viewport and scissor are dynamic so a resize does not need the pipeline
  // rebuilt.
  const std::array dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = dynamicStates.size();
  dynamic.pDynamicStates    = dynamicStates.data();

  VkGraphicsPipelineCreateInfo info{};
  info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount          = stages.size();
  info.pStages             = stages.data();
  info.pVertexInputState   = &vertexInput;
  info.pInputAssemblyState = &assembly;
  info.pViewportState      = &viewportState;
  info.pRasterizationState = &raster;
  info.pMultisampleState   = &multisample;
  info.pDepthStencilState  = &depthStencil;
  info.pColorBlendState    = &blend;
  info.pDynamicState       = &dynamic;
  info.layout              = record.layout;
  info.renderPass          = renderPass;
  info.subpass             = 0;

  const auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info,
                                                nullptr, &record.pipeline);
  vkDestroyShaderModule(device, vertModule, nullptr);
  vkDestroyShaderModule(device, fragModule, nullptr);
  check(result, "vkCreateGraphicsPipelines");

  // One descriptor set per frame in flight.
  const std::array<VkDescriptorSetLayout, framesInFlight> setLayouts = {
      record.setLayout, record.setLayout};
  VkDescriptorSetAllocateInfo setAlloc{};
  setAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  setAlloc.descriptorPool     = descriptorPool;
  setAlloc.descriptorSetCount = framesInFlight;
  setAlloc.pSetLayouts        = setLayouts.data();
  check(vkAllocateDescriptorSets(device, &setAlloc, record.sets.data()),
        "vkAllocateDescriptorSets");

  const PipelineHandle handle{nextHandleId++};
  pipelines.emplace(handle.id, record);
  return handle;
}

} // namespace render::vulkan
// vi: set sw=2 sts=2 ts=2 et:
