// Q5: dynamic shape projection, lens put/get, shape values.
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/dynamic_view_registry.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

bool isDynamicShape(const ShapeLiteral &shape) {
  for (const auto &entry : shape.rules) {
    if (std::holds_alternative<DimensionPivotRule>(entry.rule) ||
        std::holds_alternative<VirtualDimensionDefRule>(entry.rule)) {
      return true;
    }
  }
  return false;
}

void VQLCompiler::compileDynamicShapeProjection(const ShapeLiteral &shape) {
  for (const auto &entry : shape.rules) {
    if (const auto *name = std::get_if<std::string>(&entry.rule)) {
      if (!varBindings_.contains(*name) && !streamBindings_.contains(*name)) {
        throw TypeContractError("unbound shape reference: $" + *name);
      }
    }
    if (const auto *dim = std::get_if<DimensionShapeRule>(&entry.rule);
        dim && dim->targetValue) {
      throw std::invalid_argument(
          "dynamic projections cannot mix eager dimension assignments");
    }
  }
}

std::vector<CellRef> VQLCompiler::compileLensStep(const PathStep &step,
                                                  const CellRef current,
                                                  const bool hasContext) {
  (void)hasContext;
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  if (fn.name == "lens") {
    return {current};
  }
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "VQL path function '" + fn.name +
                         "' is not compiled to Vortex yet");
}

std::vector<CellRef>
VQLCompiler::compileLensCall(const FunctionInvocation &fn,
                             const std::optional<CellRef> context) {
  (void)context;
  if (fn.name == "lens") {
    if (fn.args.size() != 2 || fn.argNames.size() != 2) {
      throw std::runtime_error("lens requires get: and put: callbacks");
    }
    const PathLambda *get = nullptr;
    const PathLambda *put = nullptr;
    for (std::size_t i = 0; i < fn.args.size(); ++i) {
      const auto *lambda =
          std::get_if<std::shared_ptr<PathLambda>>(&fn.args[i].kind);
      if (!lambda || !*lambda) {
        throw std::runtime_error("lens callbacks must be path lambdas");
      }
      if (fn.argNames[i] == "get" && !get)
        get = lambda->get();
      else if (fn.argNames[i] == "put" && !put)
        put = lambda->get();
      else
        throw std::runtime_error(
            "lens requires distinct get: and put: callbacks");
    }
    if (!get || !put || get->params.size() != 1 || put->params.size() != 2) {
      throw std::runtime_error("lens get takes one parameter; put takes two");
    }
    const CellRef handle   = core_.arena().makeCell();
    auto &reg              = dynamicRegistry();
    reg.cellLenses[handle] = PutGetLensRecord{.get = *get, .put = *put};
    return {handle};
  }
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "unknown compiled function: " + fn.name);
}

std::optional<CellRef>
VQLCompiler::compileLensValue(const FunctionInvocation &fn,
                              const CellRef ctxCell) {
  auto res = compileLensCall(fn, ctxCell);
  return res.empty() ? std::nullopt : std::optional{res.front()};
}

std::optional<CellRef> VQLCompiler::compileShapeValue(const ShapeLiteral &shape,
                                                      const CellRef ctxCell) {
  (void)ctxCell;
  const CellRef handle     = core_.arena().makeCell();
  auto &reg                = dynamicRegistry();
  reg.shapeHandles[handle] = shape;
  const CellRef constOp =
      emitConstant(static_cast<std::int64_t>(handle), std::nullopt);
  const CellRef box = emitOp(OpcodeKind::VqlBoxScalar, "#SHAPE_HANDLE_BOX");
  emitInput(box, constOp);
  emitOutput(box, handle);
  return handle;
}

} // namespace xanadu::vql
