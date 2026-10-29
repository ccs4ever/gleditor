#ifndef COMMON_XANADU_ZIGZAG_QUERY_SCALAR_HPP
#define COMMON_XANADU_ZIGZAG_QUERY_SCALAR_HPP

#include <cstdint>
#include <expected>
#include <string>
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

/// Resolve clone and foreign identity, then read typed bits or untyped text.
/// Text bytes and clone hops are charged to the arena's execution budget.
[[nodiscard]] std::expected<QueryScalar, QueryScalarReadError>
readQueryScalar(ArenaManifold &arena, CellRef cell, DimRef cloneDimension);

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_QUERY_SCALAR_HPP
