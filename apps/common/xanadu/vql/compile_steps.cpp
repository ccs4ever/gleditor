#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/invocation.hpp"

#include <stdexcept>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;
namespace {

void validatePathLambdaSignature(const PathLambda &lambda) {
  if (lambda.params.size() > 1)
    throw std::invalid_argument(
        "path-step lambdas support at most one parameter");
  if (!lambda.params.empty() && lambda.params.front().isVariadic)
    throw std::invalid_argument("variadic path-step lambdas are unsupported");

  const DefDeclaration signature{
      .name   = "#vql-inline-path-lambda-signature",
      .params = lambda.params,
      .body   = lambda.body,
  };
  try {
    validateFunctionSignature(signature);
  } catch (const TypeContractError &error) {
    throw std::invalid_argument(error.what());
  }
}

} // namespace

bool VQLCompiler::isWholeWalkStep(const FunctionInvocation &fn) {
  if (!fn.args.empty()) return false;
  return fn.name == "count" || fn.name == "any" || fn.name == "all" ||
         fn.name == "none" || fn.name == "one" || fn.name == "optional_one" ||
         fn.name == "first";
}

std::vector<CellRef>
VQLCompiler::compileWholeWalkStep(const FunctionInvocation &fn,
                                  const std::span<const CellRef> stream,
                                  const bool hasContext) {
  if (fn.name == "count") return compilePathFunctionWalk(fn, stream);
  return compileSemanticQuantifierWalk(fn, stream, hasContext);
}

std::vector<CellRef>
VQLCompiler::emitMountFallback(const SignedDimensionStep &step,
                               std::vector<CellRef> walked) {
  const auto mounted = mounts_.find(step.dimName);
  if (mounted == mounts_.end() || mounted->second.roots.empty() ||
      walked.empty())
    return walked;
  if (!step.isBidir && step.direction != mounted->second.direction)
    return walked;
  // The walk is empty only as a whole (every input found nothing), which is
  // known at run time, so the fallback is a branch around an append.
  const auto present = emitStreamPresence(walked);
  const auto absent  = core_.arena().makeCell();
  const auto negate  = emitOp(OpcodeKind::Not, "#MOUNT_FALLBACK_NOT");
  emitInput(negate, present);
  emitOutput(negate, absent);
  const auto end    = mintOpcode(OpcodeKind::Nop, "#MOUNT_FALLBACK_END");
  const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#MOUNT_FALLBACK");
  emitInput(branch, absent);
  emitInput(branch, end);
  emitAppendStream(walked.front(), mounted->second.roots);
  appendOpcode(end);
  return walked;
}

std::vector<CellRef> VQLCompiler::compileSteps(std::span<const PathStep> steps,
                                               std::vector<CellRef> stream,
                                               bool hasContext,
                                               bool allowInitialEmptyCall) {
  for (std::size_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
    const auto &written = steps[stepIndex];
    // A stream seen through a dynamic view: the step is compiled against the
    // source with the view's rules (compile_dynamic_view.cpp).
    auto view = viewOfStream(stream);
    std::optional<PathStep> viewStep;
    bool viewEnds = false;
    if (view) {
      auto plan = planViewStep(*view, written);
      if (!plan.step) {
        // Hidden by a mask: nothing is read and nothing is created.
        if (written.yieldMode != YieldMode::Keep) stream = {emitStreamClear()};
        hasContext = true;
        continue;
      }
      if (plan.computed) {
        requireComputedResultEnds(*plan.computed, steps.subspan(stepIndex + 1));
        auto computed = compileComputedDimension(*plan.computed, stream);
        if (written.rangeClamp)
          computed = emitClamp(computed, *written.rangeClamp);
        if (written.yieldMode != YieldMode::Keep) stream = std::move(computed);
        hasContext = true;
        continue;
      }
      viewStep = std::move(plan.step);
      viewEnds = plan.endsView;
    }
    const PathStep &step = viewStep ? *viewStep : written;
    if (step.versionPin) {
      stream =
          compileVersionPinnedStep(step, std::move(stream), hasContext,
                                   allowInitialEmptyCall && stepIndex == 0);
      continue;
    }
    const auto stepInput = stream;
    std::vector<CellRef> nextStream;

    const auto *function = std::get_if<FunctionInvocation>(&step.selector);
    const auto *runtimeCall =
        std::get_if<std::shared_ptr<RuntimeClosureInvocation>>(&step.selector);
    const auto *dimension   = std::get_if<SignedDimensionStep>(&step.selector);
    const bool userCall     = function && definitions_.contains(function->name);
    const bool rootUserCall = allowInitialEmptyCall && stepIndex == 0 &&
                              (userCall || (runtimeCall && *runtimeCall));
    const auto present =
        rootUserCall ? std::optional{emitStreamPresence(stream)} : std::nullopt;
    const bool creationStep =
        !step.capturedFunctor &&
        ((dimension && !dimension->dimName.starts_with('$') &&
          !dimension->creates.empty()) ||
         (function && function->name == "link" && function->args.size() >= 2));
    if (const auto *lambda =
            std::get_if<std::shared_ptr<PathLambda>>(&step.selector)) {
      if (!*lambda)
        throw std::invalid_argument("null VQL path lambda selector");
      validatePathLambdaSignature(**lambda);
    }
    if (step.capturedFunctor) {
      nextStream = compileSteps(*step.capturedFunctor, stream, hasContext);
    } else if (dimension && dimension->dimName.starts_with('$')) {
      const std::string name = dimension->dimName.substr(1);
      const auto lens =
          functorBindings_.contains(name) ? std::nullopt : lensBoundTo(name);
      if (lens) {
        if (step.yieldMode != YieldMode::Default &&
            step.yieldMode != YieldMode::New)
          throw QueryRefusal(QueryErrorCategory::Unsupported,
                             "VQL lens " + dimension->dimName +
                                 " does not take a yield mode (!keep, !last, "
                                 "!both)");
        nextStream = emitMap(stream, [&](const CellRef source) {
          return compileLensApplication(*dimension, *lens, source, hasContext);
        });
      } else if (const auto found = functorBindings_.find(name);
                 found != functorBindings_.end()) {
        // Calls can replace binding maps while compiling their bodies.
        const auto captured = found->second;
        nextStream          = compileSteps(captured, stream, hasContext);
      } else if (streamBindings_.contains(name) ||
                 varBindings_.contains(name)) {
        if (dimension->direction != zigzag::DimVector::POS ||
            dimension->isBidir || dimension->placement != Placement::Default ||
            !dimension->creates.empty() || step.yieldMode != YieldMode::Default)
          throw std::invalid_argument(
              "dynamic closure aliases do not support dimension modifiers");
        std::vector<CellRef> aliases;
        if (const auto bound = streamBindings_.find(name);
            bound != streamBindings_.end()) {
          aliases = bound->second;
        } else if (const auto bound = varBindings_.find(name);
                   bound != varBindings_.end() && bound->second) {
          aliases.push_back(*bound->second);
        }
        // A session variable that holds an empty cell and no closure is what
        // a lens handle looks like once the table that explained it is gone
        // (another engine or session): say so instead of failing in the VM.
        for (const CellRef alias : aliases)
          if (constantCells_.contains(alias) &&
              core_.arena().textOf(alias).empty() && !vm_.closureValue(alias))
            throw QueryRefusal(
                QueryErrorCategory::Unsupported,
                "VQL lens value $" + name +
                    " is not statically known: it is bound to an empty cell "
                    "that is neither a closure nor a lens this session "
                    "compiled (a handle from another engine or session)");
        if (!hasContext) {
          nextStream = aliases;
          hasContext = true;
          stream     = nextStream;
          continue;
        }
        nextStream = emitMap(stream, [&](const CellRef candidate) {
          RuntimeClosureInvocation invocation{
              .callee = ValueExpr{.kind = name}, .args = {}, .argNames = {}};
          return compileClosureInvocation(
              invocation, candidate,
              zigzag::vortex::ClosureCallMode::Contextual);
        });
      } else {
        throw std::invalid_argument("unbound point-free functor '" +
                                    dimension->dimName + "'");
      }
    } else if (function && !userCall && hasContext &&
               isWholeWalkStep(*function)) {
      // Whole-walk builtins consume the incoming stream once, including when
      // it is empty. User definitions and argument-taking calls stay per-cell.
      nextStream = compileWholeWalkStep(*function, stream, hasContext);
    } else if (std::holds_alternative<PerspectiveStep>(step.selector)) {
      std::vector<CellRef> res;
      for (const auto cell : stream) {
        auto stepRes = compileSelector(step, cell, hasContext);
        res.insert(res.end(), stepRes.begin(), stepRes.end());
      }
      nextStream = res;
    } else {
      nextStream = emitMap(stream, [&](CellRef current) {
        return compileSelector(step, current, hasContext);
      });
      if (dimension && dimension->creates.empty())
        nextStream = emitMountFallback(*dimension, std::move(nextStream));
    }
    if (rootUserCall) {
      const CellRef empty = mintOpcode(OpcodeKind::Nop, "#ROOT_CALL_EMPTY");
      const CellRef end   = mintOpcode(OpcodeKind::Nop, "#ROOT_CALL_END");
      const CellRef choose =
          emitOp(OpcodeKind::BranchFalseCell, "#ROOT_CALL_TEST");
      emitInput(choose, *present);
      emitInput(choose, empty);
      const CellRef done =
          emitOp(OpcodeKind::BranchFalseCell, "#ROOT_CALL_SKIP");
      emitInputConstant(done, false);
      emitInput(done, end);
      appendOpcode(empty);
      if (userCall) {
        requirePositionalArguments(*function);
        const auto fallback = compileCall(definitions_.at(function->name),
                                          function->args, std::nullopt, true);
        emitAppendStream(nextStream.front(), fallback);
      } else {
        const auto fallback = compileSelector(step, core_.home(), false);
        emitAppendStream(nextStream.front(), fallback);
      }
      appendOpcode(end);
    }
    if (step.derefMaster) {
      nextStream = emitMap(nextStream, [&](CellRef candidate) {
        const auto master = emitDereference(candidate);
        return std::vector<CellRef>{master};
      });
    }
    for (const auto &predicate : step.predicates) {
      nextStream = emitMap(nextStream, [&](CellRef candidate) {
        const auto result = core_.arena().makeCell();
        const auto clear  = emitOp(OpcodeKind::ClearCellRefs, "#FILTER_CLEAR");
        emitOutput(clear, result);
        auto condition = compileCandidatePredicate(predicate, candidate);
        if (!condition) condition = emitConstant(false);
        const auto end = mintOpcode(OpcodeKind::Nop, "#FILTER_END");
        const auto branch =
            emitOp(OpcodeKind::BranchFalseCell, "#FILTER_BRANCH");
        emitInput(branch, *condition);
        emitInput(branch, end);
        const auto copy = emitOp(OpcodeKind::CopyCellRefs, "#FILTER_RESULT");
        emitInput(copy, candidate);
        emitOutput(copy, result);
        appendOpcode(end);
        return std::vector<CellRef>{result};
      });
    }

    for (const auto &shape : step.shapePredicates) {
      nextStream = emitMap(nextStream, [&](const CellRef candidate) {
        const auto result = core_.arena().makeCell();
        const auto clear  = emitOp(OpcodeKind::ClearCellRefs, "#SHAPE_CLEAR");
        emitOutput(clear, result);
        const auto condition = compileShapePredicate(shape, candidate);
        const auto end       = mintOpcode(OpcodeKind::Nop, "#SHAPE_FILTER_END");
        const auto branch =
            emitOp(OpcodeKind::BranchFalseCell, "#SHAPE_FILTER");
        emitInput(branch, condition);
        emitInput(branch, end);
        const auto copy = emitOp(OpcodeKind::CopyCellRefs, "#SHAPE_KEEP");
        emitInput(copy, candidate);
        emitOutput(copy, result);
        appendOpcode(end);
        return std::vector<CellRef>{result};
      });
    }

    if (step.rangeClamp) nextStream = emitClamp(nextStream, *step.rangeClamp);

    if (view && creationStep) {
      // The created cell is a plain cell, not a handle (a handle needs a
      // compile-time source). Marking it makes a later step from it refuse by
      // name rather than walk it without the view's rules.
      const auto marker =
          static_cast<std::int64_t>(resolveDimension(viewMarkerDimension));
      nextStream = emitMap(nextStream, [&](const CellRef created) {
        const CellRef group = beginMutationGroup("#VIEW_MARK_GROUP");
        const auto mint     = emitOp(OpcodeKind::New, "#VIEW_MARK");
        emitInput(mint, created);
        emitInputConstant(mint, marker);
        emitInputConstant(mint, std::int64_t{1});
        endMutationGroup(group, "#VIEW_MARK_GROUP_END");
        return std::vector<CellRef>{created};
      });
    }
    if (creationStep) {
      if (step.yieldMode == YieldMode::Keep) {
        // Keep the incoming context while retaining the selector's writes.
      } else if (step.yieldMode == YieldMode::Last) {
        if (!nextStream.empty()) {
          nextStream = emitClamp(nextStream, RangeClamp{.start = -1});
        }
        stream = std::move(nextStream);
      } else if (step.yieldMode == YieldMode::Both) {
        const auto both = emitStreamClear();
        emitAppendStream(both, stepInput);
        emitAppendStream(both, nextStream);
        stream = {both};
      } else {
        stream = std::move(nextStream);
      }
    } else if (step.yieldMode != YieldMode::Keep) {
      stream = std::move(nextStream);
    }
    if (step.projectionAfter)
      stream = applyShapeProjection(stream, *step.projectionAfter);
    if (view && !viewEnds) {
      ViewMark reached = *view;
      reached.created  = creationStep;
      markViewStream(stream, reached);
    }
    hasContext = true;
  }

  return stream;
}
} // namespace xanadu::vql
