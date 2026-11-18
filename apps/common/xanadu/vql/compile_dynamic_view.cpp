// Q5: dynamic shape projection, lens put/get, shape values.
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

namespace {

[[noreturn]] void refuseForm(const std::string &form) {
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     form + " is not compiled to Vortex yet");
}

/// Every lens entry point refuses with the same words, so a lens never reads
/// as a misspelt function.
[[noreturn]] void refuseLens(const FunctionInvocation &fn) {
  throw QueryRefusal(
      QueryErrorCategory::Unsupported,
      "VQL " + fn.name +
          "(...) is not compiled to Vortex: a lens couples get and put "
          "callbacks over a computed view, and no lowering of that view "
          "exists");
}

} // namespace

void VQLCompiler::compileDynamicShapeProjection(const ShapeLiteral &shape) {
  const auto bound = [this](const std::string &name) {
    return streamBindings_.contains(name) || varBindings_.contains(name) ||
           functorBindings_.contains(name);
  };
  // The first rule the static projection cannot take names the refusal; the
  // rules are walked in source order so the message points at the leftmost one.
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      (void)wildcard;
      continue;
    }
    if (const auto *name = std::get_if<std::string>(&entry.rule)) {
      if (!bound(*name))
        throw TypeContractError("shape variable $" + *name + " is not bound");
      refuseForm("shape-valued variable '$" + *name +
                 "' in a shape projection (its rules are known only at run "
                 "time)");
    }
    if (const auto *call =
            std::get_if<std::shared_ptr<FunctionInvocation>>(&entry.rule)) {
      refuseForm("shape function call '" + (*call ? (*call)->name : "") +
                 "(...)' in a shape projection");
    }
    if (const auto *pivot = std::get_if<DimensionPivotRule>(&entry.rule)) {
      refuseForm("dimension pivot '" + pivot->fromDim + " -> " + pivot->toDim +
                 "' in a shape projection");
    }
    if (const auto *computed =
            std::get_if<VirtualDimensionDefRule>(&entry.rule)) {
      refuseForm("computed dimension '" + computed->dimName +
                 "' in a shape projection");
    }
    if (std::holds_alternative<std::shared_ptr<ShapeLiteral>>(entry.rule)) {
      refuseForm("a nested shape in a shape projection");
    }
    if (const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule)) {
      if (rule->targetValue && (rule->negated || rule->isBidir))
        refuseForm("field binding in a shape ('" + rule->dimName +
                   "' with a negated or bidirectional target)");
      refuseForm("dimension rule '" + rule->dimName +
                 "' in a shape projection (not a canonical d.-named "
                 "single-direction dimension)");
    }
  }
  refuseForm("this shape projection");
}

std::vector<CellRef> VQLCompiler::compileLensStep(const PathStep &step,
                                                  const CellRef /*current*/,
                                                  const bool /*hasContext*/) {
  refuseLens(std::get<FunctionInvocation>(step.selector));
}

std::vector<CellRef>
VQLCompiler::compileLensCall(const FunctionInvocation &fn,
                             const std::optional<CellRef> /*context*/) {
  refuseLens(fn);
}

std::optional<CellRef>
VQLCompiler::compileLensValue(const FunctionInvocation &fn,
                              const CellRef /*ctxCell*/) {
  refuseLens(fn);
}

std::optional<CellRef>
VQLCompiler::compileShapeValue(const ShapeLiteral & /*shape*/,
                               const CellRef /*ctxCell*/) {
  refuseForm("a shape-valued variable or argument ('{ ... }' as a value)");
}

} // namespace xanadu::vql
