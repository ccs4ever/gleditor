#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vql/closed_functor_syntax.hpp"
#include "common/xanadu/vql/closure_lambda_syntax.hpp"
#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/invocation.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::FrameDescriptorSpec;
using zigzag::vortex::FramePortKind;
using zigzag::vortex::OpcodeKind;

bool frameDescriptorCountsFit(const zigzag::vortex::FrameLimits limits,
                              const std::size_t locals,
                              const std::size_t argumentPorts,
                              const std::size_t resultPorts) {
  if (locals > limits.maxLocalsPerFrame || limits.maxDescriptorRecords == 0 ||
      locals > limits.maxDescriptorRecords - 1)
    return false;
  std::size_t remaining = limits.maxDescriptorRecords - 1 - locals;
  if (argumentPorts > remaining) return false;
  remaining -= argumentPorts;
  return resultPorts <= remaining;
}

bool addFrameCount(std::size_t &total, const std::size_t amount,
                   const std::size_t limit) {
  if (total > limit || amount > limit - total) return false;
  total += amount;
  return true;
}

std::string
frameVariantSuffix(const std::optional<std::size_t> restGroupCount) {
  if (!restGroupCount) return {};
  return "<rest=" + std::to_string(*restGroupCount) + ">";
}

template <class Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

} // namespace

class VQLCompiler::IsolatedCompileState {
public:
  explicit IsolatedCompileState(VQLCompiler &compiler)
      : compiler_(compiler), entry_(compiler.entryOp_),
        current_(compiler.currentOp_), variables_(compiler.varBindings_),
        streams_(compiler.streamBindings_),
        functors_(compiler.functorBindings_),
        context_(compiler.ambientContext_), results_(compiler.lastResultCells_),
        dynamicStreams_(compiler.dynamicStreams_),
        frameMutableSlots_(compiler.frameMutableSlots_),
        activeCalls_(compiler.activeCalls_),
        compilingFrameDefinition_(compiler.compilingFrameDefinition_),
        compilingFrameDefinitionName_(compiler.compilingFrameDefinitionName_) {
    compiler_.entryOp_.reset();
    compiler_.currentOp_.reset();
    compiler_.lastResultCells_.clear();
    compiler_.varBindings_.clear();
    compiler_.streamBindings_.clear();
    compiler_.functorBindings_.clear();
    compiler_.dynamicStreams_.clear();
    compiler_.frameMutableSlots_.clear();
    compiler_.ambientContext_.reset();
  }

  IsolatedCompileState(const IsolatedCompileState &)            = delete;
  IsolatedCompileState &operator=(const IsolatedCompileState &) = delete;

  ~IsolatedCompileState() {
    compiler_.entryOp_                  = entry_;
    compiler_.currentOp_                = current_;
    compiler_.varBindings_              = std::move(variables_);
    compiler_.streamBindings_           = std::move(streams_);
    compiler_.functorBindings_          = std::move(functors_);
    compiler_.ambientContext_           = context_;
    compiler_.lastResultCells_          = std::move(results_);
    compiler_.dynamicStreams_           = std::move(dynamicStreams_);
    compiler_.frameMutableSlots_        = std::move(frameMutableSlots_);
    compiler_.activeCalls_              = std::move(activeCalls_);
    compiler_.compilingFrameDefinition_ = compilingFrameDefinition_;
    compiler_.compilingFrameDefinitionName_ =
        std::move(compilingFrameDefinitionName_);
  }

private:
  VQLCompiler &compiler_;
  std::optional<CellRef> entry_;
  std::optional<CellRef> current_;
  std::unordered_map<std::string, std::optional<CellRef>> variables_;
  std::unordered_map<std::string, std::vector<CellRef>> streams_;
  std::unordered_map<std::string, std::vector<PathStep>> functors_;
  std::optional<CellRef> context_;
  std::vector<CellRef> results_;
  std::unordered_set<CellRef> dynamicStreams_;
  std::unordered_set<CellRef> frameMutableSlots_;
  std::vector<std::string> activeCalls_;
  bool compilingFrameDefinition_;
  std::string compilingFrameDefinitionName_;
};

std::vector<CellRef>
VQLCompiler::emitContractDefaultSlots(const TypeExpr &type) {
  std::vector<CellRef> slots;
  std::vector<std::string> names;
  try {
    names = contractDefaultVariables(type);
  } catch (const ContractDescriptorError &error) {
    throw std::invalid_argument(error.what());
  }
  for (const auto &name : names) {
    if (functorBindings_.contains(name)) {
      throw std::invalid_argument("contract default $" + name +
                                  " cannot capture a path functor");
    }

    const auto found = streamBindings_.find(name);
    if (found == streamBindings_.end()) {
      if (const auto scalar = varBindings_.find(name);
          scalar != varBindings_.end() && scalar->second) {
        slots.push_back(*scalar->second);
      } else {
        throw std::invalid_argument("contract default $" + name +
                                    " has no prior lexical binding");
      }
    } else {
      const auto &bound = found->second;
      if (bound.size() > 1) {
        throw std::invalid_argument("contract default $" + name +
                                    " must resolve to at most one cell");
      }
      slots.push_back(bound.empty() ? emitStreamClear() : bound.front());
    }
    if (core_.hasPipeline(slots.back())) {
      throw std::invalid_argument(
          "contract default aliases cannot depend on parameter pipelines");
    }
  }
  return slots;
}

std::optional<CellRef>
VQLCompiler::compileBuiltinInvocation(const FunctionInvocation &invocation,
                                      const std::optional<CellRef> context) {
  if (invocation.name != "string" || definitions_.contains(invocation.name))
    return std::nullopt;
  if (invocation.isSliceRead)
    throw std::invalid_argument("string() does not support slice reads");
  requirePositionalArguments(invocation);
  if (invocation.args.size() != 1)
    throw std::invalid_argument("string() requires exactly one argument");
  if (functorArgument(invocation.args.front()))
    throw std::invalid_argument("string() does not accept path functors");

  const auto argument = compileArgument(invocation.args.front(), context);
  const CellRef input = normalizeFrameStream(argument);
  const CellRef op    = emitOp(OpcodeKind::VqlString, "#STRING");
  emitInput(op, input);
  const CellRef result = core_.arena().makeCell();
  emitOutput(op, result);
  const CellRef box = emitOp(OpcodeKind::VqlBoxScalar, "#BOX_SCALAR");
  emitInput(box, result);
  const CellRef boxed = core_.arena().makeCell();
  emitOutput(box, boxed);
  return boxed;
}

std::vector<CellRef>
VQLCompiler::compileArgument(const ValueExpr &argument,
                             std::optional<CellRef> context) {
  if (const auto *name = std::get_if<std::string>(&argument.kind)) {
    if (const auto found = streamBindings_.find(*name);
        found != streamBindings_.end())
      return found->second;
    if (const auto found = varBindings_.find(*name);
        found != varBindings_.end() && found->second)
      return {*found->second};
    return {};
  }
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&argument.kind)) {
    const auto saved = ambientContext_;
    ambientContext_  = context;
    const RestoreScope restore{[&] { ambientContext_ = saved; }};
    compilePathExpression(**path, context.value_or(core_.home()));
    return lastResultCells_;
  }
  if (const auto *function =
          std::get_if<std::shared_ptr<FunctionInvocation>>(&argument.kind)) {
    if (!*function)
      throw std::invalid_argument("null compiled function invocation");
    if (const auto builtin = compileBuiltinInvocation(**function, context))
      return {*builtin};
    const auto found = definitions_.find((*function)->name);
    if (found == definitions_.end())
      throw std::invalid_argument("unknown compiled function: " +
                                  (*function)->name);
    requirePositionalArguments(**function);
    return compileCall(found->second, (*function)->args, context, false);
  }
  const auto value = compileValueExpr(argument, context.value_or(core_.home()));
  return value ? std::vector<CellRef>{*value} : std::vector<CellRef>{};
}

std::vector<CellRef>
VQLCompiler::compileCall(const DefDeclaration &definition,
                         const std::vector<ValueExpr> &arguments,
                         std::optional<CellRef> context, bool ufcs,
                         const bool entersActiveCallStack) {
  std::vector<ClosedFunctorFormalKey> functorTemplates;
  bool unsupportedFunctorArgument = false;
  const auto requiredFunctorFormals =
      frameFunctorFormalIndices_.find(definition.name);
  const std::span<const std::size_t> requiredFormalIndices =
      requiredFunctorFormals == frameFunctorFormalIndices_.end()
          ? std::span<const std::size_t>{}
          : std::span<const std::size_t>{requiredFunctorFormals->second};
  std::size_t functorIndex =
      ufcs && context && definition.params.size() > arguments.size() ? 1 : 0;
  for (const auto &argument : arguments) {
    const bool acceptsFunctor = functorIndex < definition.params.size() &&
                                !definition.params[functorIndex].type &&
                                !definition.params[functorIndex].isVariadic &&
                                std::holds_alternative<std::string>(
                                    definition.params[functorIndex].target);
    const bool isFunctor = acceptsFunctor && isFunctorArgument(argument);
    const bool templateFormal =
        acceptsFunctor && !definition.params[functorIndex].defaultValue;
    const bool requiredFormal =
        std::ranges::binary_search(requiredFormalIndices, functorIndex);
    if (isFunctor) {
      const auto steps = closedFunctorArgumentSteps(argument);
      if ((!templateFormal && !requiredFormal) || !steps) {
        unsupportedFunctorArgument = true;
      } else {
        auto formal = normalizeClosedFunctorTemplate(*steps, functorIndex);
        if (!formal) {
          unsupportedFunctorArgument = true;
        } else {
          functorTemplates.push_back(std::move(*formal));
        }
      }
    } else if (requiredFormal) {
      unsupportedFunctorArgument = true;
    }
    ++functorIndex;
  }
  const std::size_t suppliedActualCount =
      arguments.size() +
      static_cast<std::size_t>(ufcs && context &&
                               definition.params.size() > arguments.size());
  for (const std::size_t requiredIndex : requiredFormalIndices) {
    if (requiredIndex >= suppliedActualCount) {
      const auto &parameter = definition.params[requiredIndex];
      const auto defaultSteps =
          parameter.defaultValue
              ? closedTraversalFunctorLiteralSteps(*parameter.defaultValue)
              : std::nullopt;
      auto formal = defaultSteps ? normalizeClosedFunctorTemplate(*defaultSteps,
                                                                  requiredIndex)
                                 : std::nullopt;
      if (formal)
        functorTemplates.push_back(std::move(*formal));
      else
        unsupportedFunctorArgument = true;
    }
    if (!std::ranges::binary_search(functorTemplates, requiredIndex, {},
                                    &ClosedFunctorFormalKey::formalIndex))
      unsupportedFunctorArgument = true;
  }
  std::ranges::sort(functorTemplates, {}, &ClosedFunctorFormalKey::formalIndex);
  bool hasInlineCapture = false;
  if (const auto captures = frameFreeCaptureNames_.find(definition.name);
      captures != frameFreeCaptureNames_.end()) {
    hasInlineCapture =
        std::ranges::any_of(captures->second, [this](const std::string &name) {
          if (functorBindings_.contains(name)) return true;
          if (const auto stream = streamBindings_.find(name);
              stream != streamBindings_.end()) {
            return std::ranges::any_of(stream->second, [this](CellRef cell) {
              return core_.hasPipeline(cell);
            });
          }
          if (const auto scalar = varBindings_.find(name);
              scalar != varBindings_.end() && scalar->second)
            return core_.hasPipeline(*scalar->second);
          return false;
        });
  }
  if (reusableFrameDefinitions_.contains(definition.name) &&
      !unsupportedFunctorArgument && !hasInlineCapture) {
    if (compilingFrameDefinition_ && activeCalls_.size() > 1 &&
        activeCalls_.back() != compilingFrameDefinitionName_ &&
        std::ranges::contains(activeCalls_, definition.name))
      throw std::invalid_argument(
          "recursive cycle crosses an inline-only definition: " +
          definition.name);
    return compileReusableFrameCall(definition, arguments, context, ufcs,
                                    std::move(functorTemplates));
  }
  if (entersActiveCallStack &&
      std::ranges::contains(activeCalls_, definition.name))
    throw std::invalid_argument(
        "recursive compiled call requires a VM frame: " + definition.name);

  auto savedVariables     = varBindings_;
  auto savedStreams       = streamBindings_;
  auto savedFunctors      = functorBindings_;
  const auto savedContext = ambientContext_;
  bool callActivated      = false;
  const RestoreScope restore{[&] {
    varBindings_     = std::move(savedVariables);
    streamBindings_  = std::move(savedStreams);
    functorBindings_ = std::move(savedFunctors);
    ambientContext_  = savedContext;
    if (callActivated) activeCalls_.pop_back();
  }};

  // Arguments belong to the caller's lexical scope. Evaluate every operand
  // before introducing any parameter binding or executing the body.
  std::vector<std::vector<CellRef>> values;
  std::vector<std::optional<std::vector<PathStep>>> functors;
  if (ufcs && context && definition.params.size() > arguments.size()) {
    values.push_back({*context});
    functors.push_back(std::nullopt);
  }
  for (const auto &argument : arguments) {
    const auto index = values.size();
    std::optional<std::vector<PathStep>> functor;
    if (index < definition.params.size()) {
      const auto &parameter = definition.params[index];
      if (!parameter.type && !parameter.isVariadic &&
          std::holds_alternative<std::string>(parameter.target))
        functor = functorArgument(argument);
    }
    values.push_back(functor ? std::vector<CellRef>{}
                             : compileArgument(argument, context));
    functors.push_back(std::move(functor));
  }
  ambientContext_ = context;
  if (entersActiveCallStack) {
    activeCalls_.push_back(definition.name);
    callActivated = true;
  }

  const auto clear =
      emitOp(OpcodeKind::ClearCellRefs, "#CALL_CLEAR " + definition.name);
  const CellRef guardScope =
      openGuardScope("#CALL_GUARD_SCOPE " + definition.name);
  const CellRef filterExit =
      mintOpcode(OpcodeKind::Nop, "#PARAM_FILTER_CLEANUP " + definition.name);
  const auto end  = mintOpcode(OpcodeKind::Nop, "#CALL_END " + definition.name);
  const auto bind = [&](const std::string &name, std::vector<CellRef> cells) {
    functorBindings_.erase(name);
    streamBindings_[name] = cells;
    varBindings_[name] =
        cells.empty() ? std::nullopt : std::optional{cells.front()};
  };
  const auto check = [&](const TypeExpr &type,
                         const std::vector<CellRef> &subjects,
                         const bool bindCaptures,
                         const PreparedContractValidator &prepared,
                         const std::string &diagnostic) {
    const CellRef matched = emitContractMatch(type, subjects, bindCaptures);
    const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#PARAM_FILTER");
    emitInput(branch, matched);
    emitInput(branch, filterExit);
    pushContractGuard(prepared, diagnostic, "#PARAM_GUARD");
  };

  std::size_t argumentIndex = 0;
  for (std::size_t parameterIndex = 0;
       parameterIndex < definition.params.size(); ++parameterIndex) {
    const auto &parameter = definition.params[parameterIndex];
    if (!parameter.isVariadic && argumentIndex < functors.size() &&
        functors[argumentIndex]) {
      const auto &name       = std::get<std::string>(parameter.target);
      functorBindings_[name] = *functors[argumentIndex++];
      varBindings_.erase(name);
      streamBindings_.erase(name);
      continue;
    }
    std::vector<CellRef> cells;
    if (parameter.isVariadic) {
      while (argumentIndex < values.size()) {
        if (parameter.type) {
          std::string diagnostic = definition.name + " parameter ";
          diagnostic += std::to_string(parameterIndex + 1);
          if (const auto *name = std::get_if<std::string>(&parameter.target))
            diagnostic += " ($" + *name + ")";
          if (diagnostic.size() > vm_.guardLimits().maxDiagnosticBytes)
            throw std::invalid_argument("parameter guard diagnostic exceeds "
                                        "the configured text budget");
          const auto prepared =
              prepareContractValidator(*parameter.type, values[argumentIndex]);
          check(*parameter.type, values[argumentIndex], false, prepared,
                diagnostic);
        }
        cells.insert(cells.end(), values[argumentIndex].begin(),
                     values[argumentIndex].end());
        ++argumentIndex;
      }
    } else {
      if (argumentIndex < values.size())
        cells = values[argumentIndex++];
      else if (parameter.defaultValue) {
        cells = compileArgument(*parameter.defaultValue,
                                ambientContext_.has_value() ? ambientContext_
                                                            : context);
      }
    }
    std::optional<PreparedContractValidator> typePrepared;
    std::optional<PreparedContractValidator> destructurePrepared;
    std::optional<TypeExpr> destructureType;
    std::string diagnostic = definition.name + " parameter ";
    diagnostic += std::to_string(parameterIndex + 1);
    if (const auto *name = std::get_if<std::string>(&parameter.target))
      diagnostic += " ($" + *name + ")";
    else if (std::holds_alternative<DestructurePattern>(parameter.target))
      diagnostic += "destructure";
    else
      diagnostic += "context";
    if (!parameter.isVariadic &&
        (parameter.type ||
         std::holds_alternative<DestructurePattern>(parameter.target))) {
      if (diagnostic.size() > vm_.guardLimits().maxDiagnosticBytes)
        throw std::invalid_argument(
            "parameter guard diagnostic exceeds the configured text budget");
      if (parameter.type)
        typePrepared = prepareContractValidator(*parameter.type, cells);
      if (parameter.type)
        check(*parameter.type, cells, true, *typePrepared, diagnostic);
      if (const auto *pattern =
              std::get_if<DestructurePattern>(&parameter.target)) {
        destructureType.emplace(TypeExpr{
            .unionType =
                UnionType{.members = {PrimaryType{
                              .kind = ShapeType{.pattern = *pattern}}}}});
        destructurePrepared = prepareContractValidator(*destructureType, cells);
      }
    }
    if (!parameter.isVariadic &&
        std::holds_alternative<DestructurePattern>(parameter.target)) {
      if (destructureType && destructurePrepared)
        check(*destructureType, cells, true, *destructurePrepared, diagnostic);
    } else if (const auto *name = std::get_if<std::string>(&parameter.target)) {
      bind(*name, std::move(cells));
    } else {
      bind(".", std::move(cells));
      const auto &contextCells = streamBindings_.at(".");
      if (contextCells.empty()) {
        ambientContext_.reset();
      } else if (contextCells.size() == 1) {
        ambientContext_ = contextCells.front();
      } else {
        const CellRef contextStream = emitStreamClear();
        emitAppendStream(contextStream, contextCells);
        ambientContext_ = contextStream;
      }
    }
    if (parameter.isVariadic) break;
  }

  std::vector<CellRef> bodyResults;
  if (const auto *value =
          std::get_if<std::shared_ptr<ValueExpr>>(&definition.body)) {
    bodyResults = compileArgument(
        **value, ambientContext_.has_value() ? ambientContext_ : context);
  } else {
    const auto &body = *std::get<std::shared_ptr<ScriptBody>>(definition.body);
    for (const auto &statement : body.statements) {
      lastResultCells_.clear();
      if (const auto *block = std::get_if<ExecutionBlock>(&statement))
        compileExecutionBlock(*block);
      else if (const auto *path = std::get_if<PathExpression>(&statement))
        compilePathExpression(*path, context.value_or(core_.home()));
      else if (const auto *weave = std::get_if<WeaveStatement>(&statement)) {
        compileExecutionBlock(
            ExecutionBlock{.action = ActionClause{.clause = weave->effect}});
      } else
        throw std::invalid_argument(
            "compiled definition body requires a supported block");
      bodyResults = lastResultCells_;
    }
  }
  if (definition.returnType) {
    emitRequiredContractMatch(*definition.returnType, bodyResults,
                              definition.name);
  }
  std::vector<CellRef> results;
  const auto copy =
      emitOp(OpcodeKind::CopyCellRefs, "#CALL_RESULTS " + definition.name);
  for (const auto result : bodyResults) {
    const auto slot = core_.arena().makeCell();
    emitInput(copy, result);
    emitOutput(copy, slot);
    emitOutput(clear, slot);
    if (dynamicStreams_.contains(result)) dynamicStreams_.insert(slot);
    results.push_back(slot);
  }
  lastResultCells_ = results;
  closeGuardScope(guardScope, "#CALL_GUARD_SCOPE_CLOSE " + definition.name);
  const CellRef skipFilterCleanup =
      emitOp(OpcodeKind::BranchFalseCell, "#CALL_SKIP_FILTER_CLEANUP");
  emitInputConstant(skipFilterCleanup, false);
  emitInput(skipFilterCleanup, end);
  appendOpcode(filterExit);
  closeGuardScope(guardScope,
                  "#CALL_FILTER_GUARD_SCOPE_CLOSE " + definition.name);
  appendOpcode(end);
  return results;
}

std::optional<VQLCompiler::ClosedFunctorFormalKey>
VQLCompiler::normalizeClosedFunctorTemplate(
    const std::span<const PathStep> steps, const std::size_t formalIndex) {
  if (steps.empty()) return std::nullopt;
  std::size_t work   = 1;
  const auto addWork = [&work](const std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - work)
      work = std::numeric_limits<std::size_t>::max();
    else
      work += amount;
  };
  for (const auto &step : steps) {
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (!dimension || !isClosedTraversalFunctorStep(step)) return std::nullopt;
    addWork(closedFunctorStepModifierWork);
    addWork(dimension->dimName.size());
    if (step.rangeClamp)
      addWork(closedFunctorClampWork(step.rangeClamp->end.has_value()));
    for (const auto &predicate : step.predicates) {
      if (!closedLiteralPredicateView(predicate)) return std::nullopt;
      const auto predicateWork = closedFunctorPredicateWork(predicate);
      if (predicateWork == std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(
            "compiled VQL closed functor template work budget exhausted");
      addWork(predicateWork);
    }
  }
  if (steps.size() > std::numeric_limits<std::size_t>::max() - work)
    work = std::numeric_limits<std::size_t>::max();
  else
    work += steps.size();
  chargeClosedFunctorTemplateWork(work);

  ClosedFunctorFormalKey formal{.formalIndex = formalIndex};
  formal.steps.reserve(steps.size());
  for (const auto &step : steps) {
    const auto &dimension = std::get<SignedDimensionStep>(step.selector);
    ClosedFunctorStepKey stepKey{
        .dimName     = dimension.dimName,
        .direction   = dimension.direction,
        .isBidir     = dimension.isBidir,
        .derefMaster = step.derefMaster,
        .rangeClamp  = step.rangeClamp ? std::optional{ClosedFunctorClampKey{
                                             .start = step.rangeClamp->start,
                                             .end   = step.rangeClamp->end}}
                                       : std::nullopt};
    stepKey.predicates.reserve(step.predicates.size());
    for (const auto &predicate : step.predicates) {
      const auto view = closedLiteralPredicateView(predicate);
      if (!view)
        throw std::logic_error(
            "closed traversal predicate changed after validation");
      ClosedFunctorPredicateKey predicateKey;
      predicateKey.terms.reserve(view->terms.size());
      for (const auto &term : view->terms) {
        ClosedFunctorPredicateTermKey termKey;
        termKey.factors.reserve(term.factors.size());
        for (const auto &factor : term.factors) {
          const auto comparison = closedLiteralComparisonView(factor);
          if (!comparison)
            throw std::logic_error(
                "closed traversal factor changed after validation");
          auto literal = std::visit(
              [](const auto &value) -> ClosedFunctorPredicateLiteral {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, double>)
                  return scalarValue(value).bits;
                else
                  return value;
              },
              comparison->literal.get().value);
          termKey.factors.push_back(ClosedFunctorPredicateFactorKey{
              .negated       = comparison->negated,
              .op            = comparison->op,
              .literalOnLeft = comparison->literalOnLeft,
              .literal       = std::move(literal)});
        }
        predicateKey.terms.push_back(std::move(termKey));
      }
      stepKey.predicates.push_back(std::move(predicateKey));
    }
    formal.steps.push_back(std::move(stepKey));
  }
  return formal;
}

CellRef
VQLCompiler::normalizeFrameStream(const std::span<const CellRef> cells) {
  const CellRef stream = emitStreamClear();
  emitAppendStream(stream, cells);
  return stream;
}

void VQLCompiler::finalizeNativeFrame(
    const CellRef descriptor, FrameDescriptorSpec &spec,
    const std::optional<FrameDefinitionKey> definitionKey) {
  std::unordered_set<CellRef> localSet(spec.locals.begin(), spec.locals.end());
  std::vector<CellRef> mutableSlots(frameMutableSlots_.begin(),
                                    frameMutableSlots_.end());
  std::ranges::sort(mutableSlots);
  for (const auto output : mutableSlots) {
    if (core_.arena().contains(output) && !vm_.getOpcodeKind(output) &&
        localSet.insert(output).second) {
      if (core_.hasPipeline(output))
        throw std::invalid_argument(
            "reusable frame body produced a pipelined local slot");
      spec.locals.push_back(output);
    }
  }
  std::size_t recordCount = 1;
  const auto addRecords   = [this, &recordCount](const std::size_t count) {
    if (recordCount > remainingFrameDescriptorRecords_ ||
        count > remainingFrameDescriptorRecords_ - recordCount)
      throw std::invalid_argument(
          "compiled VQL frame descriptor record budget exhausted");
    recordCount += count;
  };
  addRecords(spec.locals.size());
  addRecords(spec.argumentPorts.size());
  addRecords(spec.resultPorts.size());
  remainingFrameDescriptorRecords_ -= recordCount;
  spec.entry           = entryOp_;
  auto definitionState = frameDefinitions_.end();
  if (definitionKey) {
    chargeClosedFunctorTemplateWork(
        closedFunctorTemplateWork(definitionKey->functorTemplates));
    definitionState = frameDefinitions_.find(*definitionKey);
    if (definitionState == frameDefinitions_.end())
      throw std::invalid_argument("missing finalized frame definition");
  }
  const auto finalized = vm_.finalizeFrameDescriptor(descriptor, spec);
  if (!finalized) throw std::invalid_argument(finalized.error().message);
  if (definitionKey) {
    auto &state    = definitionState->second;
    state.entry    = entryOp_;
    state.compiled = true;
    state.queued   = false;
  }
}

CellRef VQLCompiler::emitNativeFrameReturn(const CellRef descriptor,
                                           const std::string_view label) {
  const CellRef opcode = mintOpcode(OpcodeKind::ReturnFrame, label);
  emitInput(opcode, descriptor);
  return opcode;
}

VQLCompiler::PreparedContractValidator
VQLCompiler::prepareContractValidator(const TypeExpr &type,
                                      const std::span<const CellRef> subjects) {
  validateContractAliases(type);
  struct FrozenInput {
    std::string name;
    std::vector<CellRef> cells;
    bool hasPort{true};
    std::optional<CellRef> slot;
  };

  std::vector<std::string> names;
  names.reserve(streamBindings_.size() + varBindings_.size());
  for (const auto &[name, _] : streamBindings_)
    if (name != ".") names.push_back(name);
  for (const auto &[name, _] : varBindings_)
    if (name != "." && !streamBindings_.contains(name)) names.push_back(name);
  std::ranges::sort(names);

  const auto frozenFunctors = functorBindings_;
  std::vector<FrozenInput> frozen;
  frozen.reserve(names.size());
  for (const auto &name : names) {
    if (const auto stream = streamBindings_.find(name);
        stream != streamBindings_.end()) {
      frozen.push_back({name, stream->second, true, std::nullopt});
      continue;
    }
    const auto variable = varBindings_.find(name);
    if (variable == varBindings_.end() || !variable->second) {
      frozen.push_back({name, {}, false, std::nullopt});
      continue;
    }
    frozen.push_back({name, {*variable->second}, true, std::nullopt});
  }

  const auto reserved = vm_.reserveFrameDescriptor();
  if (!reserved) throw std::invalid_argument(reserved.error().message);
  const CellRef descriptor = *reserved;
  FrameDescriptorSpec spec;
  const CellRef contextSlot = core_.arena().makeCell();
  const CellRef subjectSlot = core_.arena().makeCell();
  spec.locals               = {contextSlot, subjectSlot};
  spec.argumentPorts.emplace_back(contextSlot, FramePortKind::Stream);
  spec.argumentPorts.emplace_back(subjectSlot, FramePortKind::Stream);
  for (auto &input : frozen) {
    if (!input.hasPort) {
      continue;
    }
    const CellRef slot = core_.arena().makeCell();
    spec.locals.push_back(slot);
    spec.argumentPorts.emplace_back(slot, FramePortKind::Stream);
    input.slot = slot;
  }
  const auto internalId = nextContractValidatorId_++;

  {
    IsolatedCompileState isolated(*this);
    const std::size_t graphBegin = allOps_.size();
    compilingFrameDefinition_    = true;
    compilingFrameDefinitionName_ =
        "#contract-validator-" + std::to_string(internalId);
    activeCalls_.push_back(compilingFrameDefinitionName_);
    streamBindings_["."] = {contextSlot};
    varBindings_["."]    = contextSlot;
    ambientContext_      = contextSlot;
    dynamicStreams_.insert(contextSlot);
    dynamicStreams_.insert(subjectSlot);
    for (std::size_t index = 0; index < frozen.size(); ++index) {
      const auto &input = frozen[index];
      if (!input.slot) {
        varBindings_[input.name] = std::nullopt;
        continue;
      }
      const CellRef slot          = *input.slot;
      streamBindings_[input.name] = {slot};
      varBindings_[input.name]    = slot;
      dynamicStreams_.insert(slot);
    }
    functorBindings_           = frozenFunctors;
    const CellRef returnOpcode = emitNativeFrameReturn(
        descriptor, "#RETURN_CONTRACT_VALIDATOR " + std::to_string(internalId));
    const CellRef matched = emitContractGraphMatch(
        type, std::span{&subjectSlot, std::size_t{1}}, false);
    spec.resultPorts.emplace_back(matched, FramePortKind::Cell);
    appendOpcode(returnOpcode);

    std::unordered_set<CellRef> referencedInputs;
    for (std::size_t index = graphBegin; index < allOps_.size(); ++index) {
      for (const CellRef input : core_.inputsOf(allOps_[index]))
        referencedInputs.insert(input);
    }
    std::unordered_set<CellRef> unusedSlots;
    for (auto &input : frozen) {
      if (!input.slot || referencedInputs.contains(*input.slot)) continue;
      unusedSlots.insert(*input.slot);
      input.slot.reset();
    }
    std::erase_if(spec.argumentPorts, [&](const auto &port) {
      return unusedSlots.contains(port.slot);
    });
    std::erase_if(spec.locals, [&](const CellRef slot) {
      return unusedSlots.contains(slot);
    });
    if ((ambientContext_ && core_.hasPipeline(*ambientContext_)) ||
        std::ranges::any_of(
            subjects,
            [this](const CellRef cell) { return core_.hasPipeline(cell); }) ||
        std::ranges::any_of(frozen, [this](const FrozenInput &input) {
          return input.slot &&
                 std::ranges::any_of(input.cells, [this](const CellRef cell) {
                   return core_.hasPipeline(cell);
                 });
        }))
      throw std::invalid_argument(
          "mutation guards cannot freeze pipelined contract inputs");
    finalizeNativeFrame(descriptor, spec);
  }

  const CellRef contextStream =
      ambientContext_
          ? normalizeFrameStream(std::span{&*ambientContext_, std::size_t{1}})
          : normalizeFrameStream(std::span<const CellRef>{});
  const CellRef subjectStream = normalizeFrameStream(subjects);
  PreparedContractValidator prepared{.descriptor = descriptor,
                                     .id         = internalId};
  prepared.frozenStreams.reserve(frozen.size() + 2);
  prepared.frozenStreams.push_back(contextStream);
  prepared.frozenStreams.push_back(subjectStream);
  for (const auto &input : frozen)
    if (input.slot)
      prepared.frozenStreams.push_back(normalizeFrameStream(input.cells));
  return prepared;
}

CellRef VQLCompiler::invokeContractValidator(
    const PreparedContractValidator &prepared) {
  const CellRef invoke =
      emitOp(OpcodeKind::InvokeFrame,
             "#INVOKE_CONTRACT_VALIDATOR " + std::to_string(prepared.id));
  emitInput(invoke, prepared.descriptor);
  for (const CellRef stream : prepared.frozenStreams) emitInput(invoke, stream);
  const CellRef result = core_.arena().makeCell();
  emitOutput(invoke, result);
  return result;
}

CellRef
VQLCompiler::compileContractValidator(const TypeExpr &type,
                                      const std::span<const CellRef> subjects) {
  return invokeContractValidator(prepareContractValidator(type, subjects));
}

std::vector<CellRef> VQLCompiler::compileReusableFrameCall(
    const DefDeclaration &definition, const std::vector<ValueExpr> &arguments,
    const std::optional<CellRef> context, const bool ufcs,
    std::vector<ClosedFunctorFormalKey> functorTemplates) {
  const auto callerCaptureCells = [this](const std::string &name) {
    if (const auto found = streamBindings_.find(name);
        found != streamBindings_.end())
      return found->second;
    if (const auto found = varBindings_.find(name);
        found != varBindings_.end() && found->second)
      return std::vector<CellRef>{*found->second};
    return std::vector<CellRef>{};
  };
  const bool hasVariadicRest =
      !definition.params.empty() && definition.params.back().isVariadic;
  const bool typedRest =
      hasVariadicRest && definition.params.back().type != nullptr;
  const bool injectContext =
      ufcs && context && definition.params.size() > arguments.size();
  const std::size_t fixedParameterCount =
      definition.params.size() - static_cast<std::size_t>(hasVariadicRest);
  const std::size_t suppliedActualCount =
      arguments.size() + static_cast<std::size_t>(injectContext);
  const std::size_t restGroupCount =
      typedRest && suppliedActualCount > fixedParameterCount
          ? suppliedActualCount - fixedParameterCount
          : 0;
  const VQLCompiler::FrameDefinitionKey key{
      .name = definition.name,
      .restGroupCount =
          typedRest ? std::optional{restGroupCount} : std::nullopt,
      .functorTemplates = std::move(functorTemplates)};

  chargeClosedFunctorTemplateWork(
      closedFunctorTemplateWork(key.functorTemplates));
  const auto limits              = vm_.frameLimits();
  const std::size_t captureCount = [&] {
    const auto captures = frameFreeCaptureNames_.find(definition.name);
    return captures == frameFreeCaptureNames_.end() ? std::size_t{0}
                                                    : captures->second.size();
  }();
  const std::size_t defaultCount =
      static_cast<std::size_t>(std::ranges::count_if(
          definition.params, [](const LambdaParam &parameter) {
            return parameter.defaultValue != nullptr;
          }));
  const std::size_t inputParameterCount =
      typedRest ? fixedParameterCount : definition.params.size();
  std::size_t locals                      = 2;
  std::size_t argumentPortCount           = 1;
  const std::size_t descriptorRecordLimit = limits.maxDescriptorRecords;
  if (!addFrameCount(locals, captureCount, limits.maxLocalsPerFrame) ||
      !addFrameCount(locals, definition.params.size(),
                     limits.maxLocalsPerFrame) ||
      !addFrameCount(locals, defaultCount, limits.maxLocalsPerFrame) ||
      !addFrameCount(locals, typedRest ? restGroupCount : 0,
                     limits.maxLocalsPerFrame) ||
      !addFrameCount(argumentPortCount, captureCount, descriptorRecordLimit) ||
      !addFrameCount(argumentPortCount, inputParameterCount,
                     descriptorRecordLimit) ||
      !addFrameCount(argumentPortCount, defaultCount, descriptorRecordLimit) ||
      !addFrameCount(argumentPortCount, typedRest ? restGroupCount : 0,
                     descriptorRecordLimit) ||
      !frameDescriptorCountsFit(limits, locals, argumentPortCount, 1))
    throw std::invalid_argument(
        "native frame exceeds configured local or descriptor-port limits");

  std::optional<std::size_t> functorVariantOrdinal;
  std::optional<CellRef> frameDescriptor;
  {
    chargeClosedFunctorTemplateWork(
        closedFunctorTemplateWork(key.functorTemplates));
    auto state = frameDefinitions_.find(key);
    if (state == frameDefinitions_.end()) {
      if (frameDefinitions_.size() >= maxNamedFrameVariants_)
        throw std::invalid_argument(
            "named frame variant budget exhausted for " + definition.name);
      const auto templateCopyWork =
          closedFunctorTemplateWork(key.functorTemplates);
      chargeClosedFunctorTemplateWork(templateCopyWork);
      chargeClosedFunctorTemplateWork(templateCopyWork);
      chargeClosedFunctorTemplateWork(templateCopyWork);
      std::vector<ClosedFunctorFormalKey> stateTemplates = key.functorTemplates;
      const auto reserved = vm_.reserveFrameDescriptor();
      if (!reserved) throw std::invalid_argument(reserved.error().message);
      if (!key.functorTemplates.empty()) {
        if (nextFunctorVariantOrdinal_ ==
            std::numeric_limits<std::size_t>::max())
          throw std::invalid_argument(
              "compiled VQL functor variant ordinal exhausted");
        functorVariantOrdinal = nextFunctorVariantOrdinal_++;
      }
      FrameDefinitionState newState{
          .descriptor            = *reserved,
          .functorTemplates      = std::move(stateTemplates),
          .functorVariantOrdinal = functorVariantOrdinal,
          .queued                = true};
      state = frameDefinitions_.emplace(key, std::move(newState)).first;
      pendingFrameDefinitions_.push_back(key);
    } else {
      functorVariantOrdinal = state->second.functorVariantOrdinal;
      if (!state->second.queued && !state->second.compiled) {
        chargeClosedFunctorTemplateWork(
            closedFunctorTemplateWork(key.functorTemplates));
        state->second.queued = true;
        pendingFrameDefinitions_.push_back(key);
      }
    }
    if (!functorVariantOrdinal)
      functorVariantOrdinal = state->second.functorVariantOrdinal;
    frameDescriptor = state->second.descriptor;
  }
  const CellRef descriptor = frameDescriptor.value();
  const auto functorSuffix =
      functorVariantOrdinal
          ? "<functor=" + std::to_string(*functorVariantOrdinal) + ">"
          : std::string{};

  std::vector<std::vector<CellRef>> actuals;
  if (injectContext) actuals.push_back({*context});
  std::size_t actualFormalIndex = static_cast<std::size_t>(injectContext);
  for (const auto &argument : arguments) {
    const auto functor =
        std::ranges::lower_bound(key.functorTemplates, actualFormalIndex, {},
                                 &ClosedFunctorFormalKey::formalIndex);
    if (functor != key.functorTemplates.end() &&
        functor->formalIndex == actualFormalIndex)
      actuals.emplace_back();
    else
      actuals.push_back(compileArgument(argument, context));
    ++actualFormalIndex;
  }

  std::vector<std::vector<CellRef>> typedRestGroups;
  if (hasVariadicRest) {
    if (typedRest) {
      for (std::size_t index = fixedParameterCount; index < actuals.size();
           ++index)
        typedRestGroups.push_back(std::move(actuals[index]));
      actuals.resize(fixedParameterCount);
    } else {
      std::vector<CellRef> restCells;
      for (std::size_t index = fixedParameterCount; index < actuals.size();
           ++index) {
        const auto &actual = actuals[index];
        restCells.insert(restCells.end(), actual.begin(), actual.end());
      }
      actuals.resize(definition.params.size());
      actuals.back() = std::move(restCells);
    }
  }

  std::vector<std::string> requiredNames;
  if (const auto required =
          frameRequiredDefaultCaptureBindings_.find(definition.name);
      required != frameRequiredDefaultCaptureBindings_.end())
    requiredNames.insert(requiredNames.end(), required->second.begin(),
                         required->second.end());
  if (key.restGroupCount && *key.restGroupCount > 0) {
    if (const auto conditional =
            frameConditionalRequiredDefaultCaptureBindings_.find(
                definition.name);
        conditional != frameConditionalRequiredDefaultCaptureBindings_.end())
      requiredNames.insert(requiredNames.end(), conditional->second.begin(),
                           conditional->second.end());
  }
  std::ranges::sort(requiredNames);
  requiredNames.erase(std::unique(requiredNames.begin(), requiredNames.end()),
                      requiredNames.end());
  for (const auto &name : requiredNames) {
    if (functorBindings_.contains(name))
      throw std::invalid_argument("contract default $" + name +
                                  " cannot capture a path functor");
    if (const auto stream = streamBindings_.find(name);
        stream != streamBindings_.end()) {
      if (stream->second.size() > 1)
        throw std::invalid_argument("contract default $" + name +
                                    " must resolve to at most one cell");
      if (!stream->second.empty() && core_.hasPipeline(stream->second.front()))
        throw std::invalid_argument(
            "contract default aliases cannot depend on parameter pipelines");
      continue;
    }
    if (const auto scalar = varBindings_.find(name);
        scalar != varBindings_.end() && scalar->second) {
      if (core_.hasPipeline(*scalar->second))
        throw std::invalid_argument(
            "contract default aliases cannot depend on parameter pipelines");
      continue;
    }
    throw std::invalid_argument("contract default $" + name +
                                " has no prior lexical binding");
  }

  std::vector<std::vector<CellRef>> capturedValues;
  if (const auto captures = frameFreeCaptureNames_.find(definition.name);
      captures != frameFreeCaptureNames_.end()) {
    capturedValues.reserve(captures->second.size());
    for (const auto &name : captures->second)
      capturedValues.push_back(callerCaptureCells(name));
  }

  const CellRef contextStream =
      context ? normalizeFrameStream(std::span{&*context, std::size_t{1}})
              : normalizeFrameStream(std::span<const CellRef>{});
  std::vector<CellRef> captureStreams;
  captureStreams.reserve(capturedValues.size());
  for (const auto &capture : capturedValues)
    captureStreams.push_back(normalizeFrameStream(capture));
  std::vector<CellRef> argumentStreams;
  std::vector<std::optional<CellRef>> presenceFlags;
  argumentStreams.reserve(inputParameterCount);
  presenceFlags.reserve(inputParameterCount);
  for (std::size_t index = 0; index < inputParameterCount; ++index) {
    const std::vector<CellRef> empty;
    const bool present   = index < suppliedActualCount;
    const bool hasActual = index < actuals.size();
    const auto &actual   = hasActual ? actuals[index] : empty;
    argumentStreams.push_back(normalizeFrameStream(actual));
    presenceFlags.push_back(
        definition.params[index].defaultValue
            ? std::optional{emitConstant(CellValue(present))}
            : std::nullopt);
  }
  std::vector<CellRef> typedRestStreams;
  typedRestStreams.reserve(typedRestGroups.size());
  for (const auto &group : typedRestGroups)
    typedRestStreams.push_back(normalizeFrameStream(group));

  const CellRef resultStream = emitStreamClear();
  const CellRef invoke =
      emitOp(OpcodeKind::InvokeFrame,
             "#INVOKE_FRAME " + definition.name +
                 frameVariantSuffix(key.restGroupCount) + functorSuffix);
  emitInput(invoke, descriptor);
  emitInput(invoke, contextStream);
  for (const auto capture : captureStreams) emitInput(invoke, capture);

  for (std::size_t index = 0; index < inputParameterCount; ++index) {
    emitInput(invoke, argumentStreams[index]);
    if (presenceFlags[index]) emitInput(invoke, *presenceFlags[index]);
  }
  for (const CellRef groupStream : typedRestStreams)
    emitInput(invoke, groupStream);
  emitOutput(invoke, resultStream);
  dynamicStreams_.insert(resultStream);
  lastResultCells_ = {resultStream};
  return lastResultCells_;
}

void VQLCompiler::compilePendingFrameDefinitions() {
  for (std::size_t index = 0; index < pendingFrameDefinitions_.size();
       ++index) {
    chargeClosedFunctorTemplateWork(closedFunctorTemplateWork(
        pendingFrameDefinitions_[index].functorTemplates));
    const auto key = pendingFrameDefinitions_[index];
    bool compiled  = false;
    {
      chargeClosedFunctorTemplateWork(
          closedFunctorTemplateWork(key.functorTemplates));
      const auto state = frameDefinitions_.find(key);
      compiled = state == frameDefinitions_.end() || state->second.compiled;
    }
    if (compiled) continue;
    std::optional<DefDeclaration> definition;
    {
      const auto found = definitions_.find(key.name);
      if (found != definitions_.end()) definition = found->second;
    }
    if (!definition)
      throw std::invalid_argument("missing frame definition: " + key.name);
    compileFrameDefinition(*definition, key);
  }
}

void VQLCompiler::compileFrameDefinition(const DefDeclaration &definition,
                                         const FrameDefinitionKey &key) {
  IsolatedCompileState isolated(*this);

  std::optional<CellRef> reservedDescriptor;
  std::optional<std::size_t> functorVariantOrdinal;
  std::vector<ClosedFunctorFormalKey> functorTemplates;
  {
    chargeClosedFunctorTemplateWork(
        closedFunctorTemplateWork(key.functorTemplates));
    const auto state = frameDefinitions_.find(key);
    if (state == frameDefinitions_.end())
      throw std::invalid_argument("missing reserved frame: " + definition.name);
    reservedDescriptor    = state->second.descriptor;
    functorVariantOrdinal = state->second.functorVariantOrdinal;
    chargeClosedFunctorTemplateWork(
        closedFunctorTemplateWork(state->second.functorTemplates));
    functorTemplates = state->second.functorTemplates;
  }
  const CellRef descriptor      = reservedDescriptor.value();
  compilingFrameDefinition_     = true;
  compilingFrameDefinitionName_ = definition.name;
  activeCalls_.push_back(definition.name);

  const bool typedRest             = key.restGroupCount.has_value();
  const std::size_t restGroupCount = key.restGroupCount.value_or(0);
  const std::size_t fixedParameterCount =
      definition.params.size() -
      static_cast<std::size_t>(!definition.params.empty() &&
                               definition.params.back().isVariadic);
  const std::size_t inputParameterCount =
      typedRest ? fixedParameterCount : definition.params.size();
  const std::size_t captureCount = [&] {
    const auto captures = frameFreeCaptureNames_.find(definition.name);
    return captures == frameFreeCaptureNames_.end() ? std::size_t{0}
                                                    : captures->second.size();
  }();
  const std::size_t defaultCount =
      static_cast<std::size_t>(std::ranges::count_if(
          definition.params, [](const LambdaParam &parameter) {
            return parameter.defaultValue != nullptr;
          }));
  const auto limits             = vm_.frameLimits();
  std::size_t localCount        = 2;
  std::size_t argumentPortCount = 1;
  if (!addFrameCount(localCount, captureCount, limits.maxLocalsPerFrame) ||
      !addFrameCount(localCount, definition.params.size(),
                     limits.maxLocalsPerFrame) ||
      !addFrameCount(localCount, defaultCount, limits.maxLocalsPerFrame) ||
      !addFrameCount(localCount, typedRest ? restGroupCount : 0,
                     limits.maxLocalsPerFrame) ||
      !addFrameCount(argumentPortCount, captureCount,
                     limits.maxDescriptorRecords) ||
      !addFrameCount(argumentPortCount, inputParameterCount,
                     limits.maxDescriptorRecords) ||
      !addFrameCount(argumentPortCount, defaultCount,
                     limits.maxDescriptorRecords) ||
      !addFrameCount(argumentPortCount, typedRest ? restGroupCount : 0,
                     limits.maxDescriptorRecords) ||
      !frameDescriptorCountsFit(limits, localCount, argumentPortCount, 1))
    throw std::invalid_argument(
        "native frame exceeds configured local or descriptor-port limits");

  FrameDescriptorSpec spec;
  const CellRef contextSlot = core_.arena().makeCell();
  const CellRef resultSlot  = core_.arena().makeCell();
  std::vector<CellRef> captureSlots;
  std::vector<CellRef> parameterSlots;
  std::vector<CellRef> restGroupSlots;
  std::vector<std::optional<CellRef>> presenceSlots;
  parameterSlots.reserve(definition.params.size());
  presenceSlots.reserve(definition.params.size());
  restGroupSlots.reserve(restGroupCount);
  spec.locals = {contextSlot, resultSlot};
  spec.argumentPorts.emplace_back(contextSlot, FramePortKind::Stream);
  spec.resultPorts.emplace_back(resultSlot, FramePortKind::Stream);
  if (const auto captures = frameFreeCaptureNames_.find(definition.name);
      captures != frameFreeCaptureNames_.end()) {
    captureSlots.reserve(captures->second.size());
    for (std::size_t index = 0; index < captures->second.size(); ++index) {
      const CellRef slot = core_.arena().makeCell();
      captureSlots.push_back(slot);
      spec.locals.push_back(slot);
      spec.argumentPorts.emplace_back(slot, FramePortKind::Stream);
    }
  }
  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    const auto &parameter = definition.params[index];
    if (!std::holds_alternative<std::string>(parameter.target) &&
        !std::holds_alternative<bool>(parameter.target) &&
        !std::holds_alternative<DestructurePattern>(parameter.target))
      throw std::invalid_argument(
          "frame definition has an unsupported parameter target");
    const CellRef slot = core_.arena().makeCell();
    parameterSlots.push_back(slot);
    presenceSlots.push_back(std::nullopt);
    spec.locals.push_back(slot);
    if (!(typedRest && index == definition.params.size() - 1))
      spec.argumentPorts.emplace_back(slot, FramePortKind::Stream);
    if (parameter.defaultValue) {
      const CellRef present = core_.arena().makeCell();
      presenceSlots.back()  = present;
      spec.locals.push_back(present);
      spec.argumentPorts.emplace_back(present, FramePortKind::Cell);
    }
  }
  for (std::size_t index = 0; index < restGroupCount; ++index) {
    const CellRef slot = core_.arena().makeCell();
    restGroupSlots.push_back(slot);
    spec.locals.push_back(slot);
    spec.argumentPorts.emplace_back(slot, FramePortKind::Stream);
  }

  streamBindings_["."] = {contextSlot};
  varBindings_["."]    = contextSlot;
  ambientContext_      = contextSlot;
  dynamicStreams_.insert(contextSlot);
  if (const auto captures = frameFreeCaptureNames_.find(definition.name);
      captures != frameFreeCaptureNames_.end()) {
    for (std::size_t index = 0; index < captures->second.size(); ++index) {
      const auto &name      = captures->second[index];
      const CellRef slot    = captureSlots[index];
      streamBindings_[name] = {slot};
      varBindings_[name]    = slot;
      dynamicStreams_.insert(slot);
    }
  }
  for (const auto slot : parameterSlots) {
    dynamicStreams_.insert(slot);
  }
  for (const auto slot : restGroupSlots) dynamicStreams_.insert(slot);
  dynamicStreams_.insert(resultSlot);

  const std::string suffix =
      frameVariantSuffix(key.restGroupCount) +
      (functorVariantOrdinal
           ? "<functor=" + std::to_string(*functorVariantOrdinal) + ">"
           : std::string{});
  const CellRef returnOpcode = emitNativeFrameReturn(
      descriptor, "#RETURN_FRAME " + definition.name + suffix);
  const auto emitMatch = [&](const TypeExpr &type,
                             const std::vector<CellRef> &subjects,
                             const PreparedContractValidator &prepared,
                             const std::string &diagnostic) {
    const CellRef matched = emitContractMatch(type, subjects, true);
    const CellRef branch =
        emitOp(OpcodeKind::BranchFalseCell, "#FRAME_PARAM_FILTER");
    emitInput(branch, matched);
    emitInput(branch, returnOpcode);
    pushContractGuard(prepared, diagnostic, "#FRAME_PARAM_GUARD");
  };

  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    const auto &parameter      = definition.params[index];
    const CellRef slot         = parameterSlots[index];
    const auto functorTemplate = std::ranges::lower_bound(
        functorTemplates, index, {}, &ClosedFunctorFormalKey::formalIndex);
    if (functorTemplate != functorTemplates.end() &&
        functorTemplate->formalIndex == index) {
      const auto *name    = std::get_if<std::string>(&parameter.target);
      const auto required = frameFunctorFormalIndices_.find(definition.name);
      const bool requiredFunctor =
          required != frameFunctorFormalIndices_.end() &&
          std::ranges::binary_search(required->second, index);
      if (!name || parameter.type || parameter.isVariadic ||
          (parameter.defaultValue &&
           (!requiredFunctor ||
            !closedTraversalFunctorLiteralSteps(*parameter.defaultValue))))
        throw std::invalid_argument(
            "closed functor frame requires an untyped named formal");
      chargeClosedFunctorTemplateWork(closedFunctorTemplateWork(
          std::span{&*functorTemplate, std::size_t{1}}));
      std::vector<PathStep> steps;
      steps.reserve(functorTemplate->steps.size());
      for (const auto &templateStep : functorTemplate->steps) {
        SignedDimensionStep dimension{.dimName   = templateStep.dimName,
                                      .direction = templateStep.direction,
                                      .isBidir   = templateStep.isBidir};
        const auto rangeClamp =
            templateStep.rangeClamp
                ? std::optional{RangeClamp{.start =
                                               templateStep.rangeClamp->start,
                                           .end = templateStep.rangeClamp->end}}
                : std::nullopt;
        PathStep step{.selector    = std::move(dimension),
                      .derefMaster = templateStep.derefMaster,
                      .rangeClamp  = rangeClamp};
        step.predicates.reserve(templateStep.predicates.size());
        for (const auto &predicate : templateStep.predicates) {
          BooleanExpr predicateExpr;
          predicateExpr.terms.reserve(predicate.terms.size());
          for (const auto &term : predicate.terms) {
            BooleanTerm termExpr;
            termExpr.factors.reserve(term.factors.size());
            for (const auto &factor : term.factors) {
              auto literal = std::visit(
                  [](const auto &value) -> ScalarLiteral {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::uint64_t>)
                      return ScalarLiteral{.value =
                                               std::bit_cast<double>(value)};
                    else
                      return ScalarLiteral{.value = value};
                  },
                  factor.literal);
              ValueExpr literalExpr{.kind = std::move(literal)};
              PathExpression contextPath;
              contextPath.anchor.kind = AnchorKind::Context;
              ValueExpr contextExpr{.kind = std::make_shared<PathExpression>(
                                        std::move(contextPath))};
              ComparisonExpr comparison;
              comparison.op = factor.op;
              comparison.left =
                  factor.literalOnLeft
                      ? std::make_shared<ValueExpr>(std::move(literalExpr))
                      : std::make_shared<ValueExpr>(std::move(contextExpr));
              comparison.right =
                  factor.literalOnLeft
                      ? std::make_shared<ValueExpr>(std::move(contextExpr))
                      : std::make_shared<ValueExpr>(std::move(literalExpr));
              termExpr.factors.push_back(BooleanFactor{
                  .negated = factor.negated, .test = std::move(comparison)});
            }
            predicateExpr.terms.push_back(std::move(termExpr));
          }
          step.predicates.push_back(std::move(predicateExpr));
        }
        steps.push_back(std::move(step));
      }
      functorBindings_[*name] = std::move(steps);
      streamBindings_.erase(*name);
      varBindings_.erase(*name);
      continue;
    }
    if (typedRest && index == definition.params.size() - 1) {
      const auto *name = std::get_if<std::string>(&parameter.target);
      if (!name || !parameter.type)
        throw std::invalid_argument(
            "typed variadic frame requires a named typed rest formal");
      const std::string diagnostic = definition.name + " parameter " +
                                     std::to_string(index + 1) + " ($" + *name +
                                     ")";
      if (diagnostic.size() > vm_.guardLimits().maxDiagnosticBytes)
        throw std::invalid_argument(
            "parameter guard diagnostic exceeds the configured text budget");

      const CellRef clearRest =
          emitOp(OpcodeKind::StreamClear, "#FRAME_TYPED_REST_CLEAR");
      emitOutput(clearRest, slot);
      for (const CellRef group : restGroupSlots) {
        const auto prepared = prepareContractValidator(
            *parameter.type, std::span{&group, std::size_t{1}});
        const CellRef matched = emitContractMatch(
            *parameter.type, std::span{&group, std::size_t{1}}, false);
        const CellRef branch =
            emitOp(OpcodeKind::BranchFalseCell, "#FRAME_REST_PARAM_FILTER");
        emitInput(branch, matched);
        emitInput(branch, returnOpcode);
        pushContractGuard(prepared, diagnostic, "#FRAME_REST_PARAM_GUARD");
        emitAppendStream(slot, std::span{&group, std::size_t{1}});
      }
      streamBindings_[*name] = {slot};
      varBindings_[*name]    = slot;
      continue;
    }
    if (parameter.defaultValue && !(functorTemplate != functorTemplates.end() &&
                                    functorTemplate->formalIndex == index)) {
      const CellRef notPresent = core_.arena().makeCell();
      const CellRef negate     = emitOp(OpcodeKind::Not, "#FRAME_DEFAULT_TEST");
      emitInput(negate, *presenceSlots[index]);
      emitOutput(negate, notPresent);
      const CellRef skip = mintOpcode(OpcodeKind::Nop, "#FRAME_DEFAULT_SKIP");
      const CellRef branch =
          emitOp(OpcodeKind::BranchFalseCell, "#FRAME_DEFAULT_MISSING");
      emitInput(branch, notPresent);
      emitInput(branch, skip);
      const auto defaultCells =
          compileArgument(*parameter.defaultValue, ambientContext_);
      const CellRef clear =
          emitOp(OpcodeKind::StreamClear, "#FRAME_DEFAULT_CLEAR");
      emitOutput(clear, slot);
      if (!defaultCells.empty()) emitAppendStream(slot, defaultCells);
      appendOpcode(skip);
    }
    std::string diagnostic = definition.name + " parameter ";
    diagnostic += std::to_string(index + 1);
    if (const auto *name = std::get_if<std::string>(&parameter.target))
      diagnostic += " ($" + *name + ")";
    else if (std::holds_alternative<DestructurePattern>(parameter.target))
      diagnostic += "destructure";
    else
      diagnostic += "context";
    if ((parameter.type ||
         std::holds_alternative<DestructurePattern>(parameter.target)) &&
        diagnostic.size() > vm_.guardLimits().maxDiagnosticBytes)
      throw std::invalid_argument(
          "parameter guard diagnostic exceeds the configured text budget");
    std::optional<PreparedContractValidator> typePrepared;
    std::optional<PreparedContractValidator> destructurePrepared;
    std::optional<TypeExpr> destructureType;
    if (parameter.type)
      typePrepared = prepareContractValidator(*parameter.type,
                                              std::span{&slot, std::size_t{1}});
    if (parameter.type)
      emitMatch(*parameter.type, {slot}, *typePrepared, diagnostic);
    if (const auto *pattern =
            std::get_if<DestructurePattern>(&parameter.target)) {
      destructureType.emplace(TypeExpr{
          .unionType =
              UnionType{.members = {PrimaryType{
                            .kind = ShapeType{.pattern = *pattern}}}}});
      destructurePrepared = prepareContractValidator(
          *destructureType, std::span{&slot, std::size_t{1}});
    }
    if (destructureType && destructurePrepared) {
      emitMatch(*destructureType, {slot}, *destructurePrepared, diagnostic);
    } else if (const auto *name = std::get_if<std::string>(&parameter.target)) {
      functorBindings_.erase(*name);
      streamBindings_[*name] = {slot};
      varBindings_[*name]    = slot;
    } else {
      streamBindings_["."] = {slot};
      varBindings_["."]    = slot;
      ambientContext_      = slot;
    }
  }

  const CellRef clearResult =
      emitOp(OpcodeKind::StreamClear, "#FRAME_RESULT_CLEAR");
  emitOutput(clearResult, resultSlot);
  std::vector<CellRef> bodyResults;
  if (const auto *value =
          std::get_if<std::shared_ptr<ValueExpr>>(&definition.body)) {
    bodyResults = compileArgument(**value, ambientContext_);
  } else {
    const auto &body = *std::get<std::shared_ptr<ScriptBody>>(definition.body);
    for (const auto &statement : body.statements) {
      lastResultCells_.clear();
      if (const auto *block = std::get_if<ExecutionBlock>(&statement))
        compileExecutionBlock(*block);
      else if (const auto *path = std::get_if<PathExpression>(&statement))
        compilePathExpression(*path, ambientContext_.value_or(core_.home()));
      else if (const auto *weave = std::get_if<WeaveStatement>(&statement)) {
        compileExecutionBlock(
            ExecutionBlock{.action = ActionClause{.clause = weave->effect}});
      } else
        throw std::invalid_argument(
            "frame definition body contains an unsupported statement");
      bodyResults = lastResultCells_;
    }
  }
  if (!bodyResults.empty()) emitAppendStream(resultSlot, bodyResults);

  if (definition.returnType) {
    emitRequiredContractMatch(*definition.returnType,
                              std::span{&resultSlot, std::size_t{1}},
                              definition.name);
  }

  if (!entryOp_)
    throw std::invalid_argument("frame definition emitted no entry opcode");
  appendOpcode(returnOpcode);
  chargeClosedFunctorTemplateWork(
      closedFunctorTemplateWork(key.functorTemplates));
  const std::optional<FrameDefinitionKey> completedKey{key};
  finalizeNativeFrame(descriptor, spec, completedKey);
}

CellRef VQLCompiler::compileClosureLambda(const PathLambda &lambda) {
  const auto analyzed = analyzeClosureLambdaSyntax(lambda);
  if (!analyzed) throw std::invalid_argument(analyzed.error().message);
  if (!*analyzed)
    throw std::invalid_argument(
        "VQL lambda value is outside the supported closure syntax");
  const auto &syntax = **analyzed;

  const auto closureLimits = vm_.closureLimits();
  if (closureLimits.maxCaptureStreams == 0 ||
      syntax.freeCaptureNames.size() >= closureLimits.maxCaptureStreams ||
      closureLimits.maxAppliedStreams == 0)
    throw std::invalid_argument(
        "VQL lambda exceeds configured closure stream limits");

  if (frameDefinitions_.size() >= maxNamedFrameVariants_)
    throw std::invalid_argument("compiled VQL frame variant budget exhausted");
  const auto maxSize = std::numeric_limits<std::size_t>::max();
  if (definitions_.size() > maxSize - frameDefinitions_.size() ||
      definitions_.size() + frameDefinitions_.size() == maxSize)
    throw std::invalid_argument(
        "compiled VQL lambda name search bound is not representable");
  std::size_t nameAttempts = definitions_.size() + frameDefinitions_.size() + 1;
  std::optional<std::string> syntheticName;
  while (nameAttempts > 0) {
    if (nextClosureLambdaId_ == maxSize)
      throw std::invalid_argument("compiled VQL lambda identifier exhausted");
    std::string candidate =
        "#vql-closure-lambda-" + std::to_string(nextClosureLambdaId_++);
    --nameAttempts;
    const bool frameNameInUse =
        std::ranges::any_of(frameDefinitions_, [&candidate](const auto &entry) {
          return entry.first.name == candidate;
        });
    if (!definitions_.contains(candidate) && !frameNameInUse) {
      syntheticName = std::move(candidate);
      break;
    }
  }
  if (!syntheticName)
    throw std::invalid_argument(
        "compiled VQL lambda name search exhausted its bounded candidates");
  const std::string &name = *syntheticName;
  for (const auto &capture : syntax.freeCaptureNames)
    if (functorBindings_.contains(capture))
      throw std::invalid_argument(
          "VQL lambda value cannot capture path functor $" + capture);

  DefDeclaration definition{
      .name   = name,
      .params = {LambdaParam{.target = syntax.parameterName}},
      .body   = lambda.body,
  };
  if (!frameFreeCaptureNames_.emplace(name, syntax.freeCaptureNames).second)
    throw std::invalid_argument("compiled VQL lambda name is already in use");

  const FrameDefinitionKey key{.name = name};
  const auto reserved = vm_.reserveFrameDescriptor();
  if (!reserved) throw std::invalid_argument(reserved.error().message);
  FrameDefinitionState frameState{.descriptor = *reserved, .queued = true};
  if (!frameDefinitions_.emplace(key, std::move(frameState)).second)
    throw std::invalid_argument(
        "compiled VQL lambda frame is already reserved");
  compileFrameDefinition(definition, key);

  std::vector<CellRef> captureSources;
  captureSources.reserve(syntax.freeCaptureNames.size() + 1);
  captureSources.push_back(normalizeFrameStream(std::span<const CellRef>{}));
  for (const auto &capture : syntax.freeCaptureNames) {
    std::vector<CellRef> cells;
    if (const auto found = streamBindings_.find(capture);
        found != streamBindings_.end()) {
      cells = found->second;
    } else if (const auto found = varBindings_.find(capture);
               found != varBindings_.end() && found->second) {
      cells.push_back(*found->second);
    }
    captureSources.push_back(normalizeFrameStream(cells));
  }

  std::vector<zigzag::vortex::ClosureFrameInput> frameInputs;
  frameInputs.reserve(syntax.freeCaptureNames.size() + 2);
  frameInputs.push_back(
      {.source = zigzag::vortex::ClosureInputSource::Capture, .index = 0});
  for (std::size_t index = 0; index < syntax.freeCaptureNames.size(); ++index)
    frameInputs.push_back(
        {.source = zigzag::vortex::ClosureInputSource::Capture,
         .index  = index + 1});
  frameInputs.push_back(
      {.source = zigzag::vortex::ClosureInputSource::Applied, .index = 0});

  const auto closureDescriptor =
      vm_.makeClosureDescriptor({.frameDescriptor    = *reserved,
                                 .captureStreamCount = captureSources.size(),
                                 .appliedStreamCount = 1,
                                 .frameInputs        = std::move(frameInputs)});
  if (!closureDescriptor)
    throw std::invalid_argument(closureDescriptor.error().message);

  const CellRef create = emitOp(OpcodeKind::CreateClosure, "#CREATE_CLOSURE");
  emitInput(create, *closureDescriptor);
  for (const CellRef source : captureSources) emitInput(create, source);
  const CellRef handle = core_.arena().makeCell();
  emitOutput(create, handle);
  return handle;
}
} // namespace xanadu::vql
