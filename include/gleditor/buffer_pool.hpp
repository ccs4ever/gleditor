/**
 * @file buffer_pool.hpp
 * @brief Sub-allocator over a single device vertex buffer.
 *
 * Documents reserve contiguous runs of fixed-size rows and write into them.
 * The pool grows the underlying buffer when a request cannot be satisfied,
 * preserving previously handed out offsets so live allocations stay valid.
 *
 * This replaces the old VAOSupports, which mixed allocation bookkeeping with
 * direct OpenGL calls. Nothing here names a graphics API.
 */
#ifndef GLEDITOR_BUFFER_POOL_H
#define GLEDITOR_BUFFER_POOL_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <span>
#include <utility>

#include <gleditor/render/types.hpp>

namespace render {
class RenderDevice;
}

/**
 * @class BufferPool
 * @brief Row-granular allocator over one device buffer.
 */
class BufferPool {
public:
  /**
   * @brief A reserved run of rows.
   */
  struct Allocation {
    std::uint32_t rowOffset{}; ///< First row of the run.
    std::uint32_t rowCount{};  ///< Length of the run in rows.

    [[nodiscard]] bool empty() const { return 0 == rowCount; }
  };

  /**
   * @param aDevice Device the buffer lives on. Not owned; must outlive the pool.
   * @param aRowStride Size of one row in bytes.
   * @param initialRows Rows to allocate up front.
   */
  BufferPool(render::RenderDevice *aDevice, std::uint32_t aRowStride,
             std::uint32_t initialRows);
  ~BufferPool();

  BufferPool(const BufferPool &)            = delete;
  BufferPool &operator=(const BufferPool &) = delete;
  BufferPool(BufferPool &&)                 = delete;
  BufferPool &operator=(BufferPool &&)      = delete;

  /**
   * @brief Reserve @p rows contiguous rows, growing the buffer if needed.
   * @throws std::runtime_error if the request cannot be satisfied even after
   *         growing.
   */
  Allocation reserve(std::uint32_t rows);

  /// Return an allocation to the free list.
  void release(const Allocation &allocation);

  /**
   * @brief Write rows into an allocation.
   * @param allocation Run to write into.
   * @param firstRow Row index within the allocation.
   * @param data Row-aligned bytes; must not run past the allocation.
   */
  void write(const Allocation &allocation, std::uint32_t firstRow,
             std::span<const std::byte> data);

  /// Byte offset of an allocation within the buffer.
  [[nodiscard]] std::size_t byteOffset(const Allocation &allocation) const {
    return static_cast<std::size_t>(allocation.rowOffset) * rowStrideBytes;
  }

  [[nodiscard]] render::BufferHandle buffer() const { return handle; }
  [[nodiscard]] std::uint32_t rowStride() const { return rowStrideBytes; }
  [[nodiscard]] std::uint32_t capacityRows() const { return totalRows; }

private:
  /// Free runs as (first row, row count), kept sorted by offset so that
  /// adjacent runs can be merged on release.
  using FreeList = std::list<std::pair<std::uint32_t, std::uint32_t>>;

  /// Double the buffer until it holds at least @p neededRows more contiguous
  /// rows at the end.
  void grow(std::uint32_t neededRows);

  render::RenderDevice *device;
  render::BufferHandle handle{};
  std::uint32_t rowStrideBytes;
  std::uint32_t totalRows;
  FreeList free;
};

#endif // GLEDITOR_BUFFER_POOL_H
// vi: set sw=2 sts=2 ts=2 et:
