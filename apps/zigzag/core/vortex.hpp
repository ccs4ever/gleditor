/**
 * @file vortex.hpp
 * @brief Forwarding header to common/xanadu/vortex/vortex_core.hpp and
 * vortex_vm.hpp.
 */
#ifndef ZIGZAG_CORE_VORTEX_HPP
#define ZIGZAG_CORE_VORTEX_HPP

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag {
namespace vortex = ::zigzag::vortex;
using vortex::CellValue;
using vortex::ContractViolationKind;
using vortex::ExecutionResult;
using vortex::OpcodeKind;
using vortex::SystemDimensions;
using vortex::VortexCore;
using vortex::VortexVM;
} // namespace zigzag

#endif // ZIGZAG_CORE_VORTEX_HPP
