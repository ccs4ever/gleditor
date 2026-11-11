/**
 * @file run_vm.cpp
 * @brief Card L1: the run loop. Build a LowerContext, lower, assemble, run the
 *        VM, read the result back; and the Driver's evaluation half.
 *
 * Stubs written by card L0 keep the pre-lowering refusals byte for byte.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

std::expected<ValuePtr, EvalError> evaluateExpressionOnVm(Compiler &,
                                                          const Expr &) {
  return notLowered("expressions");
}

std::expected<ValuePtr, EvalError> evaluateProgramOnVm(Compiler &,
                                                       const Program &) {
  return notLowered("programs");
}

std::expected<DriverResult, DriverError> driverEvaluate(const Program &,
                                                        DriverResult) {
  return std::unexpected(
      DriverError{.code   = CompileError::UnexpectedToken,
                  .detail = "VM lowering is not implemented for VLace programs",
                  .phase  = "compile"});
}

} // namespace xanadu::vlace::lower
