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
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

CellRef lowerCatenateDyadic(LowerContext &, const DyadicExpr &) {
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

Maybe lowerVectorOfRanks(LowerContext &, const VectorExpr &,
                         const std::vector<CellRef> &) {
  throw std::invalid_argument(
      "nested VPL ranks require explicit array lowering");
}

} // namespace xanadu::vpl::lower
