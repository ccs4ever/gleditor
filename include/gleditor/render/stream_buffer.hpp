/**
 * @file stream_buffer.hpp
 * @brief Abstract interface for backend-agnostic persistent mapped ring
 * buffers.
 */
#ifndef GLEDITOR_RENDER_STREAM_BUFFER_HPP
#define GLEDITOR_RENDER_STREAM_BUFFER_HPP

#include <cstddef>

namespace render {

/**
 * @brief Represents a mapped memory slice within a persistent ring buffer.
 */
struct MappedChunk {
  void *ptr{nullptr};
  std::size_t offset{0};
};

/**
 * @brief Abstract interface for persistent mapped ring buffers.
 *
 * Implemented by StreamBufferGL and StreamBufferVK to allow backend-agnostic
 * staging of dynamic geometry, instances, and text quads without driver stalls.
 */
class IStreamBuffer {
public:
  virtual ~IStreamBuffer() = default;

  /// Allocate a mapped chunk of @p size bytes with alignment @p alignment.
  [[nodiscard]] virtual MappedChunk allocate(std::size_t size,
                                             std::size_t alignment = 64) = 0;

  /// Explicitly flush the mapped chunk range and insert/prepare fence sync.
  virtual void flushAndUnmap(std::size_t offset, std::size_t size) = 0;

  /// Wait on all outstanding sync fences and release them.
  virtual void waitAll() = 0;

  /// Total buffer capacity in bytes.
  [[nodiscard]] virtual std::size_t capacityBytes() const noexcept = 0;
};

} // namespace render

#endif // GLEDITOR_RENDER_STREAM_BUFFER_HPP
