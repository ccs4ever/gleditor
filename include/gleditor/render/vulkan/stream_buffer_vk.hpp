/**
 * @file stream_buffer_vk.hpp
 * @brief Lock-free, zero-copy ring staging buffer uploader for Vulkan 1.0.
 *
 * Provides persistent memory-mapped staging for dynamic GPU uploads:
 * - Staging Buffer (VK_BUFFER_USAGE_TRANSFER_SRC_BIT) for asynchronous glyph
 *   atlas DMA texture transfers (vkCmdCopyBufferToImage).
 * - Dynamic Vertex / Instance Streaming (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) for
 *   transclusion beams, carets, and reflow quad updates.
 * - Dynamic Uniform Buffer slices (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) for
 *   per-frame highlights.
 *
 * Synchronisation is tracked via explicit VkFence objects.
 */
#ifndef GLEDITOR_RENDER_VULKAN_STREAM_BUFFER_VK_HPP
#define GLEDITOR_RENDER_VULKAN_STREAM_BUFFER_VK_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <stdexcept>
#include <utility>
#include <vulkan/vulkan.h>

namespace render::vulkan {

struct MappedChunkVK {
  void *ptr{nullptr};
  std::size_t offset{0};
};

struct SyncSegmentVK {
  VkFence fence{VK_NULL_HANDLE};
  std::size_t offset{0};
  std::size_t size{0};
};

class StreamBufferVK {
public:
  StreamBufferVK(
      const VkDevice dev, const VkPhysicalDeviceMemoryProperties &memProperties,
      const VkDeviceSize capacityBytes,
      const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
      : device(dev), memoryProperties(memProperties), capacity(capacityBytes),
        head(0) {
    if (0 == capacity) {
      throw std::invalid_argument("StreamBufferVK: capacity cannot be zero");
    }

    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = capacity;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VK_SUCCESS != vkCreateBuffer(device, &info, nullptr, &buffer)) {
      throw std::runtime_error("StreamBufferVK: vkCreateBuffer failed");
    }

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device, buffer, &reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = reqs.size;
    alloc.memoryTypeIndex = findMemoryType(
        reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (VK_SUCCESS != vkAllocateMemory(device, &alloc, nullptr, &memory)) {
      vkDestroyBuffer(device, buffer, nullptr);
      throw std::runtime_error("StreamBufferVK: vkAllocateMemory failed");
    }

    if (VK_SUCCESS != vkBindBufferMemory(device, buffer, memory, 0)) {
      vkFreeMemory(device, memory, nullptr);
      vkDestroyBuffer(device, buffer, nullptr);
      throw std::runtime_error("StreamBufferVK: vkBindBufferMemory failed");
    }

    if (VK_SUCCESS !=
        vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped)) {
      vkFreeMemory(device, memory, nullptr);
      vkDestroyBuffer(device, buffer, nullptr);
      throw std::runtime_error("StreamBufferVK: vkMapMemory failed");
    }
  }

  ~StreamBufferVK() {
    waitAll();
    if (nullptr != mapped) {
      vkUnmapMemory(device, memory);
      mapped = nullptr;
    }
    if (VK_NULL_HANDLE != buffer) {
      vkDestroyBuffer(device, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
    }
    if (VK_NULL_HANDLE != memory) {
      vkFreeMemory(device, memory, nullptr);
      memory = VK_NULL_HANDLE;
    }
  }

  StreamBufferVK(const StreamBufferVK &)            = delete;
  StreamBufferVK &operator=(const StreamBufferVK &) = delete;

  StreamBufferVK(StreamBufferVK &&other) noexcept
      : device(other.device), memoryProperties(other.memoryProperties),
        buffer(other.buffer), memory(other.memory), mapped(other.mapped),
        capacity(other.capacity), head(other.head),
        syncs(std::move(other.syncs)) {
    other.buffer = VK_NULL_HANDLE;
    other.memory = VK_NULL_HANDLE;
    other.mapped = nullptr;
    other.head   = 0;
  }

  StreamBufferVK &operator=(StreamBufferVK &&other) noexcept {
    if (this != &other) {
      waitAll();
      if (nullptr != mapped) {
        vkUnmapMemory(device, memory);
      }
      if (VK_NULL_HANDLE != buffer) {
        vkDestroyBuffer(device, buffer, nullptr);
      }
      if (VK_NULL_HANDLE != memory) {
        vkFreeMemory(device, memory, nullptr);
      }

      device           = other.device;
      memoryProperties = other.memoryProperties;
      buffer           = other.buffer;
      memory           = other.memory;
      mapped           = other.mapped;
      capacity         = other.capacity;
      head             = other.head;
      syncs            = std::move(other.syncs);

      other.buffer = VK_NULL_HANDLE;
      other.memory = VK_NULL_HANDLE;
      other.mapped = nullptr;
      other.head   = 0;
    }
    return *this;
  }

  /// Allocate a mapped chunk of @p size bytes with alignment @p alignment.
  MappedChunkVK allocate(const std::size_t size,
                         const std::size_t alignment = 64) {
    if (0 == size) {
      return {static_cast<std::byte *>(mapped) + head, head};
    }
    if (size > capacity) {
      throw std::runtime_error(
          std::format("StreamBufferVK: allocation size {} exceeds capacity {}",
                      size, capacity));
    }

    // Align head
    const auto mask = alignment - 1;
    head            = (head + mask) & ~mask;

    // Wrap buffer if not enough room at the tail
    if (head + size > capacity) {
      head = 0;
    }

    waitForSpace(head, size);

    const auto allocOffset = head;
    head += size;
    return {static_cast<std::byte *>(mapped) + allocOffset, allocOffset};
  }

  /// Register a fence guarding the chunk range [offset, offset + size).
  void trackFence(const VkFence fence, const std::size_t offset,
                  const std::size_t size) {
    if (0 == size || VK_NULL_HANDLE == fence) {
      return;
    }
    syncs.push_back(SyncSegmentVK{fence, offset, size});
  }

  /// Wait on all outstanding sync fences and release tracking.
  void waitAll() {
    for (const auto &s : syncs) {
      if (VK_NULL_HANDLE != s.fence) {
        vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX);
      }
    }
    syncs.clear();
  }

  [[nodiscard]] VkBuffer vkBuffer() const { return buffer; }
  [[nodiscard]] VkDeviceSize capacityBytes() const { return capacity; }
  [[nodiscard]] void *rawMappedPtr() const { return mapped; }

private:
  [[nodiscard]] std::uint32_t
  findMemoryType(const std::uint32_t typeBits,
                 const VkMemoryPropertyFlags props) const {
    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
      if (0 != (typeBits & (1U << i)) &&
          props == (memoryProperties.memoryTypes[i].propertyFlags & props)) {
        return i;
      }
    }
    throw std::runtime_error(
        "StreamBufferVK: no memory type with the required properties");
  }

  /// Wait on any in-flight sync fence that intersects [reqOffset, reqOffset +
  /// reqSize).
  void waitForSpace(const std::size_t reqOffset, const std::size_t reqSize) {
    const auto reqEnd = reqOffset + reqSize;

    // Prune completed fences from the front lazily
    while (!syncs.empty()) {
      const auto &front = syncs.front();
      if (VK_SUCCESS == vkGetFenceStatus(device, front.fence)) {
        syncs.pop_front();
      } else {
        break;
      }
    }

    // Wait on any remaining sync segments that overlap with [reqOffset, reqEnd)
    while (!syncs.empty()) {
      const auto &front     = syncs.front();
      const auto segEnd     = front.offset + front.size;
      const bool intersects = (reqOffset < segEnd) && (front.offset < reqEnd);

      if (intersects) {
        vkWaitForFences(device, 1, &front.fence, VK_TRUE, UINT64_MAX);
        syncs.pop_front();
      } else {
        break;
      }
    }
  }

  VkDevice device{VK_NULL_HANDLE};
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceMemory memory{VK_NULL_HANDLE};
  void *mapped{nullptr};
  VkDeviceSize capacity{0};
  VkDeviceSize head{0};
  std::deque<SyncSegmentVK> syncs;
};

} // namespace render::vulkan

#endif // GLEDITOR_RENDER_VULKAN_STREAM_BUFFER_VK_HPP
