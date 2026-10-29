#include "common/xanadu/zigzag/query_scalar.hpp"

#include "common/xanadu/zigzag/query_clone_master.hpp"

namespace zigzag {

std::expected<QueryScalar, QueryScalarReadError>
readQueryScalar(ArenaManifold &arena, const CellRef cell,
                const DimRef cloneDimension) {
  if (!arena.contains(cell))
    return std::unexpected(QueryScalarReadError::Missing);

  const CellRef master =
      queryCloneMaster(arena, cell, cloneDimension).value_or(cell);
  if (!arena.contains(master))
    return std::unexpected(QueryScalarReadError::Missing);

  auto kind = arena.valueKindOf(master);
  if (const auto foreign = arena.foreignOf(master)) {
    const auto space = arena.spaceAt(foreign->space);
    if (!space || !space->manifold ||
        !space->manifold->contains(foreign->index)) {
      return std::unexpected(QueryScalarReadError::InvalidForeign);
    }
    kind = space->manifold->valueKindOf(foreign->index);
  }

  if (kind == xanadu::ValueKind::OpHandle ||
      kind == xanadu::ValueKind::ExternRef ||
      kind == xanadu::ValueKind::ForeignOpHandle) {
    return std::unexpected(QueryScalarReadError::UnsupportedHandle);
  }
  if (kind == xanadu::ValueKind::Int64) {
    if (const auto value = arena.asInt64(master)) return QueryScalar{*value};
    return std::unexpected(QueryScalarReadError::Unreadable);
  }
  if (kind == xanadu::ValueKind::Double) {
    if (const auto value = arena.asDouble(master)) return QueryScalar{*value};
    return std::unexpected(QueryScalarReadError::Unreadable);
  }
  if (kind == xanadu::ValueKind::Bool) {
    if (const auto value = arena.asBool(master)) return QueryScalar{*value};
    return std::unexpected(QueryScalarReadError::Unreadable);
  }

  if (kind != xanadu::ValueKind::None && kind != xanadu::ValueKind::Timestamp)
    return std::unexpected(QueryScalarReadError::Unreadable);

  const auto spans = arena.contentOf(master);
  for (const auto &span : spans) {
    for (std::uint64_t byte = 0; byte < span.length; ++byte)
      arena.consumeWork();
  }
  return QueryScalar{arena.textOf(master)};
}

} // namespace zigzag
