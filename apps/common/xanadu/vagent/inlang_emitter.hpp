#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

struct InlangEmitResult {
  bool ok{false};
  std::optional<zigzag::CellRef> head{std::nullopt};
  std::string label{};
  std::string error{};
};

/// Stage 5: Emitter-in-Language
/// Emits an R1 AST @p r1Root with parameters @p params into an executable
/// fragment using `join_batch` and `build_fragment`, and registers it via
/// `register(head)`.
[[nodiscard]] InlangEmitResult
runInlangEmitter(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                 zigzag::vortex::VortexVM &vm, MetaRuntime &meta,
                 std::string_view label, zigzag::CellRef r1Root,
                 std::optional<zigzag::CellRef> params = std::nullopt);

} // namespace xanadu::vagent
