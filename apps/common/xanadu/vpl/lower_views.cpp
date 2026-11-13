/**
 * @file lower_views.cpp
 * @brief Card V4 view verbs: transpose, reverse, dimension values,
 * rho-to-dimensions.
 *
 * A view is (origin, axes, reversed flags), so these verbs mint no value cells
 * and copy none, and none walks anything when the program runs. A transpose
 * or a dimension relabelling changes the compile-time axis list (the flags
 * travel with their axes); a reverse toggles the flag of the flipped axis and
 * leaves the origin where it is, which is what the spec means by changing an
 * axis's sign (card V4b). Each result gets a register of its own (an alias of
 * the operand's), because rankAxes_ is keyed by register and two views over
 * one origin would otherwise share a key.
 *
 * Because nothing moves, a reversed view is live (it shows cells appended to
 * the rank later), `⌽⌽X` is X, `⍴⌽X` is `⍴X`, and a reversal of a matrix holds
 * exactly the cells the matrix holds, so ragged rows cannot change what is
 * read. The price is paid by whoever reads a reversed axis: it finds the far
 * end from the origin and walks back to the origin (emit::walkAxis), so a
 * reversed read walks the rank twice.
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
                 std::vector<DirectedDim> axes, AxisReversed reversed = {}) {
  const auto view      = emit::aliasRegister(ctx, origin);
  ctx.rankAxes()[view] = std::move(axes);
  if (std::ranges::any_of(reversed, [](const bool flag) { return flag; })) {
    ctx.rankReversed()[view] = std::move(reversed);
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
  AxisReversed flags;
  if (const auto *held = ctx.reversedOf(operand)) {
    flags = *held;
    std::ranges::reverse(flags);
  }
  return viewOver(ctx, operand, operand, std::move(reversed), std::move(flags));
}

Maybe lowerReverseMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  const auto operand = compileOperand(ctx, expr, "VPL reverse");
  if (ctx.dimensionValuesOf(operand) != nullptr) refuse(kReverseDimension);
  if (ctx.isEnclosed(operand)) refuse(kReverseEnclosed);
  const auto *axes = ctx.axesOf(operand);
  if (axes == nullptr || axes->empty()) return operand;

  const auto index = expr.verb() == TokenKind::ReverseFirst ? std::size_t{0}
                                                            : axes->size() - 1;
  AxisReversed flags(axes->size(), false);
  if (const auto *held = ctx.reversedOf(operand)) {
    flags = *held;
    flags.resize(axes->size(), false);
  }
  flags[index] = !flags[index];
  return viewOver(ctx, operand, operand, *axes, std::move(flags));
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
  AxisReversed permutedFlags;
  permuted.reserve(axes->size());
  for (const auto index : *permutation) {
    if (index < 0 || static_cast<std::size_t>(index) >= axes->size() ||
        seen[static_cast<std::size_t>(index)]) {
      refuse(kPermutationInvalid);
    }
    seen[static_cast<std::size_t>(index)] = true;
    permuted.push_back((*axes)[static_cast<std::size_t>(index)]);
    permutedFlags.push_back(
        ctx.isReversed(rCell, static_cast<std::size_t>(index)));
  }
  return viewOver(ctx, rCell, rCell, std::move(permuted),
                  std::move(permutedFlags));
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
