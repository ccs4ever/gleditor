/**
 * @file lower_catenate.cpp
 * @brief Card V8 catenate and ranks inside literal vectors.
 *
 * Refusing stubs for the dispatch sites listed in lower_decls.hpp. The card
 * that owns this file replaces each body and keeps the refusals for the forms
 * it still does not lower.
 */
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

Maybe lowerRavelMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

CellRef lowerCatenateDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

Maybe lowerVectorOfRanks(LowerContext &, const VectorExpr &,
                         const std::vector<CellRef> &) {
  refuse(msg::nestedRanks);
}

} // namespace xanadu::vpl::lower
