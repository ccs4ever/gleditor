/**
 * @file logic_compile_builtins.cpp
 * @brief Card P2: control and arithmetic builtins (true, fail, !, is, the six
 * comparisons, \\+ and \\=) lowered to ordinary Vortex graph ops.
 *
 * Arithmetic is emitted over dereferenced terms: a constant leaf is a static
 * cell, a variable leaf is `Deref` plus a numeric type test, and each operator
 * is one VM arithmetic op. A variable bound at run time to a compound
 * (`E = 1+2, X is E`) takes the slow path through the internal `'$eval'/2`
 * clause predicate, which walks the term and calls back into `is`.
 */
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
// Internal names: the guards the stdlib's `'$eval'/2` clauses are written with.
constexpr std::string_view kNumberTest      = "$number";
constexpr std::string_view kEvalName        = "$eval";
constexpr std::string_view kVarTest         = "var";
constexpr std::string_view kNonvarTest      = "nonvar";
constexpr std::string_view kInternalVarTest = "$var";
constexpr std::string_view kRaise           = "$raise";
constexpr std::string_view kInstantiation   = "instantiation";

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

// A failure to evaluate is an error, not a quiet backtrack: the query aborts
// with this text (the driver rethrows an Assert's message as a
// std::runtime_error, the way integer overflow already aborts).
void emitRaise(CompileContext &ctx, Chain &chain, std::string message) {
  const std::array<CellRef, 2> inputs{emitConstant(ctx, chain, false),
                                      emitConstant(ctx, chain, message)};
  appendOp(ctx, chain, OpcodeKind::Assert, inputs);
}

[[nodiscard]] std::string render(CompileContext &ctx, CellRef term) {
  return ctx.stdlib.renderTerm(term).value_or("?");
}

// Evaluates @p term into a slot that holds a number at run time, emitting the
// ops. @p site is the rendered goal, named by every error raised for it.
[[nodiscard]] CellRef emitEval(CompileContext &ctx, FrameScope &scope,
                               Chain &chain, CellRef term,
                               const std::string &site);

// A Bool: `bound` is an Int64 or a Double. `x + 0` equals `x` as a variant only
// then: a string, Bool or unbound variable converts to a different
// alternative. That is the one numeric type test the instruction set offers,
// and it stops the arithmetic ops from coercing an atom to 0.
[[nodiscard]] CellRef emitNumericTest(CompileContext &ctx, FrameScope &scope,
                                      Chain &chain, CellRef bound) {
  const CellRef zero    = emitConstant(ctx, chain, std::int64_t{0});
  const CellRef shifted = op2(ctx, scope, chain, OpcodeKind::Add, bound, zero);
  return op2(ctx, scope, chain, OpcodeKind::Eq, bound, shifted);
}

// A variable leaf holds a number at run time or is evaluated: a number takes
// the fast path; an unbound variable raises the instantiation error naming the
// goal; anything else goes to '$eval'(Var, Result), which raises for a
// non-number and otherwise binds Result to the value. The returned slot is the
// same local on every path, so the join needs no copy.
[[nodiscard]] CellRef emitVariableLeaf(CompileContext &ctx, FrameScope &scope,
                                       Chain &chain, CellRef variable,
                                       const std::string &site) {
  const CellRef slot    = variableSlot(ctx, scope, chain, variable);
  const CellRef bound   = op1(ctx, scope, chain, OpcodeKind::Deref, slot);
  const CellRef numeric = emitNumericTest(ctx, scope, chain, bound);

  // The call goal is built over two fresh placeholder variables that are then
  // mapped to the existing slots: makeTerm would otherwise link the clause's
  // own variable cell into a term rank.
  const CellRef argument = ctx.stdlib.makeVar();
  const CellRef result   = ctx.stdlib.makeVar();
  scope.variableSlots.emplace(argument, slot);
  const CellRef resultSlot = variableSlot(ctx, scope, chain, result);
  const CellRef goal       = ctx.stdlib.makeTerm(kEvalName, {argument, result});

  const Label done     = newLabel(ctx);
  const Label evaluate = newLabel(ctx);
  emitBranchIf(ctx, chain, numeric, done);
  const CellRef unbound = op1(ctx, scope, chain, OpcodeKind::IsVar, bound);
  emitBranchUnless(ctx, chain, unbound, evaluate);
  emitRaise(ctx, chain,
            "instantiation error: " + render(ctx, variable) +
                " is unbound in " + site);
  placeLabel(ctx, chain, evaluate);
  emitUserCall(ctx, scope, chain, PredicateKey{std::string(kEvalName), kBinary},
               goal);
  const std::array<CellRef, 1> inputs{resultSlot};
  const std::array<CellRef, 1> outputs{bound};
  appendOp(ctx, chain, OpcodeKind::Deref, inputs, outputs);
  placeLabel(ctx, chain, done);
  scope.variableSlots.erase(argument);
  scope.variableSlots.erase(result);
  return bound;
}

// `value` as a Double, whatever it was.
[[nodiscard]] CellRef asDouble(CompileContext &ctx, FrameScope &scope,
                               Chain &chain, CellRef value) {
  return op2(ctx, scope, chain, OpcodeKind::Add, value,
             emitConstant(ctx, chain, 0.0));
}

// Truncates @p value to an Int64 and raises on zero: the divisor rule of `//`,
// `mod` and `rem`. DivMod by one is the VM's only truncating cast.
[[nodiscard]] CellRef emitNonZeroDivisor(CompileContext &ctx, FrameScope &scope,
                                         Chain &chain, CellRef value,
                                         const std::string &site) {
  const CellRef truncated = newLocal(ctx, scope);
  const CellRef remainder = newLocal(ctx, scope);
  const std::array<CellRef, 2> inputs{
      value, emitConstant(ctx, chain, std::int64_t{1})};
  const std::array<CellRef, 2> outputs{truncated, remainder};
  appendOp(ctx, chain, OpcodeKind::DivMod, inputs, outputs);
  const CellRef nonZero = op2(ctx, scope, chain, OpcodeKind::Neq, truncated,
                              emitConstant(ctx, chain, std::int64_t{0}));
  const Label proceed   = newLabel(ctx);
  emitBranchIf(ctx, chain, nonZero, proceed);
  emitRaise(ctx, chain, "evaluation error: zero divisor in " + site);
  placeLabel(ctx, chain, proceed);
  return truncated;
}

// The VM's Mod keeps the dividend's sign (rem); `mod` takes the divisor's, so
// a non-zero remainder whose sign differs from the divisor's is shifted by it.
[[nodiscard]] CellRef emitFlooredMod(CompileContext &ctx, FrameScope &scope,
                                     Chain &chain, CellRef left,
                                     CellRef divisor) {
  const CellRef zero = emitConstant(ctx, chain, std::int64_t{0});
  const CellRef remainder =
      op2(ctx, scope, chain, OpcodeKind::Mod, left, divisor);
  const CellRef nonZero =
      op2(ctx, scope, chain, OpcodeKind::Neq, remainder, zero);
  const CellRef remNeg =
      op2(ctx, scope, chain, OpcodeKind::Lt, remainder, zero);
  const CellRef divNeg = op2(ctx, scope, chain, OpcodeKind::Lt, divisor, zero);
  const CellRef differ =
      op2(ctx, scope, chain, OpcodeKind::Neq, remNeg, divNeg);
  const CellRef shift =
      op2(ctx, scope, chain, OpcodeKind::And, nonZero, differ);
  const Label keep = newLabel(ctx);
  emitBranchUnless(ctx, chain, shift, keep);
  const std::array<CellRef, 2> inputs{remainder, divisor};
  const std::array<CellRef, 1> outputs{remainder};
  appendOp(ctx, chain, OpcodeKind::Add, inputs, outputs);
  placeLabel(ctx, chain, keep);
  return remainder;
}

[[nodiscard]] std::optional<CellRef>
emitCompound(CompileContext &ctx, FrameScope &scope, Chain &chain, CellRef term,
             std::string_view functor, const std::string &site) {
  const auto args = ctx.stdlib.argumentsOf(term);
  if (args.size() == kUnary && (functor == "-" || functor == "+")) {
    const CellRef operand = emitEval(ctx, scope, chain, args[0], site);
    if (functor == "+") return operand;
    return op1(ctx, scope, chain, OpcodeKind::Neg, operand);
  }
  if (args.size() != kBinary) return std::nullopt;
  const bool known = functor == "+" || functor == "-" || functor == "*" ||
                     functor == "/" || functor == "//" || functor == "mod" ||
                     functor == "rem";
  if (!known) return std::nullopt;

  const CellRef left  = emitEval(ctx, scope, chain, args[0], site);
  const CellRef right = emitEval(ctx, scope, chain, args[1], site);

  if (functor == "+")
    return op2(ctx, scope, chain, OpcodeKind::Add, left, right);
  if (functor == "-")
    return op2(ctx, scope, chain, OpcodeKind::Sub, left, right);
  if (functor == "*")
    return op2(ctx, scope, chain, OpcodeKind::Mul, left, right);
  if (functor == "/") {
    // The VM's Div is integer division on two integers and answers 0 for a
    // zero divisor; `/` is always a float quotient.
    const CellRef dividend = asDouble(ctx, scope, chain, left);
    const CellRef divisor  = asDouble(ctx, scope, chain, right);
    const CellRef nonZero  = op2(ctx, scope, chain, OpcodeKind::Neq, divisor,
                                 emitConstant(ctx, chain, 0.0));
    const Label proceed    = newLabel(ctx);
    emitBranchIf(ctx, chain, nonZero, proceed);
    emitRaise(ctx, chain, "evaluation error: zero divisor in " + site);
    placeLabel(ctx, chain, proceed);
    return op2(ctx, scope, chain, OpcodeKind::Div, dividend, divisor);
  }
  const CellRef divisor = emitNonZeroDivisor(ctx, scope, chain, right, site);
  if (functor == "mod") return emitFlooredMod(ctx, scope, chain, left, divisor);
  if (functor == "rem")
    return op2(ctx, scope, chain, OpcodeKind::Mod, left, divisor);
  const CellRef quotient  = newLocal(ctx, scope);
  const CellRef remainder = newLocal(ctx, scope);
  const std::array<CellRef, 2> inputs{left, divisor};
  const std::array<CellRef, 2> outputs{quotient, remainder};
  appendOp(ctx, chain, OpcodeKind::DivMod, inputs, outputs);
  return quotient;
}

CellRef emitEval(CompileContext &ctx, FrameScope &scope, Chain &chain,
                 CellRef term, const std::string &site) {
  const auto actual = ctx.stdlib.deref(term);
  if (!actual) throw LogicRefusal("arithmetic operand is not a cell");
  if (ctx.stdlib.isVar(*actual))
    return emitVariableLeaf(ctx, scope, chain, *actual, site);

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
      functor && !ctx.stdlib.argumentsOf(*actual).empty()) {
    if (const auto value =
            emitCompound(ctx, scope, chain, *actual, *functor, site))
      return *value;
  }
  // An atom (quoted digits included), a Bool or an unknown functor. The raise
  // always aborts; the constant only keeps the surrounding ops well formed.
  emitRaise(ctx, chain,
            "type error: " + render(ctx, *actual) + " is not a number in " +
                site);
  return emitConstant(ctx, chain, std::int64_t{0});
}

void emitIs(CompileContext &ctx, FrameScope &scope, Chain &chain,
            CellRef goal) {
  const auto args     = ctx.stdlib.argumentsOf(goal);
  const CellRef value = emitEval(ctx, scope, chain, args[1], render(ctx, goal));
  const CellRef target = emitTerm(ctx, scope, chain, args[0]);
  emitUnifyOrFail(ctx, scope, chain, target, value);
}

void emitComparison(CompileContext &ctx, FrameScope &scope, Chain &chain,
                    const PredicateKey &key, CellRef goal) {
  const auto args        = ctx.stdlib.argumentsOf(goal);
  const std::string site = render(ctx, goal);
  const CellRef left     = emitEval(ctx, scope, chain, args[0], site);
  const CellRef right    = emitEval(ctx, scope, chain, args[1], site);
  CellRef holds{};
  if (key.name == kLess) {
    holds = op2(ctx, scope, chain, OpcodeKind::Lt, left, right);
  } else if (key.name == kGreater) {
    holds = op2(ctx, scope, chain, OpcodeKind::Gt, left, right);
  } else if (key.name == kLessEqual) {
    holds = op2(ctx, scope, chain, OpcodeKind::Lte, left, right);
  } else if (key.name == kGreaterEqual) {
    holds = op2(ctx, scope, chain, OpcodeKind::Gte, left, right);
  } else if (key.name == kNumericEqual) {
    // Eq compares variants, so 1 =:= 1.0 would be false; Lte and Gte compare
    // numerically and stay exact on two integers.
    const CellRef below = op2(ctx, scope, chain, OpcodeKind::Lte, left, right);
    const CellRef above = op2(ctx, scope, chain, OpcodeKind::Gte, left, right);
    holds               = op2(ctx, scope, chain, OpcodeKind::And, below, above);
  } else {
    const CellRef below = op2(ctx, scope, chain, OpcodeKind::Lt, left, right);
    const CellRef above = op2(ctx, scope, chain, OpcodeKind::Gt, left, right);
    holds               = op2(ctx, scope, chain, OpcodeKind::Or, below, above);
  }
  emitFailUnless(ctx, scope, chain, holds);
}

// '$number', '$var', var and nonvar: one VM test on the dereferenced argument.
void emitTypeTest(CompileContext &ctx, FrameScope &scope, Chain &chain,
                  const PredicateKey &key, CellRef argument) {
  if (key.name == kRaise) {
    // The inner operands of a runtime-bound expression, where the goal text is
    // no longer known; the top-level goal is named by the static checks.
    emitRaise(ctx, chain,
              ctx.stdlib.functorOf(argument) == kInstantiation
                  ? "instantiation error: unbound variable in an arithmetic "
                    "expression"
                  : "type error: a non-number in an arithmetic expression");
    return;
  }
  const CellRef term = emitTerm(ctx, scope, chain, argument);
  if (key.name == kNumberTest) {
    const CellRef bound = op1(ctx, scope, chain, OpcodeKind::Deref, term);
    emitFailUnless(ctx, scope, chain,
                   emitNumericTest(ctx, scope, chain, bound));
    return;
  }
  const CellRef unbound = op1(ctx, scope, chain, OpcodeKind::IsVar, term);
  emitFailUnless(ctx, scope, chain,
                 key.name == kNonvarTest
                     ? op1(ctx, scope, chain, OpcodeKind::Not, unbound)
                     : unbound);
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
  if (key.arity == kUnary)
    return key.name == kNegation || key.name == kNumberTest ||
           key.name == kInternalVarTest || key.name == kRaise ||
           key.name == kVarTest || key.name == kNonvarTest;
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
  if (key.arity == kUnary && key.name != kNegation) {
    emitTypeTest(ctx, scope, chain, key, args[0]);
    return;
  }
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
