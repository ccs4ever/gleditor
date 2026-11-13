/**
 * @file lower_views.cpp
 * @brief Card V4 view verbs: transpose, reverse, dimension values,
 * rho-to-dimensions.
 *
 * A view is (origin, axes, stops), so these verbs mint no value cells and
 * copy none: a transpose or a dimension relabelling only changes the
 * compile-time axis list (stops travel with their axes), and a reverse moves
 * the origin to the far end of the flipped axis and bounds that axis there
 * (card V4b). Each result gets a register of its own (an alias of the
 * operand's), because rankAxes_ is keyed by register and two views over one
 * origin would otherwise share a key.
 *
 * The reverse rule, per flipped axis, for a view [origin .. end] where `end`
 * is the axis's stop when it has one and the rank's end when it has not:
 *  - the result's origin is `end` and its stop is the operand's origin, so
 *    the result is [end .. origin] walked the other way;
 *  - an unstopped axis finds its `end` by walking (emit::extreme), a stopped
 *    one already has it, so reversing twice costs no walk and lands exactly
 *    on the operand's cells;
 *  - the result is a snapshot: cells appended to the rank after the reverse
 *    lie beyond `end` and are not in it, whereas the operand (no stop) grows.
 * A stop only identifies the end of the walk from the origin, which on a
 * valence-2 view is one row, so a reverse of a view that is already reversed
 * on another axis is refused (kReverseRows) rather than guessed.
 */
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::DimVector;
using zigzag::DirectedDim;

constexpr std::string_view kTransposeDimension =
    "VPL transpose of a dimension value is unsupported by the VM";
constexpr std::string_view kTransposeEnclosed =
    "VPL transpose of an enclosed value is unsupported by the VM";
constexpr std::string_view kReverseDimension =
    "VPL reverse of a dimension value is unsupported by the VM";
constexpr std::string_view kReverseEnclosed =
    "VPL reverse of an enclosed value is unsupported by the VM";
constexpr std::string_view kReverseRows =
    "VPL reverse of an array already reversed along another axis is "
    "unsupported by the VM (a stop cell names one row's end)";
constexpr std::string_view kViewOriginDimension =
    "VPL dimension view of a dimension value is unsupported by the VM";
constexpr std::string_view kViewOriginEnclosed =
    "VPL dimension view of an enclosed value is unsupported by the VM";
constexpr std::string_view kViewOfRank =
    "VPL dimension view of a rank is unsupported by the VM; use the shape "
    "link form";
constexpr std::string_view kPermutationLiteral =
    "VPL dyadic transpose requires a literal integer permutation";
constexpr std::string_view kPermutationInvalid =
    "VPL dyadic transpose left operand must be a permutation of the axes";
constexpr std::string_view kTransposeDyadicDimension =
    "VPL dyadic transpose of a dimension value is unsupported by the VM";
constexpr std::string_view kTransposeDyadicEnclosed =
    "VPL dyadic transpose of an enclosed value is unsupported by the VM";

// A fresh register that holds no cell at run time. A dimension is structure
// with nothing to point at, so its register is cleared and only the
// compile-time table says what it is.
CellRef clearedRegister(LowerContext &ctx) {
  return emit::aliasRegister(ctx, ctx.core().arena().makeCell());
}

// The result register of a view over `operand`: its own key in rankAxes_,
// the operand's input dependencies.
CellRef viewOver(LowerContext &ctx, CellRef origin, CellRef operand,
                 std::vector<DirectedDim> axes, AxisStops stops = {}) {
  const auto view      = emit::aliasRegister(ctx, origin);
  ctx.rankAxes()[view] = std::move(axes);
  if (std::ranges::any_of(stops,
                          [](const auto &stop) { return stop.has_value(); })) {
    ctx.rankStops()[view] = std::move(stops);
  }
  const std::array inputs{operand};
  emit::mergeRankInputDependencies(ctx, view, inputs);
  return view;
}

CellRef compileOperand(LowerContext &ctx, const MonadicExpr &expr,
                       std::string_view what) {
  if (!expr.right()) {
    throw std::invalid_argument(std::string(what) + " requires an argument");
  }
  return requireCompiledCell(ctx.compileNode(*expr.right()));
}

// The integers of a literal permutation as written (`1 0`, or a lone `0`),
// read from the syntax because an axis list is compile-time structure.
std::optional<std::vector<std::int64_t>>
literalPermutation(const AstNode *node) {
  const auto integer =
      [](const AstNode *element) -> std::optional<std::int64_t> {
    const auto *scalar = dynamic_cast<const ScalarExpr *>(element);
    if (scalar == nullptr || scalar->isString() || scalar->isFloat()) {
      return std::nullopt;
    }
    return scalar->intValue();
  };
  if (const auto *vec = dynamic_cast<const VectorExpr *>(node)) {
    std::vector<std::int64_t> out;
    for (const auto &element : vec->elements()) {
      const auto value = integer(element.get());
      if (!value) return std::nullopt;
      out.push_back(*value);
    }
    return out;
  }
  if (const auto value = integer(node)) return std::vector{*value};
  return std::nullopt;
}

} // namespace

Maybe lowerTransposeMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  const auto operand = compileOperand(ctx, expr, "VPL transpose");
  if (ctx.dimensionValuesOf(operand) != nullptr) refuse(kTransposeDimension);
  if (ctx.isEnclosed(operand)) refuse(kTransposeEnclosed);
  const auto *axes = ctx.axesOf(operand);
  // A scalar has no axes to permute.
  if (axes == nullptr) return operand;
  auto reversed = *axes;
  std::ranges::reverse(reversed);
  AxisStops stops;
  if (const auto *held = ctx.stopsOf(operand)) {
    stops = *held;
    std::ranges::reverse(stops);
  }
  return viewOver(ctx, operand, operand, std::move(reversed), std::move(stops));
}

Maybe lowerReverseMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  const auto operand = compileOperand(ctx, expr, "VPL reverse");
  if (ctx.dimensionValuesOf(operand) != nullptr) refuse(kReverseDimension);
  if (ctx.isEnclosed(operand)) refuse(kReverseEnclosed);
  const auto *axes = ctx.axesOf(operand);
  if (axes == nullptr || axes->empty()) return operand;

  auto flipped     = *axes;
  const auto index = expr.verb() == TokenKind::ReverseFirst
                         ? std::size_t{0}
                         : flipped.size() - 1;
  for (std::size_t other = 0; other < flipped.size(); ++other) {
    if (other != index && ctx.stopOf(operand, other)) refuse(kReverseRows);
  }
  const auto old     = flipped[index];
  flipped[index].dir = -old.dir;
  const auto oldStop = ctx.stopOf(operand, index);
  const auto newOrigin =
      oldStop ? *oldStop : emit::extreme(ctx, operand, old.dim, old.dir);
  AxisStops stops(flipped.size());
  stops[index] = emit::aliasRegister(ctx, operand);
  return viewOver(ctx, newOrigin, operand, std::move(flipped),
                  std::move(stops));
}

CellRef lowerRhoViewDyadic(LowerContext &ctx, const DyadicExpr &expr) {
  if (!expr.left() || !expr.right()) {
    throw std::invalid_argument("VPL dyadic verb requires two operands");
  }
  const auto lCell = requireCompiledCell(ctx.compileNode(*expr.left()));
  const auto rCell = requireCompiledCell(ctx.compileNode(*expr.right()));
  return lowerRhoViewCompiled(ctx, expr, lCell, rCell);
}

CellRef lowerRhoViewCompiled(LowerContext &ctx, const DyadicExpr &,
                             CellRef lCell, CellRef rCell) {
  const auto *dims = ctx.dimensionValuesOf(rCell);
  if (dims == nullptr) refuse(msg::dyadicUnsupported);
  if (ctx.dimensionValuesOf(lCell) != nullptr) refuse(kViewOriginDimension);
  if (ctx.isEnclosed(lCell)) refuse(kViewOriginEnclosed);
  // A rank on the left is the link form (`1 2 3 ⍴ d`), which another card
  // owns; relabelling its first cell would silently mean something else.
  if (ctx.isRank(lCell)) refuse(kViewOfRank);
  return viewOver(ctx, lCell, lCell, *dims);
}

CellRef lowerTransposeDyadic(LowerContext &ctx, const DyadicExpr &expr,
                             CellRef lCell, CellRef rCell) {
  if (ctx.dimensionValuesOf(rCell) != nullptr) {
    refuse(kTransposeDyadicDimension);
  }
  if (ctx.isEnclosed(rCell)) refuse(kTransposeDyadicEnclosed);
  const auto *axes = ctx.axesOf(rCell);
  // Not a view operand: the other forms keep their existing refusals.
  if (axes == nullptr || ctx.dimensionValuesOf(lCell) != nullptr ||
      ctx.isEnclosed(lCell)) {
    refuseDyadicOperands(ctx, lCell, rCell);
  }
  const auto permutation = literalPermutation(expr.left().get());
  if (!permutation) refuse(kPermutationLiteral);
  if (permutation->size() != axes->size()) refuse(kPermutationInvalid);
  std::vector<bool> seen(axes->size(), false);
  std::vector<DirectedDim> permuted;
  AxisStops permutedStops;
  permuted.reserve(axes->size());
  for (const auto index : *permutation) {
    if (index < 0 || static_cast<std::size_t>(index) >= axes->size() ||
        seen[static_cast<std::size_t>(index)]) {
      refuse(kPermutationInvalid);
    }
    seen[static_cast<std::size_t>(index)] = true;
    permuted.push_back((*axes)[static_cast<std::size_t>(index)]);
    permutedStops.push_back(ctx.stopOf(rCell, static_cast<std::size_t>(index)));
  }
  return viewOver(ctx, rCell, rCell, std::move(permuted),
                  std::move(permutedStops));
}

CellRef lowerDimension(LowerContext &ctx, const DimensionExpr &expr) {
  const auto value = clearedRegister(ctx);
  ctx.setDimensionValues(
      value, {DirectedDim{ctx.resolveDimension(expr.name()), DimVector::POS}});
  return value;
}

Maybe lowerDimensionVector(LowerContext &ctx, const VectorExpr &,
                           const std::vector<CellRef> &cells) {
  std::vector<DirectedDim> joined;
  for (const auto cell : cells) {
    const auto *dims = ctx.dimensionValuesOf(cell);
    if (dims == nullptr) refuse(msg::dimensionValues);
    joined.insert(joined.end(), dims->begin(), dims->end());
  }
  const auto value = clearedRegister(ctx);
  ctx.setDimensionValues(value, std::move(joined));
  return value;
}

} // namespace xanadu::vpl::lower
