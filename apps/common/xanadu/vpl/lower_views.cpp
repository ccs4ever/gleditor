/**
 * @file lower_views.cpp
 * @brief Card V4 view verbs: transpose, reverse, dimension values,
 * rho-to-dimensions.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerTransposeMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerReverseMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

CellRef lowerRhoViewDyadic(LowerContext &, const DyadicExpr &) {
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

CellRef lowerTransposeDyadic(LowerContext &ctx, const DyadicExpr &,
                             CellRef lCell, CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

CellRef lowerDimension(LowerContext &, const DimensionExpr &) {
  throw std::invalid_argument(
      "VPL dimension values require typed-handle VM lowering");
}

} // namespace xanadu::vpl::lower
