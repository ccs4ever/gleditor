/**
 * @file lower_index.cpp
 * @brief Card V11 indexing, grade, where, match, member.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerGradeMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  refuseMonadicOperand(ctx, rCell);
}

Maybe lowerWhereMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  refuseMonadicOperand(ctx, rCell);
}

Maybe lowerMatchMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  refuseMonadicOperand(ctx, rCell);
}

CellRef lowerMemberDyadic(LowerContext &ctx, const DyadicExpr &, CellRef lCell,
                          CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

CellRef lowerMatchDyadic(LowerContext &ctx, const DyadicExpr &, CellRef lCell,
                         CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

Maybe lowerIndexing(LowerContext &, const IndexingExpr &) {
  refuse(msg::indexing);
}

} // namespace xanadu::vpl::lower
