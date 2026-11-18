/**
 * @file lower_take_drop.cpp
 * @brief Card V5: take and drop.
 *
 * The spec (vpl-array-language.md, 3 and 3.1) gives `↑` and `↓` as "bounded
 * and skipped read-form walks; nothing is written", so a result is a view over
 * the operand's own cells, never a copy.
 *
 *  - `n ↓ A` is lowered. Skipping the first `n` cells of a rank is a new
 *    origin: the origin hopped `n` read-form links along the axis, in a
 *    register of its own. The view stays open at the tail exactly as `A` is,
 *    so it grows when `A` does, and a count beyond the length leaves the
 *    register absent, which the executor reads as a zero-length array that
 *    keeps its axis.
 *  - `n ↑ A` is refused. A prefix needs a far end that is not the rank's end,
 *    and a view is (origin, axes, reversed flags): there is nowhere to put
 *    that bound. Copying the first `n` cells would give the extent the test
 *    wants but would be a snapshot that stops being `A`, which the live-views
 *    ruling forbids. See the design question in the V5 report.
 *
 * Everything else a drop could mean (a negative count, a reversed or
 * multi-axis operand, a scalar operand) needs the same missing bound or a
 * rule the spec does not give, and is refused by name.
 */
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/lower_support.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

constexpr std::string_view kTakeUnsupported =
    "VPL take is unsupported by the VM: a prefix of a rank needs a bound on "
    "the view's far end, and a view has none";
constexpr std::string_view kDropNegativeLiteral =
    "VPL drop of a negative count is unsupported by the VM: it ends the view "
    "before the rank's end, and a view has no bound there";
constexpr std::string_view kDropNegativeRuntime =
    "VPL drop of a negative count is unsupported by the VM (run time)";
constexpr std::string_view kDropNonIntegerLiteral =
    "VPL drop of a non-integer count is a DOMAIN ERROR";
constexpr std::string_view kDropTextLiteral =
    "VPL drop count must be a number, not text";
constexpr std::string_view kDropCountNotNumber =
    "VPL drop count is not a number";
constexpr std::string_view kDropCountNotWhole =
    "VPL drop count is not a whole number (DOMAIN ERROR)";
constexpr std::string_view kDropCountOnRank =
    "VPL drop with a rank as its count is unsupported by the VM";
constexpr std::string_view kDropCountOnDimension =
    "VPL drop with a dimension value as its count is unsupported by the VM";
constexpr std::string_view kDropDimension =
    "VPL drop of a dimension value is unsupported by the VM";
constexpr std::string_view kDropEnclosed =
    "VPL drop of an enclosed value is unsupported by the VM";
constexpr std::string_view kDropScalar =
    "VPL drop of a scalar is unsupported by the VM";
constexpr std::string_view kDropValence =
    "VPL drop of a rank with more than one axis is unsupported by the VM";
constexpr std::string_view kDropReversed =
    "VPL drop of a reversed rank is unsupported by the VM: the new far end "
    "would need a bound the view does not have";

// A literal count APL or this lowering rejects is refused at compile time.
void refuseBadLiteral(const AstNode *node) {
  if (const auto *scalar = dynamic_cast<const ScalarExpr *>(node);
      scalar != nullptr && scalar->isString()) {
    refuse(kDropTextLiteral);
  }
  const auto value = literalNumber(node);
  if (!value) return;
  if (std::trunc(*value) != *value) refuse(kDropNonIntegerLiteral);
  if (*value < 0.0) refuse(kDropNegativeLiteral);
}

/// Emits the run-time checks that `count` is a whole number and returns an
/// integer-typed register holding it. A count is read by the VM through
/// arithmetic that coerces, so text such as "2" or "x" would otherwise be
/// taken for a number; the checks use only the variant type of the values.
CellRef checkedCount(LowerContext &ctx, CellRef count) {
  // Largest int64: truncating `count % max` leaves any in-range count as it
  // is, as an integer-typed value, whatever type it came in.
  constexpr std::int64_t kTruncate = std::numeric_limits<std::int64_t>::max();
  auto &arena                      = ctx.core().arena();

  // Add of a number and zero keeps its type, and of text turns it into an
  // integer, so the sum equals the count exactly when the count is a number.
  const auto sum   = arena.makeCell();
  const auto sumOp = ctx.emitOp(OpcodeKind::Add, "#DROP_COUNT_PLUS_ZERO");
  ctx.emitInput(sumOp, count);
  ctx.emitInputConstant(sumOp, std::int64_t{0});
  ctx.emitOutput(sumOp, sum);
  const auto isNumber = arena.makeCell();
  const auto equal    = ctx.emitOp(OpcodeKind::Eq, "#DROP_COUNT_IS_NUMBER");
  ctx.emitInput(equal, count);
  ctx.emitInput(equal, sum);
  ctx.emitOutput(equal, isNumber);
  assertCell(ctx, isNumber, "#DROP_COUNT_NUMBER", kDropCountNotNumber);

  const auto whole   = arena.makeCell();
  const auto wholeOp = ctx.emitOp(OpcodeKind::Mod, "#DROP_COUNT_TRUNCATE");
  ctx.emitInput(wholeOp, count);
  ctx.emitInputConstant(wholeOp, kTruncate);
  ctx.emitOutput(wholeOp, whole);
  const auto fraction   = arena.makeCell();
  const auto fractionOp = ctx.emitOp(OpcodeKind::Sub, "#DROP_COUNT_FRACTION");
  ctx.emitInput(fractionOp, count);
  ctx.emitInput(fractionOp, whole);
  ctx.emitOutput(fractionOp, fraction);
  // Zero is compared by order, not Eq: Eq on a double 0.0 and an int 0 differ.
  const auto atLeast = arena.makeCell();
  const auto atMost  = arena.makeCell();
  const auto ge      = ctx.emitOp(OpcodeKind::Gte, "#DROP_COUNT_FRACTION_GE");
  ctx.emitInput(ge, fraction);
  ctx.emitInputConstant(ge, std::int64_t{0});
  ctx.emitOutput(ge, atLeast);
  const auto le = ctx.emitOp(OpcodeKind::Lte, "#DROP_COUNT_FRACTION_LE");
  ctx.emitInput(le, fraction);
  ctx.emitInputConstant(le, std::int64_t{0});
  ctx.emitOutput(le, atMost);
  const auto isWhole = arena.makeCell();
  const auto both    = ctx.emitOp(OpcodeKind::And, "#DROP_COUNT_IS_WHOLE");
  ctx.emitInput(both, atLeast);
  ctx.emitInput(both, atMost);
  ctx.emitOutput(both, isWhole);
  assertCell(ctx, isWhole, "#DROP_COUNT_WHOLE", kDropCountNotWhole);
  return whole;
}

/// A register holding the cell `count` cells after the origin of the rank on
/// `dimension` in `direction`, absent when the rank is shorter. One pass over
/// the rank, so a count far beyond its length costs the rank's length, not
/// the count: a hop per count would run a billion times for `1e9 ↓ A`.
CellRef skipped(LowerContext &ctx, CellRef origin, CellRef count,
                DimRef dimension, DimVector direction) {
  auto &arena = ctx.core().arena();
  // Cleared, not merely unwritten: the executor reads an unwritten result
  // register as the register's own cell, and a cleared one as an empty array.
  const auto cursor = arena.makeCell();
  const auto clear  = ctx.emitOp(OpcodeKind::ClearCellRefs, "#DROP_EMPTY");
  ctx.emitOutput(clear, cursor);
  const auto target = arena.makeCell();
  const auto plus   = ctx.emitOp(OpcodeKind::Add, "#DROP_TARGET");
  ctx.emitInput(plus, count);
  ctx.emitInputConstant(plus, std::int64_t{1});
  ctx.emitOutput(plus, target);
  const auto position = arena.makeCell();
  zero(ctx, position, "#DROP_POSITION");
  emit::walkRank(
      ctx, origin, dimension, direction,
      [&](CellRef item) {
        increment(ctx, position, "#DROP_POSITION_NEXT");
        const auto reached = arena.makeCell();
        const auto test    = ctx.emitOp(OpcodeKind::Eq, "#DROP_REACHED");
        ctx.emitInput(test, position);
        ctx.emitInput(test, target);
        ctx.emitOutput(test, reached);
        const auto next = emit::mintLabel(ctx, "#DROP_NEXT");
        emit::branchIfFalse(ctx, reached, next, "#DROP_IF_REACHED");
        const auto keep = ctx.emitOp(OpcodeKind::CopyCellRefs, "#DROP_KEEP");
        ctx.emitInput(keep, item);
        ctx.emitOutput(keep, cursor);
        emit::placeLabel(ctx, next);
      },
      std::nullopt);
  return cursor;
}

} // namespace

CellRef lowerTakeDyadic(LowerContext &, const DyadicExpr &) {
  refuse(kTakeUnsupported);
}

CellRef lowerDropDyadic(LowerContext &ctx, const DyadicExpr &expr) {
  if (!expr.left() || !expr.right()) {
    throw std::invalid_argument("VPL drop requires two operands");
  }
  if (dynamic_cast<const DimensionExpr *>(expr.left().get()) != nullptr) {
    refuse(kDropCountOnDimension);
  }
  refuseBadLiteral(expr.left().get());
  const auto count = requireCompiledCell(ctx.compileNode(*expr.left()));
  if (ctx.dimensionValuesOf(count) != nullptr) refuse(kDropCountOnDimension);
  if (ctx.isRank(count) || ctx.isEnclosed(count)) refuse(kDropCountOnRank);

  const auto operand = requireCompiledCell(ctx.compileNode(*expr.right()));
  if (ctx.dimensionValuesOf(operand) != nullptr) refuse(kDropDimension);
  if (ctx.isEnclosed(operand)) refuse(kDropEnclosed);
  const auto *axes = ctx.axesOf(operand);
  if (axes == nullptr || axes->empty()) refuse(kDropScalar);
  if (axes->size() > 1) refuse(kDropValence);
  if (ctx.isReversed(operand, 0)) refuse(kDropReversed);

  const auto axis  = axes->front();
  const auto whole = checkedCount(ctx, count);
  assertNotNegative(ctx, whole, "#DROP_NONNEGATIVE", kDropNegativeRuntime);
  const auto view      = skipped(ctx, operand, whole, axis.dim, axis.dir);
  ctx.rankAxes()[view] = {axis};
  const std::array inputs{operand, count};
  emit::mergeRankInputDependencies(ctx, view, inputs);
  return view;
}

} // namespace xanadu::vpl::lower
