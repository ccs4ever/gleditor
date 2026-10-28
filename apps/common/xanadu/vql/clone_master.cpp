#include "common/xanadu/vql/clone_master.hpp"

#include <unordered_set>

namespace xanadu::vql {

std::optional<zigzag::CellRef> queryCloneMaster(zigzag::ArenaManifold &arena,
                                                zigzag::CellRef cell,
                                                zigzag::DimRef cloneDimension) {
  if (!arena.contains(cell)) return std::nullopt;
  std::unordered_set<zigzag::CellRef> seen{cell};
  auto current = cell;
  for (;;) {
    arena.consumeWork();
    const auto next =
        arena.linkedMinting(current, cloneDimension, zigzag::DimVector::NEG);
    if (!next) return current;
    if (!seen.insert(*next).second) return cell;
    current = *next;
  }
}

} // namespace xanadu::vql
