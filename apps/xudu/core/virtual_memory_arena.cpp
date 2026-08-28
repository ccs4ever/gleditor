#include "virtual_memory_arena.hpp"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

namespace xudu {

VirtualMemoryArena::~VirtualMemoryArena() { release(); }

VirtualMemoryArena::VirtualMemoryArena(VirtualMemoryArena &&other) noexcept
    : baseAddress(other.baseAddress), reservedBytes(other.reservedBytes) {
  other.baseAddress   = nullptr;
  other.reservedBytes = 0;
}

VirtualMemoryArena &
VirtualMemoryArena::operator=(VirtualMemoryArena &&other) noexcept {
  if (this != &other) {
    release();
    baseAddress         = other.baseAddress;
    reservedBytes       = other.reservedBytes;
    other.baseAddress   = nullptr;
    other.reservedBytes = 0;
  }
  return *this;
}

std::size_t VirtualMemoryArena::pageSize() {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<std::size_t>(si.dwPageSize);
#else
  const auto sz = sysconf(_SC_PAGESIZE);
  return sz > 0 ? static_cast<std::size_t>(sz) : 4096U;
#endif
}

std::size_t VirtualMemoryArena::alignToPage(const std::size_t size) {
  const auto ps = pageSize();
  return (size + ps - 1U) & ~(ps - 1U);
}

bool VirtualMemoryArena::reserve(const std::size_t maxBytes) {
  release();
  if (0 == maxBytes) {
    return false;
  }
  const auto aligned = alignToPage(maxBytes);

#ifdef _WIN32
  baseAddress = static_cast<std::uint8_t *>(
      VirtualAlloc(nullptr, aligned, MEM_RESERVE, PAGE_NOACCESS));
  if (nullptr == baseAddress) {
    return false;
  }
#else
  void *const ptr =
      mmap(nullptr, aligned, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (MAP_FAILED == ptr) {
    return false;
  }
  baseAddress = static_cast<std::uint8_t *>(ptr);
#endif

  reservedBytes = aligned;
  return true;
}

bool VirtualMemoryArena::mapFileFixed(void *const targetAddr, const int fd,
                                      const std::uint64_t fileOffset,
                                      const std::size_t length,
                                      const bool writable) {
  if (nullptr == baseAddress || nullptr == targetAddr || 0 == length ||
      fd < 0) {
    return false;
  }
  const auto *const target = static_cast<const std::uint8_t *>(targetAddr);
  if (target < baseAddress ||
      (target + length) > (baseAddress + reservedBytes)) {
    return false;
  }

#ifdef _WIN32
  // Windows MapViewOfFileEx requires matching allocation granularity.
  // When MAP_FIXED isn't supported, caller falls back to commitAnonymous +
  // read.
  return false;
#else
  const int prot     = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
  void *const mapped = mmap(targetAddr, length, prot, MAP_SHARED | MAP_FIXED,
                            fd, static_cast<off_t>(fileOffset));
  return mapped == targetAddr;
#endif
}

bool VirtualMemoryArena::commitAnonymous(void *const targetAddr,
                                         const std::size_t length) {
  if (nullptr == baseAddress || nullptr == targetAddr || 0 == length) {
    return false;
  }
  const auto *const target = static_cast<const std::uint8_t *>(targetAddr);
  if (target < baseAddress ||
      (target + length) > (baseAddress + reservedBytes)) {
    return false;
  }

#ifdef _WIN32
  return nullptr !=
         VirtualAlloc(targetAddr, length, MEM_COMMIT, PAGE_READWRITE);
#else
  return 0 == mprotect(targetAddr, length, PROT_READ | PROT_WRITE);
#endif
}

bool VirtualMemoryArena::flush(void *const addr, const std::size_t length) {
  if (nullptr == baseAddress || nullptr == addr || 0 == length) {
    return false;
  }

#ifdef _WIN32
  return FALSE != FlushViewOfFile(addr, length);
#else
  return 0 == msync(addr, length, MS_SYNC);
#endif
}

void VirtualMemoryArena::release() {
  if (nullptr == baseAddress) {
    return;
  }
#ifdef _WIN32
  VirtualFree(baseAddress, 0, MEM_RELEASE);
#else
  munmap(baseAddress, reservedBytes);
#endif
  baseAddress   = nullptr;
  reservedBytes = 0;
}

} // namespace xudu
