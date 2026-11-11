#pragma once

#include <optional>
#include <string_view>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vagent {

/// Stage 3 (P14) Host-service compile boundary:
/// Compiles a fragment body expression from `srcCell`'s text with head `label`,
/// parameters read from `paramsCell` (either a cell whose `+d.grab` ->
/// `+d.step` chain holds parameter cells with text = name and `+d.values` ->
/// type cell, or a `+d.step` list of `"name:Type"` / `"name"` cells), and
/// `resultType` string. Bills `arena.consumeWork()`, emits into the live arena,
/// calls `meta.registerCode(head)`, and returns the head `CellRef` on success.
/// On failure, writes `fault:<label>` on `meta.agentCell() +d.vars` and returns
/// `std::nullopt`.
[[nodiscard]] std::optional<zigzag::CellRef>
hostCompileFragment(MetaRuntime &meta, zigzag::vortex::VortexCore &core,
                    zigzag::vortex::VortexVM &vm, std::string_view label,
                    zigzag::CellRef paramsCell, std::string_view resultType,
                    zigzag::CellRef srcCell);

} // namespace xanadu::vagent
