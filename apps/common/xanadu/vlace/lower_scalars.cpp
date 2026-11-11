/**
 * @file lower_scalars.cpp
 * @brief Card L1: literals, variables, scalar operators, `if`, True/False.
 *
 * Stubs written by card L0; each refuses until L1 fills it in.
 */
#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

Lowered<Operand> lowerLiteral(LowerContext &, const ELiteral &,
                              SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerVar(LowerContext &ctx, const EVar &e,
                          SourceLocation loc) {
  if (const auto hit = ctx.lookup(e.name); hit && hit->frameDepth > 0)
    return captureOperand(ctx, *hit, loc); // L2
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerBinOp(LowerContext &, const EBinOp &,
                            SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerIf(LowerContext &, const EIf &, SourceLocation loc) {
  return notLowered("expressions", loc);
}

Lowered<Operand> lowerBoolCtor(LowerContext &, bool, SourceLocation loc) {
  return notLowered("expressions", loc);
}

} // namespace xanadu::vlace::lower
