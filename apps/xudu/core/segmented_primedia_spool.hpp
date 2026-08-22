/**
 * @file segmented_primedia_spool.hpp
 * @brief Contiguous multi-segment virtual memory primedia spool.
 */
#ifndef XUDU_SEGMENTED_PRIMEDIA_SPOOL_HPP
#define XUDU_SEGMENTED_PRIMEDIA_SPOOL_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "spool.hpp"
#include "virtual_memory_arena.hpp"

namespace xudu {

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
   * @brief Read a span of content as a copy.
   */
  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  /**
   * @brief Zero-copy view into the contiguous virtual memory span.
   */
  [[nodiscard]] std::string_view readView(const PrimediaSpan &span) const;

  /// Total number of bytes recorded across all segments.
  [[nodiscard]] std::uint64_t size() const { return totalBytes; }

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
   * @brief Set the active writable segment file.
   */
  bool openActiveSegment(const std::filesystem::path &path);

  /**
   * @brief Seal the current active segment and start a new active segment.
   */
  bool sealActive(const std::filesystem::path &newActivePath);

  /// Synchronize unwritten active bytes to disk.
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
  std::uint64_t totalBytes{0};
  std::size_t committedBytes{0};

  int activeFd{-1};
  std::string activePath;
};

} // namespace xudu

#endif // XUDU_SEGMENTED_PRIMEDIA_SPOOL_HPP
