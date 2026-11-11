// Q9: field weaving in return clauses.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

void VQLCompiler::compileFieldWeave(const FieldWeave & /*weave*/,
                                    const CellRef /*resultStream*/) {
  throw std::invalid_argument("field weaving requires Vortex graph lowering");
}

} // namespace xanadu::vql
