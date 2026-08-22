#include "segmented_ops_spool.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace xudu {

namespace {
constexpr std::size_t defaultOpsReservation = 512 * 1024 * 1024; // 512 MB
}

SegmentedOpsSpool::SegmentedOpsSpool() {
  arena.reserve(defaultOpsReservation);
  indexLookup.push_back(MicroversionId{}); // Index 0 represents state zero
}

SegmentedOpsSpool::~SegmentedOpsSpool() { clear(); }

SegmentedOpsSpool::SegmentedOpsSpool(SegmentedOpsSpool &&other) noexcept
    : arena(std::move(other.arena)),
      segmentList(std::move(other.segmentList)),
      opCount(other.opCount),
      committedBytes(other.committedBytes),
      idLookup(std::move(other.idLookup)),
      indexLookup(std::move(other.indexLookup)),
      activeFd(other.activeFd),
      activePath(std::move(other.activePath)) {
  other.opCount        = 0;
  other.committedBytes = 0;
  other.activeFd       = -1;
}

SegmentedOpsSpool &
SegmentedOpsSpool::operator=(SegmentedOpsSpool &&other) noexcept {
  if (this != &other) {
    clear();
    arena                = std::move(other.arena);
    segmentList          = std::move(other.segmentList);
    opCount              = other.opCount;
    committedBytes       = other.committedBytes;
    idLookup             = std::move(other.idLookup);
    indexLookup          = std::move(other.indexLookup);
    activeFd             = other.activeFd;
    activePath           = std::move(other.activePath);
    other.opCount        = 0;
    other.committedBytes = 0;
    other.activeFd       = -1;
  }
  return *this;
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

  auto *const opsArray =
      reinterpret_cast<CompactOpNode *>(arena.base());
  opsArray[newIndex] = node;

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

  idLookup[produces.str()] = newIndex;
  indexLookup.push_back(produces);
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

const CompactOpNode *
SegmentedOpsSpool::get(const MicroversionId &id) const {
  const auto it = idLookup.find(id.str());
  if (it == idLookup.end()) {
    return nullptr;
  }
  return get(it->second);
}

bool SegmentedOpsSpool::contains(const MicroversionId &id) const {
  return idLookup.contains(id.str());
}

std::uint32_t
SegmentedOpsSpool::indexOf(const MicroversionId &id) const {
  const auto it = idLookup.find(id.str());
  return it == idLookup.end() ? 0U : it->second;
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
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) < 0 || st.st_size <= 0) {
    ::close(fd);
    return false;
  }
  const auto bytes    = static_cast<std::size_t>(st.st_size);
  const auto segCount = static_cast<std::uint32_t>(bytes / sizeof(CompactOpNode));
  if (0 == segCount) {
    ::close(fd);
    return false;
  }

  const auto startOffset = (opCount + 1U) * sizeof(CompactOpNode);

  bool mapped = false;
  const auto ps = VirtualMemoryArena::pageSize();
  if (arena.isValid() && (startOffset % ps == 0) && (bytes % ps == 0)) {
    mapped = arena.mapFileFixed(arena.base() + startOffset, fd, 0, bytes, false);
    if (mapped) {
      committedBytes = std::max(committedBytes, startOffset + bytes);
    }
  }

  if (!mapped) {
    if (!ensureCommitted(startOffset + bytes)) {
      ::close(fd);
      return false;
    }
    const auto readBytes =
        ::read(fd, arena.base() + startOffset, bytes);
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

bool SegmentedOpsSpool::openActiveSegment(
    const std::filesystem::path &path) {
  if (activeFd >= 0) {
    ::close(activeFd);
    activeFd = -1;
  }
  activePath = path.string();
  activeFd   = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  return activeFd >= 0;
}

bool SegmentedOpsSpool::sealActive(
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

bool SegmentedOpsSpool::flush() {
  if (nullptr != arena.base() && opCount > 0) {
    const auto bytes = (opCount + 1U) * sizeof(CompactOpNode);
    arena.flush(arena.base(), bytes);
  }
  if (activeFd >= 0 && !activePath.empty()) {
    ::fsync(activeFd);
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
  idLookup.clear();
  indexLookup.clear();
  indexLookup.push_back(MicroversionId{});
  arena.release();
  opCount        = 0;
  committedBytes = 0;
  activePath.clear();
}

} // namespace xudu
