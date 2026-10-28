#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>

#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/invocation.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

template <class Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

void checkDefaultSupport(const TypeExpr &type) {
  for (const auto &primary : type.unionType.members) {
    const auto shape = [&](const ShapeType &shapeType) {
      for (const auto &field : shapeType.pattern.fields) {
        if (field.defaultValue &&
            !std::holds_alternative<ScalarLiteral>(field.defaultValue->kind))
          throw std::invalid_argument(
              "nonliteral contract defaults require compiled lazy thunks");
        if (field.type) checkDefaultSupport(*field.type);
      }
    };
    if (primary.refinement) shape(*primary.refinement);
    if (const auto *s = std::get_if<ShapeType>(&primary.kind)) shape(*s);
    if (const auto *g = std::get_if<GenericType>(&primary.kind))
      for (const auto &argument : g->args) checkDefaultSupport(*argument);
    if (const auto *nested =
            std::get_if<std::shared_ptr<TypeExpr>>(&primary.kind))
      checkDefaultSupport(**nested);
  }
}
} // namespace

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
    checkDefaultSupport(type);
    const auto descriptor = emitConstant(encodeTypeDescriptor(type));
    const auto match =
        emitOp(OpcodeKind::MatchContract, "#MATCH " + definition.name);
    emitInput(match, descriptor);
    for (const auto subject : subjects) emitInput(match, subject);
    const auto matched = core_.arena().makeCell();
    emitOutput(match, matched);
    for (const auto &name : ContractMatcher::contractCaptureNames(type)) {
      const auto capture = core_.arena().makeCell();
      emitOutput(match, capture);
      if (bindCaptures) bind(name, {capture});
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
        cells = compileArgument(*parameter.defaultValue, context);
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
    }
    if (parameter.isVariadic) break;
  }

  std::vector<CellRef> bodyResults;
  if (const auto *value =
          std::get_if<std::shared_ptr<ValueExpr>>(&definition.body)) {
    bodyResults = compileArgument(**value, context);
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
    checkDefaultSupport(*definition.returnType);
    const auto descriptor =
        emitConstant(encodeTypeDescriptor(*definition.returnType));
    const auto require =
        emitOp(OpcodeKind::RequireContract, "#RETURN_TYPE " + definition.name);
    emitInput(require, descriptor);
    emitInputConstant(require, definition.name);
    for (const auto result : bodyResults) emitInput(require, result);
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
} // namespace xanadu::vql
