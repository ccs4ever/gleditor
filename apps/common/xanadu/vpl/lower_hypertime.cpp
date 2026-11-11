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
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerTranscludeMonadic(LowerContext &ctx, const MonadicExpr &,
                             CellRef rCell) {
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

} // namespace xanadu::vpl::lower
