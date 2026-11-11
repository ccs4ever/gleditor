// Q1: path functions sort_into, find, contains, count.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef>
VQLCompiler::compilePathFunctionStep(const PathStep &step,
                                     const CellRef /*current*/,
                                     const bool /*hasContext*/) {
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "VQL path function '" + fn.name +
                         "' is not compiled to Vortex yet");
}

std::vector<CellRef>
VQLCompiler::compilePathFunctionCall(const FunctionInvocation &fn,
                                     const std::optional<CellRef> /*context*/) {
  throw QueryRefusal(QueryErrorCategory::Unknown,
                     "unknown compiled function: " + fn.name);
}

} // namespace xanadu::vql
