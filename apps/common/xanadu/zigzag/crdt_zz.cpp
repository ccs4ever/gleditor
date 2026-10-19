/**
 * @file crdt_zz.cpp
 * @brief Implementation of the CRDT-ZZ resolution engine.
 */
#include "common/xanadu/zigzag/crdt_zz.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zigzag::crdt {

DimRef CRDTEngine::getOrCreateConflictDim(ArenaManifold &manifold,
                                          const DimRef primaryDim,
                                          const std::uint64_t v0Hash) {
  const std::string name = std::format("d.conflict_{}_{}_{}", primaryDim,
                                       v0Hash, ++conflictCounter_);
  return manifold.ensureDimension(name);
}

ResolutionResult CRDTEngine::applyLink(ArenaManifold &manifold,
                                       const ConcurrentLinkOp &op,
                                       const ConflictStrategy strategy) {
  if (!op.fromCell.has_value() || !op.toCell.has_value() ||
      !op.dim.has_value() || *op.fromCell == noCell || *op.toCell == noCell ||
      *op.dim == noCell) {
    return ResolutionResult{.applied     = false,
                            .promoted    = false,
                            .resolvedDim = std::nullopt,
                            .explanation =
                                "Invalid or missing endpoint or dimension"};
  }

  const auto fromCell = *op.fromCell;
  const auto toCell   = *op.toCell;
  const auto dim      = *op.dim;

  // Check if slot along (fromCell, dim, dir) is already occupied
  const auto existing = manifold.linked(fromCell, dim, op.dir);

  if (!existing.has_value()) {
    // Slot free: link directly
    expectWritten(manifold.link(fromCell, dim, op.dir, toCell));
    return ResolutionResult{
        .applied     = true,
        .promoted    = false,
        .resolvedDim = dim,
        .explanation = "Linked directly onto free slot",
    };
  }

  if (*existing == toCell) {
    // Idempotent duplicate
    return ResolutionResult{
        .applied     = true,
        .promoted    = false,
        .resolvedDim = dim,
        .explanation = "Idempotent link match",
    };
  }

  // Conflict detected!
  if (ConflictStrategy::DimensionPromotion == strategy) {
    // Strategy B: Allocate an orthogonal collision dimension
    const DimRef cDim =
        getOrCreateConflictDim(manifold, dim, op.key.microversion);
    expectWritten(manifold.link(fromCell, cDim, op.dir, toCell));
    return ResolutionResult{
        .applied     = true,
        .promoted    = true,
        .resolvedDim = cDim,
        .explanation = std::format(
            "Promoted conflicting link to orthogonal dimension {}", cDim),
    };
  }

  // Strategy A on a single op arrival when slot is occupied:
  // Splice after the existing target
  CellRef insertPoint = *existing;
  while (true) {
    const auto next = manifold.linked(insertPoint, dim, op.dir);
    if (!next.has_value()) {
      break;
    }
    insertPoint = *next;
  }
  expectWritten(manifold.link(insertPoint, dim, op.dir, toCell));
  return ResolutionResult{
      .applied     = true,
      .promoted    = false,
      .resolvedDim = dim,
      .explanation = "Spliced sequentially after existing rank tail",
  };
}

std::vector<ResolutionResult>
CRDTEngine::applyBatchSplice(ArenaManifold &manifold,
                             std::vector<ConcurrentLinkOp> ops) {
  std::vector<ResolutionResult> results;
  results.reserve(ops.size());

  if (ops.empty()) {
    return results;
  }

  // Sort descending by OpKey (RGA total order)
  std::sort(ops.begin(), ops.end(),
            [](const ConcurrentLinkOp &a, const ConcurrentLinkOp &b) {
              return a.key > b.key;
            });

  CellRef curr        = *ops.front().fromCell;
  const DimRef dim    = *ops.front().dim;
  const DimVector dir = ops.front().dir;

  for (const auto &op : ops) {
    if (!op.toCell.has_value()) {
      continue;
    }
    expectWritten(manifold.link(curr, dim, dir, *op.toCell));
    results.push_back(ResolutionResult{
        .applied     = true,
        .promoted    = false,
        .resolvedDim = dim,
        .explanation = "Spliced sequentially in RGA total order",
    });
    curr = *op.toCell;
  }

  return results;
}

bool CRDTEngine::verifyManifoldInvariants(
    const ArenaManifold &manifold) noexcept {
  const auto allCells = manifold.cells();

  for (std::size_t i = 0; i < allCells.size(); ++i) {
    const auto cellRef = static_cast<CellRef>(i);
    if (!manifold.contains(cellRef)) {
      continue;
    }

    const auto dims = manifold.dimensionsOf(cellRef);
    std::unordered_set<DimRef> seenPos;
    std::unordered_set<DimRef> seenNeg;

    for (const auto &dl : dims) {
      if (dl.pos != noCell) {
        if (seenPos.contains(dl.dim)) {
          return false; // deg^+(c, d) > 1 violation!
        }
        seenPos.insert(dl.dim);
      }
      if (dl.neg != noCell) {
        if (seenNeg.contains(dl.dim)) {
          return false; // deg^-(c, d) > 1 violation!
        }
        seenNeg.insert(dl.dim);
      }
    }
  }

  return true;
}

} // namespace zigzag::crdt
