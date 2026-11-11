/**
 * @file lower_enclose.cpp
 * @brief Card V10 enclose and disclose.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerEncloseMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerDiscloseMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

} // namespace xanadu::vpl::lower
