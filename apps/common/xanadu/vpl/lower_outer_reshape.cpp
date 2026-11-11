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
  refuse(msg::dyadicUnsupported);
}

Maybe lowerOuterProductMonadic(LowerContext &, const MonadicExpr &,
                               const ConjunctionExpr &) {
  refuse(msg::customUnsupported);
}

CellRef lowerOuterProductDyadic(LowerContext &ctx, const DyadicExpr &,
                                const ConjunctionExpr &, CellRef lCell,
                                CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

} // namespace xanadu::vpl::lower
