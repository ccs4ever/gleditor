#include "segmented_primedia_spool.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32)
#include <io.h> // _commit
#endif

namespace xudu {

namespace {
constexpr std::size_t defaultPrimediaReservation = 512 * 1024 * 1024; // 512 MB
}

SegmentedPrimediaSpool::SegmentedPrimediaSpool() {
  arena.reserve(defaultPrimediaReservation);
}

SegmentedPrimediaSpool::~SegmentedPrimediaSpool() { clear(); }

SegmentedPrimediaSpool::SegmentedPrimediaSpool(
    SegmentedPrimediaSpool &&other) noexcept
    : arena(std::move(other.arena)), segmentList(std::move(other.segmentList)),
      totalBytes(other.totalBytes), committedBytes(other.committedBytes),
      activeFd(other.activeFd), activePath(std::move(other.activePath)) {
  other.totalBytes     = 0;
  other.committedBytes = 0;
  other.activeFd       = -1;
}

SegmentedPrimediaSpool &
SegmentedPrimediaSpool::operator=(SegmentedPrimediaSpool &&other) noexcept {
  if (this != &other) {
    clear();
    arena                = std::move(other.arena);
    segmentList          = std::move(other.segmentList);
    totalBytes           = other.totalBytes;
    committedBytes       = other.committedBytes;
    activeFd             = other.activeFd;
    activePath           = std::move(other.activePath);
    other.totalBytes     = 0;
    other.committedBytes = 0;
    other.activeFd       = -1;
  }
  return *this;
}

bool SegmentedPrimediaSpool::ensureCommitted(const std::size_t requiredBytes) {
  if (!arena.isValid()) {
    if (!arena.reserve(defaultPrimediaReservation)) {
      return false;
    }
  }
  if (requiredBytes <= committedBytes) {
    return true;
  }
  const auto aligned = VirtualMemoryArena::alignToPage(requiredBytes);
  if (aligned > arena.capacity()) {
    return false;
  }
  const auto toCommit = aligned - committedBytes;
  if (!arena.commitAnonymous(arena.base() + committedBytes, toCommit)) {
    return false;
  }
  committedBytes = aligned;
  return true;
}

PrimediaSpan SegmentedPrimediaSpool::append(const std::string_view bytes) {
  if (bytes.empty()) {
    return PrimediaSpan{localScroll, totalBytes, 0};
  }
  const auto nextTotal = totalBytes + bytes.size();
  if (!ensureCommitted(nextTotal)) {
    throw std::bad_alloc();
  }
  std::memcpy(arena.base() + totalBytes, bytes.data(), bytes.size());
  const auto start = totalBytes;
  totalBytes       = nextTotal;
  return PrimediaSpan{localScroll, start, bytes.size()};
}

std::string SegmentedPrimediaSpool::read(const PrimediaSpan &span) const {
  if (!span.isLocal()) {
    throw std::runtime_error("primedia spool read: span is not local");
  }
  if (span.empty() || span.start >= totalBytes || nullptr == arena.base()) {
    return {};
  }
  const auto count = std::min(span.length, totalBytes - span.start);
  return {reinterpret_cast<const char *>(arena.base() + span.start),
          static_cast<std::size_t>(count)};
}

std::string_view
SegmentedPrimediaSpool::readView(const PrimediaSpan &span) const {
  if (!span.isLocal() || span.empty() || span.start >= totalBytes ||
      nullptr == arena.base()) {
    return {};
  }
  const auto count = std::min(span.length, totalBytes - span.start);
  return {reinterpret_cast<const char *>(arena.base() + span.start),
          static_cast<std::size_t>(count)};
}

std::string_view SegmentedPrimediaSpool::bytes() const {
  if (0 == totalBytes || nullptr == arena.base()) {
    return {};
  }
  return {reinterpret_cast<const char *>(arena.base()),
          static_cast<std::size_t>(totalBytes)};
}

void SegmentedPrimediaSpool::adopt(const std::string_view data) {
  clear();
  append(data);
}

bool SegmentedPrimediaSpool::addSealedSegment(
    const std::filesystem::path &path) {
  // path.c_str() is a wchar_t* on Windows; open() needs a narrow string.
  const int fd = ::open(path.string().c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) < 0 || st.st_size <= 0) {
    ::close(fd);
    return false;
  }
  const auto segSize = static_cast<std::size_t>(st.st_size);
  const auto start   = totalBytes;

  // Try Tier 1 MAP_FIXED only if both start offset and segment size are
  // page-aligned
  bool mapped   = false;
  const auto ps = VirtualMemoryArena::pageSize();
  if (arena.isValid() && (start % ps == 0) && (segSize % ps == 0)) {
    mapped = arena.mapFileFixed(arena.base() + start, fd, 0, segSize, false);
    if (mapped) {
      committedBytes =
          std::max(committedBytes, static_cast<std::size_t>(start + segSize));
    }
  }

  if (!mapped) {
    // Tier 2 Fallback: commit anonymous and read bytes directly
    if (!ensureCommitted(start + segSize)) {
      ::close(fd);
      return false;
    }
    const auto readBytes = ::read(fd, arena.base() + start, segSize);
    if (readBytes != static_cast<ssize_t>(segSize)) {
      ::close(fd);
      return false;
    }
  }

  SegmentInfo info;
  info.startOffset = start;
  info.length      = segSize;
  info.path        = path.string();
  info.fd          = fd;
  info.isReadOnly  = true;
  segmentList.push_back(std::move(info));

  totalBytes += segSize;
  return true;
}

bool SegmentedPrimediaSpool::openActiveSegment(
    const std::filesystem::path &path) {
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  activePath = path.string();
  activeFd   = ::open(activePath.c_str(), O_RDWR | O_CREAT, 0644);
  return activeFd >= 0;
}

bool SegmentedPrimediaSpool::sealActive(
    const std::filesystem::path &newActivePath) {
  flush();
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  if (!activePath.empty()) {
    addSealedSegment(activePath);
  }
  return openActiveSegment(newActivePath);
}

bool SegmentedPrimediaSpool::flush() {
  if (nullptr != arena.base() && totalBytes > 0) {
    arena.flush(arena.base(), static_cast<std::size_t>(totalBytes));
  }
  if (activeFd >= 0 && !activePath.empty()) {
    // MinGW's io.h has no fsync(); _commit() is its file-durability
    // equivalent.
#if defined(_WIN32)
    ::_commit(activeFd);
#else
    ::fsync(activeFd);
#endif
  }
  return true;
}

void SegmentedPrimediaSpool::clear() {
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  for (auto &seg : segmentList) {
    if (seg.fd >= 0) {
      ::close(seg.fd);
      seg.fd = -1;
    }
  }
  segmentList.clear();
  arena.release();
  totalBytes     = 0;
  committedBytes = 0;
  activePath.clear();
}

} // namespace xudu
