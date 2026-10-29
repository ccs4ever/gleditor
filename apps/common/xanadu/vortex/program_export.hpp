#ifndef COMMON_XANADU_VORTEX_PROGRAM_EXPORT_HPP
#define COMMON_XANADU_VORTEX_PROGRAM_EXPORT_HPP

#include <optional>
#include <span>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {

/// Persist an executable graph with explicit operand and cursor relocation.
[[nodiscard]] xanadu::MicroversionId
exportProgram(const VortexCore &core, const VortexVM &vm,
              std::span<const CellRef> opcodes, xanadu::Store &store,
              std::optional<xanadu::MicroversionId> parent = std::nullopt);

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_PROGRAM_EXPORT_HPP
