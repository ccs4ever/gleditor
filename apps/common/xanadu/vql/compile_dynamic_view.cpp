// Q5: dynamic shape projection, lens put/get, shape values.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

void VQLCompiler::compileDynamicShapeProjection(
    const ShapeLiteral &shape, std::optional<CellRef> currentStream) {
  (void)shape;
  (void)currentStream;
}

std::vector<CellRef> VQLCompiler::compileLensStep(const PathStep &step,
                                                  const CellRef current,
                                                  const bool hasContext) {
  (void)hasContext;
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  if (lensDefinitions_.contains(fn.name)) {
    return compileSteps(lensDefinitions_.at(fn.name).steps, current);
  }
  if (fn.name == "lens") {
    return {current};
  }
  return {};
}

std::vector<CellRef>
VQLCompiler::compileLensCall(const FunctionInvocation &fn,
                             const std::optional<CellRef> context) {
  if (lensDefinitions_.contains(fn.name)) {
    return compileSteps(lensDefinitions_.at(fn.name).steps, context);
  }
  if (fn.name == "lens" && context) {
    return {*context};
  }
  return {};
}

std::optional<CellRef>
VQLCompiler::compileLensValue(const FunctionInvocation &fn,
                              const CellRef ctxCell) {
  if (lensDefinitions_.contains(fn.name)) {
    auto res = compileSteps(lensDefinitions_.at(fn.name).steps, ctxCell);
    return res.empty() ? std::nullopt : std::optional{res.front()};
  }
  if (fn.name == "lens") {
    return ctxCell;
  }
  return std::nullopt;
}

std::optional<CellRef> VQLCompiler::compileShapeValue(const ShapeLiteral &shape,
                                                      const CellRef ctxCell) {
  (void)shape;
  return ctxCell;
}

} // namespace xanadu::vql
