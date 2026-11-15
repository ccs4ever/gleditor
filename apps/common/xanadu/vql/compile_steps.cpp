#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/dynamic_view_registry.hpp"
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
      auto &reg              = dynamicRegistry();
      std::optional<PutGetLensRecord> lensRec;
      if (const auto it = reg.namedLenses.find(name);
          it != reg.namedLenses.end()) {
        lensRec = it->second;
      } else if (const auto it = varBindings_.find(name);
                 it != varBindings_.end() && it->second) {
        if (const auto cIt = reg.cellLenses.find(*it->second);
            cIt != reg.cellLenses.end()) {
          lensRec = cIt->second;
        }
      }
      if (lensRec) {
        if (dimension->creates.empty()) {
          std::vector<CellRef> res;
          for (const auto candidate : stream) {
            const auto savedVars    = varBindings_;
            const auto savedStreams = streamBindings_;
            try {
              if (!lensRec->get.params.empty()) {
                const auto &param = lensRec->get.params.front();
                if (const auto *str = std::get_if<std::string>(&param.target)) {
                  varBindings_[*str]    = candidate;
                  streamBindings_[*str] = {candidate};
                }
              }
              if (const auto *valExprPtr =
                      std::get_if<std::shared_ptr<ValueExpr>>(
                          &lensRec->get.body);
                  valExprPtr && *valExprPtr) {
                if (const auto val =
                        compileValueExpr(**valExprPtr, candidate)) {
                  res.push_back(*val);
                }
              } else if (const auto *scriptPtr =
                             std::get_if<std::shared_ptr<ScriptBody>>(
                                 &lensRec->get.body);
                         scriptPtr && *scriptPtr) {
                for (const auto &stmt : (*scriptPtr)->statements) {
                  if (const auto *path = std::get_if<PathExpression>(&stmt)) {
                    if (const auto pRes =
                            compilePathExpression(*path, candidate)) {
                      res.push_back(*pRes);
                    }
                  }
                }
              }
              varBindings_    = savedVars;
              streamBindings_ = savedStreams;
            } catch (...) {
              varBindings_    = savedVars;
              streamBindings_ = savedStreams;
              throw;
            }
          }
          nextStream = res;
        } else {
          std::string valStr;
          for (const auto &create : dimension->creates) {
            if (create.kind == CreateValue::Kind::Literal) {
              valStr = create.literal;
            }
          }
          const CellRef valCell = core_.arena().makeCell(valStr);
          for (const auto candidate : stream) {
            const auto savedVars    = varBindings_;
            const auto savedStreams = streamBindings_;
            try {
              if (lensRec->put.params.size() >= 2) {
                if (const auto *str0 = std::get_if<std::string>(
                        &lensRec->put.params[0].target)) {
                  varBindings_[*str0]    = candidate;
                  streamBindings_[*str0] = {candidate};
                }
                if (const auto *str1 = std::get_if<std::string>(
                        &lensRec->put.params[1].target)) {
                  varBindings_[*str1]    = valCell;
                  streamBindings_[*str1] = {valCell};
                }
              }
              if (const auto *valExprPtr =
                      std::get_if<std::shared_ptr<ValueExpr>>(
                          &lensRec->put.body);
                  valExprPtr && *valExprPtr) {
                compileValueExpr(**valExprPtr, candidate);
              } else if (const auto *scriptPtr =
                             std::get_if<std::shared_ptr<ScriptBody>>(
                                 &lensRec->put.body);
                         scriptPtr && *scriptPtr) {
                for (const auto &stmt : (*scriptPtr)->statements) {
                  if (const auto *path = std::get_if<PathExpression>(&stmt)) {
                    compilePathExpression(*path, candidate);
                  }
                }
              }
              const auto span = core_.arena().intern(valStr);
              static_cast<void>(
                  core_.arena().setContent(candidate, std::span{&span, 1}));
              varBindings_    = savedVars;
              streamBindings_ = savedStreams;
            } catch (...) {
              varBindings_    = savedVars;
              streamBindings_ = savedStreams;
              throw;
            }
            nextStream.push_back(candidate);
          }
        }
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
        if (!hasContext) {
          nextStream = aliases;
          hasContext = true;
          stream     = nextStream;
          continue;
        }
        if (stream.empty()) {
          nextStream = aliases;
        } else {
          nextStream = emitMap(stream, [&](const CellRef candidate) {
            RuntimeClosureInvocation invocation{
                .callee = ValueExpr{.kind = name}, .args = {}, .argNames = {}};
            return compileClosureInvocation(
                invocation, candidate,
                zigzag::vortex::ClosureCallMode::Contextual);
          });
        }
      } else {
        throw std::invalid_argument("unbound point-free functor '" +
                                    dimension->dimName + "'");
      }
    } else if (function && !userCall && hasContext &&
               isWholeWalkStep(*function)) {
      // Ahead of the user-call empty-stream fallback and the per-cell emitMap:
      // an empty walk must still reach the step (count() answers a present 0).
      // Without a context there is no walk to count, so the step keeps the
      // per-cell path and its refusal.
      nextStream = compileWholeWalkStep(*function, stream, hasContext);
    } else if (std::holds_alternative<PerspectiveStep>(step.selector)) {
      std::vector<CellRef> res;
      for (const auto cell : stream) {
        auto stepRes = compileSelector(step, cell, hasContext);
        res.insert(res.end(), stepRes.begin(), stepRes.end());
      }
      nextStream = res;
    } else if (userCall && !hasContext) {
      requirePositionalArguments(*function);
      const auto fallback = compileCall(definitions_.at(function->name),
                                        function->args, std::nullopt, true);
      nextStream          = fallback;
    } else {
      bool hasViewCell = false;
      auto &reg        = dynamicRegistry();
      if (dimension) {
        for (const auto c : stream) {
          if (reg.viewCells.contains(c)) {
            hasViewCell = true;
            break;
          }
        }
      }
      if (hasViewCell) {
        std::vector<CellRef> res;
        for (const auto current : stream) {
          const auto vIt = reg.viewCells.find(current);
          if (vIt == reg.viewCells.end()) {
            auto sub = compileSelector(step, current, hasContext);
            res.insert(res.end(), sub.begin(), sub.end());
            continue;
          }
          const auto &viewRecord = vIt->second;
          const CellRef source   = viewRecord.source;
          const auto &shape      = viewRecord.view->shape;
          for (const auto &rule : shape.rules) {
            if (const auto *pivot =
                    std::get_if<DimensionPivotRule>(&rule.rule)) {
              if (pivot->fromDim == dimension->dimName) {
                const auto targetDim = resolveDimension(pivot->toDim);
                if (!dimension->creates.empty()) {
                  for (const auto &create : dimension->creates) {
                    CellRef newCell = zigzag::noCell;
                    if (create.kind == CreateValue::Kind::Literal) {
                      newCell = core_.arena().makeCell(create.literal);
                    }
                    if (newCell != zigzag::noCell) {
                      static_cast<void>(core_.arena().link(
                          source, targetDim, dimension->direction, newCell));
                      res.push_back(newCell);
                    }
                  }
                  break;
                }
                if (const auto linked = core_.arena().linked(
                        source, targetDim, dimension->direction)) {
                  CellRef targetCell = *linked;
                  auto hIt = viewRecord.view->handles.find(targetCell);
                  if (hIt == viewRecord.view->handles.end()) {
                    CellRef viewTarget = zigzag::noCell;
                    if (const auto text = core_.render(targetCell);
                        text && std::holds_alternative<std::string>(*text)) {
                      viewTarget =
                          core_.arena().makeCell(std::get<std::string>(*text));
                    } else {
                      viewTarget = core_.arena().makeCell();
                    }
                    viewRecord.view->handles[targetCell] = viewTarget;
                    reg.viewCells[viewTarget]            = ViewCellRecord{
                                   .source = targetCell, .view = viewRecord.view};
                    res.push_back(viewTarget);
                  } else {
                    res.push_back(hIt->second);
                  }
                }
                break;
              }
            } else if (const auto *virt =
                           std::get_if<VirtualDimensionDefRule>(&rule.rule)) {
              if (virt->dimName == dimension->dimName &&
                  virt->direction == dimension->direction) {
                if (!dimension->creates.empty()) {
                  throw std::runtime_error(
                      "computed dimension has no put transform: " +
                      dimension->dimName);
                }
                if (virt->expr) {
                  if (const auto text = core_.render(source);
                      text && std::holds_alternative<std::string>(*text)) {
                    CellRef valCell =
                        core_.arena().makeCell(std::get<std::string>(*text));
                    res.push_back(valCell);
                  }
                }
                break;
              }
            }
          }
        }
        nextStream = res;
      } else {
        nextStream = emitMap(stream, [&](CellRef current) {
          return compileSelector(step, current, hasContext);
        });
        if (dimension && dimension->creates.empty())
          nextStream = emitMountFallback(*dimension, std::move(nextStream));
      }
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
