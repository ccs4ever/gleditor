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

std::vector<CellRef> VQLCompiler::compileSteps(std::span<const PathStep> steps,
                                               std::vector<CellRef> stream,
                                               bool hasContext) {
  for (const auto &step : steps) {
    if (step.versionPin) {
      stream = compileVersionPinnedStep(step, std::move(stream), hasContext);
      continue;
    }
    const auto stepInput = stream;
    std::vector<CellRef> nextStream;

    const auto *function  = std::get_if<FunctionInvocation>(&step.selector);
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    const bool userCall   = function && definitions_.contains(function->name);
    const bool creationStep =
        !step.capturedFunctor &&
        ((dimension && !dimension->dimName.starts_with('$') &&
          !dimension->creates.empty()) ||
         (function && function->name == "link" && function->args.size() >= 2));
    const auto present =
        userCall ? std::optional{emitStreamPresence(stream)} : std::nullopt;
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
      if (const auto found = functorBindings_.find(name);
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
        const CellRef closureHandle = normalizeFrameStream(aliases);
        nextStream = emitMap(stream, [&](const CellRef candidate) {
          const CellRef applied =
              normalizeFrameStream(std::span{&candidate, std::size_t{1}});
          const CellRef apply =
              emitOp(OpcodeKind::ApplyClosure, "#APPLY_CLOSURE " + name);
          emitInput(apply, closureHandle);
          emitInput(apply, applied);
          const CellRef result = core_.arena().makeCell();
          emitOutput(apply, result);
          dynamicStreams_.insert(result);
          return std::vector<CellRef>{result};
        });
      } else {
        throw std::invalid_argument("unbound point-free functor '" +
                                    dimension->dimName + "'");
      }
    } else {
      nextStream = emitMap(stream, [&](CellRef current) {
        return compileSelector(step, current, hasContext);
      });
    }
    // User calls may produce a value without a flowing context. Preserve the
    // direct evaluator's one context-free call when the input stream is empty.
    if (userCall) {
      const auto empty = mintOpcode(OpcodeKind::Nop, "#EMPTY_CALL");
      const auto end   = mintOpcode(OpcodeKind::Nop, "#EMPTY_CALL_END");
      const auto choose =
          emitOp(OpcodeKind::BranchFalseCell, "#EMPTY_CALL_TEST");
      emitInput(choose, *present);
      emitInput(choose, empty);
      const auto done = emitOp(OpcodeKind::BranchFalseCell, "#EMPTY_CALL_SKIP");
      emitInputConstant(done, false);
      emitInput(done, end);
      appendOpcode(empty);
      requirePositionalArguments(*function);
      const auto fallback = compileCall(definitions_.at(function->name),
                                        function->args, std::nullopt, true);
      emitAppendStream(nextStream.front(), fallback);
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
        auto condition = compileBooleanExpr(predicate, candidate);
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
    hasContext = true;
  }

  return stream;
}
} // namespace xanadu::vql
