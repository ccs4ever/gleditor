// Q2: root selectors, perspective read steps, macro-group selectors.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef>
VQLCompiler::compileRootSelectorStep(const PathStep & /*step*/,
                                     const CellRef /*current*/,
                                     const bool /*hasContext*/) {
  throw std::invalid_argument(
      "this VQL path selector does not have Vortex graph lowering yet");
}

} // namespace xanadu::vql
