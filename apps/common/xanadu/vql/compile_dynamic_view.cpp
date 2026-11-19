// Q5: dynamic shape projection, lens put/get. Shape values are in
// compile_shape_value.cpp.
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"

#include <algorithm>
#include <string>

#include <gleditor/logging.hpp>

namespace xanadu::vql {
using zigzag::CellRef;

namespace {

[[noreturn]] void refuseForm(const std::string &form) {
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     form + " is not compiled to Vortex yet");
}

/// docs(...), files(...) and lens("name", ...) instantiate a registered host
/// lens provider (spec 4.11.3). They have no provider in the compiler, so they
/// refuse by name and say what they are, not "a lens couples callbacks".
[[noreturn]] void refuseProviderLens(const FunctionInvocation &fn) {
  std::string spelled = fn.name + "(...)";
  if (fn.name == "lens") {
    const auto *type = fn.args.empty()
                           ? nullptr
                           : std::get_if<ScalarLiteral>(&fn.args.front().kind);
    const auto *name = type ? std::get_if<std::string>(&type->value) : nullptr;
    spelled = name ? "lens(\"" + *name + "\", ...)" : "lens(type, ...)";
  }
  throw QueryRefusal(
      QueryErrorCategory::Unsupported,
      "VQL " + spelled +
          " names a host lens provider (design/vql-query-language.md "
          "4.11.3), and no projection lens provider is registered with the "
          "Vortex compiler, so it is refused");
}

bool isCallbackLens(const FunctionInvocation &fn) {
  return fn.name == "lens" &&
         std::ranges::any_of(fn.argNames, [](const std::string &name) {
           return name == "get" || name == "put";
         });
}

[[noreturn]] void refuseLensForm(const std::string &message) {
  throw QueryRefusal(QueryErrorCategory::Malformed,
                     "VQL lens(...): " + message);
}

/// Reads `lens(get: \(...) ..., put: \(..., ...) ...)`. Neither callback is
/// compiled here; the step that applies the lens compiles them.
LensPair lensCallbacks(const FunctionInvocation &fn) {
  if (fn.args.size() != fn.argNames.size())
    refuseLensForm("get: and put: callbacks must both be named");
  LensPair pair;
  for (std::size_t index = 0; index < fn.args.size(); ++index) {
    const std::string &name                 = fn.argNames[index];
    std::shared_ptr<const PathLambda> *slot = name == "get"   ? &pair.get
                                              : name == "put" ? &pair.put
                                                              : nullptr;
    if (!slot)
      refuseLensForm("unknown argument '" + name +
                     "'; a lens takes get: and optionally put:");
    if (*slot) refuseLensForm("'" + name + ":' is given twice");
    const auto *lambda =
        std::get_if<std::shared_ptr<PathLambda>>(&fn.args[index].kind);
    if (!lambda || !*lambda)
      refuseLensForm("the '" + name + ":' callback must be a path lambda");
    *slot = *lambda;
  }
  if (!pair.get) refuseLensForm("a lens needs a get: callback");
  if (pair.get->params.size() != 1)
    refuseLensForm("get: takes exactly one parameter");
  if (pair.put && pair.put->params.size() != 2)
    refuseLensForm("put: takes exactly two parameters (the source cell and "
                   "the replacement)");
  return pair;
}

} // namespace

void VQLCompiler::compileDynamicShapeProjection(const ShapeLiteral &shape) {
  // Variables, shape-returning calls and nested shapes were expanded before
  // this point (compile_shape_value.cpp), so what is left is a rule the static
  // projection cannot take. The first one in source order names the refusal.
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      (void)wildcard;
      continue;
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

CellRef VQLCompiler::registerLens(LensPair pair) {
  const CellRef handle = core_.arena().makeCell();
  shapeHandles_.emplace(handle, std::move(pair));
  return handle;
}

std::optional<LensPair>
VQLCompiler::lensBoundTo(const std::string &name) const {
  std::vector<CellRef> cells;
  if (const auto found = streamBindings_.find(name);
      found != streamBindings_.end()) {
    cells = found->second;
  } else if (const auto found = varBindings_.find(name);
             found != varBindings_.end() && found->second) {
    cells.push_back(*found->second);
  }
  std::optional<LensPair> lens;
  for (const CellRef cell : cells) {
    const auto handle = shapeHandles_.find(cell);
    const auto *pair  = handle == shapeHandles_.end()
                            ? nullptr
                            : std::get_if<LensPair>(&handle->second);
    if (!pair) continue;
    if (lens)
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL variable $" + name +
                             " holds several lenses; applying a stream of "
                             "lenses is not compiled to Vortex");
    lens = *pair;
  }
  return lens;
}

std::vector<CellRef>
VQLCompiler::compileLensApplication(const SignedDimensionStep &step,
                                    const LensPair &lens, const CellRef source,
                                    const bool hasContext) {
  const std::string spelled = step.dimName;
  if (!hasContext)
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL lens " + spelled +
                           " is applied to a source cell with /, and has none "
                           "here");
  if (step.direction != zigzag::DimVector::POS || step.isBidir ||
      step.placement != Placement::Default)
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL lens " + spelled +
                           " does not take a direction, bidirectional or "
                           "placement modifier");
  if (step.creates.empty()) {
    const DefDeclaration get{
        .name   = "#vql-lens-get-" + std::to_string(allOps_.size()),
        .params = lens.get->params,
        .body   = lens.get->body,
    };
    return compileCall(get, {}, source, true, false);
  }
  if (!lens.put)
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL lens " + spelled +
                           " has no put: callback, so it cannot be written "
                           "through");
  // One put per replacement, each in its own mutation group so a callback
  // that fails after writing leaves nothing half applied. The group is the
  // same bracket `%` creation uses.
  std::vector<CellRef> sources;
  for (const CreateValue &create : step.creates) {
    ValueExpr replacement{.kind = ScalarLiteral{std::string{}}};
    if (create.kind == CreateValue::Kind::Literal)
      replacement.kind = ScalarLiteral{create.literal};
    else if (create.kind == CreateValue::Kind::Expression && create.expr)
      replacement = *create.expr;
    const DefDeclaration put{
        .name   = "#vql-lens-put-" + std::to_string(allOps_.size()),
        .params = lens.put->params,
        .body   = lens.put->body,
    };
    const CellRef group = beginMutationGroup("#LENS_PUT_GROUP");
    (void)compileCall(put, {replacement}, source, true, false);
    endMutationGroup(group, "#LENS_PUT_GROUP_END");
    sources.push_back(source);
  }
  return sources;
}

std::vector<CellRef> VQLCompiler::compileLensStep(const PathStep &step,
                                                  const CellRef /*current*/,
                                                  const bool /*hasContext*/) {
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  if (!isCallbackLens(fn)) refuseProviderLens(fn);
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "VQL lens(get:, put:) is a value, not a path step: bind "
                     "it with let and apply it as $source/$lens");
}

std::vector<CellRef>
VQLCompiler::compileLensCall(const FunctionInvocation &fn,
                             const std::optional<CellRef> /*context*/) {
  if (!isCallbackLens(fn)) refuseProviderLens(fn);
  const CellRef handle = registerLens(lensCallbacks(fn));
  GLEDITOR_LOG_DEBUG("vql.compile", "lens registered as handle {}",
                     static_cast<std::uint64_t>(handle));
  return {handle};
}

std::optional<CellRef>
VQLCompiler::compileLensValue(const FunctionInvocation &fn,
                              const CellRef /*ctxCell*/) {
  return compileLensCall(fn, std::nullopt).front();
}

} // namespace xanadu::vql
