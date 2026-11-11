// S1: semantic builtins project and apply_shape.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef>
VQLCompiler::compileSemanticShapeStep(const PathStep &step,
                                      const CellRef /*current*/,
                                      const bool /*hasContext*/) {
  refuseSemanticBuiltin(std::get<FunctionInvocation>(step.selector).name);
}

std::vector<CellRef> VQLCompiler::compileSemanticShapeCall(
    const FunctionInvocation &fn, const std::optional<CellRef> /*context*/) {
  refuseSemanticBuiltin(fn.name);
}

} // namespace xanadu::vql
