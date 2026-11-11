#pragma once

#include <expected>
#include <string>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vagent {

struct BootstrapPair {
  zigzag::CellRef writer{zigzag::noCell};
  zigzag::CellRef linker{zigzag::noCell};
};

/// Stage 4 (P7) Bootstrap fragment pair (`boot:writer` and `boot:linker`):
/// Mints (or returns if already filed on `programRoot +d.vars` under
/// `"$boot:writer"` and `"$boot:linker"`) the two bootstrap fragment objects
/// with full head, descriptor, `+d.refs`, and `+d.recipe` apparatus wrapped in
/// `Choice` -> reverse `Resolve` prologue -> `OpcodeKind::Value` (4-in/1-out) /
/// `OpcodeKind::Link` (4-in/1-out) -> `Cut` -> `Return`.
[[nodiscard]] std::expected<BootstrapPair, std::string>
ensureBootstrapPair(zigzag::vortex::VortexCore &core,
                    zigzag::vortex::VortexVM &vm, zigzag::CellRef programRoot);

} // namespace xanadu::vagent
