/**
 * @file logic_compile_builtins.cpp
 * @brief Card P2: control and arithmetic builtins (true, fail, !, is, the six
 * comparisons, \\+ and \\=) lowered to ordinary Vortex graph ops.
 *
 * Arithmetic is emitted over dereferenced terms: a constant leaf is a static
 * cell, a variable leaf is `Deref` plus a numeric type test, and each operator
 * is one VM arithmetic op. Nothing here walks a term at run time, so a
 * variable that is bound to a compound expression (`E = 1+2, X is E`) is not
 * evaluated: it fails like any other non-number (a divergence from the old
 * solver, which re-read the term; see the card report).
 */
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "common/xanadu/vortex/logic_compile.hpp"

namespace zigzag::vortex::logic {

namespace {

constexpr std::string_view kNegation       = "\\+";
constexpr std::string_view kNotUnifiable   = "\\=";
constexpr std::string_view kIs             = "is";
constexpr std::string_view kConjunction    = ",";
constexpr std::string_view kCut            = "!";
constexpr std::string_view kTrue           = "true";
constexpr std::string_view kFail           = "fail";
constexpr std::string_view kLess           = "<";
constexpr std::string_view kGreater        = ">";
constexpr std::string_view kLessEqual      = "=<";
constexpr std::string_view kGreaterEqual   = ">=";
constexpr std::string_view kNumericEqual   = "=:=";
constexpr std::string_view kNumericUnequal = "=\\=";

constexpr std::size_t kNoArguments = 0;
constexpr std::size_t kUnary       = 1;
constexpr std::size_t kBinary      = 2;

[[nodiscard]] bool isComparison(std::string_view name) {
  return name == kLess || name == kGreater || name == kLessEqual ||
         name == kGreaterEqual || name == kNumericEqual ||
         name == kNumericUnequal;
}

CellRef op1(CompileContext &ctx, FrameScope &scope, Chain &chain,
            OpcodeKind kind, CellRef a) {
  const CellRef out = newLocal(ctx, scope);
  const std::array<CellRef, 1> inputs{a};
  const std::array<CellRef, 1> outputs{out};
  appendOp(ctx, chain, kind, inputs, outputs);
  return out;
}

CellRef op2(CompileContext &ctx, FrameScope &scope, Chain &chain,
            OpcodeKind kind, CellRef a, CellRef b) {
  const CellRef out = newLocal(ctx, scope);
  const std::array<CellRef, 2> inputs{a, b};
  const std::array<CellRef, 1> outputs{out};
  appendOp(ctx, chain, kind, inputs, outputs);
  return out;
}

// Backtracks. A real Jump (not a branch on a constant) so the intent reads in a
// graph dump; the target is named by a static Int64 cell, as Jump requires.
void emitFailNow(CompileContext &ctx, FrameScope &scope, Chain &chain) {
  const std::array<CellRef, 1> inputs{
      emitConstant(ctx, chain, static_cast<std::int64_t>(scope.failOp))};
  appendOp(ctx, chain, OpcodeKind::Jump, inputs);
}

// The old evaluator's text leaf: digits (optionally negative) with a decimal
// point meaning float, otherwise integer; anything else is not a number.
[[nodiscard]] std::optional<ScalarConstant> numericText(std::string_view text) {
  if (text.empty()) return std::nullopt;
  const auto digit = [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  };
  if (!digit(text[0]) && !(text.size() > 1 && text[0] == '-' && digit(text[1])))
    return std::nullopt;
  const char *const first = text.data();
  const char *const last  = text.data() + text.size();
  if (text.contains('.')) {
    double value{};
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{}) return std::nullopt;
    return ScalarConstant{value};
  }
  std::int64_t value{};
  const auto parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{}) return std::nullopt;
  return ScalarConstant{value};
}

// Evaluates @p term into a slot that holds a number at run time, emitting the
// ops. Nullopt means the term can never evaluate (an atom, an unknown
// functor): the caller emits a failure, which is what the old solver did.
[[nodiscard]] std::optional<CellRef>
emitEval(CompileContext &ctx, FrameScope &scope, Chain &chain, CellRef term);

[[nodiscard]] CellRef emitVariableLeaf(CompileContext &ctx, FrameScope &scope,
                                       Chain &chain, CellRef variable) {
  const CellRef slot  = variableSlot(ctx, scope, chain, variable);
  const CellRef bound = op1(ctx, scope, chain, OpcodeKind::Deref, slot);
  // `x + 0` equals `x` as a variant only when x is an Int64 or a Double: a
  // string, Bool or unbound variable converts to a different alternative. That
  // is the one numeric type test the instruction set offers, and it stops the
  // arithmetic ops from coercing an atom to 0.
  const CellRef zero    = emitConstant(ctx, chain, std::int64_t{0});
  const CellRef shifted = op2(ctx, scope, chain, OpcodeKind::Add, bound, zero);
  const CellRef numeric =
      op2(ctx, scope, chain, OpcodeKind::Eq, bound, shifted);
  emitFailUnless(ctx, scope, chain, numeric);
  return bound;
}

// `value` as a Double, whatever it was.
[[nodiscard]] CellRef asDouble(CompileContext &ctx, FrameScope &scope,
                               Chain &chain, CellRef value) {
  return op2(ctx, scope, chain, OpcodeKind::Add, value,
             emitConstant(ctx, chain, 0.0));
}

// Truncates @p value to an Int64 and fails on zero: the divisor rule of `//`
// and `mod`. DivMod by one is the VM's only truncating cast.
[[nodiscard]] CellRef emitNonZeroDivisor(CompileContext &ctx, FrameScope &scope,
                                         Chain &chain, CellRef value) {
  const CellRef truncated = newLocal(ctx, scope);
  const CellRef remainder = newLocal(ctx, scope);
  const std::array<CellRef, 2> inputs{
      value, emitConstant(ctx, chain, std::int64_t{1})};
  const std::array<CellRef, 2> outputs{truncated, remainder};
  appendOp(ctx, chain, OpcodeKind::DivMod, inputs, outputs);
  const CellRef nonZero = op2(ctx, scope, chain, OpcodeKind::Neq, truncated,
                              emitConstant(ctx, chain, std::int64_t{0}));
  emitFailUnless(ctx, scope, chain, nonZero);
  return truncated;
}

[[nodiscard]] std::optional<CellRef> emitCompound(CompileContext &ctx,
                                                  FrameScope &scope,
                                                  Chain &chain, CellRef term,
                                                  std::string_view functor) {
  const auto args = ctx.stdlib.argumentsOf(term);
  if (args.size() == kUnary && (functor == "-" || functor == "+")) {
    const auto operand = emitEval(ctx, scope, chain, args[0]);
    if (!operand) return std::nullopt;
    if (functor == "+") return operand;
    return op1(ctx, scope, chain, OpcodeKind::Neg, *operand);
  }
  if (args.size() != kBinary) return std::nullopt;
  const bool known = functor == "+" || functor == "-" || functor == "*" ||
                     functor == "/" || functor == "//" || functor == "mod";
  if (!known) return std::nullopt;

  const auto left = emitEval(ctx, scope, chain, args[0]);
  if (!left) return std::nullopt;
  const auto right = emitEval(ctx, scope, chain, args[1]);
  if (!right) return std::nullopt;

  if (functor == "+")
    return op2(ctx, scope, chain, OpcodeKind::Add, *left, *right);
  if (functor == "-")
    return op2(ctx, scope, chain, OpcodeKind::Sub, *left, *right);
  if (functor == "*")
    return op2(ctx, scope, chain, OpcodeKind::Mul, *left, *right);
  if (functor == "/") {
    // The VM's Div is integer division on two integers and answers 0 for a
    // zero divisor; the old `/` was always a float quotient that failed on
    // zero.
    const CellRef dividend = asDouble(ctx, scope, chain, *left);
    const CellRef divisor  = asDouble(ctx, scope, chain, *right);
    const CellRef nonZero  = op2(ctx, scope, chain, OpcodeKind::Neq, divisor,
                                 emitConstant(ctx, chain, 0.0));
    emitFailUnless(ctx, scope, chain, nonZero);
    return op2(ctx, scope, chain, OpcodeKind::Div, dividend, divisor);
  }
  const CellRef divisor = emitNonZeroDivisor(ctx, scope, chain, *right);
  if (functor == "mod")
    return op2(ctx, scope, chain, OpcodeKind::Mod, *left, divisor);
  const CellRef quotient  = newLocal(ctx, scope);
  const CellRef remainder = newLocal(ctx, scope);
  const std::array<CellRef, 2> inputs{*left, divisor};
  const std::array<CellRef, 2> outputs{quotient, remainder};
  appendOp(ctx, chain, OpcodeKind::DivMod, inputs, outputs);
  return quotient;
}

std::optional<CellRef> emitEval(CompileContext &ctx, FrameScope &scope,
                                Chain &chain, CellRef term) {
  const auto actual = ctx.stdlib.deref(term);
  if (!actual) throw LogicRefusal("arithmetic operand is not a cell");
  if (ctx.stdlib.isVar(*actual))
    return emitVariableLeaf(ctx, scope, chain, *actual);

  auto &arena = ctx.core.arena();
  switch (arena.valueKindOf(*actual)) {
  case xanadu::ValueKind::Int64:
    if (const auto v = arena.asInt64(*actual))
      return emitConstant(ctx, chain, *v);
    break;
  case xanadu::ValueKind::Double:
    if (const auto v = arena.asDouble(*actual))
      return emitConstant(ctx, chain, *v);
    break;
  default:
    break;
  }

  if (const auto functor = ctx.stdlib.functorOf(*actual);
      functor && !ctx.stdlib.argumentsOf(*actual).empty())
    return emitCompound(ctx, scope, chain, *actual, *functor);
  if (const auto number = numericText(arena.textOf(*actual)))
    return emitConstant(ctx, chain, *number);
  return std::nullopt;
}

void emitIs(CompileContext &ctx, FrameScope &scope, Chain &chain,
            CellRef goal) {
  const auto args  = ctx.stdlib.argumentsOf(goal);
  const auto value = emitEval(ctx, scope, chain, args[1]);
  if (!value) {
    emitFailNow(ctx, scope, chain);
    return;
  }
  const CellRef target = emitTerm(ctx, scope, chain, args[0]);
  emitUnifyOrFail(ctx, scope, chain, target, *value);
}

void emitComparison(CompileContext &ctx, FrameScope &scope, Chain &chain,
                    const PredicateKey &key, CellRef goal) {
  const auto args = ctx.stdlib.argumentsOf(goal);
  const auto left = emitEval(ctx, scope, chain, args[0]);
  if (!left) {
    emitFailNow(ctx, scope, chain);
    return;
  }
  const auto right = emitEval(ctx, scope, chain, args[1]);
  if (!right) {
    emitFailNow(ctx, scope, chain);
    return;
  }
  CellRef holds{};
  if (key.name == kLess) {
    holds = op2(ctx, scope, chain, OpcodeKind::Lt, *left, *right);
  } else if (key.name == kGreater) {
    holds = op2(ctx, scope, chain, OpcodeKind::Gt, *left, *right);
  } else if (key.name == kLessEqual) {
    holds = op2(ctx, scope, chain, OpcodeKind::Lte, *left, *right);
  } else if (key.name == kGreaterEqual) {
    holds = op2(ctx, scope, chain, OpcodeKind::Gte, *left, *right);
  } else if (key.name == kNumericEqual) {
    // Eq compares variants, so 1 =:= 1.0 would be false; Lte and Gte compare
    // numerically and stay exact on two integers.
    const CellRef below =
        op2(ctx, scope, chain, OpcodeKind::Lte, *left, *right);
    const CellRef above =
        op2(ctx, scope, chain, OpcodeKind::Gte, *left, *right);
    holds = op2(ctx, scope, chain, OpcodeKind::And, below, above);
  } else {
    const CellRef below = op2(ctx, scope, chain, OpcodeKind::Lt, *left, *right);
    const CellRef above = op2(ctx, scope, chain, OpcodeKind::Gt, *left, *right);
    holds               = op2(ctx, scope, chain, OpcodeKind::Or, below, above);
  }
  emitFailUnless(ctx, scope, chain, holds);
}

// `\+ G` and `\=` share the shape: push a CONT choice, run a test that FAILS
// when the goal does not hold (backtracking into CONT, which rolls the test's
// bindings back and continues), and when the test succeeds cut CONT away and
// fail. The caller supplies the test, emitted inside the negation scope.
template <typename Test>
void emitNegated(CompileContext &ctx, FrameScope &scope, Chain &chain,
                 Test &&test) {
  const NegationScope negation = openNegationScope(ctx, scope, chain);
  const Label cont             = newLabel(ctx);
  emitChoice(ctx, chain, cont);
  test();
  closeNegationScope(scope, negation);
  emitCutTo(ctx, chain, negation.barrier);
  emitFailNow(ctx, scope, chain);
  placeLabel(ctx, chain, cont);
}

} // namespace

bool isControlBuiltin(const PredicateKey &key) {
  if (key.arity == kNoArguments)
    return key.name == kTrue || key.name == kFail || key.name == kCut;
  if (key.arity == kUnary) return key.name == kNegation;
  if (key.arity == kBinary)
    return key.name == kIs || key.name == kNotUnifiable ||
           key.name == kConjunction || isComparison(key.name);
  return false;
}

void emitControlBuiltin(CompileContext &ctx, FrameScope &scope, Chain &chain,
                        const PredicateKey &key, CellRef goal) {
  if (!isControlBuiltin(key)) refuseUnlowered("P2", "builtin " + key.display());
  if (key.name == kTrue) return;
  if (key.name == kFail) {
    emitFailNow(ctx, scope, chain);
    return;
  }
  if (key.name == kCut) {
    emitCut(ctx, scope, chain);
    return;
  }
  const auto args = ctx.stdlib.argumentsOf(goal);
  if (key.name == kIs) {
    emitIs(ctx, scope, chain, goal);
  } else if (key.name == kConjunction) {
    // Only reachable as a goal argument (`\+ (a, b)`); a clause body is
    // already a goal list. The old solver found no predicate named "," and
    // failed, which made every negated conjunction succeed.
    emitGoal(ctx, scope, chain, args[0]);
    emitGoal(ctx, scope, chain, args[1]);
  } else if (key.name == kNegation) {
    emitNegated(ctx, scope, chain,
                [&] { emitGoal(ctx, scope, chain, args[0]); });
  } else if (key.name == kNotUnifiable) {
    emitNegated(ctx, scope, chain, [&] {
      const CellRef left  = emitTerm(ctx, scope, chain, args[0]);
      const CellRef right = emitTerm(ctx, scope, chain, args[1]);
      emitUnifyOrFail(ctx, scope, chain, left, right);
    });
  } else {
    emitComparison(ctx, scope, chain, key, goal);
  }
}

} // namespace zigzag::vortex::logic
