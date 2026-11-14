/**
 * @file lower_io.cpp
 * @brief Card L6: IO descriptions (`pure bind then putStr putStrLn getLine`,
 *        `>>=`, `>>`) and the host trampoline that performs them.
 */
#include <array>
#include <format>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vlace/ast.hpp"
#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/vlace/lower_context.hpp"

namespace xanadu::vlace::lower {

namespace {

Lowered<Operand> toWhnf(LowerContext &ctx, Operand value, SourceLocation loc) {
  if (value.state == Evaluation::Whnf) return value;
  return forceOperand(ctx, value, loc);
}

ValuePtr makeUnit(Compiler &compiler) {
  LowerContext ctx(compiler);
  auto op = makeTerm(ctx, "Unit", {}, {});
  if (op) {
    if (auto v = readbackCell(compiler, op->cell)) return *v;
  }
  auto fallback  = std::make_shared<Value>();
  fallback->kind = Value::Kind::Ctor;
  fallback->tag  = "Unit";
  if (op) fallback->cell = op->cell;
  fallback->ctorArity = 0;
  return fallback;
}

struct Continuation {
  enum class Kind : std::uint8_t {
    Then,
    Bind,
  } kind;
  ValuePtr target;
};

} // namespace

Lowered<Operand> lowerIoBind(LowerContext &ctx, const EBinOp &e,
                             SourceLocation loc) {
  if (e.op != ">>=" && e.op != ">>") {
    return fail(CompileError::UnexpectedToken,
                "unknown IO operator '" + e.op + "'", loc);
  }
  const std::string_view tag = (e.op == ">>=") ? "IO.Bind" : "IO.Then";
  auto lhs                   = lowerExpr(ctx, *e.lhs);
  if (!lhs) return lhs;
  auto forcedL = toWhnf(ctx, *lhs, e.lhs->loc);
  if (!forcedL) return forcedL;

  auto rhs = lowerExpr(ctx, *e.rhs);
  if (!rhs) return rhs;
  auto forcedR = toWhnf(ctx, *rhs, e.rhs->loc);
  if (!forcedR) return forcedR;

  const std::array fields{*forcedL, *forcedR};
  return makeTerm(ctx, tag, fields, loc);
}

std::optional<Lowered<Operand>>
lowerIoBuiltin(LowerContext &ctx, std::string_view name, SourceLocation loc) {
  if (name == "pure") return termBuilder(ctx, "IO.Pure", 1, loc);
  if (name == "bind") return termBuilder(ctx, "IO.Bind", 2, loc);
  if (name == "then") return termBuilder(ctx, "IO.Then", 2, loc);
  if (name == "putStr") return termBuilder(ctx, "IO.PutStr", 1, loc);
  if (name == "putStrLn") return termBuilder(ctx, "IO.PutStrLn", 1, loc);
  if (name == "getLine") return termBuilder(ctx, "IO.GetLine", 0, loc);
  return std::nullopt;
}

std::expected<ValuePtr, EvalError> runIoOnVm(Compiler &compiler,
                                             const ValuePtr &action,
                                             std::istream &in,
                                             std::ostream &out) {
  if (!action) {
    return fail(CompileError::TypeMismatch,
                "runIO: not an IO action (did the program return an IO value?)",
                {});
  }

  std::vector<Continuation> stack;
  ValuePtr curAction  = action;
  ValuePtr currentVal = nullptr;

  try {
    while (curAction != nullptr || !stack.empty()) {
      if (curAction != nullptr) {
        if (curAction->kind == Value::Kind::Thunk) {
          auto forced = compiler.forceValue(curAction);
          if (!forced) return std::unexpected(forced.error());
          curAction = *forced;
        }

        if (!curAction || curAction->kind != Value::Kind::Ctor ||
            !curAction->tag.starts_with("IO.")) {
          return fail(
              CompileError::TypeMismatch,
              "runIO: not an IO action (did the program return an IO value?)",
              {});
        }

        const std::string &tag = curAction->tag;
        if (tag == "IO.Then") {
          if (curAction->args.size() != 2) {
            return fail(CompileError::TypeMismatch,
                        "runIO: 'IO.Then' wants 2 argument(s), got " +
                            std::to_string(curAction->args.size()),
                        {});
          }
          stack.push_back(Continuation{.kind   = Continuation::Kind::Then,
                                       .target = curAction->args[1]});
          curAction = curAction->args[0];
          continue;
        }

        if (tag == "IO.Bind") {
          if (curAction->args.size() != 2) {
            return fail(CompileError::TypeMismatch,
                        "runIO: 'IO.Bind' wants 2 argument(s), got " +
                            std::to_string(curAction->args.size()),
                        {});
          }
          stack.push_back(Continuation{.kind   = Continuation::Kind::Bind,
                                       .target = curAction->args[1]});
          curAction = curAction->args[0];
          continue;
        }

        if (tag == "IO.Pure") {
          if (curAction->args.empty()) {
            currentVal = makeUnit(compiler);
          } else {
            currentVal = curAction->args[0];
          }
          curAction = nullptr;
        } else if (tag == "IO.PutStr" || tag == "IO.PutStrLn") {
          if (curAction->args.size() != 1) {
            return fail(CompileError::TypeMismatch,
                        "runIO: '" + tag + "' wants 1 argument(s), got " +
                            std::to_string(curAction->args.size()),
                        {});
          }
          auto sVal = compiler.forceValue(curAction->args[0]);
          if (!sVal) return std::unexpected(sVal.error());
          if ((*sVal)->kind != Value::Kind::Str) {
            return fail(CompileError::TypeMismatch,
                        "putStr: argument is not a String", {});
          }
          out << (*sVal)->s;
          if (tag == "IO.PutStrLn") out << '\n';
          currentVal = makeUnit(compiler);
          curAction  = nullptr;
        } else if (tag == "IO.GetLine") {
          if (!curAction->args.empty()) {
            return fail(CompileError::TypeMismatch,
                        "runIO: 'IO.GetLine' wants 0 argument(s), got " +
                            std::to_string(curAction->args.size()),
                        {});
          }
          std::string line;
          std::getline(in, line);
          const auto cell = compiler.core().arena().makeCell();
          compiler.core().writeValue(cell, 0, std::nullopt,
                                     zigzag::vortex::CellValue{line});
          auto strVal  = std::make_shared<Value>();
          strVal->kind = Value::Kind::Str;
          strVal->cell = cell;
          strVal->s    = std::move(line);
          currentVal   = strVal;
          curAction    = nullptr;
        } else {
          // L8 effect leaf fallback
          LowerContext ctx(compiler);
          IoRunContext runCtx{
              .compiler = compiler, .lower = ctx, .in = in, .out = out};
          std::vector<Operand> argOps;
          argOps.reserve(curAction->args.size());
          for (const auto &arg : curAction->args) {
            if (arg && arg->cell) {
              argOps.emplace_back(*arg->cell);
            }
          }
          auto leaf = performEffectLeaf(runCtx, tag, argOps, {});
          if (!leaf) {
            return fail(CompileError::TypeMismatch,
                        "runIO: unknown IO node '" + tag + "'", {});
          }
          if (!*leaf) return std::unexpected(leaf->error());
          auto rb = readbackCell(compiler, (*leaf)->cell);
          if (!rb) return std::unexpected(rb.error());
          currentVal = *rb;
          curAction  = nullptr;
        }
      }

      // Process continuation stack for completed leaf
      while (curAction == nullptr && !stack.empty()) {
        auto cont = std::move(stack.back());
        stack.pop_back();

        if (cont.kind == Continuation::Kind::Then) {
          curAction = cont.target;
        } else if (cont.kind == Continuation::Kind::Bind) {
          ValuePtr fnVal = cont.target;
          if (fnVal->kind == Value::Kind::Thunk) {
            auto forcedFn = compiler.forceValue(fnVal);
            if (!forcedFn) return std::unexpected(forcedFn.error());
            fnVal = *forcedFn;
          }
          if (!fnVal->cell) {
            return fail(CompileError::TypeMismatch,
                        "runIO: continuation has no cell", {});
          }

          if (!currentVal->cell) {
            LowerContext ctx(compiler);
            switch (currentVal->kind) {
            case Value::Kind::Int:
              currentVal->cell =
                  ctx.emitConstant(zigzag::vortex::CellValue{currentVal->i})
                      .cell;
              break;
            case Value::Kind::Float:
              currentVal->cell =
                  ctx.emitConstant(zigzag::vortex::CellValue{currentVal->f})
                      .cell;
              break;
            case Value::Kind::Bool:
              currentVal->cell =
                  ctx.emitConstant(zigzag::vortex::CellValue{currentVal->b})
                      .cell;
              break;
            case Value::Kind::Str:
              currentVal->cell =
                  ctx.emitConstant(zigzag::vortex::CellValue{currentVal->s})
                      .cell;
              break;
            default:
              return fail(
                  CompileError::TypeMismatch,
                  "runIO: intermediate value has no cell representation", {});
            }
          }

          LowerContext ctx(compiler);
          auto applied = applyOperand(ctx, Operand(*fnVal->cell),
                                      Operand(*currentVal->cell), {});
          if (!applied) return std::unexpected(applied.error());
          auto ran = runBlock(ctx, ctx.swapBlock(Block{}), *applied);
          if (!ran) return std::unexpected(ran.error());
          auto nextAction = readbackCell(compiler, ran->cell);
          if (!nextAction) return std::unexpected(nextAction.error());
          curAction = *nextAction;
        }
      }
    }
  } catch (const std::exception &e) {
    return fail(CompileError::TypeMismatch,
                std::string("runIO: threw: ") + e.what(), {});
  }

  return currentVal ? currentVal : makeUnit(compiler);
}

std::expected<DriverResult, DriverError> driverEvaluateIO(const Program &prog,
                                                          DriverResult partial,
                                                          std::istream &in,
                                                          std::ostream &out) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Compiler compiler(core, vm);
  const auto driverFailure = [](const EvalError &e, std::string_view phase) {
    return std::unexpected(DriverError{.code   = e.code,
                                       .detail = e.detail,
                                       .loc    = e.loc,
                                       .phase  = std::string(phase)});
  };

  LowerContext ctx(compiler);
  std::optional<Operand> result;
  try {
    auto lowered = lowerProgram(ctx, prog);
    if (!lowered) return driverFailure(lowered.error(), "compile");
    result = lowered->value_or(ctx.emitConstant(std::int64_t{0}));
  } catch (const LoweringBudgetExceeded &budget) {
    return driverFailure(budget.error(), "compile");
  } catch (const std::exception &error) {
    return driverFailure(
        EvalError{.code   = CompileError::UnexpectedToken,
                  .detail = std::string("vlace: ") + error.what(),
                  .loc    = {}},
        "compile");
  }

  ValuePtr value;
  try {
    auto ran = runBlock(ctx, ctx.swapBlock(Block{}), *result);
    if (!ran) return driverFailure(ran.error(), "eval");
    auto read = readbackCell(compiler, ran->cell);
    if (!read) return driverFailure(read.error(), "eval");
    value = *read;
  } catch (const std::exception &error) {
    return driverFailure(
        EvalError{.code   = CompileError::UnexpectedToken,
                  .detail = std::string("vlace: ") + error.what(),
                  .loc    = {}},
        "eval");
  }

  std::expected<ValuePtr, EvalError> ran;
  try {
    ran = compiler.runIO(value, in, out);
  } catch (const std::exception &error) {
    return driverFailure(EvalError{.code   = CompileError::UnexpectedToken,
                                   .detail = error.what(),
                                   .loc    = {}},
                         "eval");
  }
  if (!ran) return driverFailure(ran.error(), "eval");

  try {
    partial.value = compiler.render(*ran);
  } catch (const std::exception &error) {
    return driverFailure(EvalError{.code   = CompileError::UnexpectedToken,
                                   .detail = error.what(),
                                   .loc    = {}},
                         "eval");
  }
  return partial;
}

} // namespace xanadu::vlace::lower
