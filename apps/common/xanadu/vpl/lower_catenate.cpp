/**
 * @file lower_catenate.cpp
 * @brief Card V8 catenate and ranks inside literal vectors.
 *
 * Catenation creates fresh cells for both operands (copies), keeping
 * types (int, double, text) intact via copyValue and leaving source ranks
 * untouched. Dependencies are propagated to the result rank and preserved.
 */
#include <array>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/lower_support.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::DirectedDim;

constexpr std::string_view kValence =
    "VPL catenate on a rank with more than one axis is unsupported by the VM";
constexpr std::string_view kEnclosed =
    "VPL catenate on an enclosed value is unsupported by the VM";
constexpr std::string_view kDimension =
    "VPL catenate on a dimension value is unsupported by the VM";

void refuseOperandKind(LowerContext &ctx, CellRef cell) {
  if (ctx.dimensionValuesOf(cell) != nullptr) refuse(kDimension);
  if (ctx.isEnclosed(cell)) refuse(kEnclosed);
}

void appendOperand(LowerContext &ctx, const Minting &result, CellRef cell) {
  refuseOperandKind(ctx, cell);
  if (ctx.isRank(cell)) {
    const auto *axes = ctx.axesOf(cell);
    if (axes == nullptr || axes->size() != 1) refuse(kValence);
    const auto axis             = axes->front();
    const bool reversed         = ctx.isReversed(cell, 0);
    CellRef anchor              = cell;
    DimVector dir               = axis.dir;
    std::optional<CellRef> stop = std::nullopt;
    if (reversed) {
      anchor = emit::extreme(ctx, cell, axis.dim, axis.dir, std::nullopt);
      dir    = -axis.dir;
      stop   = cell;
    }
    emit::walkRank(
        ctx, anchor, axis.dim, dir,
        [&](CellRef item) { mintValue(ctx, result, item); }, stop);
  } else {
    const auto fresh              = mintValue(ctx, result, cell);
    ctx.cellDependencies()[fresh] = ctx.rankDependenciesOf(cell);
  }
}

} // namespace

Maybe lowerRavelMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  if (!expr.right()) {
    throw std::invalid_argument("VPL ravel requires an operand");
  }
  const auto rCell = ctx.compileNode(*expr.right());
  if (!rCell) {
    throw std::invalid_argument("VPL ravel operand has no VM result");
  }

  const DimRef dim  = ctx.isRank(*rCell) ? ctx.axesOf(*rCell)->front().dim
                                         : ctx.core().dims().step;
  const auto result = beginResult(ctx, dim);
  appendOperand(ctx, result, *rCell);
  const auto first      = endResult(ctx, result);
  ctx.rankAxes()[first] = {DirectedDim{dim, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, first, std::array{*rCell});
  return first;
}

CellRef lowerCatenateDyadic(LowerContext &ctx, const DyadicExpr &expr) {
  if (!expr.left() || !expr.right()) {
    throw std::invalid_argument("VPL catenate requires both operands");
  }
  const auto lCell = ctx.compileNode(*expr.left());
  const auto rCell = ctx.compileNode(*expr.right());
  if (!lCell || !rCell) {
    throw std::invalid_argument("VPL catenate operand has no VM result");
  }

  refuseOperandKind(ctx, *lCell);
  refuseOperandKind(ctx, *rCell);

  const bool lRank = ctx.isRank(*lCell);
  const bool rRank = ctx.isRank(*rCell);

  DimRef dim = ctx.core().dims().step;
  if (lRank) {
    dim = ctx.axesOf(*lCell)->front().dim;
  } else if (rRank) {
    dim = ctx.axesOf(*rCell)->front().dim;
  }

  // Compile-time scalar catenation keeps exact per-cell provenance.
  if (!lRank && !rRank) {
    const auto c0 = ctx.core().arena().makeCell();
    const auto c1 = ctx.core().arena().makeCell();
    emit::copyValue(ctx, *lCell, c0);
    emit::copyValue(ctx, *rCell, c1);
    zigzag::expectWritten(ctx.core().arena().link(c0, dim, DimVector::POS, c1));
    ctx.cellDependencies()[c0] = ctx.rankDependenciesOf(*lCell);
    ctx.cellDependencies()[c1] = ctx.rankDependenciesOf(*rCell);
    ctx.rankAxes()[c0]         = {DirectedDim{dim, DimVector::POS}};
    emit::mergeRankInputDependencies(ctx, c0, std::array{*lCell, *rCell});
    return c0;
  }

  // Every case mints fresh cells. The spec's glyph table (vpl-array-language.md
  // 3, 3.1) lists `,` in neither its view-returning rows (`⍉ ⌽ ↑ ↓`) nor its
  // weaving ones, but 4.3 says a verb that produces a value mints cells, and
  // VPLEngineTest.CatenateKeepsBudgetAndAliasesIntact pins copies. Linking a
  // new cell into an operand's own cells would also make a second catenation
  // steal the operand's NEG link and mutate the operand.
  const auto result = beginResult(ctx, dim);
  appendOperand(ctx, result, *lCell);
  appendOperand(ctx, result, *rCell);

  const auto first      = endResult(ctx, result);
  ctx.rankAxes()[first] = {DirectedDim{dim, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, first, std::array{*lCell, *rCell});
  return first;
}

Maybe lowerVectorOfRanks(LowerContext &ctx, const VectorExpr &,
                         const std::vector<CellRef> &cells) {
  const auto dim    = ctx.core().dims().step;
  const auto result = beginResult(ctx, dim);
  for (const auto cell : cells) {
    appendOperand(ctx, result, cell);
  }
  const auto first      = endResult(ctx, result);
  ctx.rankAxes()[first] = {DirectedDim{dim, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, first, cells);
  return first;
}

} // namespace xanadu::vpl::lower
