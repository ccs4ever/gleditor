/**
 * @file lower_take_drop.cpp
 * @brief Cards V5 and V6: take and drop, as live views with far-end bounds.
 *
 * The spec (vpl-array-language.md, 3 and 3.1) gives `↑` and `↓` as "bounded
 * and skipped read-form walks; nothing is written", so a result is a view over
 * the operand's own cells, never a copy. A view is (origin, axes, reversed
 * flags, far-end bounds): skipping the first cells moves the origin, and
 * ending the view before the rank's end sets the axis's bound, the last cell
 * of the walk. Both are cells found by one read-form pass, held in registers
 * of their own, so the operand is untouched.
 *
 * Every form reduces to two cuts of the operand's NATURAL walk (origin to its
 * bound or the rank's end), and a reversed operand swaps which cut a count
 * means, because the first cells of `⌽A` are the last cells of `A`:
 *
 *   front cut by f:  origin moves to the f-th cell; the bound is the operand's
 *                    own, so the view keeps growing with the rank when it did.
 *   back cut to m:   the origin stays; the bound is the m-th cell, so the view
 *                    holds the first m cells and does not grow past them.
 *
 *   n ↓ A  = front cut by n        n ↑ A  = back cut to n
 *   ¯k ↓ A = back cut to len-k     n ↓ ⌽A = back cut to len-n
 *   ¯k ↓ ⌽A = front cut by k       n ↑ ⌽A = front cut by len-n
 *
 * The rule: take, negative drop and drop of a reversed rank fix their far end
 * when they are evaluated, and a view does not grow past it. A back cut does
 * so by its bound; a front cut on a reversed rank (`n ↑ ⌽A`, `¯k ↓ ⌽A`) takes
 * the rank's far end as it is then, when the operand has no bound of its own.
 * Only a positive drop of an unreversed rank (`n ↓ A`) grows with the rank.
 * Growth at the origin end is not a thing a view can have: the first cells of
 * a reversed rank are its last.
 *
 * The spec gives negative counts to drop only ("negative counts from the end,
 * as everywhere else" is the drop row; the take row says "the first n
 * cells"), so a negative take is refused by name. A take of more cells than
 * the rank holds would pad with fill in APL; the spec does not say to, and no
 * cell could be minted without making the view a copy, so it fails by name at
 * run time. A drop of more than the rank holds is the empty array, as in APL.
 */
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/lower_support.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

constexpr std::string_view kTakeNegative =
    "VPL take of a negative count is unsupported by the VM: the spec gives "
    "negative counts to drop only";
constexpr std::string_view kTakePads =
    "VPL take of more cells than the rank holds is unsupported by the VM: "
    "it would pad with fill, and a view mints no cells";

/// The wording of one of the two verbs' refusals.
struct VerbText {
  std::string_view verb;
  [[nodiscard]] std::string text(std::string_view tail) const {
    return "VPL " + std::string{verb} + std::string{tail};
  }
};

// A literal count APL or this lowering rejects is refused at compile time.
void refuseBadLiteral(const AstNode *node, const VerbText &name,
                      bool negativeAllowed) {
  if (const auto *scalar = dynamic_cast<const ScalarExpr *>(node);
      scalar != nullptr && scalar->isString()) {
    refuse(name.text(" count must be a number, not text"));
  }
  const auto value = literalNumber(node);
  if (!value) return;
  if (std::trunc(*value) != *value) {
    refuse(name.text(" of a non-integer count is a DOMAIN ERROR"));
  }
  if (*value < 0.0 && !negativeAllowed) refuse(kTakeNegative);
}

/// Take and drop share the whole-count check with indexing; see
/// checkedWholeNumber.
CellRef checkedCount(LowerContext &ctx, CellRef count, const VerbText &name) {
  return checkedWholeNumber(
      ctx, count, name.text(" count is not a number"),
      name.text(" count is not a whole number (DOMAIN ERROR)"));
}

void clearRegister(LowerContext &ctx, CellRef target, std::string_view label) {
  const auto clear = ctx.emitOp(OpcodeKind::ClearCellRefs, label);
  ctx.emitOutput(clear, target);
}

/// The operand's natural walk: origin to the bound, or to the rank's end,
/// whichever way the view is read.
AxisWalk naturalWalk(LowerContext &ctx, CellRef operand,
                     const zigzag::DirectedDim &axis) {
  return {operand, axis.dim, axis.dir, ctx.boundOf(operand, 0)};
}

/// The result of a cut: a register for the new origin and one for the new
/// bound, both cleared (an empty array has neither) until a cut fills them.
struct Cut {
  CellRef origin;
  CellRef bound;
};

Cut emptyCut(LowerContext &ctx) {
  const auto origin = ctx.core().arena().makeCell();
  const auto bound  = ctx.core().arena().makeCell();
  clearRegister(ctx, origin, "#CUT_EMPTY_ORIGIN");
  clearRegister(ctx, bound, "#CUT_EMPTY_BOUND");
  return {origin, bound};
}

/// origin := the f-th cell of the natural walk; bound := the operand's own.
/// An empty result gets no bound at all: a reversed read would otherwise start
/// from it and, with no origin to stop at, run on past the rank.
void frontCut(LowerContext &ctx, const AxisWalk &natural, CellRef skip,
              const Cut &cut, bool fixFarEnd) {
  const auto nth = nthCell(ctx, natural, skip);
  copyRegister(ctx, nth.cell, cut.origin, "#FRONT_CUT_ORIGIN");
  if (natural.stop || fixFarEnd) {
    const auto done = emit::mintLabel(ctx, "#FRONT_CUT_DONE");
    emit::branchIfFalse(ctx, nth.found, done, "#FRONT_CUT_IF_FOUND");
    // The operand's own bound, or, for a view that must not grow, the rank's
    // far end as it is now.
    const auto far = natural.stop
                         ? *natural.stop
                         : emit::extreme(ctx, natural.anchor, natural.dimension,
                                         natural.direction, std::nullopt);
    copyRegister(ctx, far, cut.bound, "#FRONT_CUT_BOUND");
    emit::placeLabel(ctx, done);
  }
}

/// origin := the operand's; bound := the m-th cell of the natural walk. Empty
/// when m is not positive.
void backCut(LowerContext &ctx, const AxisWalk &natural, CellRef operand,
             CellRef keep, const Cut &cut) {
  const auto positive =
      emitBinaryOp(ctx, OpcodeKind::Gt, "#BACK_CUT_POSITIVE", keep, kNoneYet);
  const auto done = emit::mintLabel(ctx, "#BACK_CUT_DONE");
  emit::branchIfFalse(ctx, positive, done, "#BACK_CUT_IF_ANY");
  const auto last =
      emitBinaryOp(ctx, OpcodeKind::Sub, "#BACK_CUT_LAST", keep, kOneStep);
  const auto nth = nthCell(ctx, natural, last);
  copyRegister(ctx, operand, cut.origin, "#BACK_CUT_ORIGIN");
  copyRegister(ctx, nth.cell, cut.bound, "#BACK_CUT_BOUND");
  emit::placeLabel(ctx, done);
}

struct Operands {
  CellRef count;
  CellRef operand;
  zigzag::DirectedDim axis;
  bool reversed;
};

/// The checks both verbs share, and the compiled operands.
Operands compileOperands(LowerContext &ctx, const DyadicExpr &expr,
                         const VerbText &name, bool negativeAllowed) {
  if (!expr.left() || !expr.right()) {
    throw std::invalid_argument(name.text(" requires two operands"));
  }
  const auto countOnDimension = name.text(
      " with a dimension value as its count is unsupported by the VM");
  if (dynamic_cast<const DimensionExpr *>(expr.left().get()) != nullptr) {
    refuse(countOnDimension);
  }
  refuseBadLiteral(expr.left().get(), name, negativeAllowed);
  const auto count = requireCompiledCell(ctx.compileNode(*expr.left()));
  if (ctx.dimensionValuesOf(count) != nullptr) refuse(countOnDimension);
  if (ctx.isRank(count) || ctx.isEnclosed(count)) {
    refuse(name.text(" with a rank as its count is unsupported by the VM"));
  }

  const auto operand = requireCompiledCell(ctx.compileNode(*expr.right()));
  if (ctx.dimensionValuesOf(operand) != nullptr) {
    refuse(name.text(" of a dimension value is unsupported by the VM"));
  }
  if (ctx.isEnclosed(operand)) {
    refuse(name.text(" of an enclosed value is unsupported by the VM"));
  }
  const auto *axes = ctx.axesOf(operand);
  if (axes == nullptr || axes->empty()) {
    refuse(name.text(" of a scalar is unsupported by the VM"));
  }
  if (axes->size() > 1) {
    refuse(name.text(
        " of a rank with more than one axis is unsupported by the VM"));
  }
  return {count, operand, axes->front(), ctx.isReversed(operand, 0)};
}

/// Registers the cut as the view of `operands`: its axis, reversed flag and
/// bound, and the operand's and count's input dependencies.
CellRef finishView(LowerContext &ctx, const Operands &operands,
                   const Cut &cut) {
  const auto view      = cut.origin;
  ctx.rankAxes()[view] = {operands.axis};
  if (operands.reversed) ctx.rankReversed()[view] = {true};
  ctx.rankBounds()[view] = {cut.bound};
  const std::array inputs{operands.operand, operands.count};
  emit::mergeRankInputDependencies(ctx, view, inputs);
  return view;
}

} // namespace

CellRef lowerTakeDyadic(LowerContext &ctx, const DyadicExpr &expr) {
  const VerbText name{"take"};
  const auto operands = compileOperands(ctx, expr, name, false);
  const auto whole    = checkedCount(ctx, operands.count, name);
  // Zero is the least count a take accepts.
  const auto notNegative =
      emitBinaryOp(ctx, OpcodeKind::Gte, "#TAKE_NONNEGATIVE", whole, kNoneYet);
  assertCell(ctx, notNegative, "#TAKE_NONNEGATIVE", kTakeNegative);

  const auto natural = naturalWalk(ctx, operands.operand, operands.axis);
  const auto length  = emit::extentOf(ctx, natural.anchor, natural.dimension,
                                      natural.direction, natural.stop);
  const auto fits =
      emitBinaryOp(ctx, OpcodeKind::Lte, "#TAKE_FITS", whole, length);
  assertCell(ctx, fits, "#TAKE_FITS", kTakePads);

  const auto cut = emptyCut(ctx);
  if (operands.reversed) {
    const auto skip =
        emitBinaryOp(ctx, OpcodeKind::Sub, "#TAKE_SKIP", length, whole);
    frontCut(ctx, natural, skip, cut, true);
  } else {
    backCut(ctx, natural, operands.operand, whole, cut);
  }
  return finishView(ctx, operands, cut);
}

CellRef lowerDropDyadic(LowerContext &ctx, const DyadicExpr &expr) {
  const VerbText name{"drop"};
  const auto operands = compileOperands(ctx, expr, name, true);
  const auto whole    = checkedCount(ctx, operands.count, name);
  const auto natural  = naturalWalk(ctx, operands.operand, operands.axis);
  const auto cut      = emptyCut(ctx);

  const auto emitFront = [&](CellRef skip) {
    frontCut(ctx, natural, skip, cut, operands.reversed);
  };
  const auto emitBack = [&](CellRef magnitude) {
    const auto length = emit::extentOf(ctx, natural.anchor, natural.dimension,
                                       natural.direction, natural.stop);
    const auto keep =
        emitBinaryOp(ctx, OpcodeKind::Sub, "#DROP_KEEP", length, magnitude);
    backCut(ctx, natural, operands.operand, keep, cut);
  };
  const auto magnitudeOf = [&](CellRef count) {
    const auto out = ctx.core().arena().makeCell();
    const auto op  = ctx.emitOp(OpcodeKind::Abs, "#DROP_MAGNITUDE");
    ctx.emitInput(op, count);
    ctx.emitOutput(op, out);
    return out;
  };

  // A literal count has its sign at compile time, so only the cut it needs is
  // emitted; a computed one emits both behind a branch on its sign.
  if (const auto literal = literalNumber(expr.left().get())) {
    const bool negative = *literal < 0.0;
    const auto count    = negative ? magnitudeOf(whole) : whole;
    if (negative != operands.reversed) {
      emitBack(count);
    } else {
      emitFront(count);
    }
    return finishView(ctx, operands, cut);
  }

  const auto negative =
      emitBinaryOp(ctx, OpcodeKind::Lt, "#DROP_IS_NEGATIVE", whole, kNoneYet);
  const auto magnitude = magnitudeOf(whole);
  // The back cut is the one a negative count on an unreversed rank, or a
  // non-negative one on a reversed rank, means.
  auto back = negative;
  if (operands.reversed) {
    back            = ctx.core().arena().makeCell();
    const auto flip = ctx.emitOp(OpcodeKind::Not, "#DROP_BACK");
    ctx.emitInput(flip, negative);
    ctx.emitOutput(flip, back);
  }
  const auto front = emit::mintLabel(ctx, "#DROP_FRONT");
  const auto done  = emit::mintLabel(ctx, "#DROP_DONE");
  emit::branchIfFalse(ctx, back, front, "#DROP_IF_BACK");
  emitBack(magnitude);
  emit::branchAlways(ctx, done, "#DROP_GOTO_DONE");
  emit::placeLabel(ctx, front);
  emitFront(magnitude);
  emit::placeLabel(ctx, done);
  return finishView(ctx, operands, cut);
}

} // namespace xanadu::vpl::lower
