/**
 * @file lower_support.hpp
 * @brief Small emission helpers shared by the verb-lowering files.
 *
 * These sit above graph_emit.hpp (they are built from it and LowerContext, and
 * add nothing to its frozen interface). They are here so that the same
 * run-time check, with the same message shape, is not copied into each card's
 * file.
 */
#ifndef COMMON_XANADU_VPL_LOWER_SUPPORT_HPP
#define COMMON_XANADU_VPL_LOWER_SUPPORT_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

/// The value of a literal number as written: `3`, `2.5`, `¯3`, or `-3` (a
/// Minus over a literal). Nothing else is folded, so a computed operand is
/// std::nullopt.
[[nodiscard]] inline std::optional<double> literalNumber(const AstNode *node) {
  if (const auto *scalar = dynamic_cast<const ScalarExpr *>(node)) {
    if (scalar->isString()) return std::nullopt;
    return scalar->floatValue();
  }
  if (const auto *monadic = dynamic_cast<const MonadicExpr *>(node)) {
    if (monadic->customVerb() || monadic->verb() != TokenKind::Minus) {
      return std::nullopt;
    }
    if (const auto value = literalNumber(monadic->right().get())) {
      return -*value;
    }
  }
  return std::nullopt;
}

/// Emits an Assert on the cell `condition` with `message`: the VM stops with a
/// run-time failure carrying that text when the condition is false.
inline void assertCell(LowerContext &ctx, CellRef condition,
                       std::string_view label, std::string_view message) {
  const auto assertion = ctx.emitOp(zigzag::vortex::OpcodeKind::Assert, label);
  ctx.emitInput(assertion, condition);
  ctx.emitInputConstant(assertion, std::string{message});
}

/// Emits Assert(0 <= count) with `message`.
inline void assertNotNegative(LowerContext &ctx, CellRef count,
                              std::string_view label,
                              std::string_view message) {
  // Zero is the least count APL accepts; a smaller one is a DOMAIN ERROR.
  constexpr std::int64_t kLeastCount = 0;
  const auto valid                   = ctx.core().arena().makeCell();
  const auto test = ctx.emitOp(zigzag::vortex::OpcodeKind::Lte, label);
  ctx.emitInputConstant(test, kLeastCount);
  ctx.emitInput(test, count);
  ctx.emitOutput(test, valid);
  assertCell(ctx, valid, label, message);
}

/// The opcode and trace label of a scalar dyadic verb, as the scalar path of
/// compileDyadic emits it. One table, so scalar arithmetic, element-wise
/// arithmetic on ranks and fold cannot disagree about what a glyph means.
struct ScalarVerb {
  zigzag::vortex::OpcodeKind opcode;
  std::string_view label;
};

[[nodiscard]] inline std::optional<ScalarVerb>
scalarDyadicVerb(const TokenKind verb) {
  using zigzag::vortex::OpcodeKind;
  switch (verb) {
  case TokenKind::Plus:
    return ScalarVerb{OpcodeKind::Add, "#ADD"};
  case TokenKind::Minus:
    return ScalarVerb{OpcodeKind::Sub, "#SUB"};
  case TokenKind::Times:
    return ScalarVerb{OpcodeKind::Mul, "#MUL"};
  case TokenKind::Divide:
    return ScalarVerb{OpcodeKind::Div, "#DIV"};
  case TokenKind::Magnitude:
    return ScalarVerb{OpcodeKind::Mod, "#MOD"};
  case TokenKind::Ceiling:
    return ScalarVerb{OpcodeKind::Max, "#MAX"};
  case TokenKind::Floor:
    return ScalarVerb{OpcodeKind::Min, "#MIN"};
  case TokenKind::Equal:
    return ScalarVerb{OpcodeKind::Eq, "#EQ"};
  case TokenKind::NotEqual:
    return ScalarVerb{OpcodeKind::Neq, "#NEQ"};
  case TokenKind::LessThan:
    return ScalarVerb{OpcodeKind::Lt, "#LT"};
  case TokenKind::LessEqual:
    return ScalarVerb{OpcodeKind::Lte, "#LTE"};
  case TokenKind::GreaterThan:
    return ScalarVerb{OpcodeKind::Gt, "#GT"};
  case TokenKind::GreaterEqual:
    return ScalarVerb{OpcodeKind::Gte, "#GTE"};
  case TokenKind::And:
    return ScalarVerb{OpcodeKind::And, "#AND"};
  case TokenKind::Or:
    return ScalarVerb{OpcodeKind::Or, "#OR"};
  default:
    return std::nullopt;
  }
}

/// Integer constants the emitters use for what they mean, not as bare digits.
inline constexpr std::int64_t kNoneYet = 0; ///< a counter before its first step
inline constexpr std::int64_t kOneStep = 1; ///< one cell, one position
inline constexpr double kZeroDouble    = 0.0;

/// Emits `out := left <opcode> right` on two cells, or on a cell and an
/// integer constant, and returns `out`.
inline CellRef emitBinaryOp(LowerContext &ctx,
                            zigzag::vortex::OpcodeKind opcode,
                            std::string_view label, CellRef left,
                            CellRef right) {
  const auto out = ctx.core().arena().makeCell();
  const auto op  = ctx.emitOp(opcode, label);
  ctx.emitInput(op, left);
  ctx.emitInput(op, right);
  ctx.emitOutput(op, out);
  return out;
}

inline CellRef emitBinaryOp(LowerContext &ctx,
                            zigzag::vortex::OpcodeKind opcode,
                            std::string_view label, CellRef left,
                            std::int64_t right) {
  const auto out = ctx.core().arena().makeCell();
  const auto op  = ctx.emitOp(opcode, label);
  ctx.emitInput(op, left);
  ctx.emitInputConstant(op, right);
  ctx.emitOutput(op, out);
  return out;
}

inline CellRef emitUnaryOp(LowerContext &ctx, zigzag::vortex::OpcodeKind opcode,
                           std::string_view label, CellRef operand) {
  const auto out = ctx.core().arena().makeCell();
  const auto op  = ctx.emitOp(opcode, label);
  ctx.emitInput(op, operand);
  ctx.emitOutput(op, out);
  return out;
}

/// A Bool register: true when `value` is a number (int, double or bool) and
/// false for text. The VM coerces text in arithmetic (`"x"` is 0, `"5"` is 5),
/// so a value's own type is the only honest test: `x + 0` keeps a number's
/// type and turns text into an integer, so it equals `x` only for a number;
/// `¬¬x` is a Bool, which equals `x` only for one.
[[nodiscard]] inline CellRef emitIsNumeric(LowerContext &ctx, CellRef value) {
  using zigzag::vortex::OpcodeKind;
  const auto sum =
      emitBinaryOp(ctx, OpcodeKind::Add, "#NUM_PLUS_ZERO", value, kNoneYet);
  const auto keepsType =
      emitBinaryOp(ctx, OpcodeKind::Eq, "#NUM_KEEPS_TYPE", value, sum);
  const auto asBool =
      emitUnaryOp(ctx, OpcodeKind::Not, "#NUM_NOT_NOT",
                  emitUnaryOp(ctx, OpcodeKind::Not, "#NUM_NOT", value));
  const auto isBool =
      emitBinaryOp(ctx, OpcodeKind::Eq, "#NUM_IS_BOOL", value, asBool);
  return emitBinaryOp(ctx, OpcodeKind::Or, "#NUM_OR", keepsType, isBool);
}

/// True when the cell already holds a number as the program is compiled: a
/// literal, or a stored scalar typed as one. Nothing writes such a cell at run
/// time but a copy of another number, so no run-time test is owed.
[[nodiscard]] inline bool isKnownNumber(LowerContext &ctx, CellRef cell) {
  auto &arena = ctx.core().arena();
  return arena.contains(cell) &&
         (arena.asInt64(cell) || arena.asDouble(cell) || arena.asBool(cell));
}

/// Fails at run time, with `message`, unless `value` is a number.
inline void assertNumeric(LowerContext &ctx, CellRef value,
                          std::string_view label, std::string_view message) {
  if (isKnownNumber(ctx, value)) return;
  assertCell(ctx, emitIsNumeric(ctx, value), label, message);
}

/// Emits the run-time checks that `count` is a whole number and returns an
/// integer-typed register holding it. A count is read by the VM through
/// arithmetic that coerces, so text such as "2" or "x" would otherwise be
/// taken for a number; the checks use only the variant type of the values.
/// Take, drop and indexing share it so a literal and a computed count agree.
inline CellRef checkedWholeNumber(LowerContext &ctx, CellRef count,
                                  std::string_view notNumber,
                                  std::string_view notWhole) {
  using zigzag::vortex::OpcodeKind;
  // Largest int64: truncating `count % max` leaves any in-range count as it
  // is, as an integer-typed value, whatever type it came in.
  constexpr std::int64_t kTruncate = std::numeric_limits<std::int64_t>::max();
  // A count beyond this is clamped to it: no rank holds so many cells, a
  // double is exact for every whole value up to 2^53 (about 9e15), and the
  // clamp keeps `9223372036854775807 ↓ A` and `1e30 ↓ A` the empty array they
  // mean instead of an arithmetic overflow.
  constexpr double kLargestCount = 1e15;

  assertNumeric(ctx, count, "#COUNT_NUMBER", notNumber);

  const auto clamp = ctx.emitOp(OpcodeKind::Clamp, "#COUNT_CLAMP");
  ctx.emitInput(clamp, count);
  ctx.emitInputConstant(clamp, -kLargestCount);
  ctx.emitInputConstant(clamp, kLargestCount);
  const auto bounded = ctx.core().arena().makeCell();
  ctx.emitOutput(clamp, bounded);

  const auto whole =
      emitBinaryOp(ctx, OpcodeKind::Mod, "#COUNT_TRUNCATE", bounded, kTruncate);
  const auto fraction =
      emitBinaryOp(ctx, OpcodeKind::Sub, "#COUNT_FRACTION", bounded, whole);
  // Zero is compared by order, not Eq: Eq on a double 0.0 and an int 0 differ.
  const auto atLeast = emitBinaryOp(ctx, OpcodeKind::Gte, "#COUNT_FRACTION_GE",
                                    fraction, kNoneYet);
  const auto atMost  = emitBinaryOp(ctx, OpcodeKind::Lte, "#COUNT_FRACTION_LE",
                                    fraction, kNoneYet);
  const auto isWhole =
      emitBinaryOp(ctx, OpcodeKind::And, "#COUNT_IS_WHOLE", atLeast, atMost);
  assertCell(ctx, isWhole, "#COUNT_WHOLE", notWhole);
  return whole;
}

/// A Bool register: `left = right`. Two numbers compare by value, so the int 2
/// and the double 2.0 are equal (the VM's own Eq compares the stored types);
/// anything else falls back to Eq, so text is never read as 0.
[[nodiscard]] inline CellRef emitEquals(LowerContext &ctx, CellRef left,
                                        CellRef right) {
  using zigzag::vortex::OpcodeKind;
  const auto both =
      emitBinaryOp(ctx, OpcodeKind::And, "#EQ_BOTH_NUMBERS",
                   emitIsNumeric(ctx, left), emitIsNumeric(ctx, right));
  const auto byValue =
      emitBinaryOp(ctx, OpcodeKind::And, "#EQ_BY_VALUE",
                   emitBinaryOp(ctx, OpcodeKind::Lte, "#EQ_LTE", left, right),
                   emitBinaryOp(ctx, OpcodeKind::Gte, "#EQ_GTE", left, right));
  const auto plain =
      emitBinaryOp(ctx, OpcodeKind::Eq, "#EQ_PLAIN", left, right);
  const auto numeric =
      emitBinaryOp(ctx, OpcodeKind::And, "#EQ_NUMERIC", both, byValue);
  const auto textual = emitBinaryOp(
      ctx, OpcodeKind::And, "#EQ_TEXTUAL",
      emitUnaryOp(ctx, OpcodeKind::Not, "#EQ_NOT_BOTH", both), plain);
  return emitBinaryOp(ctx, OpcodeKind::Or, "#EQ_RESULT", numeric, textual);
}

/// `left ÷ right` on doubles: `5 ÷ 2` is 2.5, as in APL, not the VM's integer
/// quotient, and a whole result is a double with a whole value. The spec is
/// silent on a zero divisor and the VM would answer 0, a silent wrong value,
/// so it fails by name.
[[nodiscard]] inline CellRef emitDivide(LowerContext &ctx, CellRef left,
                                        CellRef right) {
  using zigzag::vortex::OpcodeKind;
  const auto asDouble = [&](CellRef value) {
    const auto out = ctx.core().arena().makeCell();
    const auto op  = ctx.emitOp(OpcodeKind::Add, "#DIV_AS_DOUBLE");
    ctx.emitInput(op, value);
    ctx.emitInputConstant(op, kZeroDouble);
    ctx.emitOutput(op, out);
    return out;
  };
  const auto divisor = asDouble(right);
  const auto nonZero = ctx.core().arena().makeCell();
  const auto test    = ctx.emitOp(OpcodeKind::Neq, "#DIV_NONZERO");
  ctx.emitInput(test, divisor);
  ctx.emitInputConstant(test, kZeroDouble);
  ctx.emitOutput(test, nonZero);
  assertCell(ctx, nonZero, "#DIV_NONZERO", "VPL divide by zero (DOMAIN ERROR)");
  return emitBinaryOp(ctx, OpcodeKind::Div, "#DIV", asDouble(left), divisor);
}

/// `1 ÷ value`, with the same zero refusal.
[[nodiscard]] inline CellRef emitReciprocal(LowerContext &ctx, CellRef value) {
  using zigzag::vortex::OpcodeKind;
  const auto one = ctx.core().arena().makeCell();
  const auto op  = ctx.emitOp(OpcodeKind::Add, "#RECIPROCAL_ONE");
  ctx.emitInputConstant(op, 1.0);
  ctx.emitInputConstant(op, kZeroDouble);
  ctx.emitOutput(op, one);
  return emitDivide(ctx, one, value);
}

/// True for the verbs that are arithmetic on numbers (text is no operand).
[[nodiscard]] constexpr bool isArithmeticVerb(TokenKind verb) noexcept {
  switch (verb) {
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Times:
  case TokenKind::Divide:
  case TokenKind::Magnitude:
  case TokenKind::Ceiling:
  case TokenKind::Floor:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] inline std::string textArithmeticMessage(TokenKind verb) {
  return "VPL arithmetic on text is unsupported: " +
         std::string{tokenKindToString(verb)} + " needs numbers";
}

/// One application of the scalar dyadic `verb` to two cells: arithmetic
/// refuses non-numbers at run time, `÷` divides as doubles, `=` and `≠`
/// compare numbers by value. Every other verb is its VM opcode.
[[nodiscard]] inline CellRef emitScalarDyadic(LowerContext &ctx, TokenKind verb,
                                              const ScalarVerb &scalar,
                                              CellRef left, CellRef right) {
  using zigzag::vortex::OpcodeKind;
  if (isArithmeticVerb(verb)) {
    const auto message = textArithmeticMessage(verb);
    assertNumeric(ctx, left, "#ARITH_LEFT_NUMBER", message);
    assertNumeric(ctx, right, "#ARITH_RIGHT_NUMBER", message);
  }
  if (verb == TokenKind::Divide) return emitDivide(ctx, left, right);
  if (verb == TokenKind::Equal) return emitEquals(ctx, left, right);
  if (verb == TokenKind::NotEqual) {
    return emitUnaryOp(ctx, OpcodeKind::Not, "#NEQ",
                       emitEquals(ctx, left, right));
  }
  return emitBinaryOp(ctx, scalar.opcode, scalar.label, left, right);
}

} // namespace xanadu::vpl::lower

namespace xanadu::vpl::lower {

inline void zero(LowerContext &ctx, zigzag::CellRef counter,
                 std::string_view label) {
  using zigzag::vortex::OpcodeKind;
  const auto op = ctx.emitOp(OpcodeKind::Add, label);
  ctx.emitInputConstant(op, std::int64_t{0});
  ctx.emitInputConstant(op, std::int64_t{0});
  ctx.emitOutput(op, counter);
}

inline void increment(LowerContext &ctx, zigzag::CellRef counter,
                      std::string_view label) {
  using zigzag::vortex::OpcodeKind;
  const auto op = ctx.emitOp(OpcodeKind::Add, label);
  ctx.emitInput(op, counter);
  ctx.emitInputConstant(op, std::int64_t{1});
  ctx.emitOutput(op, counter);
}

/// Points the register `to` at the cell `from` references.
inline void copyRegister(LowerContext &ctx, zigzag::CellRef from,
                         zigzag::CellRef to, std::string_view label) {
  const auto copy = ctx.emitOp(zigzag::vortex::OpcodeKind::CopyCellRefs, label);
  ctx.emitInput(copy, from);
  ctx.emitOutput(copy, to);
}

inline zigzag::CellRef hop(LowerContext &ctx, zigzag::CellRef origin,
                           zigzag::CellRef count, zigzag::DimRef dimension,
                           zigzag::DimVector direction) {
  using zigzag::vortex::OpcodeKind;
  const auto cursor = emit::aliasRegister(ctx, origin);
  emit::countedLoop(ctx, count, [&](zigzag::CellRef) {
    const auto read = ctx.emitOp(OpcodeKind::Link, "#HOP_LINK");
    ctx.emitInput(read, cursor);
    ctx.emitInputConstant(read, static_cast<std::int64_t>(dimension));
    ctx.emitInputConstant(read,
                          static_cast<std::int64_t>(zigzag::toSign(direction)));
    const auto next = ctx.core().arena().makeCell();
    ctx.emitOutput(read, next);
    const auto advance = ctx.emitOp(OpcodeKind::CopyCellRefs, "#HOP_ADVANCE");
    ctx.emitInput(advance, next);
    ctx.emitOutput(advance, cursor);
  });
  return cursor;
}

/// Where and how one axis of a view is read, with its far-end bound honoured
/// (card V6 take bound): the walker arguments that read exactly the cells the
/// view holds, in the order it presents them. Unreversed, the walk is from the
/// origin to the bound (or the rank's end). Reversed, it is from the bound (or
/// the far end, found by walking the rank) back to the origin, which is its
/// own stop, so a reversed bounded view reads the bounded prefix reversed.
struct AxisWalk {
  zigzag::CellRef anchor;
  zigzag::DimRef dimension;
  zigzag::DimVector direction;
  std::optional<zigzag::CellRef> stop;
};

[[nodiscard]] inline AxisWalk axisWalkOf(LowerContext &ctx,
                                         zigzag::CellRef cell,
                                         const zigzag::DirectedDim &axis,
                                         const std::size_t index) {
  const auto bound = ctx.boundOf(cell, index);
  if (!ctx.isReversed(cell, index)) {
    return {cell, axis.dim, axis.dir, bound};
  }
  const auto far = emit::extreme(ctx, cell, axis.dim, axis.dir, std::nullopt);
  if (bound) {
    // The bound is a register that may hold no cell at run time (the view
    // ends where the rank does), so the read starts from the rank's far end
    // unless the bound is there.
    const auto present =
        emit::extentOf(ctx, *bound, axis.dim, axis.dir, *bound);
    const auto keepFar = emit::mintLabel(ctx, "#AXIS_FAR_END");
    emit::branchIfFalse(ctx, present, keepFar, "#AXIS_IF_BOUND");
    const auto copy =
        ctx.emitOp(zigzag::vortex::OpcodeKind::CopyCellRefs, "#AXIS_AT_BOUND");
    ctx.emitInput(copy, *bound);
    ctx.emitOutput(copy, far);
    emit::placeLabel(ctx, keepFar);
  }
  return {far, axis.dim, -axis.dir, cell};
}

/// The walk of the first (only) axis of the valence-1 view `cell`.
[[nodiscard]] inline AxisWalk axisWalkOf(LowerContext &ctx,
                                         zigzag::CellRef cell) {
  return axisWalkOf(ctx, cell, ctx.axesOf(cell)->front(), 0);
}

/// Calls `body` for each cell of axis `axis` of the view `cell`, in the order
/// the view presents them. Also valid for a scalar, which has no flags or
/// bounds: it is walked from itself, unbounded.
inline void
walkView(LowerContext &ctx, zigzag::CellRef cell,
         const zigzag::DirectedDim &axis,
         gleditor::cpp26::function_ref<void(zigzag::CellRef item)> body) {
  const auto walk = axisWalkOf(ctx, cell, axis, 0);
  emit::walkRank(ctx, walk.anchor, walk.dimension, walk.direction, body,
                 walk.stop);
}

/// The cell at 0-based position `index` of `walk`, in a register that is
/// cleared when the walk is shorter, plus an integer register that is 1 when
/// the cell was found and 0 when not. One pass over the walk, so a position
/// far beyond its length costs the length, not the position. `index` must be
/// an integer-typed register.
struct NthCell {
  zigzag::CellRef cell;
  zigzag::CellRef found;
};

[[nodiscard]] inline NthCell nthCell(LowerContext &ctx, const AxisWalk &walk,
                                     zigzag::CellRef index) {
  using zigzag::vortex::OpcodeKind;
  auto &arena = ctx.core().arena();
  // Cleared, not merely unwritten: the executor reads an unwritten result
  // register as the register's own cell, and a cleared one as an empty array.
  const auto cell  = arena.makeCell();
  const auto clear = ctx.emitOp(OpcodeKind::ClearCellRefs, "#NTH_EMPTY");
  ctx.emitOutput(clear, cell);
  const auto found = arena.makeCell();
  zero(ctx, found, "#NTH_FOUND");
  const auto target = arena.makeCell();
  const auto plus   = ctx.emitOp(OpcodeKind::Add, "#NTH_TARGET");
  ctx.emitInput(plus, index);
  ctx.emitInputConstant(plus, kOneStep);
  ctx.emitOutput(plus, target);
  const auto position = arena.makeCell();
  zero(ctx, position, "#NTH_POSITION");
  emit::walkRank(
      ctx, walk.anchor, walk.dimension, walk.direction,
      [&](zigzag::CellRef item) {
        increment(ctx, position, "#NTH_POSITION_NEXT");
        const auto reached = arena.makeCell();
        const auto test    = ctx.emitOp(OpcodeKind::Eq, "#NTH_REACHED");
        ctx.emitInput(test, position);
        ctx.emitInput(test, target);
        ctx.emitOutput(test, reached);
        const auto next = emit::mintLabel(ctx, "#NTH_NEXT");
        emit::branchIfFalse(ctx, reached, next, "#NTH_IF_REACHED");
        const auto keep = ctx.emitOp(OpcodeKind::CopyCellRefs, "#NTH_KEEP");
        ctx.emitInput(keep, item);
        ctx.emitOutput(keep, cell);
        const auto mark = ctx.emitOp(OpcodeKind::Add, "#NTH_MARK");
        ctx.emitInputConstant(mark, kOneStep);
        ctx.emitInputConstant(mark, kNoneYet);
        ctx.emitOutput(mark, found);
        emit::placeLabel(ctx, next);
      },
      walk.stop);
  return {cell, found};
}

struct Minting {
  zigzag::CellRef tail;
  zigzag::CellRef before;
  zigzag::DimRef dimension;
};

inline Minting beginResult(LowerContext &ctx, zigzag::DimRef dimension) {
  const auto anchor = ctx.core().arena().makeCell();
  const auto tail   = emit::extreme(ctx, anchor, dimension,
                                    zigzag::DimVector::POS, std::nullopt);
  return Minting{tail, emit::aliasRegister(ctx, tail), dimension};
}

inline zigzag::CellRef mintValue(LowerContext &ctx, const Minting &result,
                                 zigzag::CellRef number) {
  const auto fresh = emit::mintAfter(ctx, result.tail, result.dimension,
                                     zigzag::DimVector::POS);
  emit::copyValue(ctx, number, fresh);
  return fresh;
}

inline zigzag::CellRef endResult(LowerContext &ctx, const Minting &result) {
  using zigzag::vortex::OpcodeKind;
  const auto read = ctx.emitOp(OpcodeKind::Link, "#RESULT_FIRST");
  ctx.emitInput(read, result.before);
  ctx.emitInputConstant(read, static_cast<std::int64_t>(result.dimension));
  ctx.emitInputConstant(
      read, static_cast<std::int64_t>(zigzag::toSign(zigzag::DimVector::POS)));
  const auto first = ctx.core().arena().makeCell();
  ctx.emitOutput(read, first);
  return first;
}

} // namespace xanadu::vpl::lower

#endif // COMMON_XANADU_VPL_LOWER_SUPPORT_HPP
