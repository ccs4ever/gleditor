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
      activeFd(other.activeFd), activePath(std::move(other.activePath)),
      activeStartIndex(other.activeStartIndex),
      activeFlushedOps(other.activeFlushedOps) {
  other.opCount          = 0;
  other.committedBytes   = 0;
  other.idHashCount      = 0;
  other.activeFd         = -1;
  other.activeStartIndex = 1;
  other.activeFlushedOps = 0;
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
    arena                  = std::move(other.arena);
    segmentList            = std::move(other.segmentList);
    opCount                = other.opCount;
    committedBytes         = other.committedBytes;
    indexLookup            = std::move(other.indexLookup);
    idHashSlots            = std::move(other.idHashSlots);
    idHashCount            = other.idHashCount;
    activeFd               = other.activeFd;
    activePath             = std::move(other.activePath);
    activeStartIndex       = other.activeStartIndex;
    activeFlushedOps       = other.activeFlushedOps;
    other.opCount          = 0;
    other.committedBytes   = 0;
    other.idHashCount      = 0;
    other.activeFd         = -1;
    other.activeStartIndex = 1;
    other.activeFlushedOps = 0;
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

bool SegmentedOpsSpool::adoptSegmentNodes(const int fd,
                                          const std::uint32_t nodeCount,
                                          const bool mayMap) {
  const auto bytes       = nodeCount * sizeof(CompactOpNode);
  const auto startIndex  = opCount + 1U;
  const auto startOffset = startIndex * sizeof(CompactOpNode);

  bool mapped   = false;
  const auto ps = VirtualMemoryArena::pageSize();
  if (mayMap && arena.isValid() && (startOffset % ps == 0) &&
      (bytes % ps == 0)) {
    mapped =
        arena.mapFileFixed(arena.base() + startOffset, fd, 0, bytes, false);
    if (mapped) {
      committedBytes = std::max(committedBytes, startOffset + bytes);
    }
  }
  if (!mapped) {
    if (!ensureCommitted(startOffset + bytes)) {
      return false;
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
      return false;
    }
    if (::read(fd, arena.base() + startOffset, bytes) !=
        static_cast<ssize_t>(bytes)) {
      return false;
    }
  }

  // Name each node before anything is committed to opCount, so a segment that
  // does not belong here -- one whose parents are missing because it was
  // loaded out of order -- leaves the spool as it was rather than half read.
  std::vector<MicroversionId> names;
  names.reserve(nodeCount);
  const auto *const nodes =
      reinterpret_cast<const CompactOpNode *>(arena.base());
  for (std::uint32_t i = 0; i < nodeCount; i++) {
    const auto &node  = nodes[startIndex + i];
    const auto parent = node.parentIndex < startIndex
                            ? idOf(node.parentIndex)
                            : names[node.parentIndex - startIndex];
    if (node.parentIndex >= startIndex + i) {
      return false; // a parent at or after its own child is not a tree
    }
    if (0 != node.parentIndex && parent.isZero()) {
      return false; // names a parent this spool does not hold
    }
    const auto name = 0 == node.branchOrdinal
                          ? parent.next()
                          : parent.branch(node.branchOrdinal);
    // A state is produced by one operation and no more. Two nodes claiming the
    // same parent and the same ordinal name the same state, which a tree
    // cannot mean -- and taking them both would leave two entries a lookup
    // could return either of.
    if (0 != idHashFind(name)) {
      return false;
    }
    names.push_back(name);
  }
  {
    auto sorted = names;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      return false; // two nodes in this segment name the same state
    }
  }

  for (std::uint32_t i = 0; i < nodeCount; i++) {
    indexLookup.push_back(names[i]);
    idHashInsert(names[i], startIndex + i);
  }
  opCount += nodeCount;
  return true;
}

bool SegmentedOpsSpool::addSealedSegment(const std::filesystem::path &path) {
  // path.c_str() is a wchar_t* on Windows; open() needs a narrow string.
  const int fd = ::open(path.string().c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) < 0 || st.st_size <= 0 ||
      static_cast<std::size_t>(st.st_size) % sizeof(CompactOpNode) != 0) {
    ::close(fd);
    return false;
  }
  const auto segCount =
      static_cast<std::uint32_t>(st.st_size / sizeof(CompactOpNode));

  const auto startIndex = opCount + 1U;
  if (!adoptSegmentNodes(fd, segCount, true)) {
    ::close(fd);
    return false;
  }

  SegmentInfo info;
  info.startOpIndex = startIndex;
  info.opCount      = segCount;
  info.path         = path.string();
  info.fd           = fd;
  info.isReadOnly   = true;
  segmentList.push_back(std::move(info));
  return true;
}

bool SegmentedOpsSpool::openActiveSegment(const std::filesystem::path &path) {
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  const int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) < 0 ||
      static_cast<std::size_t>(st.st_size) % sizeof(CompactOpNode) != 0) {
    ::close(fd);
    return false;
  }
  // Whatever the file already holds is taken in rather than written over --
  // never mapped, since this range has to stay writable for what comes next.
  const auto held =
      static_cast<std::uint32_t>(st.st_size / sizeof(CompactOpNode));
  const auto startIndex = opCount + 1U;
  if (held > 0 && !adoptSegmentNodes(fd, held, false)) {
    ::close(fd);
    return false;
  }

  activeFd         = fd;
  activePath       = path.string();
  activeStartIndex = startIndex;
  activeFlushedOps = held;
  return true;
}

bool SegmentedOpsSpool::sealActive(const std::filesystem::path &newActivePath) {
  if (!flush()) {
    return false;
  }
  // The operations stay exactly where they are. Sealing is a change of what
  // the range is called -- read-only, backed by a file that is now complete
  // -- and reading them back in would file every one of them a second time.
  if (activeFd >= 0 && activeFlushedOps > 0) {
    SegmentInfo info;
    info.startOpIndex = activeStartIndex;
    info.opCount      = activeFlushedOps;
    info.path         = activePath;
    info.fd           = activeFd;
    info.isReadOnly   = true;
    segmentList.push_back(std::move(info));
    activeFd = -1; // the segment owns the descriptor now
  } else if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  activePath.clear();
  return openActiveSegment(newActivePath);
}

bool SegmentedOpsSpool::flush() {
  if (activeFd >= 0 && opCount >= activeStartIndex) {
    const auto pending = (opCount - activeStartIndex + 1U) - activeFlushedOps;
    if (pending > 0) {
      const auto *const nodes =
          reinterpret_cast<const CompactOpNode *>(arena.base());
      const auto from  = activeStartIndex + activeFlushedOps;
      const auto bytes = pending * sizeof(CompactOpNode);
      const auto at =
          static_cast<off_t>(activeFlushedOps * sizeof(CompactOpNode));
      if (::lseek(activeFd, at, SEEK_SET) < 0) {
        return false;
      }
      if (::write(activeFd, &nodes[from], bytes) !=
          static_cast<ssize_t>(bytes)) {
        return false;
      }
      activeFlushedOps += pending;
    }
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
  opCount          = 0;
  committedBytes   = 0;
  activeStartIndex = 1;
  activeFlushedOps = 0;
  activePath.clear();
}

} // namespace xudu
