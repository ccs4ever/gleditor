/**
 * @file vortex.hpp
 * @brief Forwarding alias to apps/common/xanadu/vortex/vortex_core.hpp and
 * vortex_vm.hpp.
 */
#ifndef XUDU_CORE_VORTEX_FORWARD_HPP
#define XUDU_CORE_VORTEX_FORWARD_HPP

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xudu {
namespace vortex = ::zigzag::vortex;
using vortex::CellValue;
using vortex::ContractViolationKind;
using vortex::ExecutionResult;
using vortex::OpcodeKind;
using vortex::SystemDimensions;
using vortex::VortexCore;
using vortex::VortexVM;
} // namespace xudu

#endif // XUDU_CORE_VORTEX_FORWARD_HPP
