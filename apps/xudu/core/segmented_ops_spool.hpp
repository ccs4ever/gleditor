/**
 * @file segmented_ops_spool.hpp
 * @brief Contiguous multi-segment virtual memory operations spool and tree.
 */
#ifndef XUDU_SEGMENTED_OPS_SPOOL_HPP
#define XUDU_SEGMENTED_OPS_SPOOL_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "compact_op.hpp"
#include "microversion.hpp"
#include "virtual_memory_arena.hpp"

namespace xudu {

/**
 * @class SegmentedOpsSpool
 * @brief Contiguous virtual memory operations tree spanning sealed and active
 *        torrent segments.
 */
class SegmentedOpsSpool {
public:
  struct SegmentInfo {
    std::uint32_t startOpIndex{0};
    std::uint32_t opCount{0};
    std::string path;
    int fd{-1};
    bool isReadOnly{true};
  };

  SegmentedOpsSpool();
  ~SegmentedOpsSpool();

  SegmentedOpsSpool(const SegmentedOpsSpool &)            = delete;
  SegmentedOpsSpool &operator=(const SegmentedOpsSpool &) = delete;
  SegmentedOpsSpool(SegmentedOpsSpool &&) noexcept;
  SegmentedOpsSpool &operator=(SegmentedOpsSpool &&) noexcept;

  /**
   * @brief Append a new operation to the active segment.
   * @param node The operation payload.
   * @param produces The microversion state produced.
   * @return The 1-based index of the new operation.
   */
  std::uint32_t append(CompactOpNode node, const MicroversionId &produces);

  /**
   * @brief Get a pointer to an operation by its 1-based index.
   */
  [[nodiscard]] const CompactOpNode *get(std::uint32_t index) const;
  [[nodiscard]] CompactOpNode *get(std::uint32_t index);

  /**
   * @brief Get an operation by the microversion it produces.
   */
  [[nodiscard]] const CompactOpNode *get(const MicroversionId &id) const;

  /**
   * @brief Check whether an operation exists for a microversion.
   */
  [[nodiscard]] bool contains(const MicroversionId &id) const;

  /**
   * @brief Find the 1-based index for a microversion.
   */
  [[nodiscard]] std::uint32_t indexOf(const MicroversionId &id) const;

  /**
   * @brief Get the microversion produced by an operation index.
   */
  [[nodiscard]] MicroversionId idOf(std::uint32_t index) const;

  /// Total number of operations recorded.
  [[nodiscard]] std::size_t size() const { return opCount; }

  /// Whether the spool contains zero operations.
  [[nodiscard]] bool empty() const { return 0 == opCount; }

  /**
   * @brief Get the direct children indices branching off @p index.
   */
  [[nodiscard]] std::vector<std::uint32_t>
  childrenOf(std::uint32_t index) const;

  /**
   * @brief Compute the ancestral index path to rebuild @p targetIndex from
   * root.
   */
  [[nodiscard]] std::vector<std::uint32_t>
  ancestralPath(std::uint32_t targetIndex) const;

  /**
   * @brief Add a sealed read-only segment file.
   */
  bool addSealedSegment(const std::filesystem::path &path);

  /**
   * @brief Open the active writable segment file.
   */
  bool openActiveSegment(const std::filesystem::path &path);

  /**
   * @brief Seal the active segment and open a new active segment.
   */
  bool sealActive(const std::filesystem::path &newActivePath);

  /// Synchronize unwritten active operations to disk.
  bool flush();

  /// Reset to empty state.
  void clear();

  [[nodiscard]] const std::vector<SegmentInfo> &segments() const {
    return segmentList;
  }

  /// Direct raw pointer to the base of the contiguous CompactOpNode array.
  [[nodiscard]] const CompactOpNode *rawOps() const {
    return reinterpret_cast<const CompactOpNode *>(arena.base());
  }

private:
  bool ensureCommitted(std::size_t requiredBytes);

  VirtualMemoryArena arena;
  std::vector<SegmentInfo> segmentList;
  std::uint32_t opCount{0};
  std::size_t committedBytes{0};

  std::unordered_map<std::string, std::uint32_t> idLookup;
  std::vector<MicroversionId> indexLookup;

  int activeFd{-1};
  std::string activePath;
};

} // namespace xudu

#endif // XUDU_SEGMENTED_OPS_SPOOL_HPP
