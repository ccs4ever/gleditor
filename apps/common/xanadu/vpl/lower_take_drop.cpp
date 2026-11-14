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

std::int64_t asValue(DimRef dimension) {
  return static_cast<std::int64_t>(dimension);
}

std::int64_t asValue(DimVector direction) {
  return static_cast<std::int64_t>(zigzag::toSign(direction));
}

// A literal count APL or this lowering rejects is refused at compile time.
void refuseBadLiteral(const AstNode *node) {
  const auto value = literalNumber(node);
  if (!value) return;
  if (std::trunc(*value) != *value) refuse(kDropNonIntegerLiteral);
  if (*value < 0.0) refuse(kDropNegativeLiteral);
}

// A register that ends `count` read-form links along the axis from `origin`.
// A link read off an absent register answers nothing and a copy of nothing
// clears its target, so running off the end leaves the register absent
// instead of stuck on the last cell.
CellRef hop(LowerContext &ctx, CellRef origin, CellRef count, DimRef dimension,
            DimVector direction) {
  const auto cursor = emit::aliasRegister(ctx, origin);
  emit::countedLoop(ctx, count, [&](CellRef) {
    const auto read = ctx.emitOp(OpcodeKind::Link, "#DROP_HOP");
    ctx.emitInput(read, cursor);
    ctx.emitInputConstant(read, asValue(dimension));
    ctx.emitInputConstant(read, asValue(direction));
    const auto next = ctx.core().arena().makeCell();
    ctx.emitOutput(read, next);
    const auto advance = ctx.emitOp(OpcodeKind::CopyCellRefs, "#DROP_ADVANCE");
    ctx.emitInput(advance, next);
    ctx.emitOutput(advance, cursor);
  });
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

  const auto axis = axes->front();
  assertNotNegative(ctx, count, "#DROP_NONNEGATIVE", kDropNegativeRuntime);
  const auto view      = hop(ctx, operand, count, axis.dim, axis.dir);
  ctx.rankAxes()[view] = {axis};
  const std::array inputs{operand, count};
  emit::mergeRankInputDependencies(ctx, view, inputs);
  return view;
}

} // namespace xanadu::vpl::lower
