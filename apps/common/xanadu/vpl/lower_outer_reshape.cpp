/**
 * @file lower_outer_reshape.cpp
 * @brief Card V9 outer product and reshape.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

CellRef lowerRhoDyadic(LowerContext &, const DyadicExpr &) {
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

Maybe lowerOuterProductMonadic(LowerContext &, const MonadicExpr &,
                               const ConjunctionExpr &) {
  throw std::invalid_argument("unsupported VPL custom verb in VM compiler");
}

CellRef lowerOuterProductDyadic(LowerContext &ctx, const DyadicExpr &,
                                const ConjunctionExpr &, CellRef lCell,
                                CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

} // namespace xanadu::vpl::lower
