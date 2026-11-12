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

std::vector<CellRef> VQLCompiler::compileSemanticQuantifierWalk(
    const FunctionInvocation &fn, const std::span<const CellRef> /*stream*/,
    const bool /*hasContext*/) {
  refuseSemanticBuiltin(fn.name);
}

std::optional<CellRef>
VQLCompiler::compileSemanticQuantifierValue(const FunctionInvocation &fn,
                                            const CellRef /*ctxCell*/) {
  // A value-position use has never had the semantic-builtin message; keep it.
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "unknown compiled function: " + fn.name);
}

} // namespace xanadu::vql
