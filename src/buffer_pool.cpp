/**
 * @file buffer_pool.cpp
 * @brief Implementation of the device buffer sub-allocator.
 */
#include <gleditor/buffer_pool.hpp> // IWYU pragma: associated

#include <algorithm>
#include <format>
#include <list>
#include <optional>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gleditor/render/device.hpp>

namespace {

/// Insert [@p first, @p first + @p count) into a list kept sorted by offset,
/// merging with the runs either side. Shared by the pool's free list and by
/// the erased rows of an allocation, which are the same bookkeeping over
/// different spaces: the buffer in one case, an allocation in the other.
void insertRun(std::list<std::pair<std::uint32_t, std::uint32_t>> &runs,
               const std::uint32_t first, const std::uint32_t count) {
  auto next = std::ranges::find_if(
      runs, [first](const auto &run) { return run.first > first; });
  auto inserted = runs.insert(next, {first, count});

  if (runs.end() != next && inserted->first + inserted->second == next->first) {
    inserted->second += next->second;
    runs.erase(next);
  }
  if (runs.begin() != inserted) {
    auto prev = std::prev(inserted);
    if (prev->first + prev->second == inserted->first) {
      prev->second += inserted->second;
      runs.erase(inserted);
    }
  }
}

} // namespace

BufferPool::BufferPool(render::RenderDevice *aDevice,
                       const std::uint32_t aRowStride,
                       const std::uint32_t initialRows)
    : device(aDevice), rowStrideBytes(aRowStride), totalRows(initialRows) {
  if (nullptr == device) {
    throw std::invalid_argument("BufferPool: null device");
  }
  if (0 == rowStrideBytes) {
    throw std::invalid_argument("BufferPool: zero row stride");
  }
  handle = device->createBuffer(render::BufferKind::Vertex,
                                static_cast<std::size_t>(totalRows) *
                                    rowStrideBytes);
  free.emplace_back(0, totalRows);
}

BufferPool::~BufferPool() {
  if (nullptr != device && handle.valid()) {
    device->destroyBuffer(handle);
  }
}

std::uint32_t BufferPool::trailingFreeRows() const {
  if (free.empty()) {
    return 0;
  }
  const auto &last = free.back();
  return last.first + last.second == totalRows ? last.second : 0;
}

void BufferPool::grow(const std::uint32_t neededRows) {
  const std::uint32_t previousRows = totalRows;

  // Rows already free at the end are part of the run this request will be cut
  // from, since growth extends that run rather than starting a new one. Only
  // the shortfall has to be found, which is what stops a pool from growing
  // because it was a hundred rows short of a request.
  const auto trailing  = trailingFreeRows();
  const auto shortfall = neededRows > trailing ? neededRows - trailing : 0;

  constexpr auto ceiling = std::numeric_limits<std::uint32_t>::max();
  if (shortfall > ceiling - previousRows) {
    throw std::runtime_error("BufferPool: buffer size overflow");
  }
  std::uint32_t target = previousRows + shortfall;

  // Geometric on top of that, so a document reserving once per page does not
  // copy the whole buffer once per page. See growthSixteenths for why the step
  // is half again rather than a doubling.
  const auto geometric =
      static_cast<std::uint64_t>(std::max<std::uint32_t>(previousRows, 1)) *
      growthSixteenths / 16;
  target = static_cast<std::uint32_t>(std::max<std::uint64_t>(
      target, std::min<std::uint64_t>(geometric, ceiling)));
  if (target <= previousRows) {
    throw std::runtime_error("BufferPool: buffer size overflow");
  }

  handle = device->resizeBuffer(handle, static_cast<std::size_t>(target) *
                                            rowStrideBytes);
  totalRows = target;

  // The newly added space is one contiguous run at the end. Merge it with a
  // trailing free run if there is one so the tail does not fragment.
  const auto added = target - previousRows;
  if (!free.empty() && free.back().first + free.back().second == previousRows) {
    free.back().second += added;
  } else {
    free.emplace_back(previousRows, added);
  }
}

BufferPool::Allocation BufferPool::reserve(const std::uint32_t rows) {
  if (0 == rows) {
    return {};
  }

  auto fits = [rows](const auto &run) { return run.second >= rows; };
  auto it   = std::ranges::find_if(free, fits);
  if (free.end() == it) {
    grow(rows);
    it = std::ranges::find_if(free, fits);
    if (free.end() == it) {
      throw std::runtime_error(
          std::format("BufferPool: no contiguous run of {} rows", rows));
    }
  }

  const Allocation allocation{it->first, rows};
  it->first += rows;
  it->second -= rows;
  if (0 == it->second) {
    free.erase(it);
  }
  // An allocation starts with nothing erased from it. release() clears this
  // already; doing it here too means a record can never outlive the rows it
  // describes, whatever else goes wrong.
  erased.erase(allocation.rowOffset);
  return allocation;
}

void BufferPool::release(const Allocation &allocation) {
  if (allocation.empty()) {
    return;
  }

  // Rows erased from inside it go with it: they are part of the run being
  // handed back, and leaving the record behind would offer them to whatever
  // allocation lands here next.
  erased.erase(allocation.rowOffset);

  // The list is kept ordered by offset so neighbouring runs can be coalesced;
  // without that, repeated reserve/release cycles would fragment the buffer
  // into unusable slivers.
  insertRun(free, allocation.rowOffset, allocation.rowCount);
}

void BufferPool::reserveCapacity(const std::uint32_t rows) {
  if (rows <= totalRows) {
    return;
  }
  const auto previousRows = totalRows;
  handle = device->resizeBuffer(handle, static_cast<std::size_t>(rows) *
                                            rowStrideBytes);
  totalRows = rows;

  const auto added = rows - previousRows;
  if (!free.empty() && free.back().first + free.back().second == previousRows) {
    free.back().second += added;
  } else {
    free.emplace_back(previousRows, added);
  }
}


void BufferPool::zeroRows(const std::uint32_t rowOffset,
                          const std::uint32_t count) {
  // A run of any length, written from a block of a bounded one: erasing half a
  // document should not need a staging buffer half the size of the document.
  static constexpr std::uint32_t chunkRows = 4096;
  const std::vector<std::byte> zeros(
      static_cast<std::size_t>(std::min(count, chunkRows)) * rowStrideBytes,
      std::byte{});

  for (std::uint32_t done = 0; done < count; done += chunkRows) {
    const auto rows = std::min(chunkRows, count - done);
    device->updateBuffer(
        handle,
        static_cast<std::size_t>(rowOffset + done) * rowStrideBytes,
        std::span<const std::byte>(zeros.data(),
                                   static_cast<std::size_t>(rows) *
                                       rowStrideBytes));
  }
}

void BufferPool::eraseRows(const Allocation &allocation,
                           const std::uint32_t firstRow,
                           const std::uint32_t count) {
  if (0 == count) {
    return;
  }
  if (firstRow > allocation.rowCount ||
      count > allocation.rowCount - firstRow) {
    throw std::out_of_range(
        std::format("BufferPool::eraseRows: erasing {} rows at {} runs past an "
                    "allocation of {} rows",
                    count, firstRow, allocation.rowCount));
  }

  // Zeroed on the device before being recorded, so that a row is never both
  // listed as reusable and still drawing something.
  zeroRows(allocation.rowOffset + firstRow, count);
  insertRun(erased[allocation.rowOffset], firstRow, count);
}

std::optional<std::uint32_t>
BufferPool::reuseRows(const Allocation &allocation, const std::uint32_t count) {
  if (0 == count) {
    return std::nullopt;
  }
  const auto holes = erased.find(allocation.rowOffset);
  if (erased.end() == holes) {
    return std::nullopt;
  }

  auto run = std::ranges::find_if(
      holes->second, [count](const auto &hole) { return hole.second >= count; });
  if (holes->second.end() == run) {
    return std::nullopt;
  }

  const auto firstRow = run->first;
  run->first += count;
  run->second -= count;
  if (0 == run->second) {
    holes->second.erase(run);
  }
  if (holes->second.empty()) {
    erased.erase(holes);
  }
  return firstRow;
}

std::uint32_t BufferPool::erasedRows(const Allocation &allocation) const {
  const auto holes = erased.find(allocation.rowOffset);
  if (erased.end() == holes) {
    return 0;
  }
  std::uint32_t rows = 0;
  for (const auto &hole : holes->second) {
    rows += hole.second;
  }
  return rows;
}

void BufferPool::trim() {
  const auto trailing = trailingFreeRows();
  if (0 == trailing) {
    return;
  }
  const auto used = totalRows - trailing;

  const std::uint64_t wanted = std::max<std::uint64_t>(
      static_cast<std::uint64_t>(used) + (used / 16 * trimHeadroom),
      trimFloorRows);
  if (wanted >= totalRows) {
    return;
  }
  // A copy of the whole buffer to save a sliver of it is not worth doing, so
  // the rows go back only when there are enough of them to matter.
  if (totalRows - wanted < totalRows / 8) {
    return;
  }

  const auto target = static_cast<std::uint32_t>(wanted);
  handle = device->resizeBuffer(handle, static_cast<std::size_t>(target) *
                                            rowStrideBytes);
  totalRows = target;

  // The trailing run is what shrank. It disappears entirely when the trim kept
  // exactly what was in use.
  if (target <= free.back().first) {
    free.pop_back();
  } else {
    free.back().second = target - free.back().first;
  }
}

void BufferPool::write(const Allocation &allocation,
                       const std::uint32_t firstRow,
                       const std::span<const std::byte> data) {
  if (data.empty()) {
    return;
  }
  if (0 != data.size() % rowStrideBytes) {
    throw std::invalid_argument("BufferPool::write: data is not row-aligned");
  }
  const auto rows = static_cast<std::uint32_t>(data.size() / rowStrideBytes);
  if (firstRow + rows > allocation.rowCount) {
    throw std::out_of_range(
        std::format("BufferPool::write: writing {} rows at {} overruns an "
                    "allocation of {} rows",
                    rows, firstRow, allocation.rowCount));
  }

  const auto offset =
      static_cast<std::size_t>(allocation.rowOffset + firstRow) * rowStrideBytes;
  device->updateBuffer(handle, offset, data);
}
// vi: set sw=2 sts=2 ts=2 et:
