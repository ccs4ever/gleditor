/**
 * @file lower_iota.cpp
 * @brief Card V3 iota and monadic rho.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerIotaMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerRhoMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

CellRef lowerIotaDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

} // namespace xanadu::vpl::lower
