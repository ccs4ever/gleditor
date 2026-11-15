#ifndef COMMON_XANADU_VORTEX_MESH_RESULT_HPP
#define COMMON_XANADU_VORTEX_MESH_RESULT_HPP

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {
class SpanReader;
}

namespace zigzag::vortex::mesh {

struct EvaluatedValue;

/// Bounds applied before result metadata is materialized or result cells are
/// appended to the returned vector.
struct ResultReadLimits {
  std::size_t maxItems{100'000};
  std::size_t maxMetadataRelations{1'000'000};
  std::size_t maxMetadataTextBytes{16 * 1024 * 1024};
};

/// Read the canonical target of one typed occurrence wrapper. This unwraps
/// exactly one layer; the target remains semantic data even if it is itself a
/// marked OpHandle.
[[nodiscard]] std::expected<CellRef, std::string>
readOccurrenceTarget(const ArenaManifold &arena, CellRef wrapper,
                     ResultReadLimits limits = {});
[[nodiscard]] std::expected<CellRef, std::string>
readOccurrenceTarget(const Manifold &manifold, CellRef wrapper,
                     const xanadu::SpanReader &reader,
                     ResultReadLimits limits = {});

/// Decode mesh:result/v2 as an ordered vector of canonical value cells. The
/// root count is an Int64 on d.mesh.count; d.mesh.items must contain exactly
/// that many rows. Errors reject the whole result and never return a partial
/// vector.
[[nodiscard]] std::expected<std::vector<CellRef>, std::string>
readResultCells(const ArenaManifold &arena, CellRef root,
                ResultReadLimits limits = {});
[[nodiscard]] std::expected<std::vector<CellRef>, std::string>
readResultCells(const Manifold &manifold, CellRef root,
                const xanadu::SpanReader &reader, ResultReadLimits limits = {});
[[nodiscard]] std::expected<std::vector<CellRef>, std::string>
readResultCells(const EvaluatedValue &value, ResultReadLimits limits = {});

} // namespace zigzag::vortex::mesh

#endif
