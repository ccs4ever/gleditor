#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

/// Stage 7: In-Language / R1 AST Parser Bridge
/// Parses expression @p source into a 2-degree R1 AST in @p arena.
[[nodiscard]] std::expected<zigzag::CellRef, std::string>
parseSourceToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                std::string_view source);

/// Parses full program module @p source into a 2-degree R1 AST in @p arena.
[[nodiscard]] std::expected<zigzag::CellRef, std::string>
parseProgramToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                 std::string_view source);

} // namespace xanadu::vagent
