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
   * @brief Add a sealed read-only segment file, mapped in after what is
   *        already held.
   *
   * The file is a bare run of CompactOpNodes and nothing else -- no header, no
   * state-zero slot, no names. The names are worked out from the nodes: each
   * one says which index produced it and by which branch ordinal, so its
   * microversion follows from its parent's, and a parent always sits at a
   * lower index than its children. Segments are therefore written and read
   * back in the same order, and the indices inside them are the ones they had
   * when they were sealed rather than positions within the file.
   *
   * @return false if the file cannot be read, is not a whole number of nodes,
   *         or names a parent this spool does not hold -- which is what a
   *         segment loaded out of order looks like.
   */
  bool addSealedSegment(const std::filesystem::path &path);

  /**
   * @brief Open the writable segment that new operations are appended to.
   *
   * A file that already holds nodes is adopted rather than overwritten, so
   * reopening a spool picks up where it left off. Anything appended after
   * that is written to this file by flush().
   */
  bool openActiveSegment(const std::filesystem::path &path);

  /**
   * @brief Seal the active segment and start a new one at @p newActivePath.
   *
   * The operations already in hand do not move: sealing writes out whatever
   * has not been written yet and re-files the range as a read-only segment.
   * It does not read them back in -- they are already here.
   */
  bool sealActive(const std::filesystem::path &newActivePath);

  /// Write operations appended since the last flush to the active segment
  /// file. Only the tail is written, so this costs what was appended rather
  /// than what is held.
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

  /// index -> the microversion produced at that index. What every lookup
  /// below is ultimately answered from; a spool index is meaningless on its
  /// own, so this is the one place that names it.
  std::vector<MicroversionId> indexLookup;

  /// microversion -> index, without holding a second copy of the id to
  /// compare against: indexLookup already has one at the index this points
  /// to, so a candidate slot is verified against that instead. Open
  /// addressing with linear probing and no tombstones, which append-only
  /// storage can get away with -- nothing is ever removed from a slot except
  /// by clear() resetting the whole table, so "empty" only ever means
  /// "nothing has been inserted here yet" and probing always terminates.
  ///
  /// This is the difference between the ~73 bytes per operation an
  /// unordered_map<std::string, uint32_t> cost here (a heap-allocated hash
  /// node plus a rendered id string per entry) and the few bytes a slot
  /// array costs: nothing is stored per entry that indexLookup was not
  /// storing already.
  std::vector<std::uint32_t> idHashSlots; // 0 = empty; else a 1-based index
  std::uint32_t idHashCount{0};

  void idHashInsert(const MicroversionId &id, std::uint32_t index);
  [[nodiscard]] std::uint32_t idHashFind(const MicroversionId &id) const;
  void idHashRehash(std::size_t newCapacity);

  /// Place @p nodeCount nodes from @p fd into the arena after what is already
  /// held, work out the microversion each produces, and index them. Shared by
  /// addSealedSegment() and openActiveSegment(), which differ only in whether
  /// the range may be mapped read-only and in what is recorded about it.
  bool adoptSegmentNodes(int fd, std::uint32_t nodeCount, bool mayMap);

  int activeFd{-1};
  std::string activePath;
  /// Where the active segment starts, and how much of it flush() has already
  /// written. The file holds the range [activeStartIndex, activeStartIndex +
  /// activeFlushedOps), so a flush writes from where it left off.
  std::uint32_t activeStartIndex{1};
  std::uint32_t activeFlushedOps{0};
};

} // namespace xudu

#endif // XUDU_SEGMENTED_OPS_SPOOL_HPP
