// S2: semantic builtins any, all, none, one, optional_one, first, same_cell.
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::noCell;
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
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  return compileSemanticQuantifierCall(fn, hasContext ? std::optional{current}
                                                      : std::nullopt);
}

std::vector<CellRef> VQLCompiler::compileSemanticQuantifierCall(
    const FunctionInvocation &fn, const std::optional<CellRef> context) {
  if (fn.name == "same_cell") {
    requireArity(fn, 2);
    requirePositional(fn);
    const auto left  = compileArgument(fn.args[0], context);
    const auto right = compileArgument(fn.args[1], context);
    if (left.size() != 1 || right.size() != 1) {
      throw std::runtime_error("same_cell requires two singleton cell streams");
    }
    const auto op = emitOp(OpcodeKind::SameCell, "#SAME_CELL");
    emitInput(op, left.front());
    emitInput(op, right.front());
    const auto out = core_.arena().makeCell();
    emitOutput(op, out);
    return {out};
  }

  if (fn.name == "one" || fn.name == "optional_one" || fn.name == "first") {
    requireArity(fn, 1);
    requirePositional(fn);
    auto cells = compileArgument(fn.args[0], context);
    if (cells.size() == 1 && dynamicStreams_.contains(cells.front())) {
      const CellRef stream = cells.front();
      if (fn.name == "first") {
        const CellRef firstItem = emitFirstCell(stream);
        return {firstItem};
      }
      const CellRef iterator = core_.arena().makeCell();
      const CellRef begin =
          emitOp(OpcodeKind::StreamBegin, "#QUANTIFIER_BEGIN");
      emitInput(begin, stream);
      emitOutput(begin, iterator);

      const CellRef first        = core_.arena().makeCell();
      const CellRef firstPresent = core_.arena().makeCell();
      const CellRef firstNext =
          emitOp(OpcodeKind::IteratorNext, "#QUANTIFIER_FIRST");
      emitInput(firstNext, iterator);
      emitOutput(firstNext, first);
      emitOutput(firstNext, firstPresent);

      if (fn.name == "one") {
        const CellRef assertZero =
            emitOp(OpcodeKind::Assert, "#ONE_CARDINALITY_ZERO");
        emitInput(assertZero, firstPresent);
        emitInput(assertZero, emitConstant(std::string(
                                  "one cardinality violation: got 0 cells")));
      }

      const CellRef second        = core_.arena().makeCell();
      const CellRef secondPresent = core_.arena().makeCell();
      const CellRef secondNext =
          emitOp(OpcodeKind::IteratorNext, "#QUANTIFIER_SECOND");
      emitInput(secondNext, iterator);
      emitOutput(secondNext, second);
      emitOutput(secondNext, secondPresent);

      const CellRef notSecond = core_.arena().makeCell();
      const CellRef negate = emitOp(OpcodeKind::Not, "#QUANTIFIER_NOT_SECOND");
      emitInput(negate, secondPresent);
      emitOutput(negate, notSecond);

      const CellRef assertMany =
          emitOp(OpcodeKind::Assert, "#QUANTIFIER_CARDINALITY_MANY");
      emitInput(assertMany, notSecond);
      emitInput(assertMany,
                emitConstant(fn.name +
                             " cardinality violation: got more than 1 cell"));

      const CellRef outStream = emitStreamClear();
      const CellRef endLabel  = mintOpcode(OpcodeKind::Nop, "#QUANTIFIER_END");
      if (fn.name == "optional_one") {
        const CellRef branch =
            emitOp(OpcodeKind::BranchFalseCell, "#OPTIONAL_ONE_BRANCH");
        emitInput(branch, firstPresent);
        emitInput(branch, endLabel);
      }
      emitAppendStream(outStream, std::span{&first, std::size_t{1}});
      if (fn.name == "optional_one") {
        appendOpcode(endLabel);
      }
      dynamicStreams_.insert(outStream);
      return {outStream};
    }

    if (fn.name == "one") {
      if (cells.size() != 1) {
        throw std::runtime_error("one cardinality violation: got " +
                                 std::to_string(cells.size()) + " cells");
      }
      return cells;
    }
    if (fn.name == "optional_one") {
      if (cells.size() > 1) {
        throw std::runtime_error("optional_one cardinality violation: got " +
                                 std::to_string(cells.size()) + " cells");
      }
      return cells;
    }
    if (fn.name == "first") {
      if (!cells.empty()) cells.resize(1);
      return cells;
    }
  }

  if (fn.name == "any" || fn.name == "all" || fn.name == "none") {
    requireArity(fn, 2);
    requirePositional(fn);
    const auto domain = compileArgument(fn.args[0], context);
    if (domain.empty()) {
      const bool emptyResult = (fn.name != "any");
      return {emitConstant(emptyResult)};
    }

    std::vector<CellRef> matchCells;
    matchCells.reserve(domain.size());

    for (const CellRef item : domain) {
      auto savedVar     = varBindings_;
      auto savedStream  = streamBindings_;
      auto savedAmbient = ambientContext_;
      const RestoreScope restore{[&] {
        varBindings_    = std::move(savedVar);
        streamBindings_ = std::move(savedStream);
        ambientContext_ = savedAmbient;
      }};
      ambientContext_      = item;
      varBindings_["."]    = item;
      streamBindings_["."] = {item};

      CellRef matchCell   = noCell;
      const auto &predArg = fn.args[1];

      if (const auto *lambda =
              std::get_if<std::shared_ptr<PathLambda>>(&predArg.kind);
          lambda && *lambda) {
        if (!(*lambda)->params.empty()) {
          if (const auto *paramName =
                  std::get_if<std::string>(&(*lambda)->params.front().target)) {
            varBindings_[*paramName]    = item;
            streamBindings_[*paramName] = {item};
          }
        }
        if (std::holds_alternative<std::shared_ptr<ValueExpr>>(
                (*lambda)->body)) {
          const auto &bodyExpr =
              *std::get<std::shared_ptr<ValueExpr>>((*lambda)->body);
          if (const auto *boolExpr =
                  std::get_if<std::shared_ptr<BooleanExpr>>(&bodyExpr.kind);
              boolExpr && *boolExpr) {
            const auto b = compileBooleanExpr(**boolExpr, item);
            matchCell    = b ? *b : emitConstant(false);
          } else {
            const auto results = compileArgument(bodyExpr, item);
            matchCell = results.empty() ? emitConstant(false) : results.front();
          }
        } else {
          matchCell = emitConstant(false);
        }
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
          matchCell = results.empty() ? emitConstant(false) : results.front();
        }
      } else if (const auto *boolExpr =
                     std::get_if<std::shared_ptr<BooleanExpr>>(&predArg.kind);
                 boolExpr && *boolExpr) {
        const auto b = compileBooleanExpr(**boolExpr, item);
        matchCell    = b ? *b : emitConstant(false);
      } else if (const auto *shape =
                     std::get_if<std::shared_ptr<ShapeLiteral>>(&predArg.kind);
                 shape && *shape) {
        matchCell = compileShapePredicate(**shape, item);
      } else {
        const auto results = compileArgument(predArg, item);
        matchCell = results.empty() ? emitConstant(false) : results.front();
      }
      matchCells.push_back(matchCell);
    }

    if (matchCells.empty()) {
      return {emitConstant(fn.name != "any")};
    }

    CellRef combined = matchCells.front();
    const OpcodeKind combineOp =
        (fn.name == "all") ? OpcodeKind::And : OpcodeKind::Or;
    for (std::size_t i = 1; i < matchCells.size(); ++i) {
      const auto op =
          emitOp(combineOp, (fn.name == "all") ? "#ALL_AND" : "#ANY_OR");
      emitInput(op, combined);
      emitInput(op, matchCells[i]);
      const auto out = core_.arena().makeCell();
      emitOutput(op, out);
      combined = out;
    }

    if (fn.name == "none") {
      const auto notOp = emitOp(OpcodeKind::Not, "#NONE_NOT");
      emitInput(notOp, combined);
      const auto out = core_.arena().makeCell();
      emitOutput(notOp, out);
      combined = out;
    }

    return {combined};
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
