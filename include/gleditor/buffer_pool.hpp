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
   * @brief Make room for @p rows in one step, for a caller that knows how much
   *        it is about to need.
   *
   * Growing a page at a time is the expensive way to arrive at a large buffer,
   * and not because of the copying: every intermediate buffer is an allocation
   * the driver may hold on to rather than hand back, so a document that grew
   * through seven sizes cost far more memory than the one it ended at. A
   * document knows its own length before it lays out a single page, so it can
   * say so and be given the whole thing at once.
   *
   * Exactly @p rows, with no geometric step on top: the caller's estimate is
   * better than any factor applied to it. Smaller than the current capacity
   * does nothing; @ref trim is what gives room back.
   */
  void reserveCapacity(std::uint32_t rows);

  /**
   * @brief Give back room reserved on the way to the current contents.
   *
   * Growth has to overshoot -- a pool that grew by exactly what was asked for
   * would copy itself on every page -- but once a document has finished
   * loading the overshoot is room nothing will ever write to. A megabyte of
   * text left half the buffer unused, which is the same size as the whole
   * saving from halving the vertex record.
   *
   * What is kept is what is in use plus @ref trimHeadroom of it, so that the
   * first edits after a load are absorbed without another allocation. Only the
   * free run at the end can be given back; rows freed in the middle stay where
   * they are, since an allocation after them cannot be moved without every
   * page that holds one being told.
   */
  void trim();

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
  /// Rows before the free run at the end -- what a trim would keep.
  [[nodiscard]] std::uint32_t rowsInUse() const {
    return totalRows - trailingFreeRows();
  }

  /**
   * @brief How much a pool grows by when it has to grow, as a sixteenth.
   *
   * Growth must be geometric or a document that reserves once per page copies
   * itself once per page. It does not have to be a doubling: a pool that
   * doubles has reserved room for a second document by the time it holds the
   * first, and on a megabyte of text that overshoot was twenty-four megabytes.
   * Half again bounds the waste at fifty per cent where doubling bounds it at
   * a hundred, and costs one more copy of the buffer over a whole load.
   */
  static constexpr std::uint32_t growthSixteenths = 24; // 1.5x

  /**
   * @brief Room a trim leaves behind, as a sixteenth of what is in use.
   *
   * A document is trimmed when it has finished loading, and the next thing it
   * is likely to do is be edited. An edit relays out a page and reserves its
   * rows again before releasing the old ones, so leaving nothing spare would
   * mean a reallocation on the first keystroke.
   */
  static constexpr std::uint32_t trimHeadroom = 1; // 1/16, about six per cent

  /// Rows below which a trim is not worth a copy of the whole buffer.
  static constexpr std::uint32_t trimFloorRows = 4096;

private:
  /// Free rows at the end of the buffer, which are the only ones a trim can
  /// give back.
  [[nodiscard]] std::uint32_t trailingFreeRows() const;

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
