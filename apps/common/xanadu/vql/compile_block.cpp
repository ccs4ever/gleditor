#include "common/xanadu/vql/compiler.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/vql/contracts.hpp"

namespace xanadu::vql {
namespace {

template <class Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

TypeExpr optionalAnyType() {
  return TypeExpr{.unionType = UnionType{
                      .members = {PrimaryType{
                          .kind       = BasicType{.kind = BasicType::Kind::Any},
                          .isOptional = true}}}};
}

void requireLiteralDefaults(const DestructurePattern &pattern);

void requireLiteralDefaults(const TypeExpr &type) {
  for (const auto &member : type.unionType.members) {
    const auto visitPattern = [](const ShapeType &shape) {
      requireLiteralDefaults(shape.pattern);
    };
    if (member.refinement) visitPattern(*member.refinement);
    if (const auto *shape = std::get_if<ShapeType>(&member.kind))
      visitPattern(*shape);
    if (const auto *generic = std::get_if<GenericType>(&member.kind)) {
      for (const auto &argument : generic->args) {
        if (argument) requireLiteralDefaults(*argument);
      }
    }
    if (const auto *nested =
            std::get_if<std::shared_ptr<TypeExpr>>(&member.kind)) {
      if (*nested) requireLiteralDefaults(**nested);
    }
  }
}

void requireLiteralDefaults(const DestructurePattern &pattern) {
  for (const auto &field : pattern.fields) {
    if (field.defaultValue &&
        !std::holds_alternative<ScalarLiteral>(field.defaultValue->kind)) {
      throw std::invalid_argument(
          "nonliteral destructure defaults require compiled lazy thunks");
    }
    if (field.type) requireLiteralDefaults(*field.type);
  }
}

TypeExpr destructureContract(const DestructurePattern &source) {
  auto pattern = source;
  for (auto &field : pattern.fields) {
    if (!field.isContext && !field.type) {
      // Shape contracts refuse missing fields; Optional Any retains VQL's
      // BindEmpty behavior for untyped destructuring fields.
      field.type = std::make_shared<TypeExpr>(optionalAnyType());
    }
  }
  TypeExpr contract{
      .unionType =
          UnionType{.members = {PrimaryType{
                        .kind = ShapeType{.pattern = std::move(pattern)}}}}};
  requireLiteralDefaults(contract);
  try {
    ContractMatcher::validateType(contract);
    (void)encodeTypeDescriptor(contract);
  } catch (const std::exception &error) {
    throw std::invalid_argument(error.what());
  }
  return contract;
}

} // namespace

std::optional<zigzag::CellRef>
VQLCompiler::compileExecutionBlock(const ExecutionBlock &block) {
  using zigzag::CellRef;
  using zigzag::vortex::OpcodeKind;

  const CellRef resultStream = emitStreamClear();
  lastResultCells_           = {resultStream};

  const auto bindLet = [&](const LetClause &let,
                           const bool inLoop) -> std::vector<CellRef> {
    const auto *functorPath  = std::get_if<PathExpression>(&let.target);
    const auto *functorValue = std::get_if<ValueExpr>(&let.target);
    const auto functor = functorPath != nullptr ? functorArgument(*functorPath)
                         : functorValue != nullptr
                             ? functorArgument(*functorValue)
                             : std::nullopt;
    if (functor) {
      functorBindings_[let.varName] = *functor;
      varBindings_.erase(let.varName);
      streamBindings_.erase(let.varName);
      return {};
    }

    functorBindings_.erase(let.varName);
    std::vector<CellRef> values;
    if (functorPath) {
      compilePathExpression(*functorPath, core_.home());
      values = lastResultCells_;
    } else if (functorValue) {
      values = compileArgument(*functorValue, ambientContext_);
    }
    streamBindings_[let.varName] = values;
    const std::optional<CellRef> value =
        values.empty() ? std::nullopt : std::optional{values.front()};
    varBindings_[let.varName] = value;
    if (value && dynamicStreams_.contains(*value)) return values;

    if (!inLoop) {
      const CellRef bind = emitOp(OpcodeKind::Bind, "#BIND " + let.varName);
      emitInputConstant(bind, let.varName);
      if (value) emitInput(bind, *value);
    }
    return values;
  };

  std::function<void(const ActionClause &, bool)> compileAction;
  compileAction = [&](const ActionClause &action, const bool inLoop) {
    std::visit(
        [&](const auto &clause) {
          using Clause = std::decay_t<decltype(clause)>;
          if constexpr (std::is_same_v<Clause, ReturnClause>) {
            for (const auto &item : clause.items) {
              if (const auto *path = std::get_if<PathExpression>(&item.item)) {
                compilePathExpression(*path, core_.home());
                emitAppendStream(resultStream, lastResultCells_);
              } else {
                throw std::invalid_argument(
                    "field weaving requires Vortex graph lowering");
              }
            }
          } else if constexpr (std::is_same_v<Clause, EffectClause>) {
            for (const auto &item : clause.items) {
              std::visit(
                  [&](const auto &effect) {
                    using Effect = std::decay_t<decltype(effect)>;
                    if constexpr (std::is_same_v<Effect, PathExpression>) {
                      compilePathExpression(effect, core_.home());
                      emitAppendStream(resultStream, lastResultCells_);
                    } else if constexpr (std::is_same_v<Effect, LetClause>) {
                      const auto values = bindLet(effect, inLoop);
                      emitAppendStream(resultStream, values);
                    } else {
                      throw std::invalid_argument(
                          "nested weave loops require Vortex control lowering");
                    }
                  },
                  item.item);
            }
          } else if constexpr (std::is_same_v<Clause, ConditionalClause>) {
            const auto condition = compileBooleanExpr(
                clause.condition, ambientContext_.value_or(core_.home()));
            if (!condition) {
              throw std::invalid_argument(
                  "conditional action has no compiled predicate");
            }
            const CellRef otherwise =
                vm_.mintOpcode(OpcodeKind::Nop, "#IF_ELSE");
            const CellRef branch =
                emitOp(OpcodeKind::BranchFalseCell, "#IF_FALSE");
            emitInput(branch, *condition);
            emitInput(branch, otherwise);
            const auto compileScopedAction = [&](const ActionClause &nested) {
              auto variables     = varBindings_;
              auto streams       = streamBindings_;
              auto functors      = functorBindings_;
              const auto context = ambientContext_;
              const RestoreScope restore{[&] {
                varBindings_     = std::move(variables);
                streamBindings_  = std::move(streams);
                functorBindings_ = std::move(functors);
                ambientContext_  = context;
              }};
              compileAction(nested, inLoop);
            };
            if (clause.thenClause) compileScopedAction(*clause.thenClause);
            if (clause.elseClause) {
              const CellRef done = vm_.mintOpcode(OpcodeKind::Nop, "#IF_DONE");
              const CellRef join =
                  emitOp(OpcodeKind::BranchFalseCell, "#IF_JOIN");
              emitInputConstant(join, false);
              emitInput(join, done);
              appendOpcode(otherwise);
              compileScopedAction(*clause.elseClause);
              appendOpcode(done);
            } else {
              appendOpcode(otherwise);
            }
          } else {
            throw std::invalid_argument("unsupported VQL action clause");
          }
        },
        action.clause);
  };

  std::function<void(std::size_t, bool)> compileBindings;
  compileBindings = [&](const std::size_t index, const bool inLoop) {
    if (index == block.bindings.size()) {
      if (!block.where) {
        compileAction(block.action, inLoop);
        return;
      }
      const auto predicate = compileBooleanExpr(
          block.where->condition, ambientContext_.value_or(core_.home()));
      if (!predicate) {
        throw std::invalid_argument("where clause has no compiled predicate");
      }
      const CellRef skip = vm_.mintOpcode(OpcodeKind::Nop, "#WHERE_SKIP");
      const CellRef branch =
          emitOp(OpcodeKind::BranchFalseCell, "#WHERE_FALSE");
      emitInput(branch, *predicate);
      emitInput(branch, skip);
      compileAction(block.action, inLoop);
      appendOpcode(skip);
      return;
    }

    const auto &binding = block.bindings[index];
    if (const auto *let = std::get_if<LetClause>(&binding)) {
      (void)bindLet(*let, inLoop);
      compileBindings(index + 1, inLoop);
      return;
    }

    const auto &loop = std::get<ForClause>(binding);
    compilePathExpression(loop.inPath, core_.home());
    std::vector<CellRef> source = lastResultCells_;
    const CellRef iterator      = core_.arena().makeCell();
    const CellRef begin         = emitOp(OpcodeKind::StreamBegin, "#FOR_BEGIN");
    for (const auto cell : source) emitInput(begin, cell);
    emitOutput(begin, iterator);
    emitIteratorLoop(iterator, [&](const CellRef item) {
      auto variables     = varBindings_;
      auto streams       = streamBindings_;
      auto functors      = functorBindings_;
      const auto context = ambientContext_;
      const RestoreScope restore{[&] {
        varBindings_     = std::move(variables);
        streamBindings_  = std::move(streams);
        functorBindings_ = std::move(functors);
        ambientContext_  = context;
      }};
      ambientContext_ = item;
      functorBindings_.erase(loop.varName);
      streamBindings_[loop.varName] = {item};
      varBindings_[loop.varName]    = item;

      std::optional<CellRef> skip;
      if (loop.destructurePattern) {
        const auto contract = destructureContract(*loop.destructurePattern);
        const auto names    = ContractMatcher::contractCaptureNames(contract);
        const CellRef descriptor = emitConstant(encodeTypeDescriptor(contract));
        const CellRef match = emitOp(OpcodeKind::MatchContract, "#FOR_MATCH");
        emitInput(match, descriptor);
        emitInput(match, item);
        const CellRef matched = core_.arena().makeCell();
        emitOutput(match, matched);
        for (const auto &name : names) {
          const CellRef capture = core_.arena().makeCell();
          emitOutput(match, capture);
          CellRef boundCapture = capture;
          if (name == ".") {
            boundCapture    = emitContextFallback(capture, ambientContext_);
            ambientContext_ = boundCapture;
          }
          functorBindings_.erase(name);
          streamBindings_[name] = {boundCapture};
          varBindings_[name]    = boundCapture;
        }
        skip = vm_.mintOpcode(OpcodeKind::Nop, "#FOR_PATTERN_SKIP");
        const CellRef branch =
            emitOp(OpcodeKind::BranchFalseCell, "#FOR_PATTERN_FALSE");
        emitInput(branch, matched);
        emitInput(branch, *skip);
      }
      compileBindings(index + 1, true);
      if (skip) appendOpcode(*skip);
    });
  };

  compileBindings(0, false);
  lastResultCells_ = {resultStream};
  return resultStream;
}

} // namespace xanadu::vql
