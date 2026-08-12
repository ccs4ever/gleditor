/**
 * @file buffer_pool.cpp
 * @brief Implementation of the device buffer sub-allocator.
 */
#include <gleditor/buffer_pool.hpp> // IWYU pragma: associated

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>

#include <gleditor/render/device.hpp>

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
  return allocation;
}

void BufferPool::release(const Allocation &allocation) {
  if (allocation.empty()) {
    return;
  }

  // Keep the list ordered by offset so neighbouring runs can be coalesced;
  // without that, repeated reserve/release cycles would fragment the buffer
  // into unusable slivers.
  auto next = std::ranges::find_if(free, [&allocation](const auto &run) {
    return run.first > allocation.rowOffset;
  });
  auto inserted = free.insert(next, {allocation.rowOffset, allocation.rowCount});

  if (free.end() != next && inserted->first + inserted->second == next->first) {
    inserted->second += next->second;
    free.erase(next);
  }
  if (free.begin() != inserted) {
    auto prev = std::prev(inserted);
    if (prev->first + prev->second == inserted->first) {
      prev->second += inserted->second;
      free.erase(inserted);
    }
  }
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
