/**
 * @file lower_hypertime.cpp
 * @brief Card V12 hypertime and transclusion.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerHypertimeMonadic(LowerContext &ctx, const MonadicExpr &,
                            CellRef rCell) {
  refuseMonadicOperand(ctx, rCell);
}

Maybe lowerTranscludeMonadic(LowerContext &ctx, const MonadicExpr &,
                             CellRef rCell) {
  refuseMonadicOperand(ctx, rCell);
}

} // namespace xanadu::vpl::lower
