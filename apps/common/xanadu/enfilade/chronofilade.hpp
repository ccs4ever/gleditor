/**
 * @file chronofilade.hpp
 * @brief The Osmic Chronofilade: Composable EDL Replay-Product B-Enfilade.
 *
 * Replaces O(K) linear replay from State 0 with O(1) amortized version
 * rebuilding, O(log N) DAG binary lifting and Lowest Common Ancestor (LCA)
 * calculation, and composable EDL transforms.
 *
 * Strict Compliance:
 * - Ruling R8: Derived replay product in memory. Navigation and tree
 *   maintenance never mint operations on disk.
 * - Ruling R9: verifyAgainstFullRebuild() ensures mathematical identity
 *   with raw ancestral replay from State 0.
 */
#ifndef XANADU_ENFILADE_CHRONOFILADE_HPP
#define XANADU_ENFILADE_CHRONOFILADE_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/enfilade/crum_node.hpp"
#include "common/xanadu/enfilade/edl_transform.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/version.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::enfilade {

/**
 * @struct ChronoDsp
 * @brief Relative displacement monoid representing operation count Δops.
 */
struct ChronoDsp {
  std::uint32_t deltaOps{0};

  [[nodiscard]] ChronoDsp compose(const ChronoDsp &other) const noexcept {
    return ChronoDsp{deltaOps + other.deltaOps};
  }
  [[nodiscard]] bool isIdentity() const noexcept { return 0 == deltaOps; }
  [[nodiscard]] EdlTransform act(const EdlTransform &w) const noexcept {
    return w;
  }
  bool operator==(const ChronoDsp &) const = default;
};

static_assert(DisplacementMonoid<ChronoDsp>);
static_assert(WidthMonoid<EdlTransform>);
static_assert(EnfiladeAction<ChronoDsp, EdlTransform>);

/// Cache-conscious interior B-enfilade crum node.
using ChronoCrum = CrumNode<ChronoDsp, EdlTransform, 8>;

/**
 * @class Chronofilade
 * @brief Timeline scrubbing and composed EDL transformation enfilade.
 */
class Chronofilade {
public:
  static constexpr std::size_t MaxLiftingPower      = 18;
  static constexpr std::uint32_t CheckpointInterval = 32;

  Chronofilade();

  /// Index all unindexed operations currently in @p store.
  void indexSpool(const Store &store);

  /// Record a newly appended operation at @p opIndex into the Chronofilade.
  void recordOp(std::uint32_t opIndex, const CompactOpNode &node,
                const MicroversionId &produces, const Store &store);

  /// Rebuild the Version at @p opIndex in O(1) amortized time using checkpoints
  /// and composed transforms.
  [[nodiscard]] Version rebuildVersion(std::uint32_t opIndex,
                                       const Store &store) const;

  /// Find the Lowest Common Ancestor of operations @p a and @p b in O(log N).
  [[nodiscard]] std::uint32_t lowestCommonAncestor(std::uint32_t a,
                                                   std::uint32_t b) const;

  /// Jump up @p steps ancestral hops from @p idx in O(log steps).
  [[nodiscard]] std::uint32_t jumpAncestor(std::uint32_t idx,
                                           std::uint32_t steps) const;

  /// Get the ancestral depth of operation @p opIndex (0 for State 0).
  [[nodiscard]] std::uint32_t depth(std::uint32_t opIndex) const;

  /// Carry @p document from @p fromIndex to @p toIndex.
  [[nodiscard]] bool advance(Version &document, std::uint32_t fromIndex,
                             std::uint32_t toIndex, const Store &store) const;

  /// Compose the EDL transform along the linear ancestral path from
  /// @p fromAncestor to @p toDescendant.
  [[nodiscard]] EdlTransform composePath(std::uint32_t fromAncestor,
                                         std::uint32_t toDescendant,
                                         const Store &store) const;

  /// Verify R9 conformance: confirm that rebuildVersion(opIndex) produces a
  /// state mathematically identical to @p fullRebuilt.
  [[nodiscard]] bool verifyAgainstFullRebuild(std::uint32_t opIndex,
                                              const Store &store,
                                              const Version &fullRebuilt) const;

  /// Total number of indexed operations.
  [[nodiscard]] std::size_t indexedCount() const noexcept {
    return depth_.empty() ? 0 : depth_.size() - 1;
  }

  /// Clear all indexed data.
  void clear();

private:
  void ensureCapacity(std::size_t count);

  std::vector<std::uint32_t> depth_{0};
  std::vector<std::array<std::uint32_t, MaxLiftingPower>> up_{
      std::array<std::uint32_t, MaxLiftingPower>{}};
  std::unordered_map<std::uint32_t, Version> checkpoints_;
  std::unordered_map<std::uint32_t, EdlTransform> leafTransforms_;
};

} // namespace xanadu::enfilade

#endif // XANADU_ENFILADE_CHRONOFILADE_HPP
