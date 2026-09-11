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

namespace xanadu {

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
      totalBytes(other.totalBytes.load(std::memory_order_relaxed)),
      committedBytes(other.committedBytes), activeFd(other.activeFd),
      activePath(std::move(other.activePath)), activeStart(other.activeStart),
      flushedBytes(other.flushedBytes), readOnly(other.readOnly) {
  // Relaxed throughout: moving a spool is not something another thread can be
  // reading through, and pretending otherwise with an ordering would only
  // suggest it were.
  other.totalBytes.store(0, std::memory_order_relaxed);
  other.committedBytes = 0;
  other.activeFd       = -1;
  other.activeStart    = 0;
  other.flushedBytes   = 0;
}

SegmentedPrimediaSpool &
SegmentedPrimediaSpool::operator=(SegmentedPrimediaSpool &&other) noexcept {
  if (this != &other) {
    clear();
    arena       = std::move(other.arena);
    segmentList = std::move(other.segmentList);
    totalBytes.store(other.totalBytes.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
    committedBytes = other.committedBytes;
    activeFd       = other.activeFd;
    activePath     = std::move(other.activePath);
    activeStart    = other.activeStart;
    flushedBytes   = other.flushedBytes;
    readOnly       = other.readOnly;
    other.totalBytes.store(0, std::memory_order_relaxed);
    other.committedBytes = 0;
    other.activeFd       = -1;
    other.activeStart    = 0;
    other.flushedBytes   = 0;
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
  // Relaxed: an appender is the only writer, so it is reading back what it
  // itself published and needs no ordering to see it.
  const auto start = totalBytes.load(std::memory_order_relaxed);
  if (bytes.empty()) {
    return PrimediaSpan{localScroll, start, 0};
  }
  const auto nextTotal = start + bytes.size();
  if (!ensureCommitted(nextTotal)) {
    throw std::bad_alloc();
  }
  std::memcpy(arena.base() + start, bytes.data(), bytes.size());
  // Release, and after the copy: this store is what publishes the bytes above,
  // and a reader's acquire load of the size is the other end of that edge. Were
  // the store first, a reader could see the size and read bytes nobody had
  // written yet -- which is the whole reason this is not a plain assignment.
  totalBytes.store(nextTotal, std::memory_order_release);
  return PrimediaSpan{localScroll, start, bytes.size()};
}

std::string SegmentedPrimediaSpool::read(const PrimediaSpan &span) const {
  if (!span.isLocal()) {
    throw std::runtime_error("primedia spool read: span is not local");
  }
  // The size first and the base address second, in that order, deliberately.
  // An acquire load stops the compiler and the processor hoisting the base()
  // read above it, so a reader that sees a nonzero size also sees the
  // reservation that size was measured against -- which is what makes reading
  // `base` without a lock sound rather than merely usually fine.
  const auto published = totalBytes.load(std::memory_order_acquire);
  if (span.empty() || span.start >= published || nullptr == arena.base()) {
    return {};
  }
  const auto count = std::min(span.length, published - span.start);
  return {reinterpret_cast<const char *>(arena.base() + span.start),
          static_cast<std::size_t>(count)};
}

std::string_view
SegmentedPrimediaSpool::readView(const PrimediaSpan &span) const {
  const auto published = totalBytes.load(std::memory_order_acquire);
  if (!span.isLocal() || span.empty() || span.start >= published ||
      nullptr == arena.base()) {
    return {};
  }
  // Clamped to what was published rather than to what was asked for, which is
  // what makes the view safe to hold while an append runs: the bytes under it
  // are already written and will never be rewritten.
  const auto count = std::min(span.length, published - span.start);
  return {reinterpret_cast<const char *>(arena.base() + span.start),
          static_cast<std::size_t>(count)};
}

std::string_view SegmentedPrimediaSpool::bytes() const {
  const auto published = totalBytes.load(std::memory_order_acquire);
  if (0 == published || nullptr == arena.base()) {
    return {};
  }
  return {reinterpret_cast<const char *>(arena.base()),
          static_cast<std::size_t>(published)};
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
  const auto start   = totalBytes.load(std::memory_order_relaxed);

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

  // Release, for the same reason append() uses one: the segment's bytes are
  // in the arena before its size is published.
  totalBytes.store(start + segSize, std::memory_order_release);
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
  if (activeFd < 0) {
    // A permascroll that can be read but not appended to: a checked-in fixture
    // in a read-only checkout, or another user's scroll. Reading one is a
    // reasonable thing to want -- it is what makes the document open at all --
    // so this opens it rather than refusing, and flush() below writes nothing
    // because nothing can be appended without a writable arena either.
    activeFd = ::open(activePath.c_str(), O_RDONLY);
    readOnly = activeFd >= 0;
    if (activeFd < 0) {
      return false;
    }
  } else {
    readOnly = false;
  }

  // Whatever the file already holds is primedia this spool has recorded
  // before, and it takes the addresses it had then: the active segment begins
  // at the spool's current end, so reopening a permascroll puts every byte
  // back where the spans naming it expect to find it. Read rather than mapped
  // -- the active segment is the one that grows, and a shared file mapping
  // cannot be extended in place the way appending needs.
  activeStart = totalBytes.load(std::memory_order_relaxed);
  struct stat st;
  if (::fstat(activeFd, &st) < 0) {
    ::close(activeFd);
    activeFd = -1;
    return false;
  }
  if (st.st_size > 0) {
    const auto have = static_cast<std::size_t>(st.st_size);
    if (!ensureCommitted(activeStart + have)) {
      ::close(activeFd);
      activeFd = -1;
      return false;
    }
    std::size_t got = 0;
    while (got < have) {
      const auto n = ::pread(activeFd, arena.base() + activeStart + got,
                             have - got, static_cast<off_t>(got));
      if (n <= 0) {
        ::close(activeFd);
        activeFd = -1;
        return false;
      }
      got += static_cast<std::size_t>(n);
    }
    totalBytes.store(activeStart + have, std::memory_order_release);
  }
  // Nothing appended since the file was read, so there is no tail to write.
  flushedBytes = totalBytes.load(std::memory_order_relaxed);
  return true;
}

bool SegmentedPrimediaSpool::sealActive(
    const std::filesystem::path &newActivePath) {
  flush();
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  // The sealed range is already in the arena at the addresses it will keep --
  // openActiveSegment() read it there. Recording it is therefore bookkeeping,
  // not a second read: addSealedSegment() would map the same bytes in *again*
  // at the spool's end, giving every one of them a second address and moving
  // the end past content that does not exist.
  const auto held = totalBytes.load(std::memory_order_relaxed);
  if (!activePath.empty() && held > activeStart) {
    SegmentInfo info;
    info.startOffset = activeStart;
    info.length      = held - activeStart;
    info.path        = activePath;
    info.fd          = -1;
    info.isReadOnly  = true;
    segmentList.push_back(std::move(info));
  }
  return openActiveSegment(newActivePath);
}

bool SegmentedPrimediaSpool::flush() {
  // Acquire, so that a flush from a thread other than the appender writes out
  // every byte that appender had published rather than a prefix of them.
  const auto published = totalBytes.load(std::memory_order_acquire);
  if (nullptr != arena.base() && published > 0) {
    // Only reaches the disk for ranges backed by a file mapping, which is the
    // sealed segments. The active segment is written below: appending commits
    // anonymous pages, and msync on anonymous memory has nowhere to write.
    // That asymmetry is why this used to persist nothing at all -- the fsync
    // that followed it synced a file no byte had ever been written to.
    arena.flush(arena.base(), static_cast<std::size_t>(published));
  }
  if (activeFd < 0 || activePath.empty() || readOnly) {
    return true;
  }
  while (flushedBytes < published) {
    const auto pending = static_cast<std::size_t>(published - flushedBytes);
    const auto n = ::pwrite(activeFd, arena.base() + flushedBytes, pending,
                            static_cast<off_t>(flushedBytes - activeStart));
    if (n <= 0) {
      return false;
    }
    flushedBytes += static_cast<std::uint64_t>(n);
  }
  // MinGW's io.h has no fsync(); _commit() is its file-durability
  // equivalent.
#if defined(_WIN32)
  ::_commit(activeFd);
#else
  ::fsync(activeFd);
#endif
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
  totalBytes.store(0, std::memory_order_relaxed);
  committedBytes = 0;
  activeStart    = 0;
  flushedBytes   = 0;
  readOnly       = false;
  activePath.clear();
}

} // namespace xanadu
