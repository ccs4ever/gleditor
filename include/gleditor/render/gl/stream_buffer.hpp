/**
 * @file stream_buffer.hpp
 * @brief Lock-free, zero-copy ring buffer uploader for OpenGL 3.3 and OpenGL
 * ES 3.0.
 *
 * Provides persistent memory-mapped staging for high-frequency dynamic GPU
 * uploads:
 * - Pixel Unpack Buffers (GL_PIXEL_UNPACK_BUFFER) for asynchronous glyph atlas
 *   DMA texture streaming.
 * - Dynamic Vertex Arrays (GL_ARRAY_BUFFER) for streaming inter-document
 *   transclusion beams, carets, toasts, and text reflow instances.
 * - Dynamic Uniform Buffers (GL_UNIFORM_BUFFER) for per-frame highlights and
 *   selections.
 *
 * Synchronization is handled via explicit range flushes
 * (glFlushMappedBufferRange), unsynchronized mapping
 * (GL_MAP_UNSYNCHRONIZED_BIT), and sync fences (glFenceSync).
 */
#ifndef GLEDITOR_RENDER_GL_STREAM_BUFFER_HPP
#define GLEDITOR_RENDER_GL_STREAM_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <stdexcept>
#include <utility>

#include <gleditor/render/gl/gl_api.hpp>

namespace render::gl {

struct MappedChunk {
  void *ptr{nullptr};
  std::size_t offset{0};
};

struct SyncSegment {
  GLsync sync{nullptr};
  std::size_t offset{0};
  std::size_t size{0};
};

class StreamBufferGL {
public:
  StreamBufferGL(const GLApi &glApi, const GLenum target,
                 const std::size_t capacityBytes)
      : api(glApi), targetKind(target), capacity(capacityBytes), head(0) {
    if (0 == capacity) {
      throw std::invalid_argument("StreamBufferGL: capacity cannot be zero");
    }

    api.GenBuffers(1, &bufferId);
    api.BindBuffer(targetKind, bufferId);
    api.BufferData(targetKind, static_cast<GLsizeiptr>(capacity), nullptr,
                   GL_DYNAMIC_DRAW);
    api.BindBuffer(targetKind, 0);
  }

  ~StreamBufferGL() {
    waitAll();
    if (0 != bufferId) {
      api.DeleteBuffers(1, &bufferId);
      bufferId = 0;
    }
  }

  StreamBufferGL(const StreamBufferGL &)            = delete;
  StreamBufferGL &operator=(const StreamBufferGL &) = delete;
  StreamBufferGL(StreamBufferGL &&other) noexcept
      : api(other.api), targetKind(other.targetKind), bufferId(other.bufferId),
        capacity(other.capacity), head(other.head),
        syncs(std::move(other.syncs)) {
    other.bufferId = 0;
    other.head     = 0;
  }
  StreamBufferGL &operator=(StreamBufferGL &&other) noexcept {
    if (this != &other) {
      waitAll();
      if (0 != bufferId) {
        api.DeleteBuffers(1, &bufferId);
      }
      bufferId       = other.bufferId;
      capacity       = other.capacity;
      head           = other.head;
      syncs          = std::move(other.syncs);
      other.bufferId = 0;
      other.head     = 0;
    }
    return *this;
  }

  /// Allocate a mapped chunk of @p size bytes with alignment @p alignment.
  MappedChunk allocate(const std::size_t size,
                       const std::size_t alignment = 64) {
    if (0 == size) {
      return {nullptr, head};
    }
    if (size > capacity) {
      throw std::runtime_error(
          std::format("StreamBufferGL: allocation size {} exceeds capacity {}",
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

    api.BindBuffer(targetKind, bufferId);
    void *ptr = api.MapBufferRange(
        targetKind, static_cast<GLintptr>(head), static_cast<GLsizeiptr>(size),
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
            GL_MAP_FLUSH_EXPLICIT_BIT);

    if (nullptr == ptr) {
      api.BindBuffer(targetKind, 0);
      throw std::runtime_error(std::format(
          "StreamBufferGL: glMapBufferRange failed at offset {} size {}", head,
          size));
    }

    const auto allocOffset = head;
    head += size;
    return {ptr, allocOffset};
  }

  /// Explicitly flush the mapped chunk range and insert a fence sync.
  void flushAndUnmap(const std::size_t offset, const std::size_t size) {
    if (0 == size) {
      return;
    }
    api.BindBuffer(targetKind, bufferId);
    // Offset in glFlushMappedBufferRange is relative to the mapped range start
    api.FlushMappedBufferRange(targetKind, 0, static_cast<GLsizeiptr>(size));
    api.UnmapBuffer(targetKind);
    api.BindBuffer(targetKind, 0);

    const auto fence = api.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (nullptr != fence) {
      syncs.push_back(SyncSegment{fence, offset, size});
    }
  }

  /// Wait on all outstanding sync fences and release them.
  void waitAll() {
    for (auto &s : syncs) {
      if (nullptr != s.sync) {
        api.ClientWaitSync(s.sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                           GL_TIMEOUT_IGNORED);
        api.DeleteSync(s.sync);
      }
    }
    syncs.clear();
  }

  [[nodiscard]] GLuint id() const { return bufferId; }
  [[nodiscard]] GLenum target() const { return targetKind; }
  [[nodiscard]] std::size_t capacityBytes() const { return capacity; }

private:
  /// Wait on any in-flight sync fence that intersects [reqOffset, reqOffset +
  /// reqSize).
  void waitForSpace(const std::size_t reqOffset, const std::size_t reqSize) {
    const auto reqEnd = reqOffset + reqSize;

    // Prune completed fences from the front lazily
    while (!syncs.empty()) {
      const auto &front = syncs.front();
      const auto res    = api.ClientWaitSync(front.sync, 0, 0);
      if (GL_ALREADY_SIGNALED == res || GL_CONDITION_SATISFIED == res) {
        api.DeleteSync(front.sync);
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
        api.ClientWaitSync(front.sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                           GL_TIMEOUT_IGNORED);
        api.DeleteSync(front.sync);
        syncs.pop_front();
      } else {
        break;
      }
    }
  }

  const GLApi &api;
  GLenum targetKind{0};
  GLuint bufferId{0};
  std::size_t capacity{0};
  std::size_t head{0};
  std::deque<SyncSegment> syncs;
};

} // namespace render::gl

#endif // GLEDITOR_RENDER_GL_STREAM_BUFFER_HPP
