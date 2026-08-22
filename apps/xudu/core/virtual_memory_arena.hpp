/**
 * @file virtual_memory_arena.hpp
 * @brief Cross-platform virtual memory reservation and multi-file mapping.
 *
 * Implements a multi-tier virtual address space manager:
 * - Tier 1: Direct multi-file zero-copy unified virtual range via POSIX
 *   mmap with MAP_FIXED (Linux, Android, macOS).
 * - Tier 2: Anonymous virtual memory reservation (VirtualAlloc / mmap MAP_ANON)
 *   with segment block loads (Windows fallback and constrained environments).
 */
#ifndef XUDU_VIRTUAL_MEMORY_ARENA_HPP
#define XUDU_VIRTUAL_MEMORY_ARENA_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace xudu {

/**
 * @class VirtualMemoryArena
 * @brief Manages a contiguous range of virtual address space.
 */
class VirtualMemoryArena {
public:
  VirtualMemoryArena() = default;
  ~VirtualMemoryArena();

  VirtualMemoryArena(const VirtualMemoryArena &)            = delete;
  VirtualMemoryArena &operator=(const VirtualMemoryArena &) = delete;
  VirtualMemoryArena(VirtualMemoryArena &&other) noexcept;
  VirtualMemoryArena &operator=(VirtualMemoryArena &&other) noexcept;

  /**
   * @brief Reserve a contiguous block of virtual address space.
   *
   * On 64-bit systems, this reserves address space without committing physical
   * RAM (using PROT_NONE or PAGE_NOACCESS).
   *
   * @param maxBytes Maximum capacity in bytes to reserve (e.g. 512 MB).
   * @return true if reservation succeeded.
   */
  bool reserve(std::size_t maxBytes);

  /**
   * @brief Map a file segment at a fixed virtual address within the reserved
   *        arena.
   *
   * Uses MAP_FIXED on POSIX. Target address must be page-aligned.
   *
   * @param targetAddr Address within the reserved arena where the file begins.
   * @param fd Open file descriptor to map.
   * @param fileOffset Offset in the file (must be page-aligned).
   * @param length Number of bytes to map.
   * @param writable If true, mapped with write permissions; otherwise
   *                 read-only.
   * @return true on success; false if MAP_FIXED is not supported or failed.
   */
  bool mapFileFixed(void *targetAddr, int fd, std::uint64_t fileOffset,
                    std::size_t length, bool writable);

  /**
   * @brief Commit anonymous physical memory to a range within the arena.
   *
   * Used for Tier 2 fallback and active in-memory buffers.
   *
   * @param targetAddr Address within the reserved arena.
   * @param length Number of bytes to commit.
   * @return true if physical memory was committed.
   */
  bool commitAnonymous(void *targetAddr, std::size_t length);

  /**
   * @brief Synchronize dirty pages in a range to backing storage or disk.
   */
  bool flush(void *addr, std::size_t length);

  /**
   * @brief Release all mappings and virtual memory.
   */
  void release();

  /// Pointer to the base of the reserved virtual address range.
  [[nodiscard]] std::uint8_t *base() const { return baseAddress; }

  /// Total reserved capacity in bytes.
  [[nodiscard]] std::size_t capacity() const { return reservedBytes; }

  /// Whether this arena currently has a valid reservation.
  [[nodiscard]] bool isValid() const { return nullptr != baseAddress; }

  /// System page size in bytes (typically 4096).
  [[nodiscard]] static std::size_t pageSize();

  /// Round up a size or offset to the next page boundary.
  [[nodiscard]] static std::size_t alignToPage(std::size_t size);

private:
  std::uint8_t *baseAddress{nullptr};
  std::size_t reservedBytes{0};
};

} // namespace xudu

#endif // XUDU_VIRTUAL_MEMORY_ARENA_HPP
