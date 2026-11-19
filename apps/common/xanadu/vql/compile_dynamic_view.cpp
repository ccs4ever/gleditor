// Q5: dynamic shape projection, lens put/get. Shape values are in
// compile_shape_value.cpp.
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"

#include <algorithm>
#include <string>

#include <gleditor/logging.hpp>

#include "common/xanadu/vql/frame_eligibility.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

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
  for (const auto *callback : {pair.get.get(), pair.put.get()}) {
    if (!callback) continue;
    // The frame scope walker records a form's captures before it declines
    // the form as a frame body, so its capture set is used even when it
    // declines (a put that creates cells is the common case).
    const auto scope = analyzeFrameClosureScope(*callback);
    if (!scope)
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL lens(...): " + scope.error().message);
    // A callback is compiled where the lens is applied, so a free variable
    // would be read from the use site's bindings, not the binding scope's:
    // silently the wrong value. Callbacks therefore take everything they use
    // through their parameters.
    std::vector<std::string> captured(scope->freeCaptureNames.begin(),
                                      scope->freeCaptureNames.end());
    captured.insert(captured.end(),
                    scope->requiredDefaultCaptureBindings.begin(),
                    scope->requiredDefaultCaptureBindings.end());
    captured.insert(captured.end(),
                    scope->conditionalRequiredDefaultCaptureBindings.begin(),
                    scope->conditionalRequiredDefaultCaptureBindings.end());
    captured.insert(captured.end(),
                    scope->omittedRestRequiredDefaultCaptureBindings.begin(),
                    scope->omittedRestRequiredDefaultCaptureBindings.end());
    std::ranges::sort(captured);
    if (!captured.empty())
      refuseLensForm("lens callback captures $" + captured.front() +
                     " from its binding scope; lens callbacks may not "
                     "capture variables");
  }
  return pair;
}

} // namespace

bool VQLCompiler::isDynamicViewShape(const ShapeLiteral &shape) {
  // Pivot and computed rules make a projection a view; mask rules alone are
  // the static projection, which writes the mask.
  return std::ranges::any_of(shape.rules, [](const ShapeRule &entry) {
    return std::holds_alternative<DimensionPivotRule>(entry.rule) ||
           std::holds_alternative<VirtualDimensionDefRule>(entry.rule);
  });
}

void VQLCompiler::compileDynamicShapeProjection(const ShapeLiteral &shape) {
  // Variables, shape-returning calls and nested shapes were expanded before
  // this point (compile_shape_value.cpp), so what is left is a rule the static
  // projection cannot take. The first one in source order names the refusal.
  // A pivot or computed rule reaches here only from project() and
  // apply_shape(), where a view handle is not a mutation result.
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      (void)wildcard;
      continue;
    }
    if (const auto *pivot = std::get_if<DimensionPivotRule>(&entry.rule)) {
      refuseForm("dimension pivot '" + pivot->fromDim + " -> " + pivot->toDim +
                 "' in project() or apply_shape() (a pivot makes a dynamic "
                 "view, which only a path projection answers)");
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

std::vector<CellRef>
VQLCompiler::lowerDynamicView(const ShapeLiteral &shape,
                              const std::span<const CellRef> owners) {
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *computed =
            std::get_if<VirtualDimensionDefRule>(&entry.rule))
      requireStaticComputedRule(*computed);
    if (const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
        rule && rule->targetValue)
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "VQL an eager dimension assignment ('" + rule->dimName +
              ": ...') cannot be mixed with a pivot or computed rule in one "
              "projection: a view writes nothing, an assignment does");
  }
  const auto view             = std::make_shared<const ViewDescriptor>(shape);
  const zigzag::DimRef marker = resolveDimension(viewMarkerDimension);
  std::vector<std::pair<CellRef, CellRef>> made;
  std::vector<CellRef> handles;
  for (const CellRef owner : owners) {
    if (const auto known = shapeHandles_.find(owner);
        known != shapeHandles_.end() &&
        std::holds_alternative<ViewHandle>(known->second))
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL a projection over a dynamic view handle is not "
                         "compiled to Vortex: project the source cell instead");
    if (viewStreams_.contains(owner))
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL a projection over the cells reached through a "
                         "dynamic view is not compiled to Vortex");
    if (!sessionCells_.contains(owner))
      throw QueryRefusal(
          QueryErrorCategory::Unsupported,
          "VQL a dynamic view over a source computed at run time is not "
          "compiled to Vortex: its handle needs a source cell the session "
          "already holds, such as a variable bound by an earlier query");
    const auto again =
        std::ranges::find(made, owner, &decltype(made)::value_type::first);
    if (again != made.end()) {
      handles.push_back(again->second);
      continue;
    }
    const CellRef handle = core_.arena().makeCell(core_.arena().textOf(owner));
    // The marker's far end is a program cell too, so this writes nothing the
    // program's release would not undo.
    zigzag::expectWritten(
        core_.arena().link(handle, marker, false, core_.arena().makeCell()));
    shapeHandles_.emplace(handle, ViewHandle{.source = owner, .view = view});
    made.emplace_back(owner, handle);
    handles.push_back(handle);
    GLEDITOR_LOG_DEBUG("vql.compile", "dynamic view handle {} over cell {}",
                       static_cast<std::uint64_t>(handle),
                       static_cast<std::uint64_t>(owner));
  }
  return handles;
}

std::optional<VQLCompiler::ViewMark>
VQLCompiler::viewOfStream(std::vector<CellRef> &stream) {
  std::optional<ViewMark> found;
  bool plain = false;
  for (const CellRef cell : stream) {
    std::optional<ViewMark> mark;
    if (const auto handle = shapeHandles_.find(cell);
        handle != shapeHandles_.end()) {
      if (const auto *view = std::get_if<ViewHandle>(&handle->second))
        mark = ViewMark{.view = view->view, .created = false};
    } else if (const auto reached = viewStreams_.find(cell);
               reached != viewStreams_.end()) {
      mark = reached->second;
    }
    if (!mark) {
      if (carriesViewMarker(cell))
        throw QueryRefusal(QueryErrorCategory::Unsupported,
                           "VQL a dynamic view handle this session cannot "
                           "resolve, or a cell created through a view, is "
                           "not a cell a step can walk: step the view's "
                           "handle instead (a handle from another engine "
                           "or from before its variables were cleared is "
                           "not statically known)");
      plain = true;
      continue;
    }
    if (found && found->view != mark->view)
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL a step reaches cells of two different dynamic "
                         "views at once, which is not compiled to Vortex");
    if (found)
      found->created = found->created && mark->created;
    else
      found = mark;
  }
  if (!found) return std::nullopt;
  if (plain)
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL a step reaches both dynamic view cells and plain "
                       "cells at once, which is not compiled to Vortex");
  for (CellRef &cell : stream)
    if (const auto handle = shapeHandles_.find(cell);
        handle != shapeHandles_.end())
      if (const auto *view = std::get_if<ViewHandle>(&handle->second))
        cell = view->source;
  return found;
}

VQLCompiler::ViewStepPlan
VQLCompiler::planViewStep(const ViewMark &view, const PathStep &step) const {
  const auto refuse = [](const std::string &what) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL " + what +
                           " on a step through a dynamic view is not compiled "
                           "to Vortex; only dimension steps, then value() or "
                           "count(), follow a view");
  };
  if (step.versionPin) refuse("a version pin");
  if (step.capturedFunctor) refuse("a captured functor");
  if (step.derefMaster) refuse("a master dereference");
  if (!step.predicates.empty()) refuse("a predicate");
  if (!step.shapePredicates.empty()) refuse("a shape predicate");
  if (step.projectionAfter) refuse("a shape projection");
  if (step.yieldMode != YieldMode::Default && step.yieldMode != YieldMode::New)
    refuse("a yield mode (!keep, !last, !both)");
  if (const auto *function = std::get_if<FunctionInvocation>(&step.selector)) {
    if ((function->name == "value" || function->name == "count") &&
        function->args.empty() && !definitions_.contains(function->name))
      return {.step = step, .endsView = true};
    refuse("function " + function->name + "()");
  }
  const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
  if (!dimension || dimension->dimName.starts_with('$'))
    refuse("this kind of step");
  // The rules read in source order, as the oracle's traverseView did: a
  // wildcard or a mask rule sets visibility, a pivot on the step's dimension
  // remaps it and shows it.
  const auto &rules = view.view->shape.rules;
  bool allowed      = std::ranges::none_of(rules, [](const ShapeRule &rule) {
    return std::holds_alternative<ShapeWildcard>(rule.rule) ||
           std::holds_alternative<DimensionShapeRule>(rule.rule);
  });
  SignedDimensionStep physical            = *dimension;
  const VirtualDimensionDefRule *computed = nullptr;
  for (const ShapeRule &entry : rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      allowed = *wildcard == ShapeWildcard::Everything;
    } else if (const auto *mask =
                   std::get_if<DimensionShapeRule>(&entry.rule)) {
      if (mask->dimName == dimension->dimName &&
          (mask->isBidir || mask->direction == dimension->direction))
        allowed = !mask->negated;
    } else if (const auto *pivot =
                   std::get_if<DimensionPivotRule>(&entry.rule)) {
      if (pivot->fromDim == dimension->dimName) {
        physical.dimName = pivot->toDim;
        allowed          = true;
      }
    } else if (const auto *virt =
                   std::get_if<VirtualDimensionDefRule>(&entry.rule)) {
      if (virt->dimName == dimension->dimName &&
          virt->direction == dimension->direction) {
        computed = virt;
        allowed  = true;
      }
    }
  }
  if (!allowed) return {};
  if (computed) {
    if (!dimension->creates.empty())
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL computed dimension '" + computed->dimName +
                             "' of a dynamic view cannot be written (%): it "
                             "has no put transform");
    if (dimension->isBidir || dimension->placement != Placement::Default)
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL computed dimension '" + computed->dimName +
                             "' of a dynamic view does not take a "
                             "bidirectional or placement modifier");
    return {.step = step, .endsView = false, .computed = computed};
  }
  PathStep rewritten = step;
  rewritten.selector = std::move(physical);
  return {.step = std::move(rewritten), .endsView = false};
}

void VQLCompiler::requireStaticComputedRule(
    const VirtualDimensionDefRule &rule) {
  const auto refuse = [&rule](const std::string &why) {
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "VQL computed dimension '" + rule.dimName +
            "' in a dynamic view cannot be compiled to Vortex: " + why +
            "; only a literal or a path from the source (./ steps along "
            "plain dimensions, then value() or count()) is lowered");
  };
  if (!rule.expr) refuse("it has no expression");
  if (std::holds_alternative<ScalarLiteral>(rule.expr->kind)) return;
  const auto *path =
      std::get_if<std::shared_ptr<PathExpression>>(&rule.expr->kind);
  if (!path || !*path) refuse("its expression is not a literal or a path");
  const PathExpression &expr = **path;
  if (expr.anchor.kind != AnchorKind::Context || expr.isFunctor ||
      expr.isInvocationRoot)
    refuse("its path does not start at the source cell (./)");
  if (!expr.anchorPredicates.empty() || expr.anchorProjection ||
      expr.shapeProjection || expr.cloneTail)
    refuse("its path carries a predicate, projection or clone");
  for (const PathStep &step : expr.steps) {
    if (step.versionPin || step.capturedFunctor || step.derefMaster ||
        !step.predicates.empty() || !step.shapePredicates.empty() ||
        step.projectionAfter || step.rangeClamp ||
        step.yieldMode != YieldMode::Default)
      refuse("a step of its path carries a predicate, projection, range, "
             "pin or yield mode");
    if (const auto *function =
            std::get_if<FunctionInvocation>(&step.selector)) {
      if ((function->name != "value" && function->name != "count") ||
          !function->args.empty())
        refuse("function " + function->name + "() is not value() or count()");
      continue;
    }
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (!dimension || dimension->dimName.starts_with('$') ||
        !dimension->creates.empty() ||
        dimension->placement != Placement::Default)
      refuse("a step of its path is not a read along a plain dimension");
  }
}

void VQLCompiler::requireComputedResultEnds(
    const VirtualDimensionDefRule &rule, const std::span<const PathStep> rest) {
  for (const PathStep &step : rest) {
    const auto *function = std::get_if<FunctionInvocation>(&step.selector);
    if (function && (function->name == "value" || function->name == "count") &&
        function->args.empty())
      continue;
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL a step after computed dimension '" + rule.dimName +
                           "' of a dynamic view is not compiled to Vortex: "
                           "its result is a fresh cell, so only value() or "
                           "count() may follow");
  }
}

std::vector<CellRef>
VQLCompiler::compileComputedDimension(const VirtualDimensionDefRule &rule,
                                      const std::span<const CellRef> sources) {
  // Evaluated against the source, as the oracle did, so a computed dimension
  // cannot resolve itself through its own view.
  return emitMap(sources, [&](const CellRef sourceCell) {
    std::vector<CellRef> reached;
    if (const auto *literal = std::get_if<ScalarLiteral>(&rule.expr->kind)) {
      reached = {std::visit(
          [this](const auto &value) { return emitConstant(CellValue(value)); },
          literal->value)};
    } else {
      const auto &path =
          *std::get<std::shared_ptr<PathExpression>>(rule.expr->kind);
      reached = compileSteps(path.steps, {sourceCell}, true);
      // One value, as evaluateValueExpr answered.
      reached = emitClamp(reached, RangeClamp{.start = 1, .end = 1});
    }
    // value() and count() already answer a fresh cell; a plain dimension step
    // or a literal answers a cell the source or the program owns.
    const auto *path =
        std::get_if<std::shared_ptr<PathExpression>>(&rule.expr->kind);
    if (path && !(*path)->steps.empty() &&
        std::holds_alternative<FunctionInvocation>(
            (*path)->steps.back().selector))
      return reached;
    return emitMap(reached, [&](const CellRef value) {
      const CellRef copy = core_.arena().makeCell();
      const auto read    = emitOp(OpcodeKind::Value, "#COMPUTED_DIM_VALUE");
      emitInput(read, value);
      emitOutput(read, copy);
      return std::vector<CellRef>{copy};
    });
  });
}

bool VQLCompiler::carriesViewMarker(const CellRef cell) const {
  // The core's dimension directory is shared by every engine over it, so a
  // handle another engine made is found; an engine with a core of its own
  // names its own dimensions and shares nothing to be confused with.
  const auto marker = core_.findDimension(viewMarkerDimension);
  return marker && core_.arena()
                       .linked(cell, *marker, zigzag::DimVector::POS)
                       .has_value();
}

void VQLCompiler::markViewStream(const std::span<const CellRef> stream,
                                 const ViewMark &view) {
  // A session cell is a source the program did not make: marking it would
  // make every later use of that variable look like view traffic.
  for (const CellRef cell : stream)
    if (!sessionCells_.contains(cell))
      viewStreams_.insert_or_assign(cell, view);
}

void VQLCompiler::refuseViewHandles(const std::span<const CellRef> cells,
                                    const std::string &where) const {
  for (const CellRef cell : cells) {
    const auto handle = shapeHandles_.find(cell);
    const bool known  = handle != shapeHandles_.end() &&
                       std::holds_alternative<ViewHandle>(handle->second);
    if (!known && !carriesViewMarker(cell)) continue;
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "VQL a dynamic view handle cannot be used by " + where +
            ": the compiler follows a view only along a path from its "
            "variable, and a handle carried elsewhere would walk as a plain "
            "cell with none of the view's rules");
  }
}

void VQLCompiler::refuseViewHandleArgument(const ValueExpr &argument) const {
  const std::string *name = std::get_if<std::string>(&argument.kind);
  if (!name) {
    const auto *path =
        std::get_if<std::shared_ptr<PathExpression>>(&argument.kind);
    if (!path || !*path || (*path)->anchor.kind != AnchorKind::Variable ||
        !(*path)->steps.empty())
      return;
    name = &(*path)->anchor.name;
  }
  std::vector<CellRef> cells;
  if (const auto found = streamBindings_.find(*name);
      found != streamBindings_.end()) {
    cells = found->second;
  } else if (const auto found = varBindings_.find(*name);
             found != varBindings_.end() && found->second) {
    cells.push_back(*found->second);
  }
  refuseViewHandles(cells, "a function call argument");
}

void VQLCompiler::refuseViewEscape(const std::span<const CellRef> stream,
                                   const std::string &what) const {
  for (const CellRef cell : stream) {
    const auto reached = viewStreams_.find(cell);
    if (reached == viewStreams_.end() || reached->second.created) continue;
    throw QueryRefusal(
        QueryErrorCategory::Unsupported,
        "VQL a step through a dynamic view " + what +
            ": the cells it reaches are plain cells, not handles of the "
            "view, so end the path with value() or count()");
  }
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
    // Spec 4.5: a bare % allocates and initialises nothing, so it carries no
    // replacement for put to write.
    if (create.kind == CreateValue::Kind::Empty ||
        (create.kind == CreateValue::Kind::Expression && !create.expr))
      throw QueryRefusal(QueryErrorCategory::Unsupported,
                         "VQL lens " + spelled +
                             " needs a replacement value after %; a bare % "
                             "carries none");
    ValueExpr replacement{.kind = ScalarLiteral{create.literal}};
    if (create.kind == CreateValue::Kind::Expression)
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
  // A call hands its result back as a fresh run-time cell, not the handle the
  // table knows, so a lens made inside a function body could not be resolved
  // where the caller applies it.
  if (compilingFrameDefinition_ || !activeCalls_.empty())
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       "VQL lens value is not statically known: a lens "
                       "created inside a function body cannot be returned "
                       "from it; bind it with let in the query body");
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
