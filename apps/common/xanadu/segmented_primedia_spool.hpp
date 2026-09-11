/**
 * @file segmented_primedia_spool.hpp
 * @brief Contiguous multi-segment virtual memory primedia spool.
 */
#ifndef XUDU_SEGMENTED_PRIMEDIA_SPOOL_HPP
#define XUDU_SEGMENTED_PRIMEDIA_SPOOL_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "spool.hpp"
#include "virtual_memory_arena.hpp"

namespace xanadu {

/**
 * @class SegmentedPrimediaSpool
 * @brief Contiguous virtual memory manager for primedia content across sealed
 *        torrent segments and the active writable segment.
 */
class SegmentedPrimediaSpool : public SpanReader {
public:
  struct SegmentInfo {
    std::uint64_t startOffset{0};
    std::uint64_t length{0};
    std::string path;
    int fd{-1};
    bool isReadOnly{true};
  };

  SegmentedPrimediaSpool();
  ~SegmentedPrimediaSpool() override;

  SegmentedPrimediaSpool(const SegmentedPrimediaSpool &)            = delete;
  SegmentedPrimediaSpool &operator=(const SegmentedPrimediaSpool &) = delete;
  SegmentedPrimediaSpool(SegmentedPrimediaSpool &&) noexcept;
  SegmentedPrimediaSpool &operator=(SegmentedPrimediaSpool &&) noexcept;

  /**
   * @brief Append bytes to the active primedia segment.
   * @return The canonical PrimediaSpan where the bytes were recorded.
   */
  PrimediaSpan append(std::string_view bytes);

  /**
   * @brief Append binary payload bytes to the active primedia segment.
   */
  PrimediaSpan append(const std::span<const std::uint8_t> bytes) {
    return append(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                                   bytes.size()));
  }

  /**
   * @brief Read a span of content as a copy.
   */
  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  /**
   * @brief Zero-copy view into the contiguous virtual memory span.
   */
  [[nodiscard]] std::string_view readView(const PrimediaSpan &span) const;

  /// Total number of bytes recorded across all segments.
  [[nodiscard]] std::uint64_t size() const {
    return totalBytes.load(std::memory_order_acquire);
  }

  /// Full byte view of the entire spool.
  [[nodiscard]] std::string_view bytes() const;

  /**
   * @brief Adopt in-memory bytes (for compatibility / text loading).
   */
  void adopt(std::string_view data);

  /**
   * @brief Add a sealed read-only segment file.
   */
  bool addSealedSegment(const std::filesystem::path &path);

  /**
   * @brief Set the active writable segment file, restoring what it holds.
   *
   * The file's bytes are read in at the spool's current end and keep those
   * addresses, so reopening a permascroll is what makes a span recorded in an
   * earlier session still resolve. This is the read half of the spool's
   * persistence; flush() is the write half.
   */
  bool openActiveSegment(const std::filesystem::path &path);

  /**
   * @brief Seal the current active segment and start a new active segment.
   */
  bool sealActive(const std::filesystem::path &newActivePath);

  /**
   * @brief Write everything appended since the last flush to the active
   *        segment file.
   *
   * Appending only ever adds to the end, so this writes the tail rather than
   * the spool: a flush costs what was typed since the last one. Nothing is
   * durable until it is called -- append() writes into anonymous memory, which
   * no msync can reach.
   */
  bool flush();

  /// Reset the spool to empty state.
  void clear();

  [[nodiscard]] const std::vector<SegmentInfo> &segments() const {
    return segmentList;
  }

private:
  bool ensureCommitted(std::size_t requiredBytes);

  VirtualMemoryArena arena;
  std::vector<SegmentInfo> segmentList;
  /**
   * @brief How much has been published, and the one thing a reader
   *        synchronises on.
   *
   * Atomic because reading a span takes no lock (see readView): one appender
   * copies bytes into `[totalBytes, next)` and *then* stores the new size with
   * release, so a reader that acquire-loads a size has a happens-before edge to
   * every byte below it. The bytes themselves need no atomics -- nothing ever
   * rewrites a byte once published, which is what append-only means here.
   *
   * The arena's base address never moves (it is reserved once and committed in
   * place with MAP_FIXED), so a reader holds no pointer that an append can
   * invalidate. That is the other half of what makes the lock unnecessary, and
   * it is a property of VirtualMemoryArena rather than a convention -- see
   * ensureCommitted(), which commits at `base() + committedBytes` and never
   * relocates what is already there.
   *
   * What this does *not* make safe is clear(), adopt() or opening a segment
   * concurrently with a reader: those unmap or re-address the arena, and no
   * ordering on a size makes that safe. They are construction-time and
   * test-time operations, and UserPermascroll keeps them behind its mutex.
   */
  std::atomic<std::uint64_t> totalBytes{0};
  std::size_t committedBytes{0};

  int activeFd{-1};
  std::string activePath;
  /// Where the active segment begins in the spool's address space. The file
  /// holds [activeStart, totalBytes), so an arena offset's place in the file
  /// is that offset less this.
  std::uint64_t activeStart{0};
  /// How much of the arena the active segment file already holds. Everything
  /// past it is typed but not yet durable; flush() is what closes the gap.
  std::uint64_t flushedBytes{0};
  /// The active segment could be opened for reading but not for writing -- a
  /// fixture in a read-only checkout, say. flush() writes nothing.
  bool readOnly{false};
};

} // namespace xanadu

#endif // XUDU_SEGMENTED_PRIMEDIA_SPOOL_HPP
