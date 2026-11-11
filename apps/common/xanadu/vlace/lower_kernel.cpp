/**
 * @file lower_kernel.cpp
 * @brief Card L2: the thunk / force / apply kernel, `let`, lambda, application,
 *        letrec and top-level declarations.
 *
 * Stubs written by card L0; each refuses until L2 fills it in. The shared
 * helpers (lowerLazy, forceOperand, applyOperand) are called by L3-L8 and must
 * keep these signatures.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

Lowered<Operand> lowerLet(LowerContext &, const ELet &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerLambda(LowerContext &, const ELambda &,
                             SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerApp(LowerContext &, const EApp &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerDollar(LowerContext &, const EBinOp &,
                             SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<void> lowerValueDecl(LowerContext &, const ValueDecl &,
                             SourceLocation loc) {
  return notLowered("programs", loc);
}

std::optional<Lowered<Operand>> lookupTopLevel(LowerContext &, std::string_view,
                                               SourceLocation) {
  // No declaration is registered until lowerValueDecl exists, so an empty
  // table answering "no such name" is the correct behaviour, not a refusal.
  return std::nullopt;
}

Lowered<Operand> lowerLazy(LowerContext &, const ExprNode &e) {
  return notLowered("expressions", e.loc);
}

Lowered<Operand> forceOperand(LowerContext &, Operand, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> applyOperand(LowerContext &, Operand, Operand,
                              SourceLocation loc) {
  return notLowered("expressions", loc);
}

std::expected<ValuePtr, EvalError> forceValueOnVm(Compiler &, ValuePtr) {
  return fail(CompileError::UnexpectedToken,
              "VM thunk forcing is not implemented for VLace values");
}

std::optional<Lowered<ValuePtr>> readbackKernel(Compiler &, zigzag::CellRef) {
  return std::nullopt;
}

} // namespace xanadu::vlace::lower
