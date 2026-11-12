/**
 * @file lower_scalars.cpp
 * @brief Card L1: literals, variables, scalar operators, `if`, True/False.
 *
 * A scalar is one register: Int and Char an int64 cell, Float a double cell,
 * Bool a bool cell (which BranchFalseCell consumes directly), String a text
 * cell. Text cells carry ValueKind::None, so a string that looks like a number
 * ("3") is still text to the VM's Add and to read-back.
 */
#include <array>
#include <utility>

#include "common/xanadu/vlace/run_vm.hpp"

namespace xanadu::vlace::lower {

namespace {

using zigzag::vortex::CellValue;
using zigzag::vortex::OpcodeKind;

/// Binary operators the VM evaluates with one opcode. `++` is Add on two text
/// operands; the type checker keeps the operand types apart.
constexpr std::array<std::pair<std::string_view, OpcodeKind>, 12> opcodeOf{{
    {"+", OpcodeKind::Add},
    {"-", OpcodeKind::Sub},
    {"*", OpcodeKind::Mul},
    {"/", OpcodeKind::Div},
    {"%", OpcodeKind::Mod},
    {"==", OpcodeKind::Eq},
    {"/=", OpcodeKind::Neq},
    {"<", OpcodeKind::Lt},
    {"<=", OpcodeKind::Lte},
    {">", OpcodeKind::Gt},
    {">=", OpcodeKind::Gte},
    {"++", OpcodeKind::Add},
}};

constexpr std::string_view andOp = "&&";
constexpr std::string_view orOp  = "||";

std::optional<OpcodeKind> opcodeFor(std::string_view op) {
  for (const auto &[name, kind] : opcodeOf) {
    if (name == op) return kind;
  }
  return std::nullopt;
}

/// A value in weak head normal form: forced unless lowering already knows it.
Lowered<Operand> toWhnf(LowerContext &ctx, Operand operand,
                        SourceLocation loc) {
  if (operand.state == Evaluation::Whnf) return operand;
  return forceOperand(ctx, operand, loc); // L2
}

Lowered<Operand> lowerForced(LowerContext &ctx, const ExprNode &e) {
  auto operand = lowerExpr(ctx, e);
  if (!operand) return operand;
  return toWhnf(ctx, *operand, e.loc);
}

Operand emitBinary(LowerContext &ctx, OpcodeKind kind, Operand lhs,
                   Operand rhs) {
  const auto out = ctx.newRegister();
  const auto op  = ctx.emitOp(kind, "#vlace_binop");
  ctx.emitInput(op, lhs.cell);
  ctx.emitInput(op, rhs.cell);
  ctx.emitOutput(op, out.cell);
  return Operand(out.cell, Evaluation::Whnf);
}

/// `lhs && rhs` and `lhs || rhs`: the right operand is lowered after a branch
/// that skips it when the left already decides, so its effects (a run-time
/// failure, a divergence) are never reached.
Lowered<Operand> lowerShortCircuit(LowerContext &ctx, const EBinOp &e,
                                   bool isAnd) {
  auto lhs = lowerForced(ctx, *e.lhs);
  if (!lhs) return lhs;
  const auto result = ctx.newRegister();
  const auto done = ctx.mintLabel(isAnd ? "#vlace_and_done" : "#vlace_or_done");
  emitCopy(ctx, *lhs, result);
  if (isAnd) {
    ctx.branchIfFalse(*lhs, done);
  } else {
    // Skip the right operand when the left is true: branch on its negation.
    const auto negated = ctx.newRegister();
    const auto notOp   = ctx.emitOp(OpcodeKind::Not, "#vlace_not");
    ctx.emitInput(notOp, lhs->cell);
    ctx.emitOutput(notOp, negated.cell);
    ctx.branchIfFalse(negated, done);
  }
  auto rhs = lowerForced(ctx, *e.rhs);
  if (!rhs) return rhs;
  emitCopy(ctx, *rhs, result);
  ctx.placeLabel(done);
  return Operand(result.cell, Evaluation::Whnf);
}

} // namespace

Lowered<Operand> lowerLiteral(LowerContext &ctx, const ELiteral &e,
                              SourceLocation) {
  switch (e.lit.kind) {
  case LiteralKind::Int:
  case LiteralKind::Char: // a Char is its code point, as it always was
    return ctx.emitConstant(CellValue{e.lit.intValue});
  case LiteralKind::Float:
    return ctx.emitConstant(CellValue{e.lit.floatValue});
  case LiteralKind::String:
    return ctx.emitConstant(CellValue{e.lit.textValue});
  }
  return ctx.emitConstant(CellValue{std::int64_t{0}});
}

Lowered<Operand> lowerVar(LowerContext &ctx, const EVar &e,
                          SourceLocation loc) {
  const auto hit = ctx.lookup(e.name);
  if (!hit) return resolveGlobalName(ctx, e.name, loc);
  if (hit->frameDepth > 0) {
    auto captured = captureOperand(ctx, *hit, loc); // L2
    if (!captured) return captured;
    return toWhnf(ctx, *captured, loc);
  }
  return toWhnf(ctx, hit->operand, loc);
}

Lowered<Operand> lowerBinOp(LowerContext &ctx, const EBinOp &e,
                            SourceLocation loc) {
  if (e.op == andOp) return lowerShortCircuit(ctx, e, true);
  if (e.op == orOp) return lowerShortCircuit(ctx, e, false);
  const auto kind = opcodeFor(e.op);
  if (!kind) {
    return fail(CompileError::UnexpectedToken,
                "unknown operator '" + e.op + "'", loc);
  }
  auto lhs = lowerForced(ctx, *e.lhs);
  if (!lhs) return lhs;
  auto rhs = lowerForced(ctx, *e.rhs);
  if (!rhs) return rhs;
  return emitBinary(ctx, *kind, *lhs, *rhs);
}

Lowered<Operand> lowerIf(LowerContext &ctx, const EIf &e, SourceLocation) {
  auto cond = lowerForced(ctx, *e.cond);
  if (!cond) return cond;
  const auto result  = ctx.newRegister();
  const auto elseArm = ctx.mintLabel("#vlace_else");
  const auto done    = ctx.mintLabel("#vlace_endif");
  ctx.branchIfFalse(*cond, elseArm);
  auto thenValue = lowerForced(ctx, *e.then);
  if (!thenValue) return thenValue;
  emitCopy(ctx, *thenValue, result);
  ctx.jumpTo(done);
  ctx.placeLabel(elseArm);
  auto elseValue = lowerForced(ctx, *e.elze);
  if (!elseValue) return elseValue;
  emitCopy(ctx, *elseValue, result);
  ctx.placeLabel(done);
  return Operand(result.cell, Evaluation::Whnf);
}

Lowered<Operand> lowerBoolCtor(LowerContext &ctx, bool value, SourceLocation) {
  return ctx.emitConstant(CellValue{value});
}

} // namespace xanadu::vlace::lower
