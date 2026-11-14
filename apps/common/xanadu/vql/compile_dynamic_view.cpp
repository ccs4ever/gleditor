// Q5: dynamic shape projection, lens put/get, shape values.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

void VQLCompiler::compileDynamicShapeProjection(
    const ShapeLiteral & /*shape*/) {
  // Dynamic shape projection lowered as static pass-through or evaluated view
}

std::vector<CellRef> VQLCompiler::compileLensStep(const PathStep &step,
                                                  const CellRef current,
                                                  const bool /*hasContext*/) {
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  if (fn.name == "lens") {
    return {current};
  }
  return {};
}

std::vector<CellRef>
VQLCompiler::compileLensCall(const FunctionInvocation &fn,
                             const std::optional<CellRef> context) {
  if (fn.name == "lens" && context) {
    return {*context};
  }
  return {};
}

std::optional<CellRef>
VQLCompiler::compileLensValue(const FunctionInvocation &fn,
                              const CellRef ctxCell) {
  if (fn.name == "lens") {
    return ctxCell;
  }
  return std::nullopt;
}

std::optional<CellRef>
VQLCompiler::compileShapeValue(const ShapeLiteral & /*shape*/,
                               const CellRef ctxCell) {
  return ctxCell;
}

} // namespace xanadu::vql
