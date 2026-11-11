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
  refuse(msg::monadicUnsupported);
}

Maybe lowerReverseMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

CellRef lowerRhoViewDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

CellRef lowerRhoViewCompiled(LowerContext &, const DyadicExpr &, CellRef,
                             CellRef) {
  refuse(msg::dyadicUnsupported);
}

CellRef lowerTransposeDyadic(LowerContext &ctx, const DyadicExpr &,
                             CellRef lCell, CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

CellRef lowerDimension(LowerContext &, const DimensionExpr &) {
  refuse(msg::dimensionValues);
}

Maybe lowerDimensionVector(LowerContext &, const VectorExpr &,
                           const std::vector<CellRef> &) {
  refuse(msg::dimensionValues);
}

} // namespace xanadu::vpl::lower
