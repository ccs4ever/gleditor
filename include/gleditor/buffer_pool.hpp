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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <span>
#include <unordered_map>
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
   * @brief A reserved run of rows: a name for it, not a place.
   *
   * Where the rows are is the pool's business and may change. An allocation
   * that outgrows the room reserved around it is moved somewhere it fits, and
   * the holder is not told, because there is nothing it could do about it:
   * everything it can ask -- where its rows begin, how many there are, where
   * to write -- it asks the pool, which knows where they are now.
   *
   * Holding one of these across a move is therefore safe. Holding a byte
   * offset across one is not, so ask for that at the point of use.
   */
  struct Allocation {
    std::uint32_t id{};

    [[nodiscard]] bool empty() const { return 0 == id; }
    bool operator==(const Allocation &) const = default;
  };

  /**
   * @param aDevice Device the buffer lives on. Not owned; must outlive the
   * pool.
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
   *
   * Room is left around them -- see @ref slackFor -- so that the run can grow
   * a little without going anywhere. @ref rowCount reports what was asked for;
   * the slack is not part of it and is never drawn, since a draw covers the
   * rows the holder says it has.
   *
   * @throws std::runtime_error if the request cannot be satisfied even after
   *         growing.
   */
  Allocation reserve(std::uint32_t rows);

  /**
   * @brief Whether a resize that has to move the rows should bring them along.
   *
   * A caller that is about to write every row again -- a page being laid out
   * after an edit -- would have the pool copy rows to a new home only to
   * overwrite them there.
   */
  enum class Contents : std::uint8_t {
    Keep,   ///< Carry the rows to the new place.
    Discard ///< The caller is about to write them all; do not copy.
  };

  /**
   * @brief Make an allocation @p rows long, wherever that has to be.
   *
   * Within the room already reserved this is bookkeeping and nothing moves.
   * Past it, the pool finds somewhere the run fits, copies the rows there,
   * and gives the old place back to the free list -- all under the same
   * handle, so a holder that kept one goes on using it.
   *
   * Shrinking keeps the place and widens the slack, since the rows that would
   * be given back are in the middle of the buffer where nothing else can use
   * them. @ref eraseRows is how a holder says rows are no longer drawn.
   *
   * @throws std::runtime_error if the pool cannot find room even after
   *         growing.
   */
  void resize(const Allocation &allocation, std::uint32_t rows,
              Contents contents = Contents::Keep);

  /// Return an allocation to the free list, along with any rows erased from
  /// inside it.
  void release(const Allocation &allocation);

  /**
   * @brief Delete @p count rows from inside @p allocation, anywhere in it.
   *
   * The rows are zeroed rather than removed. A row of zeroes describes a quad
   * of no width and no height, which the vertex stage collapses to a point
   * outside clip space, so it draws nothing and writes no picking tag -- see
   * unpackQuad() in assets/shaders/glyph.vert.glsl and the test that pins it.
   *
   * That is what keeps a page one draw. The alternative to a degenerate row is
   * a hole the draw has to step over, which turns a page into two ranges and
   * then three, each needing its own call; a page that has been edited a few
   * times would be issuing a draw per surviving run. Here the range is
   * untouched and the cost of a deleted row is one vertex shader invocation
   * that exits immediately.
   *
   * The rows are recorded as free and can be taken back by @ref reuseRows.
   * They stay part of this allocation and are not offered to any other: the
   * allocation is drawn as one range with one transform and one identity, so
   * another page's quads sitting inside it would be drawn in the wrong place
   * and answer a click with the wrong page.
   *
   * @param firstRow Row index within the allocation, not within the buffer.
   * @throws std::out_of_range if the run is not inside the allocation.
   */
  void eraseRows(const Allocation &allocation, std::uint32_t firstRow,
                 std::uint32_t count);

  /**
   * @brief Take back a run of @p count rows erased earlier from @p allocation.
   *
   * @return Where the run starts within the allocation, ready to be handed
   *         straight to @ref write, or nothing when no erased run is long
   *         enough. A caller that gets nothing has to reserve instead.
   */
  [[nodiscard]] std::optional<std::uint32_t>
  reuseRows(const Allocation &allocation, std::uint32_t count);

  /// Rows currently erased from inside @p allocation: drawn, degenerate, and
  /// waiting to be written into again.
  [[nodiscard]] std::uint32_t erasedRows(const Allocation &allocation) const;

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

  /**
   * @brief Byte offset of an allocation within the buffer, as it is now.
   *
   * Read it when it is needed rather than keeping it: a resize elsewhere in
   * the pool cannot move this allocation, but a resize of this one can.
   */
  [[nodiscard]] std::size_t byteOffset(const Allocation &allocation) const;

  /// Rows the holder asked for, which is what a draw over this allocation
  /// covers.
  [[nodiscard]] std::uint32_t rowCount(const Allocation &allocation) const;

  /// Rows actually reserved: @ref rowCount plus the room left for it to grow
  /// into.
  [[nodiscard]] std::uint32_t roomFor(const Allocation &allocation) const;

  /// How many times the pool has had to move an allocation. A number that
  /// keeps climbing means the slack is too tight for how this pool is used.
  [[nodiscard]] std::uint64_t moves() const { return movesMade; }

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

  /**
   * @brief Room left around a run of @p rows for it to grow into.
   *
   * An allocation with no slack has to move the first time it gains a row,
   * and moving means copying the whole run and leaving a hole behind. A
   * sixteenth of a page is a few hundred quads, which is a few hundred
   * characters typed into it before it has to go anywhere; the floor covers
   * the small allocations, where a sixteenth is nothing but the cost of moving
   * is the same call.
   */
  static constexpr std::uint32_t slackFloorRows = 8;
  [[nodiscard]] static constexpr std::uint32_t slackFor(std::uint32_t rows) {
    return std::max(rows / 16, slackFloorRows);
  }

private:
  /// Free runs as (first row, row count), kept sorted by offset so that
  /// adjacent runs can be merged on release.
  using FreeList = std::list<std::pair<std::uint32_t, std::uint32_t>>;

  /**
   * @brief Where an allocation's rows are, and how much room they have.
   *
   * The pool holds these rather than handing them out, which is what lets an
   * allocation be moved without anyone being told.
   */
  struct Placement {
    std::uint32_t rowOffset{}; ///< First row of the reserved run.
    std::uint32_t rowCount{};  ///< Rows the holder asked for.
    std::uint32_t roomRows{};  ///< Rows reserved, including the slack.
    /// Rows erased from inside [0, rowCount), relative to the allocation, so
    /// they travel with it when it moves.
    FreeList erased;
  };

  /// The placement of a live allocation, or a throw naming the caller.
  [[nodiscard]] const Placement &placementOf(const Allocation &allocation,
                                             const char *what) const;
  [[nodiscard]] Placement &placementOf(const Allocation &allocation,
                                       const char *what);

  /// Take a run of @p rows from the free list, growing the buffer if no run is
  /// long enough.
  [[nodiscard]] std::uint32_t takeRun(std::uint32_t rows);

  /// Free rows at the end of the buffer, which are the only ones a trim can
  /// give back.
  [[nodiscard]] std::uint32_t trailingFreeRows() const;

  /// Zero @p count rows of the buffer from @p rowOffset, in chunks, so that
  /// erasing a large run does not need a staging buffer the size of the run.
  void zeroRows(std::uint32_t rowOffset, std::uint32_t count);

  /// Double the buffer until it holds at least @p neededRows more contiguous
  /// rows at the end.
  void grow(std::uint32_t neededRows);

  render::RenderDevice *device;
  render::BufferHandle handle{};
  std::uint32_t rowStrideBytes;
  std::uint32_t totalRows;
  FreeList free;

  /// Live allocations by handle. An id is never reused, so a handle to a
  /// released allocation is not silently a handle to somebody else's.
  std::unordered_map<std::uint32_t, Placement> placements;
  std::uint32_t nextAllocationId{1};
  std::uint64_t movesMade{};
};

#endif // GLEDITOR_BUFFER_POOL_H
// vi: set sw=2 sts=2 ts=2 et:
