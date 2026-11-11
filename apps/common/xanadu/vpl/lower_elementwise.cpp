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
  refuse(msg::monadicUnsupported);
}

Maybe lowerSignumMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerFloorMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerCeilingMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

CellRef lowerPowerDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

Maybe lowerMonadicArithmeticOnRank(LowerContext &, const MonadicExpr &,
                                   CellRef) {
  refuse(msg::monadicOnRank);
}

CellRef lowerDyadicArithmeticOnRank(LowerContext &, const DyadicExpr &, CellRef,
                                    CellRef) {
  refuse(msg::dyadicOnRank);
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
    refuse(msg::reduceScanVerbs);
  }
  refuse(msg::eachUnsupported);
}

CellRef lowerEachDyadic(LowerContext &ctx, const DyadicExpr &,
                        const AdverbExpr &, CellRef lCell, CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

} // namespace xanadu::vpl::lower
