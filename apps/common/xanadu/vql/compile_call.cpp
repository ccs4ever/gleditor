#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>

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

template <class Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

} // namespace

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
                         std::optional<CellRef> context, bool ufcs) {
  bool hasStaticFunctor = false;
  std::size_t functorIndex =
      ufcs && context && definition.params.size() > arguments.size() ? 1 : 0;
  for (const auto &argument : arguments) {
    if (functorIndex < definition.params.size()) {
      const auto &parameter = definition.params[functorIndex];
      if (!parameter.type && !parameter.isVariadic &&
          std::holds_alternative<std::string>(parameter.target) &&
          functorArgument(argument)) {
        hasStaticFunctor = true;
        break;
      }
    }
    ++functorIndex;
  }
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
      !hasStaticFunctor && !hasInlineCapture) {
    if (compilingFrameDefinition_ && activeCalls_.size() > 1 &&
        activeCalls_.back() != compilingFrameDefinitionName_ &&
        std::ranges::contains(activeCalls_, definition.name))
      throw std::invalid_argument(
          "recursive cycle crosses an inline-only definition: " +
          definition.name);
    return compileReusableFrameCall(definition, arguments, context, ufcs);
  }
  if (std::ranges::contains(activeCalls_, definition.name))
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
  activeCalls_.push_back(definition.name);
  callActivated = true;

  const auto clear =
      emitOp(OpcodeKind::ClearCellRefs, "#CALL_CLEAR " + definition.name);
  const auto end =
      vm_.mintOpcode(OpcodeKind::Nop, "#CALL_END " + definition.name);
  const auto bind = [&](const std::string &name, std::vector<CellRef> cells) {
    functorBindings_.erase(name);
    streamBindings_[name] = cells;
    varBindings_[name] =
        cells.empty() ? std::nullopt : std::optional{cells.front()};
  };
  const auto check = [&](const TypeExpr &type,
                         const std::vector<CellRef> &subjects,
                         const bool bindCaptures) {
    const auto defaultSlots = emitContractDefaultSlots(type);
    const auto descriptor   = emitConstant(encodeTypeDescriptor(type));
    const auto match =
        emitOp(OpcodeKind::MatchContract, "#MATCH " + definition.name);
    emitInput(match, descriptor);
    emitInputConstant(match, static_cast<std::int64_t>(subjects.size()));
    for (const auto subject : subjects) emitInput(match, subject);
    for (const auto slot : defaultSlots) emitInput(match, slot);
    const auto matched = core_.arena().makeCell();
    emitOutput(match, matched);
    for (const auto &name : ContractMatcher::contractCaptureNames(type)) {
      const auto capture = core_.arena().makeCell();
      emitOutput(match, capture);
      if (bindCaptures) {
        bind(name, {capture});
        if (name == ".") {
          ambientContext_ = emitContextFallback(capture, ambientContext_);
        }
      }
    }
    const auto branch = emitOp(OpcodeKind::BranchFalseCell, "#PARAM_FILTER");
    emitInput(branch, matched);
    emitInput(branch, end);
  };

  std::size_t argumentIndex = 0;
  for (const auto &parameter : definition.params) {
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
        if (parameter.type)
          check(*parameter.type, values[argumentIndex], false);
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
      if (parameter.type) check(*parameter.type, cells, true);
    }
    if (const auto *pattern =
            std::get_if<DestructurePattern>(&parameter.target)) {
      TypeExpr shape{
          .unionType = UnionType{.members = {PrimaryType{
                                     .kind = ShapeType{.pattern = *pattern}}}}};
      check(shape, cells, true);
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
      else
        throw std::invalid_argument(
            "compiled definition body requires a supported block");
      bodyResults = lastResultCells_;
    }
  }
  if (definition.returnType) {
    const auto defaultSlots = emitContractDefaultSlots(*definition.returnType);
    const auto descriptor =
        emitConstant(encodeTypeDescriptor(*definition.returnType));
    const auto require =
        emitOp(OpcodeKind::RequireContract, "#RETURN_TYPE " + definition.name);
    emitInput(require, descriptor);
    emitInputConstant(require, definition.name);
    emitInputConstant(require, static_cast<std::int64_t>(bodyResults.size()));
    for (const auto result : bodyResults) emitInput(require, result);
    for (const auto slot : defaultSlots) emitInput(require, slot);
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
  zigzag::expectWritten(
      core_.arena().link(*currentOp_, core_.dims().spin, false, end));
  currentOp_ = end;
  allOps_.push_back(end);
  lastResultCells_ = results;
  return results;
}

CellRef
VQLCompiler::normalizeFrameStream(const std::span<const CellRef> cells) {
  const CellRef stream = emitStreamClear();
  emitAppendStream(stream, cells);
  return stream;
}

std::vector<CellRef> VQLCompiler::compileReusableFrameCall(
    const DefDeclaration &definition, const std::vector<ValueExpr> &arguments,
    const std::optional<CellRef> context, const bool ufcs) {
  const auto callerCaptureCells = [this](const std::string &name) {
    if (const auto found = streamBindings_.find(name);
        found != streamBindings_.end())
      return found->second;
    if (const auto found = varBindings_.find(name);
        found != varBindings_.end() && found->second)
      return std::vector<CellRef>{*found->second};
    return std::vector<CellRef>{};
  };
  const CellRef descriptor = [&] {
    auto state = frameDefinitions_.find(definition.name);
    if (state == frameDefinitions_.end()) {
      const auto reserved = vm_.reserveFrameDescriptor();
      if (!reserved) throw std::invalid_argument(reserved.error().message);
      state = frameDefinitions_
                  .emplace(definition.name,
                           FrameDefinitionState{.descriptor = *reserved})
                  .first;
    }
    const CellRef frameDescriptor = state->second.descriptor;
    if (!state->second.queued && !state->second.compiled) {
      state->second.queued = true;
      pendingFrameDefinitions_.push_back(definition.name);
    }
    return frameDescriptor;
  }();

  std::vector<std::vector<CellRef>> actuals;
  if (ufcs && context && definition.params.size() > arguments.size())
    actuals.push_back({*context});
  for (const auto &argument : arguments)
    actuals.push_back(compileArgument(argument, context));

  if (const auto required =
          frameRequiredDefaultCaptureBindings_.find(definition.name);
      required != frameRequiredDefaultCaptureBindings_.end()) {
    for (const auto &name : required->second) {
      if (functorBindings_.contains(name))
        throw std::invalid_argument("contract default $" + name +
                                    " cannot capture a path functor");
      if (const auto stream = streamBindings_.find(name);
          stream != streamBindings_.end()) {
        if (stream->second.size() > 1)
          throw std::invalid_argument("contract default $" + name +
                                      " must resolve to at most one cell");
        if (!stream->second.empty() &&
            core_.hasPipeline(stream->second.front()))
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
  argumentStreams.reserve(definition.params.size());
  presenceFlags.reserve(definition.params.size());
  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    const std::vector<CellRef> empty;
    const bool present = index < actuals.size();
    const auto &actual = present ? actuals[index] : empty;
    argumentStreams.push_back(normalizeFrameStream(actual));
    presenceFlags.push_back(
        definition.params[index].defaultValue
            ? std::optional{emitConstant(CellValue(present))}
            : std::nullopt);
  }
  const CellRef resultStream = emitStreamClear();
  const CellRef invoke =
      emitOp(OpcodeKind::InvokeFrame, "#INVOKE_FRAME " + definition.name);
  emitInput(invoke, descriptor);
  emitInput(invoke, contextStream);
  for (const auto capture : captureStreams) emitInput(invoke, capture);

  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    emitInput(invoke, argumentStreams[index]);
    if (presenceFlags[index]) emitInput(invoke, *presenceFlags[index]);
  }
  emitOutput(invoke, resultStream);
  dynamicStreams_.insert(resultStream);
  lastResultCells_ = {resultStream};
  return lastResultCells_;
}

void VQLCompiler::compilePendingFrameDefinitions() {
  for (std::size_t index = 0; index < pendingFrameDefinitions_.size();
       ++index) {
    const auto name  = pendingFrameDefinitions_[index];
    const auto state = frameDefinitions_.find(name);
    if (state == frameDefinitions_.end() || state->second.compiled) continue;
    const auto definition = definitions_.find(name);
    if (definition == definitions_.end())
      throw std::invalid_argument("missing frame definition: " + name);
    compileFrameDefinition(definition->second);
  }
}

void VQLCompiler::compileFrameDefinition(const DefDeclaration &definition) {
  struct SavedCompilerState {
    std::optional<CellRef> entry;
    std::optional<CellRef> current;
    std::unordered_map<std::string, std::optional<CellRef>> variables;
    std::unordered_map<std::string, std::vector<CellRef>> streams;
    std::unordered_map<std::string, std::vector<PathStep>> functors;
    std::optional<CellRef> context;
    std::vector<CellRef> results;
    std::unordered_set<CellRef> dynamicStreams;
    std::unordered_set<CellRef> frameMutableSlots;
    std::vector<std::string> activeCalls;
    bool compilingFrameDefinition;
    std::string compilingFrameDefinitionName;
  } saved{entryOp_,
          currentOp_,
          varBindings_,
          streamBindings_,
          functorBindings_,
          ambientContext_,
          lastResultCells_,
          dynamicStreams_,
          frameMutableSlots_,
          activeCalls_,
          compilingFrameDefinition_,
          compilingFrameDefinitionName_};
  const auto restore = RestoreScope{[&] {
    entryOp_                  = saved.entry;
    currentOp_                = saved.current;
    varBindings_              = std::move(saved.variables);
    streamBindings_           = std::move(saved.streams);
    functorBindings_          = std::move(saved.functors);
    ambientContext_           = saved.context;
    lastResultCells_          = std::move(saved.results);
    dynamicStreams_           = std::move(saved.dynamicStreams);
    frameMutableSlots_        = std::move(saved.frameMutableSlots);
    activeCalls_              = std::move(saved.activeCalls);
    compilingFrameDefinition_ = saved.compilingFrameDefinition;
    compilingFrameDefinitionName_ =
        std::move(saved.compilingFrameDefinitionName);
  }};

  const CellRef descriptor = frameDefinitions_.at(definition.name).descriptor;
  entryOp_.reset();
  currentOp_.reset();
  lastResultCells_.clear();
  varBindings_.clear();
  streamBindings_.clear();
  functorBindings_.clear();
  dynamicStreams_.clear();
  frameMutableSlots_.clear();
  ambientContext_.reset();
  compilingFrameDefinition_     = true;
  compilingFrameDefinitionName_ = definition.name;
  activeCalls_.push_back(definition.name);

  FrameDescriptorSpec spec;
  const CellRef contextSlot = core_.arena().makeCell();
  const CellRef resultSlot  = core_.arena().makeCell();
  std::vector<CellRef> captureSlots;
  std::vector<CellRef> parameterSlots;
  std::vector<std::optional<CellRef>> presenceSlots;
  parameterSlots.reserve(definition.params.size());
  presenceSlots.reserve(definition.params.size());
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
  for (const auto &parameter : definition.params) {
    if (!std::holds_alternative<std::string>(parameter.target) &&
        !std::holds_alternative<bool>(parameter.target) &&
        !std::holds_alternative<DestructurePattern>(parameter.target))
      throw std::invalid_argument(
          "frame definition has an unsupported parameter target");
    const CellRef slot = core_.arena().makeCell();
    parameterSlots.push_back(slot);
    presenceSlots.push_back(std::nullopt);
    spec.locals.push_back(slot);
    spec.argumentPorts.emplace_back(slot, FramePortKind::Stream);
    if (parameter.defaultValue) {
      const CellRef present = core_.arena().makeCell();
      presenceSlots.back()  = present;
      spec.locals.push_back(present);
      spec.argumentPorts.emplace_back(present, FramePortKind::Cell);
    }
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
  dynamicStreams_.insert(resultSlot);

  const CellRef returnOpcode = vm_.mintOpcode(
      OpcodeKind::ReturnFrame, "#RETURN_FRAME " + definition.name);
  core_.bindInput(returnOpcode, descriptor);
  const auto emitMatch = [&](const TypeExpr &type,
                             const std::vector<CellRef> &subjects) {
    const auto defaultSlots = emitContractDefaultSlots(type);
    const auto typeCell     = emitConstant(encodeTypeDescriptor(type));
    const auto match        = emitOp(OpcodeKind::MatchContract, "#FRAME_MATCH");
    emitInput(match, typeCell);
    emitInputConstant(match, static_cast<std::int64_t>(subjects.size()));
    for (const auto subject : subjects) emitInput(match, subject);
    for (const auto fallback : defaultSlots) emitInput(match, fallback);
    const CellRef matched = core_.arena().makeCell();
    emitOutput(match, matched);
    for (const auto &capture : ContractMatcher::contractCaptureNames(type)) {
      const CellRef captureSlot = core_.arena().makeCell();
      emitOutput(match, captureSlot);
      streamBindings_[capture] = {captureSlot};
      varBindings_[capture]    = captureSlot;
      if (capture == ".")
        ambientContext_ = emitContextFallback(captureSlot, ambientContext_);
    }
    const CellRef branch =
        emitOp(OpcodeKind::BranchFalseCell, "#FRAME_PARAM_FILTER");
    emitInput(branch, matched);
    emitInput(branch, returnOpcode);
  };

  for (std::size_t index = 0; index < definition.params.size(); ++index) {
    const auto &parameter = definition.params[index];
    const CellRef slot    = parameterSlots[index];
    if (parameter.defaultValue) {
      const CellRef notPresent = core_.arena().makeCell();
      const CellRef negate     = emitOp(OpcodeKind::Not, "#FRAME_DEFAULT_TEST");
      emitInput(negate, *presenceSlots[index]);
      emitOutput(negate, notPresent);
      const CellRef skip =
          vm_.mintOpcode(OpcodeKind::Nop, "#FRAME_DEFAULT_SKIP");
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
    if (parameter.type) emitMatch(*parameter.type, {slot});
    if (const auto *pattern =
            std::get_if<DestructurePattern>(&parameter.target)) {
      TypeExpr shape{
          .unionType = UnionType{.members = {PrimaryType{
                                     .kind = ShapeType{.pattern = *pattern}}}}};
      emitMatch(shape, {slot});
    } else if (const auto *name = std::get_if<std::string>(&parameter.target)) {
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
      else
        throw std::invalid_argument(
            "frame definition body contains an unsupported statement");
      bodyResults = lastResultCells_;
    }
  }
  if (!bodyResults.empty()) emitAppendStream(resultSlot, bodyResults);

  if (definition.returnType) {
    const auto defaultSlots = emitContractDefaultSlots(*definition.returnType);
    const auto typeCell =
        emitConstant(encodeTypeDescriptor(*definition.returnType));
    const CellRef require =
        emitOp(OpcodeKind::RequireContract, "#FRAME_RETURN_TYPE");
    emitInput(require, typeCell);
    emitInputConstant(require, definition.name);
    emitInputConstant(require, std::int64_t{1});
    emitInput(require, resultSlot);
    for (const auto fallback : defaultSlots) emitInput(require, fallback);
  }

  if (!entryOp_)
    throw std::invalid_argument("frame definition emitted no entry opcode");
  const CellRef bodyEntry = *entryOp_;
  appendOpcode(returnOpcode);
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
  spec.entry           = bodyEntry;
  const auto finalized = vm_.finalizeFrameDescriptor(descriptor, spec);
  if (!finalized) throw std::invalid_argument(finalized.error().message);
  auto &finalizedState    = frameDefinitions_.at(definition.name);
  finalizedState.entry    = bodyEntry;
  finalizedState.compiled = true;
  finalizedState.queued   = false;
  (void)restore;
}
} // namespace xanadu::vql
