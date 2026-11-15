#ifndef COMMON_XANADU_VQL_DYNAMIC_VIEW_REGISTRY_HPP
#define COMMON_XANADU_VQL_DYNAMIC_VIEW_REGISTRY_HPP

#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xanadu::vql {

struct DynamicViewRecord {
  ShapeLiteral shape;
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> handles;
};

struct ViewCellRecord {
  zigzag::CellRef source;
  std::shared_ptr<DynamicViewRecord> view;
};

struct PutGetLensRecord {
  PathLambda get;
  PathLambda put;
};

struct DynamicViewRegistry {
  std::unordered_map<zigzag::CellRef, ViewCellRecord> viewCells;
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> ownerViews;
  std::unordered_map<std::string, PutGetLensRecord> namedLenses;
  std::unordered_map<zigzag::CellRef, PutGetLensRecord> cellLenses;
  std::unordered_map<zigzag::CellRef, ShapeLiteral> shapeHandles;

  void clear() noexcept {
    viewCells.clear();
    ownerViews.clear();
    namedLenses.clear();
    cellLenses.clear();
    shapeHandles.clear();
  }
};

bool isDynamicShape(const ShapeLiteral &shape);

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_DYNAMIC_VIEW_REGISTRY_HPP
