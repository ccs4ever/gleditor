#ifndef COMMON_XANADU_ZIGZAG_QUERY_SCALAR_HPP
#define COMMON_XANADU_ZIGZAG_QUERY_SCALAR_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag {

/// Scalar value read by query operations, in the same order as Vortex
/// CellValue.
using QueryScalar = std::variant<std::string, double, std::int64_t, bool>;

enum class QueryScalarReadError {
  Missing,
  InvalidForeign,
  UnsupportedHandle,
  Unreadable,
};

inline constexpr std::string_view runtimeObjectProbeText = "<runtime-object>";
inline constexpr std::string_view runtimeObjectClaimDimensionName =
    "d.runtime-object";

struct RuntimeObjectReadLimits {
  std::size_t maxRootRelations{64};
  std::size_t maxMappingWork{4096};
};

enum class RuntimeObjectClaimResult {
  NoClaim,
  Claimed,
  ProbeLimitExceeded,
  Unreadable,
};

/// Recognise a structural Vortex runtime object marker. Text alone never
/// claims an object; the root must also carry the reserved positive claim edge.
[[nodiscard]] RuntimeObjectClaimResult
classifyRuntimeObjectRoot(const ArenaManifold &arena, CellRef root,
                          DimRef claimDimension,
                          RuntimeObjectReadLimits limits = {});

/// Resolve clone and foreign identity, then read typed bits or untyped text.
/// The runtime-object claim dimension is resolved once by VortexCore. Text
/// bytes and clone hops are charged to the arena's execution budget.
[[nodiscard]] std::expected<QueryScalar, QueryScalarReadError>
readQueryScalar(ArenaManifold &arena, CellRef cell, DimRef cloneDimension,
                DimRef runtimeObjectClaimDimension,
                RuntimeObjectReadLimits limits = {});

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_QUERY_SCALAR_HPP
