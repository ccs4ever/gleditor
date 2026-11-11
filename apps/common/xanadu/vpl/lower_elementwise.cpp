/**
 * @file lower_elementwise.cpp
 * @brief Card V6 rank arithmetic, each and power.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerPowerMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerSignumMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerFloorMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerCeilingMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

CellRef lowerPowerDyadic(LowerContext &, const DyadicExpr &) {
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

Maybe lowerMonadicArithmeticOnRank(LowerContext &, const MonadicExpr &,
                                   CellRef) {
  throw std::invalid_argument(
      "VPL scalar monadic arithmetic on a rank requires array lowering");
}

CellRef lowerDyadicArithmeticOnRank(LowerContext &, const DyadicExpr &, CellRef,
                                    CellRef) {
  throw std::invalid_argument(
      "VPL scalar dyadic arithmetic on ranks requires array lowering");
}

Maybe lowerEachMonadic(LowerContext &, const MonadicExpr &expr,
                       const AdverbExpr &adverb) {
  // The reduce/scan verb check ran before the each refusal when this lived in
  // compileMonadic, so `-¨ x` reported the reduce/scan message. Kept byte for
  // byte until this card lowers each.
  TokenKind innerVerb = TokenKind::Plus;
  if (const auto *v = dynamic_cast<const VerbExpr *>(adverb.operand().get())) {
    innerVerb = v->verb();
  }
  (void)expr;
  if (innerVerb != TokenKind::Plus && innerVerb != TokenKind::Times &&
      innerVerb != TokenKind::Ceiling && innerVerb != TokenKind::Floor) {
    throw std::invalid_argument(
        "VPL VM reduce/scan supports only +, *, maximum, and minimum");
  }
  throw std::invalid_argument("VPL VM each adverb is unsupported");
}

CellRef lowerEachDyadic(LowerContext &ctx, const DyadicExpr &,
                        const AdverbExpr &, CellRef lCell, CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

} // namespace xanadu::vpl::lower
