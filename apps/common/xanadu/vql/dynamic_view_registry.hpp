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

  /// Forget every entry that names a cell @p arena no longer holds. A run
  /// whose cells were rolled back leaves such keys; a later mint into a freed
  /// slot would otherwise overwrite one without changing the map's size.
  void dropFreed(const zigzag::ArenaManifold &arena) {
    const auto gone = [&arena](const zigzag::CellRef ref) {
      return !arena.contains(ref);
    };
    std::erase_if(viewCells, [&](const auto &e) { return gone(e.first); });
    std::erase_if(ownerViews, [&](const auto &e) {
      return gone(e.first) || gone(e.second);
    });
    std::erase_if(cellLenses, [&](const auto &e) { return gone(e.first); });
    std::erase_if(shapeHandles, [&](const auto &e) { return gone(e.first); });
  }

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
