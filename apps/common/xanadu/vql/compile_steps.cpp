#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/invocation.hpp"

#include <stdexcept>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

std::vector<CellRef> VQLCompiler::compileSteps(std::span<const PathStep> steps,
                                               std::vector<CellRef> stream,
                                               bool hasContext) {
  for (const auto &step : steps) {
    if (step.versionPin)
      throw std::invalid_argument(
          "version pins require historical VM lowering");
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
    if (step.capturedFunctor) {
      nextStream = compileSteps(*step.capturedFunctor, stream, hasContext);
    } else if (dimension && dimension->dimName.starts_with('$')) {
      const auto found = functorBindings_.find(dimension->dimName.substr(1));
      if (found == functorBindings_.end())
        throw std::invalid_argument("unbound point-free functor '" +
                                    dimension->dimName + "'");
      // Calls can replace binding maps while compiling their bodies.
      const auto captured = found->second;
      nextStream          = compileSteps(captured, stream, hasContext);
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
    hasContext = true;
  }

  return stream;
}
} // namespace xanadu::vql
