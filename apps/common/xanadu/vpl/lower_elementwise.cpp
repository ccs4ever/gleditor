/**
 * @file lower_elementwise.cpp
 * @brief Card V6: scalar arithmetic on ranks and scalar extension.
 *
 * Element-wise arithmetic mints. The spec says every VPL value is cells and
 * only the structural verbs (transpose, reverse, take, drop) are views, so
 * `10 + 1 2 3` weaves a new rank of three cells whose values are 11 12 13 and
 * `- A` weaves one cell per cell of A. The result is a rank on the operand's
 * axis behind a private anchor cell, as iota's is.
 *
 * Each operand is read with the same walk the other verbs use, so a reversed
 * view reads from its far end back to its origin and its elements arrive in
 * reversed order. Two ranks are walked in lockstep, and a length mismatch is
 * a run-time Assert with a named message: truncating to the shorter, as the
 * old host evaluator did, would be a silently wrong value.
 *
 * Only valence-1 ranks are lowered. A matrix operand, an enclosed value and a
 * dimension value are refused by name.
 *
 * Each, power, signum, floor and ceiling remain refusing stubs below.
 */
#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/lower_support.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::DirectedDim;
using zigzag::vortex::OpcodeKind;

constexpr std::string_view kValence =
    "VPL element-wise arithmetic on a rank with more than one axis is "
    "unsupported by the VM";
constexpr std::string_view kEnclosed =
    "VPL element-wise arithmetic on an enclosed value is unsupported by the VM";
constexpr std::string_view kDimension =
    "VPL element-wise arithmetic on a dimension value is unsupported by the VM";
constexpr std::string_view kLengthMismatch =
    "VPL length error: element-wise arithmetic needs ranks of the same length";

// A valence-1 rank operand: its origin register, its one axis, and whether
// that axis is read in reverse.
struct Side {
  CellRef origin;
  DirectedDim axis;
  bool reversed;
};

// Where and how a side's walk runs: from the origin, or from the far end back
// to the origin when reversed (the stop is the origin, as walkAxis does).
struct Walk {
  CellRef anchor;
  DimVector direction;
  std::optional<CellRef> stop;
};

void refuseOperandKind(LowerContext &ctx, CellRef cell) {
  if (ctx.dimensionValuesOf(cell) != nullptr) refuse(kDimension);
  if (ctx.isEnclosed(cell)) refuse(kEnclosed);
}

Side rankSide(LowerContext &ctx, CellRef cell) {
  refuseOperandKind(ctx, cell);
  const auto *axes = ctx.axesOf(cell);
  if (axes == nullptr || axes->size() != 1) refuse(kValence);
  return Side{cell, axes->front(), ctx.isReversed(cell, 0)};
}

Walk walkOf(LowerContext &ctx, const Side &side) {
  if (!side.reversed) return Walk{side.origin, side.axis.dir, std::nullopt};
  const auto far = emit::extreme(ctx, side.origin, side.axis.dim, side.axis.dir,
                                 std::nullopt);
  return Walk{far, -side.axis.dir, side.origin};
}

CellRef finishRank(LowerContext &ctx, CellRef first, DimRef dimension,
                   std::span<const CellRef> inputs) {
  ctx.rankAxes()[first] = {DirectedDim{dimension, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, first, inputs);
  return first;
}

CellRef emitBinary(LowerContext &ctx, const ScalarVerb &verb, CellRef left,
                   CellRef right) {
  const auto op = ctx.emitOp(verb.opcode, verb.label);
  ctx.emitInput(op, left);
  ctx.emitInput(op, right);
  const auto out = ctx.core().arena().makeCell();
  ctx.emitOutput(op, out);
  return out;
}

CellRef emitUnary(LowerContext &ctx, TokenKind verb, CellRef item) {
  const auto out = ctx.core().arena().makeCell();
  switch (verb) {
  case TokenKind::Minus:
  case TokenKind::Magnitude:
  case TokenKind::Not: {
    const bool negate = verb == TokenKind::Minus;
    const bool absval = verb == TokenKind::Magnitude;
    const auto op     = ctx.emitOp(negate   ? OpcodeKind::Neg
                                   : absval ? OpcodeKind::Abs
                                            : OpcodeKind::Not,
                               negate   ? "#NEG"
                                   : absval ? "#ABS"
                                            : "#NOT");
    ctx.emitInput(op, item);
    ctx.emitOutput(op, out);
    return out;
  }
  case TokenKind::Divide: {
    // The reciprocal is 1.0 / x, as the scalar path writes it.
    const auto op = ctx.emitOp(OpcodeKind::Div, "#RECIPROCAL");
    ctx.emitInputConstant(op, 1.0);
    ctx.emitInput(op, item);
    ctx.emitOutput(op, out);
    return out;
  }
  default:
    refuse(msg::monadicOnRank);
  }
}

// One result cell per cell of `side`, `scalar` (when given) being the other
// operand of every application.
template <typename Body>
CellRef mapRank(LowerContext &ctx, const Side &side,
                std::optional<CellRef> scalar, Body &&body) {
  const auto walk   = walkOf(ctx, side);
  const auto result = beginResult(ctx, side.axis.dim);
  emit::walkRank(
      ctx, walk.anchor, side.axis.dim, walk.direction,
      [&](CellRef item) { mintValue(ctx, result, body(item, scalar)); },
      walk.stop);
  return endResult(ctx, result);
}

template <typename Body>
CellRef zipRanks(LowerContext &ctx, const Side &left, const Side &right,
                 Body &&body) {
  const auto walkLeft  = walkOf(ctx, left);
  const auto walkRight = walkOf(ctx, right);
  // Lockstep ends with the shorter rank; only a check makes that an error
  // instead of a silent truncation.
  const auto lengthLeft  = emit::extentOf(ctx, walkLeft.anchor, left.axis.dim,
                                          walkLeft.direction, walkLeft.stop);
  const auto lengthRight = emit::extentOf(ctx, walkRight.anchor, right.axis.dim,
                                          walkRight.direction, walkRight.stop);
  const auto same        = ctx.core().arena().makeCell();
  const auto compare     = ctx.emitOp(OpcodeKind::Eq, "#LENGTH_AGREE");
  ctx.emitInput(compare, lengthLeft);
  ctx.emitInput(compare, lengthRight);
  ctx.emitOutput(compare, same);
  assertCell(ctx, same, "#LENGTH_CHECK", kLengthMismatch);

  const auto result = beginResult(ctx, left.axis.dim);
  emit::walkRanksLockstep(
      ctx, walkLeft.anchor, left.axis.dim, walkLeft.direction, walkRight.anchor,
      right.axis.dim, walkRight.direction,
      [&](CellRef a, CellRef b) { mintValue(ctx, result, body(a, b)); },
      walkLeft.stop, walkRight.stop);
  return endResult(ctx, result);
}

} // namespace

Maybe lowerPowerMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerSignumMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerFloorMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

Maybe lowerCeilingMonadic(LowerContext &, const MonadicExpr &) {
  refuse(msg::monadicUnsupported);
}

CellRef lowerPowerDyadic(LowerContext &, const DyadicExpr &) {
  refuse(msg::dyadicUnsupported);
}

Maybe lowerMonadicArithmeticOnRank(LowerContext &ctx, const MonadicExpr &expr,
                                   CellRef operand) {
  const auto side  = rankSide(ctx, operand);
  const auto verb  = expr.verb();
  const auto first = mapRank(ctx, side, std::nullopt,
                             [&](CellRef item, std::optional<CellRef>) {
                               return emitUnary(ctx, verb, item);
                             });
  return finishRank(ctx, first, side.axis.dim, std::array{operand});
}

CellRef lowerDyadicArithmeticOnRank(LowerContext &ctx, const DyadicExpr &expr,
                                    CellRef lCell, CellRef rCell) {
  const auto verb = scalarDyadicVerb(expr.verb());
  if (!verb) refuse(msg::dyadicOnRank);
  const bool leftRank  = ctx.isRank(lCell);
  const bool rightRank = ctx.isRank(rCell);
  // The scalar side of a scalar extension is checked too: an enclosed or
  // dimension-valued scalar is not a number.
  if (!leftRank) refuseOperandKind(ctx, lCell);
  if (!rightRank) refuseOperandKind(ctx, rCell);
  const std::array inputs{lCell, rCell};
  if (leftRank && rightRank) {
    const auto left  = rankSide(ctx, lCell);
    const auto right = rankSide(ctx, rCell);
    const auto first = zipRanks(ctx, left, right, [&](CellRef a, CellRef b) {
      return emitBinary(ctx, *verb, a, b);
    });
    return finishRank(ctx, first, left.axis.dim, inputs);
  }
  if (leftRank) {
    const auto left = rankSide(ctx, lCell);
    const auto first =
        mapRank(ctx, left, rCell, [&](CellRef item, std::optional<CellRef> s) {
          return emitBinary(ctx, *verb, item, *s);
        });
    return finishRank(ctx, first, left.axis.dim, inputs);
  }
  const auto right = rankSide(ctx, rCell);
  const auto first =
      mapRank(ctx, right, lCell, [&](CellRef item, std::optional<CellRef> s) {
        return emitBinary(ctx, *verb, *s, item);
      });
  return finishRank(ctx, first, right.axis.dim, inputs);
}

Maybe lowerEachMonadic(LowerContext &, const MonadicExpr &expr,
                       const AdverbExpr &adverb) {
  // The reduce/scan verb check ran before the each refusal when this lived in
  // compileMonadic, so `-¨ x` reported the reduce/scan message. Kept byte for
  // byte until this card lowers each.
  TokenKind innerVerb = TokenKind::Plus;
  if (const auto *v = dynamic_cast<const VerbExpr *>(adverb.operand().get())) {
    innerVerb = v->verb();
  }
  (void)expr;
  if (innerVerb != TokenKind::Plus && innerVerb != TokenKind::Times &&
      innerVerb != TokenKind::Ceiling && innerVerb != TokenKind::Floor) {
    refuse(msg::reduceScanVerbs);
  }
  refuse(msg::eachUnsupported);
}

CellRef lowerEachDyadic(LowerContext &ctx, const DyadicExpr &,
                        const AdverbExpr &, CellRef lCell, CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

} // namespace xanadu::vpl::lower
