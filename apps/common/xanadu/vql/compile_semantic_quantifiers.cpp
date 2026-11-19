// S2: semantic builtins any, all, none, one, optional_one, first, same_cell.
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

namespace {

template <typename Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

void requireArity(const FunctionInvocation &fn, const std::size_t expected) {
  if (fn.args.size() != expected) {
    throw QueryRefusal(QueryErrorCategory::Unsupported,
                       fn.name + " requires " + std::to_string(expected) +
                           " argument(s)");
  }
}

void requirePositional(const FunctionInvocation &fn) {
  if (std::ranges::any_of(
          fn.argNames, [](const std::string &name) { return !name.empty(); })) {
    throw QueryRefusal(QueryErrorCategory::Malformed,
                       fn.name + " does not take named arguments");
  }
}

} // namespace

std::vector<CellRef> VQLCompiler::compileSemanticQuantifierStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  return compileSemanticQuantifierCall(
      std::get<FunctionInvocation>(step.selector),
      hasContext ? std::optional<CellRef>{current} : std::nullopt);
}

std::vector<CellRef> VQLCompiler::compileSemanticQuantifierCall(
    const FunctionInvocation &fn, const std::optional<CellRef> context) {
  if (fn.name == "same_cell") {
    requireArity(fn, 2);
    requirePositional(fn);

    const auto singleton = [&](const ValueExpr &argument,
                               const std::string_view side) {
      const auto cells = compileArgument(argument, context);
      const auto begin = emitOp(OpcodeKind::StreamBegin,
                                "#SAME_CELL_" + std::string(side) + "_BEGIN");
      for (const auto cell : cells) emitInput(begin, cell);
      const auto iterator = core_.arena().makeCell();
      emitOutput(begin, iterator);
      const auto next = emitOp(OpcodeKind::IteratorNext,
                               "#SAME_CELL_" + std::string(side) + "_FIRST");
      emitInput(next, iterator);
      const auto item    = core_.arena().makeCell();
      const auto present = core_.arena().makeCell();
      emitOutput(next, item);
      emitOutput(next, present);

      const auto requirePresent =
          emitOp(OpcodeKind::Assert, "#SAME_CELL_REQUIRE_PRESENT");
      emitInput(requirePresent, present);
      emitInputConstant(
          requirePresent,
          std::string{"same_cell requires two singleton cell streams"});

      const auto extra = emitOp(OpcodeKind::IteratorNext,
                                "#SAME_CELL_" + std::string(side) + "_EXTRA");
      emitInput(extra, iterator);
      const auto extraItem    = core_.arena().makeCell();
      const auto extraPresent = core_.arena().makeCell();
      emitOutput(extra, extraItem);
      emitOutput(extra, extraPresent);
      const auto notExtra = emitOp(OpcodeKind::Not, "#SAME_CELL_NO_EXTRA");
      emitInput(notExtra, extraPresent);
      const auto exactlyOne = core_.arena().makeCell();
      emitOutput(notExtra, exactlyOne);
      const auto requireOne =
          emitOp(OpcodeKind::Assert, "#SAME_CELL_REQUIRE_ONE");
      emitInput(requireOne, exactlyOne);
      emitInputConstant(
          requireOne,
          std::string{"same_cell requires two singleton cell streams"});
      return item;
    };

    const auto left  = singleton(fn.args[0], "LEFT");
    const auto right = singleton(fn.args[1], "RIGHT");
    const auto op    = emitOp(OpcodeKind::SameCell, "#SAME_CELL");
    emitInput(op, left);
    emitInput(op, right);
    const auto out = core_.arena().makeCell();
    emitOutput(op, out);
    return {out};
  }

  if (fn.name == "one" || fn.name == "optional_one" || fn.name == "first") {
    requireArity(fn, 1);
    requirePositional(fn);
    const auto cells = compileArgument(fn.args[0], context);
    const auto begin = emitOp(OpcodeKind::StreamBegin, "#CARDINALITY_BEGIN");
    for (const auto cell : cells) emitInput(begin, cell);
    const auto iterator = core_.arena().makeCell();
    emitOutput(begin, iterator);
    const auto next = emitOp(OpcodeKind::IteratorNext, "#CARDINALITY_FIRST");
    emitInput(next, iterator);
    const auto item    = core_.arena().makeCell();
    const auto present = core_.arena().makeCell();
    emitOutput(next, item);
    emitOutput(next, present);

    const auto assertCondition = [&](CellRef condition,
                                     std::string_view message) {
      const auto assertion = emitOp(OpcodeKind::Assert, "#CARDINALITY_ASSERT");
      emitInput(assertion, condition);
      emitInputConstant(assertion, std::string{message});
    };
    if (fn.name == "one") {
      assertCondition(present, "one cardinality violation: expected one cell");
    }
    if (fn.name != "first") {
      const auto extra = emitOp(OpcodeKind::IteratorNext, "#CARDINALITY_EXTRA");
      emitInput(extra, iterator);
      const auto extraItem    = core_.arena().makeCell();
      const auto extraPresent = core_.arena().makeCell();
      emitOutput(extra, extraItem);
      emitOutput(extra, extraPresent);
      const auto notExtra = emitOp(OpcodeKind::Not, "#CARDINALITY_NO_EXTRA");
      emitInput(notExtra, extraPresent);
      const auto exactlyOne = core_.arena().makeCell();
      emitOutput(notExtra, exactlyOne);
      assertCondition(exactlyOne,
                      fn.name == "one"
                          ? "one cardinality violation: expected one cell"
                          : "optional_one cardinality violation: expected at "
                            "most one cell");
    }
    return {item};
  }

  if (fn.name == "any" || fn.name == "all" || fn.name == "none") {
    requireArity(fn, 2);
    requirePositional(fn);
    const auto domain = compileArgument(fn.args[0], context);

    // Keep the loop graph independent of the number of runtime domain members.
    const auto iteratorBegin =
        emitOp(OpcodeKind::StreamBegin, "#QUANTIFIER_DOMAIN_BEGIN");
    for (const CellRef input : domain) emitInput(iteratorBegin, input);
    const CellRef iterator = core_.arena().makeCell();
    emitOutput(iteratorBegin, iterator);

    const CellRef zero        = emitConstant(false);
    const CellRef one         = emitConstant(true);
    const CellRef accumulator = core_.arena().makeCell();
    const CellRef initialize  = emitOp(OpcodeKind::Eq, "#QUANTIFIER_IDENTITY");
    emitInput(initialize, fn.name == "any" ? zero : one);
    emitInput(initialize, one);
    emitOutput(initialize, accumulator);

    const CellRef next = emitOp(OpcodeKind::IteratorNext, "#QUANTIFIER_NEXT");
    emitInput(next, iterator);
    const CellRef item    = core_.arena().makeCell();
    const CellRef present = core_.arena().makeCell();
    emitOutput(next, item);
    emitOutput(next, present);
    const CellRef end = mintOpcode(OpcodeKind::Nop, "#QUANTIFIER_END");
    const CellRef exhausted =
        emitOp(OpcodeKind::BranchFalseCell, "#QUANTIFIER_EXHAUSTED");
    emitInput(exhausted, present);
    emitInput(exhausted, end);

    auto savedVar     = varBindings_;
    auto savedStream  = streamBindings_;
    auto savedFunctor = functorBindings_;
    auto savedAmbient = ambientContext_;
    auto savedResults = lastResultCells_;
    const RestoreScope restore{[&] {
      varBindings_     = std::move(savedVar);
      streamBindings_  = std::move(savedStream);
      functorBindings_ = std::move(savedFunctor);
      ambientContext_  = savedAmbient;
      lastResultCells_ = std::move(savedResults);
    }};
    ambientContext_      = item;
    varBindings_["."]    = item;
    streamBindings_["."] = {item};

    const auto compilePredicate = [&]() -> CellRef {
      std::optional<CellRef> matchCell;
      const auto &predArg = fn.args[1];
      if (const auto *lambda =
              std::get_if<std::shared_ptr<PathLambda>>(&predArg.kind);
          lambda) {
        if (!*lambda)
          throw std::invalid_argument("null VQL quantifier predicate lambda");
        if ((*lambda)->params.size() > 1 ||
            (!(*lambda)->params.empty() &&
             (*lambda)->params.front().isVariadic))
          throw QueryRefusal(QueryErrorCategory::Unsupported,
                             "quantifier predicate lambdas require zero or one "
                             "nonvariadic formal");
        if (std::holds_alternative<std::shared_ptr<ScriptBody>>(
                (*lambda)->body))
          throw QueryRefusal(
              QueryErrorCategory::Unsupported,
              "quantifier predicate block bodies are unsupported");

        const auto *body =
            std::get_if<std::shared_ptr<ValueExpr>>(&(*lambda)->body);
        const bool pathBody =
            body && *body &&
            std::holds_alternative<std::shared_ptr<PathExpression>>(
                (*body)->kind);
        const DefDeclaration inlineLambda{
            .name = "#vql-inline-quantifier-predicate-" +
                    std::to_string(allOps_.size()),
            .params = (*lambda)->params,
            .body   = (*lambda)->body,
        };
        const auto results = compileCall(inlineLambda, {}, item, true, false);
        if (pathBody) {
          matchCell = emitStreamPresence(results);
        } else if (results.empty()) {
          matchCell = emitConstant(false);
        } else {
          // Scalar lambda results are streams of values. The first value's
          // truthiness determines the predicate, not stream non-emptiness.
          const CellRef begin =
              emitOp(OpcodeKind::StreamBegin, "#QUANTIFIER_SCALAR_BEGIN");
          for (const CellRef result : results) emitInput(begin, result);
          const CellRef resultIterator = core_.arena().makeCell();
          emitOutput(begin, resultIterator);
          const CellRef first =
              emitOp(OpcodeKind::IteratorNext, "#QUANTIFIER_SCALAR_FIRST");
          emitInput(first, resultIterator);
          const CellRef value   = core_.arena().makeCell();
          const CellRef present = core_.arena().makeCell();
          emitOutput(first, value);
          emitOutput(first, present);
          const CellRef end =
              mintOpcode(OpcodeKind::Nop, "#QUANTIFIER_SCALAR_END");
          matchCell = core_.arena().makeCell();
          const CellRef initialize =
              emitOp(OpcodeKind::CopyCellRefs, "#QUANTIFIER_SCALAR_FALSE");
          emitInput(initialize, emitConstant(false));
          emitOutput(initialize, *matchCell);
          const CellRef absent =
              emitOp(OpcodeKind::BranchFalseCell, "#QUANTIFIER_SCALAR_ABSENT");
          emitInput(absent, present);
          emitInput(absent, end);
          const CellRef truth =
              emitOp(OpcodeKind::And, "#QUANTIFIER_SCALAR_TRUTH");
          emitInput(truth, value);
          emitInput(truth, emitConstant(true));
          emitOutput(truth, *matchCell);
          appendOpcode(end);
        }
      } else if (const auto named = compileShapePredicateValue(predArg, item)) {
        // A shape named by a variable or a call; a literal is below.
        matchCell = *named;
      } else if (const auto *path =
                     std::get_if<std::shared_ptr<PathExpression>>(
                         &predArg.kind);
                 path && *path) {
        compilePathExpression(**path, item);
        matchCell = emitStreamPresence(lastResultCells_);
      } else if (const auto *varName =
                     std::get_if<std::string>(&predArg.kind)) {
        const std::string name =
            varName->starts_with('$') ? varName->substr(1) : *varName;
        if (const auto found = functorBindings_.find(name);
            found != functorBindings_.end()) {
          const auto walked = compileSteps(found->second, {item}, true);
          matchCell         = emitStreamPresence(walked);
        } else {
          const auto results = compileArgument(predArg, item);
          if (results.empty()) {
            matchCell = emitConstant(false);
          } else if (results.size() > 1 ||
                     std::ranges::any_of(results, [&](CellRef result) {
                       return dynamicStreams_.contains(result);
                     })) {
            matchCell = emitStreamPresence(results);
          } else {
            matchCell = results.front();
          }
        }
      } else if (const auto *boolean =
                     std::get_if<std::shared_ptr<BooleanExpr>>(&predArg.kind);
                 boolean && *boolean) {
        matchCell =
            compileBooleanExpr(**boolean, item).value_or(emitConstant(false));
      } else if (const auto *shape =
                     std::get_if<std::shared_ptr<ShapeLiteral>>(&predArg.kind);
                 shape && *shape) {
        matchCell = compileShapePredicate(**shape, item);
      } else {
        const auto results = compileArgument(predArg, item);
        if (results.empty()) {
          matchCell = emitConstant(false);
        } else if (results.size() > 1 ||
                   std::ranges::any_of(results, [&](CellRef result) {
                     return dynamicStreams_.contains(result);
                   })) {
          matchCell = emitStreamPresence(results);
        } else {
          matchCell = results.front();
        }
      }
      return matchCell.value();
    };

    const CellRef predicate  = compilePredicate();
    CellRef accumulatorInput = predicate;
    if (fn.name == "none") {
      const CellRef negate = emitOp(OpcodeKind::Not, "#QUANTIFIER_NONE_NOT");
      emitInput(negate, predicate);
      accumulatorInput = core_.arena().makeCell();
      emitOutput(negate, accumulatorInput);
    }
    const CellRef combine =
        emitOp(fn.name == "any" ? OpcodeKind::Or : OpcodeKind::And,
               "#QUANTIFIER_ACCUMULATE");
    emitInput(combine, accumulator);
    emitInput(combine, accumulatorInput);
    emitOutput(combine, accumulator);

    if (fn.name == "any") {
      const CellRef continueLoop =
          mintOpcode(OpcodeKind::Nop, "#QUANTIFIER_ANY_CONTINUE_LABEL");
      const CellRef continueIfFalse =
          emitOp(OpcodeKind::BranchFalseCell, "#QUANTIFIER_ANY_BRANCH");
      emitInput(continueIfFalse, accumulator);
      emitInput(continueIfFalse, continueLoop);
      const CellRef exitIfTrue =
          emitOp(OpcodeKind::BranchFalseCell, "#QUANTIFIER_ANY_EXIT");
      emitInputConstant(exitIfTrue, false);
      emitInput(exitIfTrue, end);
      appendOpcode(continueLoop);
    } else {
      const CellRef stop =
          emitOp(OpcodeKind::BranchFalseCell, "#QUANTIFIER_ALL_NONE_EXIT");
      emitInput(stop, accumulator);
      emitInput(stop, end);
    }
    const CellRef repeat =
        emitOp(OpcodeKind::BranchFalseCell, "#QUANTIFIER_REPEAT");
    emitInputConstant(repeat, false);
    emitInput(repeat, next);
    appendOpcode(end);
    return {accumulator};
  }

  refuseSemanticBuiltin(fn.name);
}

std::vector<CellRef> VQLCompiler::compileSemanticQuantifierWalk(
    const FunctionInvocation &fn, const std::span<const CellRef> /*stream*/,
    const bool /*hasContext*/) {
  refuseSemanticBuiltin(fn.name);
}

std::optional<CellRef>
VQLCompiler::compileSemanticQuantifierValue(const FunctionInvocation &fn,
                                            const CellRef ctxCell) {
  const auto results = compileSemanticQuantifierCall(fn, ctxCell);
  if (results.empty()) return std::nullopt;
  return results.front();
}

} // namespace xanadu::vql
