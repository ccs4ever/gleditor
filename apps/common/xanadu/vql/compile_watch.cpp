// Q4: watch statements, scoped perspective blocks, mount declarations.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::optional<CellRef> VQLCompiler::compileWatchOrPerspectiveStatement(
    const Statement & /*statement*/) {
  throw std::invalid_argument(
      "watch and perspective statements require Vortex graph lowering");
}

void VQLCompiler::compileMountDeclarations(const QueryExpression & /*query*/) {
  throw std::invalid_argument(
      "VQL mount declarations require Vortex graph lowering");
}

} // namespace xanadu::vql
