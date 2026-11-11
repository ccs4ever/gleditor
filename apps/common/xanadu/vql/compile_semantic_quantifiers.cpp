// S2: semantic builtins any, all, none, one, optional_one, first, same_cell.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef>
VQLCompiler::compileSemanticQuantifierStep(const PathStep &step,
                                           const CellRef /*current*/,
                                           const bool /*hasContext*/) {
  refuseSemanticBuiltin(std::get<FunctionInvocation>(step.selector).name);
}

std::vector<CellRef> VQLCompiler::compileSemanticQuantifierCall(
    const FunctionInvocation &fn, const std::optional<CellRef> /*context*/) {
  refuseSemanticBuiltin(fn.name);
}

} // namespace xanadu::vql
