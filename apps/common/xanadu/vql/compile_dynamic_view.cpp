// Q5: dynamic shape projection, lens put/get, shape values.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

void VQLCompiler::compileDynamicShapeProjection(
    const ShapeLiteral & /*shape*/) {
  throw std::invalid_argument(
      "shape projection requires static rules and scalar, alias, or "
      "read-only simple path targets");
}

std::vector<CellRef>
VQLCompiler::compileLensCall(const FunctionInvocation &fn,
                             const std::optional<CellRef> /*context*/) {
  throw QueryRefusal(QueryErrorCategory::Unknown,
                     "unknown compiled function: " + fn.name);
}

std::optional<CellRef>
VQLCompiler::compileShapeValue(const ShapeLiteral & /*shape*/,
                               const CellRef /*ctxCell*/) {
  throw std::invalid_argument(
      "value expression requires Vortex graph lowering");
}

} // namespace xanadu::vql
