/**
 * @file lower_misc.cpp
 * @brief Card V13 print marker, compress, key, rank operator and named
 * refusals.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerScrubMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  refuseMonadicOperand(ctx, rCell);
}

CellRef lowerScrubDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

CellRef lowerCompressDyadic(LowerContext &ctx, const DyadicExpr &,
                            CellRef lCell, CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

Maybe lowerConjunctionMonadic(LowerContext &, const MonadicExpr &,
                              const ConjunctionExpr &) {
  refuse(msg::customUnsupported);
}

CellRef lowerConjunctionDyadic(LowerContext &ctx, const DyadicExpr &,
                               const ConjunctionExpr &, CellRef lCell,
                               CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

CellRef lowerConjunctionExpr(LowerContext &, const ConjunctionExpr &) {
  refuse(msg::conjunctions);
}

Maybe lowerQuad(LowerContext &, const QuadExpr &) { refuse(msg::quad); }

} // namespace xanadu::vpl::lower
