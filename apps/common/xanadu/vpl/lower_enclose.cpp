/**
 * @file lower_enclose.cpp
 * @brief Card V10: enclose and disclose.
 *
 * Enclose (⊂) boxes a view into a fresh cell whose content is linked along
 * d.enclosure (+1), marking it statically as enclosed. Disclose (⊃) unboxes
 * an enclosed cell by reading back the link along d.enclosure, or returns
 * a non-enclosed operand as is.
 */
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/lower_support.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::DirectedDim;
using zigzag::vortex::OpcodeKind;

constexpr std::string_view kEncloseDimension =
    "VPL enclose of a dimension value is unsupported by the VM";
constexpr std::string_view kEncloseBounded =
    "VPL enclose of a bounded view (a take or a negative drop) is unsupported "
    "by the VM: the box would lose the bound";
constexpr std::string_view kDiscloseDimension =
    "VPL disclose of a dimension value is unsupported by the VM";

std::int64_t asValue(DimRef dimension) {
  return static_cast<std::int64_t>(dimension);
}

std::int64_t asValue(DimVector direction) {
  return static_cast<std::int64_t>(zigzag::toSign(direction));
}

} // namespace

Maybe lowerEncloseMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  if (!expr.right()) {
    throw std::invalid_argument("VPL enclose requires an operand");
  }
  const auto rCell = ctx.compileNode(*expr.right());
  if (!rCell) {
    throw std::invalid_argument("VPL enclose operand has no VM result");
  }
  if (ctx.dimensionValuesOf(*rCell) != nullptr) {
    refuse(kEncloseDimension);
  }

  // The box holds the rank's origin only, so a far-end bound would be lost
  // and the enclosed value would be the whole rank.
  if (ctx.isBounded(*rCell)) refuse(kEncloseBounded);

  const auto dimEnclosure =
      ctx.core().findOrMintDimension(enclosureDimensionName);
  const auto box = ctx.core().arena().makeCell();

  const auto linkOp = ctx.emitOp(OpcodeKind::Link, "#ENCLOSE_LINK");
  ctx.emitInput(linkOp, box);
  ctx.emitInputConstant(linkOp, asValue(dimEnclosure));
  ctx.emitInputConstant(linkOp, asValue(DimVector::POS));
  ctx.emitInput(linkOp, *rCell);

  if (const auto *axes = ctx.axesOf(*rCell)) {
    ctx.rankAxes()[box] = *axes;
  }
  ctx.markEnclosed(box);
  emit::mergeRankInputDependencies(ctx, box, std::array{*rCell});
  return box;
}

Maybe lowerDiscloseMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  if (!expr.right()) {
    throw std::invalid_argument("VPL disclose requires an operand");
  }
  const auto rCell = ctx.compileNode(*expr.right());
  if (!rCell) {
    throw std::invalid_argument("VPL disclose operand has no VM result");
  }
  if (ctx.dimensionValuesOf(*rCell) != nullptr) {
    refuse(kDiscloseDimension);
  }

  if (!ctx.isEnclosed(*rCell)) {
    return *rCell;
  }

  const auto dimEnclosure =
      ctx.core().findOrMintDimension(enclosureDimensionName);
  const auto readOp = ctx.emitOp(OpcodeKind::Link, "#DISCLOSE_READ");
  ctx.emitInput(readOp, *rCell);
  ctx.emitInputConstant(readOp, asValue(dimEnclosure));
  ctx.emitInputConstant(readOp, asValue(DimVector::POS));
  const auto innerCell = ctx.core().arena().makeCell();
  ctx.emitOutput(readOp, innerCell);

  if (const auto *axes = ctx.axesOf(*rCell)) {
    ctx.rankAxes()[innerCell] = *axes;
  }
  emit::mergeRankInputDependencies(ctx, innerCell, std::array{*rCell});
  return innerCell;
}

} // namespace xanadu::vpl::lower
