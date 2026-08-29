#include "segmented_ops_spool.hpp"

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
constexpr std::size_t defaultOpsReservation = 512 * 1024 * 1024; // 512 MB

/// FNV-1a over a MicroversionId's segments. Nothing but idHash* uses this, and
/// they only need two ids with the same segments to land in the same bucket
/// -- which segment, branch and number is folded into, is not otherwise
/// meaningful.
std::size_t hashMicroversionId(const MicroversionId &id) {
  std::size_t h = 1469598103934665603ULL;
  for (const auto &seg : id.segments()) {
    h = (h ^ seg.branch) * 1099511628211ULL;
    h = (h ^ seg.number) * 1099511628211ULL;
  }
  return h;
}
} // namespace

SegmentedOpsSpool::SegmentedOpsSpool() {
  arena.reserve(defaultOpsReservation);
  indexLookup.push_back(MicroversionId{}); // Index 0 represents state zero
}

SegmentedOpsSpool::~SegmentedOpsSpool() { clear(); }

SegmentedOpsSpool::SegmentedOpsSpool(SegmentedOpsSpool &&other) noexcept
    : arena(std::move(other.arena)), segmentList(std::move(other.segmentList)),
      opCount(other.opCount), committedBytes(other.committedBytes),
      indexLookup(std::move(other.indexLookup)),
      idHashSlots(std::move(other.idHashSlots)), idHashCount(other.idHashCount),
      activeFd(other.activeFd), activePath(std::move(other.activePath)) {
  other.opCount        = 0;
  other.committedBytes = 0;
  other.idHashCount    = 0;
  other.activeFd       = -1;
  // A moved-from vector is left empty, not just cleared of its old contents
  // -- so the index-0 sentinel the constructor promised has to be put back,
  // or the next append() on the moved-from object would misfile everything
  // one slot short of where idOf() expects to find it.
  other.indexLookup.push_back(MicroversionId{});
}

SegmentedOpsSpool &
SegmentedOpsSpool::operator=(SegmentedOpsSpool &&other) noexcept {
  if (this != &other) {
    clear();
    arena                = std::move(other.arena);
    segmentList          = std::move(other.segmentList);
    opCount              = other.opCount;
    committedBytes       = other.committedBytes;
    indexLookup          = std::move(other.indexLookup);
    idHashSlots          = std::move(other.idHashSlots);
    idHashCount          = other.idHashCount;
    activeFd             = other.activeFd;
    activePath           = std::move(other.activePath);
    other.opCount        = 0;
    other.committedBytes = 0;
    other.idHashCount    = 0;
    other.activeFd       = -1;
    other.indexLookup.push_back(MicroversionId{});
  }
  return *this;
}

void SegmentedOpsSpool::idHashRehash(const std::size_t newCapacity) {
  std::vector<std::uint32_t> grown(newCapacity, 0);
  const auto mask = newCapacity - 1;
  for (const auto slot : idHashSlots) {
    if (0 == slot) {
      continue;
    }
    auto probe = hashMicroversionId(indexLookup[slot]) & mask;
    while (0 != grown[probe]) {
      probe = (probe + 1) & mask;
    }
    grown[probe] = slot;
  }
  idHashSlots = std::move(grown);
}

void SegmentedOpsSpool::idHashInsert(const MicroversionId &id,
                                     const std::uint32_t index) {
  // Kept below 70% full: linear probing degrades sharply past that, and this
  // table only ever grows, so there is no later chance to reclaim slack.
  if (idHashSlots.empty()) {
    idHashRehash(16);
  } else if (static_cast<double>(idHashCount + 1) >
             0.7 * static_cast<double>(idHashSlots.size())) {
    idHashRehash(idHashSlots.size() * 2);
  }
  const auto mask = idHashSlots.size() - 1;
  auto probe      = hashMicroversionId(id) & mask;
  while (0 != idHashSlots[probe]) {
    probe = (probe + 1) & mask;
  }
  idHashSlots[probe] = index;
  idHashCount++;
}

std::uint32_t SegmentedOpsSpool::idHashFind(const MicroversionId &id) const {
  if (idHashSlots.empty()) {
    return 0;
  }
  const auto mask = idHashSlots.size() - 1;
  auto probe      = hashMicroversionId(id) & mask;
  while (0 != idHashSlots[probe]) {
    if (indexLookup[idHashSlots[probe]] == id) {
      return idHashSlots[probe];
    }
    probe = (probe + 1) & mask;
  }
  return 0;
}

bool SegmentedOpsSpool::ensureCommitted(const std::size_t requiredBytes) {
  if (!arena.isValid()) {
    if (!arena.reserve(defaultOpsReservation)) {
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

std::uint32_t SegmentedOpsSpool::append(CompactOpNode node,
                                        const MicroversionId &produces) {
  const auto newIndex = opCount + 1U;
  const auto required = (newIndex + 1U) * sizeof(CompactOpNode);
  if (!ensureCommitted(required)) {
    throw std::bad_alloc();
  }

  node.firstChildIndex  = 0;
  node.nextSiblingIndex = 0;

  auto *const opsArray = reinterpret_cast<CompactOpNode *>(arena.base());
  opsArray[newIndex]   = node;

  // Maintain child & sibling tree pointers in contiguous memory
  if (node.parentIndex > 0 && node.parentIndex <= opCount) {
    auto *const parent = get(node.parentIndex);
    if (nullptr != parent) {
      if (0 == parent->firstChildIndex) {
        parent->firstChildIndex = newIndex;
      } else {
        auto sibling = parent->firstChildIndex;
        while (sibling > 0 && sibling <= opCount) {
          auto *const sibNode = get(sibling);
          if (nullptr == sibNode) {
            break;
          }
          if (0 == sibNode->nextSiblingIndex) {
            sibNode->nextSiblingIndex = newIndex;
            break;
          }
          sibling = sibNode->nextSiblingIndex;
        }
      }
    }
  }

  indexLookup.push_back(produces);
  idHashInsert(produces, newIndex);
  opCount = newIndex;
  return newIndex;
}

const CompactOpNode *SegmentedOpsSpool::get(const std::uint32_t index) const {
  if (0 == index || index > opCount || nullptr == arena.base()) {
    return nullptr;
  }
  return &reinterpret_cast<const CompactOpNode *>(arena.base())[index];
}

CompactOpNode *SegmentedOpsSpool::get(const std::uint32_t index) {
  if (0 == index || index > opCount || nullptr == arena.base()) {
    return nullptr;
  }
  return &reinterpret_cast<CompactOpNode *>(arena.base())[index];
}

const CompactOpNode *SegmentedOpsSpool::get(const MicroversionId &id) const {
  return get(idHashFind(id));
}

bool SegmentedOpsSpool::contains(const MicroversionId &id) const {
  return idHashFind(id) != 0;
}

std::uint32_t SegmentedOpsSpool::indexOf(const MicroversionId &id) const {
  return idHashFind(id);
}

MicroversionId SegmentedOpsSpool::idOf(const std::uint32_t index) const {
  if (0 == index || index >= indexLookup.size()) {
    return {};
  }
  return indexLookup[index];
}

std::vector<std::uint32_t>
SegmentedOpsSpool::childrenOf(const std::uint32_t index) const {
  std::vector<std::uint32_t> children;
  if (0 == index) {
    // Top-level chains branching from root
    for (std::uint32_t i = 1; i <= opCount; i++) {
      const auto *const op = get(i);
      if (nullptr != op && 0 == op->parentIndex) {
        children.push_back(i);
      }
    }
    return children;
  }

  const auto *const parent = get(index);
  if (nullptr == parent || 0 == parent->firstChildIndex) {
    return children;
  }

  auto current = parent->firstChildIndex;
  while (current > 0 && current <= opCount) {
    children.push_back(current);
    const auto *const node = get(current);
    if (nullptr == node) {
      break;
    }
    current = node->nextSiblingIndex;
  }
  return children;
}

std::vector<std::uint32_t>
SegmentedOpsSpool::ancestralPath(const std::uint32_t targetIndex) const {
  std::vector<std::uint32_t> path;
  auto current = targetIndex;
  while (current > 0 && current <= opCount) {
    path.push_back(current);
    const auto *const node = get(current);
    if (nullptr == node) {
      break;
    }
    current = node->parentIndex;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

bool SegmentedOpsSpool::addSealedSegment(const std::filesystem::path &path) {
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
  const auto bytes = static_cast<std::size_t>(st.st_size);
  const auto segCount =
      static_cast<std::uint32_t>(bytes / sizeof(CompactOpNode));
  if (0 == segCount) {
    ::close(fd);
    return false;
  }

  const auto startOffset = (opCount + 1U) * sizeof(CompactOpNode);

  bool mapped   = false;
  const auto ps = VirtualMemoryArena::pageSize();
  if (arena.isValid() && (startOffset % ps == 0) && (bytes % ps == 0)) {
    mapped =
        arena.mapFileFixed(arena.base() + startOffset, fd, 0, bytes, false);
    if (mapped) {
      committedBytes = std::max(committedBytes, startOffset + bytes);
    }
  }

  if (!mapped) {
    if (!ensureCommitted(startOffset + bytes)) {
      ::close(fd);
      return false;
    }
    const auto readBytes = ::read(fd, arena.base() + startOffset, bytes);
    if (readBytes != static_cast<ssize_t>(bytes)) {
      ::close(fd);
      return false;
    }
  }

  SegmentInfo info;
  info.startOpIndex = opCount + 1U;
  info.opCount      = segCount;
  info.path         = path.string();
  info.fd           = fd;
  info.isReadOnly   = true;
  segmentList.push_back(std::move(info));

  opCount += segCount;
  return true;
}

bool SegmentedOpsSpool::openActiveSegment(const std::filesystem::path &path) {
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  activePath = path.string();
  activeFd   = ::open(activePath.c_str(), O_RDWR | O_CREAT, 0644);
  return activeFd >= 0;
}

bool SegmentedOpsSpool::sealActive(const std::filesystem::path &newActivePath) {
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

bool SegmentedOpsSpool::flush() {
  if (nullptr != arena.base() && opCount > 0) {
    const auto bytes = (opCount + 1U) * sizeof(CompactOpNode);
    arena.flush(arena.base(), bytes);
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

void SegmentedOpsSpool::clear() {
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
  indexLookup.clear();
  indexLookup.push_back(MicroversionId{});
  idHashSlots.clear();
  idHashCount = 0;
  arena.release();
  opCount        = 0;
  committedBytes = 0;
  activePath.clear();
}

} // namespace xudu
