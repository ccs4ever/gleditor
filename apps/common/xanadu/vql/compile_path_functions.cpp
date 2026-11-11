// Q1: path functions sort_into, find, contains, count.
//
// count(path) is lowered in the step, call and value positions with the
// counting loop emitClamp already uses: a zeroed Int64 cell, one in-place Add
// per member of the argument stream, boxed as a scalar. The other three have
// no lowering yet and refuse by name, after their arity check and before any
// argument is compiled:
//   - contains and find need a substring search over text; no opcode does it,
//     and find also needs a quote-cell constructor over document spans, which
//     no opcode provides either.
//   - sort_into needs a stable order over computed keys and then a relink of
//     the rank. No opcode orders a stream; a lowering would have to build the
//     order from comparisons and loops, and that design does not exist yet.
//   - count() with no argument as a path step needs the whole incoming walk,
//     but a path step is lowered once per cell (compileSteps,
//     compile_steps.cpp), so that form refuses until that loop gains a
//     whole-walk hook. count(path) as a step is per cell and lowers.
#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

namespace {

[[noreturn]] void refusePathFunction(const FunctionInvocation &fn,
                                     const std::string &reason) {
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "VQL path function '" + fn.name + "' " + reason);
}

// Arity is checked before any argument is compiled, so a wrong call refuses
// without evaluating its arguments.
void requireArity(const FunctionInvocation &fn, const std::size_t expected,
                  const std::string_view shape) {
  if (fn.args.size() != expected)
    refusePathFunction(fn, "takes " + std::string(shape));
}

// The three functions with no lowering: check the contract, then refuse by
// name. Returns only for count, which is not in this set.
void refuseUnlowered(const FunctionInvocation &fn) {
  if (fn.name == "sort_into") {
    requireArity(fn, 2, "a rank path and a by: key");
    if (fn.argNames.size() != 2 || fn.argNames[1] != "by")
      refusePathFunction(fn, "takes a rank path and a by: key");
    refusePathFunction(fn, "is not compiled to Vortex yet: ordering a walk and "
                           "relinking its rank has no lowering");
  }
  if (fn.name == "find") {
    requireArity(fn, 1, "one needle");
    refusePathFunction(fn, "is not compiled to Vortex yet: searching document "
                           "prose needs a substring opcode and a quote "
                           "constructor, and neither exists");
  }
  if (fn.name == "contains") {
    requireArity(fn, 2, "two arguments");
    refusePathFunction(fn, "is not compiled to Vortex yet: a substring test "
                           "needs an opcode that does not exist");
  }
}

} // namespace

std::vector<CellRef> VQLCompiler::compilePathFunctionStep(
    const PathStep &step, const CellRef current, const bool hasContext) {
  const auto &fn = std::get<FunctionInvocation>(step.selector);
  if (fn.name == "count") {
    // A top-level count(path) parses as a step, so the argument form is
    // evaluated per current cell, as the string builtin and user calls are.
    // The argument-free form counts the whole walk, which a per-cell step
    // cannot see; it refuses until compileSteps offers a whole-walk hook.
    if (fn.args.empty())
      refusePathFunction(fn, "without an argument counts the whole walk, and "
                             "step lowering runs once per cell; not compiled "
                             "yet");
    return compilePathFunctionCall(fn, hasContext ? std::optional{current}
                                                  : std::nullopt);
  }
  refuseUnlowered(fn);
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "VQL path function '" + fn.name +
                         "' is not compiled to Vortex yet");
}

std::vector<CellRef>
VQLCompiler::compilePathFunctionCall(const FunctionInvocation &fn,
                                     const std::optional<CellRef> context) {
  if (fn.name == "count") {
    requireArity(fn, 1, "exactly one path argument");
    if (std::ranges::any_of(
            fn.argNames, [](const std::string &name) { return !name.empty(); }))
      refusePathFunction(fn, "does not take named arguments");
    const auto inputs = compileArgument(fn.args.front(), context);

    // Counters are reset on every run, so one compiled call can be invoked
    // repeatedly; an empty argument leaves the zero in place.
    const auto zero       = emitConstant(std::int64_t{0});
    const auto one        = emitConstant(std::int64_t{1});
    const auto arithmetic = [&](OpcodeKind kind, CellRef left, CellRef right,
                                std::optional<CellRef> destination =
                                    std::nullopt) {
      const auto opcode = emitOp(kind, "#COUNT_ARITHMETIC");
      emitInput(opcode, left);
      emitInput(opcode, right);
      const auto output = destination ? *destination : core_.arena().makeCell();
      emitOutput(opcode, output);
      return output;
    };
    const auto count = arithmetic(OpcodeKind::Add, zero, zero);
    const auto begin = emitOp(OpcodeKind::StreamBegin, "#COUNT_BEGIN");
    for (const auto input : inputs) emitInput(begin, input);
    const auto iterator = core_.arena().makeCell();
    emitOutput(begin, iterator);
    emitIteratorLoop(iterator, [&](CellRef) {
      arithmetic(OpcodeKind::Add, count, one, count);
    });
    const auto box = emitOp(OpcodeKind::VqlBoxScalar, "#COUNT_BOX");
    emitInput(box, count);
    const auto boxed = core_.arena().makeCell();
    emitOutput(box, boxed);
    return {boxed};
  }
  refuseUnlowered(fn);
  throw QueryRefusal(QueryErrorCategory::Unsupported,
                     "unknown compiled function: " + fn.name);
}

std::optional<CellRef>
VQLCompiler::compilePathFunctionValue(const FunctionInvocation &fn,
                                      const CellRef ctxCell) {
  // Every lowered form here answers exactly one scalar, so the value position
  // is the call position's single cell.
  const auto cells = compilePathFunctionCall(fn, ctxCell);
  if (cells.empty()) return std::nullopt;
  return cells.front();
}

} // namespace xanadu::vql
