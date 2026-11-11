/**
 * @file lower_io.cpp
 * @brief Card L6: IO descriptions (`pure bind then putStr putStrLn getLine`,
 *        `>>=`, `>>`) and the host trampoline that performs them.
 *
 * Stubs written by card L0; each refuses until L6 fills it in.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

namespace {
constexpr std::string_view ioBuiltinNames[] = {"pure",   "bind",     "then",
                                               "putStr", "putStrLn", "getLine"};
} // namespace

Lowered<Operand> lowerIoBind(LowerContext &, const EBinOp &,
                             SourceLocation loc) {
  return notLowered("expressions", loc);
}

std::optional<Lowered<Operand>>
lowerIoBuiltin(LowerContext &, std::string_view name, SourceLocation loc) {
  if (!isOneOf(name, ioBuiltinNames)) return std::nullopt;
  return notLowered("IO programs", loc);
}

std::expected<ValuePtr, EvalError> runIoOnVm(Compiler &, const ValuePtr &,
                                             std::istream &, std::ostream &) {
  return notLowered("IO actions");
}

std::expected<DriverResult, DriverError> driverEvaluateIO(const Program &,
                                                          DriverResult,
                                                          std::istream &,
                                                          std::ostream &) {
  return std::unexpected(DriverError{
      .code   = CompileError::UnexpectedToken,
      .detail = "VM lowering is not implemented for VLace IO programs",
      .phase  = "compile"});
}

} // namespace xanadu::vlace::lower
