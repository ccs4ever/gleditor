#include "common/xanadu/zigzag/query_clone_master.hpp"

#include <unordered_set>

namespace zigzag {

std::optional<CellRef> queryCloneMaster(ArenaManifold &arena, CellRef cell,
                                        DimRef cloneDimension) {
  if (!arena.contains(cell)) return std::nullopt;
  std::unordered_set<CellRef> seen{cell};
  auto current = cell;
  for (;;) {
    arena.consumeWork();
    const auto next =
        arena.linkedMinting(current, cloneDimension, DimVector::NEG);
    if (!next) return current;
    if (!seen.insert(*next).second) return cell;
    current = *next;
  }
}

} // namespace zigzag
