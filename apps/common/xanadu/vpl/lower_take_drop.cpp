/**
 * @file lower_take_drop.cpp
 * @brief Card V5 take and drop.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

CellRef lowerTakeDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

CellRef lowerDropDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

} // namespace xanadu::vpl::lower
