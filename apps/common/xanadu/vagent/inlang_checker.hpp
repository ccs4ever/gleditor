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

struct InlangCheckResult {
  bool ok{false};
  std::string resultType{"()"};
  bool pure{false};
  bool closed{false};
  std::size_t arity{0};
  std::string error{};
};

/// Stage 6: Checker-in-Language
/// Runs the in-language static checker
/// (`apps/common/xanadu/vagent/stdlib/checker.vagent`) over the 2-degree R1 AST
/// @p r1Root with optional @p params.
///
/// Passes performed by `checker.vagent`:
/// - Pass 1: Symbol & Parameter Scope Resolution (reject reserved parameter
/// names,
///   reject unimported reserved std:meta identifiers, resolve bindings/params)
/// - Pass 2: Type Inference & Per-Node Annotation (assign type names on
/// +d.values,
///   enforce condition-position rules, verify operator types)
/// - Pass 3: Purity, Closure, Arity, and Capture Synthesis (compute `pure`,
/// `closed`,
///   `arity`, and annotate the root with descriptor metadata on +d.vars)
[[nodiscard]] InlangCheckResult
runInlangChecker(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                 zigzag::vortex::VortexVM &vm, MetaRuntime &meta,
                 zigzag::CellRef r1Root,
                 std::optional<zigzag::CellRef> params = std::nullopt,
                 bool importStdMeta                    = true);

} // namespace xanadu::vagent
