/**
 * @file logic_relations_internal.hpp
 * @brief Internal definitions shared between logic_relations.cpp and
 * logic_relations_arena.cpp (Cards P3 and P4).
 */
#ifndef COMMON_XANADU_VORTEX_LOGIC_RELATIONS_INTERNAL_HPP
#define COMMON_XANADU_VORTEX_LOGIC_RELATIONS_INTERNAL_HPP

#include <cstdint>
#include <limits>
#include <optional>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex::logic {

/// Base class for relation providers that need access to VortexCore.
class CoreBoundProvider : public RelationProvider {
public:
  void bindCore(VortexCore *c) noexcept { core_ = c; }

protected:
  VortexCore *core_{nullptr};
};

/// Resolves an argument cell to a valid CellRef in the arena, checking that
/// Int64 scalars are within [0, CellRef::max()] and contained in the arena.
inline std::optional<CellRef> cellRefArgument(zigzag::ArenaManifold &arena,
                                              const CellRef argument) {
  if (const auto value = arena.asInt64(argument)) {
    if (*value < 0 || static_cast<std::uint64_t>(*value) >
                          std::numeric_limits<CellRef>::max()) {
      return std::nullopt;
    }
    const auto cell = static_cast<CellRef>(*value);
    return arena.contains(cell) ? std::optional<CellRef>{cell} : std::nullopt;
  }
  return arena.contains(argument) ? std::optional<CellRef>{argument}
                                  : std::nullopt;
}

} // namespace zigzag::vortex::logic

#endif // COMMON_XANADU_VORTEX_LOGIC_RELATIONS_INTERNAL_HPP
