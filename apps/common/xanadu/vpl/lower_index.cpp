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
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerWhereMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerMatchMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

CellRef lowerMemberDyadic(LowerContext &ctx, const DyadicExpr &, CellRef lCell,
                          CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

CellRef lowerMatchDyadic(LowerContext &ctx, const DyadicExpr &, CellRef lCell,
                         CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

Maybe lowerIndexing(LowerContext &, const IndexingExpr &) {
  throw std::invalid_argument("VPL indexing is unsupported by the VM");
}

} // namespace xanadu::vpl::lower
