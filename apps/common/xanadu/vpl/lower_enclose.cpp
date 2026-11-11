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
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

Maybe lowerDiscloseMonadic(LowerContext &, const MonadicExpr &) {
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

} // namespace xanadu::vpl::lower
