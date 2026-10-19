/**
 * @file crdt_zz.hpp
 * @brief CRDT-ZZ: Conflict-Free Replicated Resolution Engine for ZigZag Space.
 *
 * Resolves concurrent multi-peer link operations on the same (Cell, Dimension,
 * Direction) while strictly preserving the fundamental 2-rank manifold
 * invariant: deg^+(c, d) <= 1,  deg^-(c, d) <= 1
 *
 * Supported Strategies:
 * - Strategy A (Sequential Ranks): Deterministic Rank Sequence Splice (RGA
 * total order over <parentMicroversion, GlobalOpRef, authorKey>). Concurrent
 * appenders are spliced into a deterministic sub-sequence along the same
 * dimension.
 * - Strategy B (Functional Attributes): Orthogonal Collision Dimension
 * Promotion. The highest-priority operation retains the primary dimension,
 * while conflicting links are promoted to an orthogonal dimension
 * d.conflict(dim, hash(V0)).
 */
#ifndef ZIGZAG_CRDT_ZZ_HPP
#define ZIGZAG_CRDT_ZZ_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag::crdt {

/**
 * @struct OpKey
 * @brief Globally unique, totally ordered operation identifier.
 */
struct OpKey {
  std::uint64_t microversion{0};
  std::uint64_t opRef{0};
  std::uint32_t authorKey{0};

  [[nodiscard]] auto operator<=>(const OpKey &) const = default;
};

/**
 * @struct ConcurrentLinkOp
 * @brief Proposed link operation received across SpoolMesh peers.
 */
struct ConcurrentLinkOp {
  OpKey key;
  std::optional<CellRef> fromCell{std::nullopt};
  std::optional<DimRef> dim{std::nullopt};
  DimVector dir{DimVector::POS};
  std::optional<CellRef> toCell{std::nullopt};
};

enum class ConflictStrategy : std::uint8_t {
  SequenceSplice,     ///< Strategy A: RGA rank splice
  DimensionPromotion, ///< Strategy B: Promote losers to d.conflict
};

/**
 * @struct ResolutionResult
 * @brief Summary of the applied CRDT resolution.
 */
struct ResolutionResult {
  bool applied{false};
  bool promoted{false};
  std::optional<DimRef> resolvedDim{std::nullopt};
  std::string explanation;
};

/**
 * @class CRDTEngine
 * @brief Manages deterministic convergence of concurrent multi-peer graph
 * edits.
 */
class CRDTEngine {
public:
  CRDTEngine() = default;

  /**
   * @brief Apply a concurrent link operation into the target manifold according
   *        to the chosen resolution strategy.
   */
  ResolutionResult applyLink(ArenaManifold &manifold,
                             const ConcurrentLinkOp &op,
                             ConflictStrategy strategy);

  /**
   * @brief Batch resolve a collection of concurrent operations on the same rank
   *        via Strategy A (Deterministic Sequence Splice).
   */
  std::vector<ResolutionResult>
  applyBatchSplice(ArenaManifold &manifold, std::vector<ConcurrentLinkOp> ops);

  /**
   * @brief Verify the 2-rank manifold invariant across all cells in @p
   * manifold: deg^+(c, d) <= 1 and deg^-(c, d) <= 1 for all dimensions d.
   */
  [[nodiscard]] static bool
  verifyManifoldInvariants(const ArenaManifold &manifold) noexcept;

private:
  [[nodiscard]] DimRef getOrCreateConflictDim(ArenaManifold &manifold,
                                              DimRef primaryDim,
                                              std::uint64_t v0Hash);

  std::uint32_t conflictCounter_{0};
};

} // namespace zigzag::crdt

#endif // ZIGZAG_CRDT_ZZ_HPP
