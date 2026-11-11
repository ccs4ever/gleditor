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
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

CellRef lowerScrubDyadic(LowerContext &, const DyadicExpr &) {
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

CellRef lowerCompressDyadic(LowerContext &ctx, const DyadicExpr &,
                            CellRef lCell, CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

Maybe lowerConjunctionMonadic(LowerContext &, const MonadicExpr &,
                              const ConjunctionExpr &) {
  throw std::invalid_argument("unsupported VPL custom verb in VM compiler");
}

CellRef lowerConjunctionDyadic(LowerContext &ctx, const DyadicExpr &,
                               const ConjunctionExpr &, CellRef lCell,
                               CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

CellRef lowerConjunctionExpr(LowerContext &, const ConjunctionExpr &) {
  throw std::invalid_argument("VPL conjunctions are unsupported by the VM");
}

Maybe lowerQuad(LowerContext &, const QuadExpr &) {
  throw std::invalid_argument("VPL quad IO is unsupported by the VM");
}

} // namespace xanadu::vpl::lower
