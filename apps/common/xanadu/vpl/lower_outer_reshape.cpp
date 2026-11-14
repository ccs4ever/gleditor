/**
 * @file lower_outer_reshape.cpp
 * @brief Card V9: outer product and reshape.
 *
 * Outer product (∘.f or o.f) weaves a 2-valence grid between two valence-1
 * ranks or scalars on two dimensions (d.1 for rows, d.2 for columns).
 * The primary axis connects row heads; within each row, cells are linked
 * along the secondary axis.
 *
 * Reshape by shape vector remains an explicit refusal until implemented.
 */
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

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

constexpr std::string_view kValence =
    "VPL outer product on a rank with more than one axis is unsupported by the "
    "VM";
constexpr std::string_view kEnclosed =
    "VPL outer product on an enclosed value is unsupported by the VM";
constexpr std::string_view kDimension =
    "VPL outer product on a dimension value is unsupported by the VM";
constexpr std::string_view kVerbUnsupported =
    "VPL outer product requires a supported scalar dyadic verb";
constexpr std::string_view kMonadicUnsupported =
    "VPL outer product is dyadic only: it requires left and right operands";
constexpr std::string_view kReshapeUnsupported =
    "VPL dyadic reshape is unsupported by the VM";

struct Side {
  CellRef origin;
  DirectedDim axis;
  bool reversed;
};

void refuseOperandKind(LowerContext &ctx, CellRef cell) {
  if (ctx.dimensionValuesOf(cell) != nullptr) refuse(kDimension);
  if (ctx.isEnclosed(cell)) refuse(kEnclosed);
}

Side sideOf(LowerContext &ctx, CellRef cell) {
  refuseOperandKind(ctx, cell);
  const auto *axes = ctx.axesOf(cell);
  if (axes && !axes->empty()) {
    if (axes->size() > 1) refuse(kValence);
    return Side{cell, axes->front(), ctx.isReversed(cell, 0)};
  }
  const auto dim = ctx.core().dims().step;
  return Side{cell, DirectedDim{dim, DimVector::POS}, false};
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

} // namespace

CellRef lowerRhoDyadic(LowerContext &, const DyadicExpr &) {
  refuse(kReshapeUnsupported);
}

Maybe lowerOuterProductMonadic(LowerContext &, const MonadicExpr &,
                               const ConjunctionExpr &) {
  refuse(kMonadicUnsupported);
}

CellRef lowerOuterProductDyadic(LowerContext &ctx, const DyadicExpr &,
                                const ConjunctionExpr &conjunction,
                                CellRef lCell, CellRef rCell) {
  refuseOperandKind(ctx, lCell);
  refuseOperandKind(ctx, rCell);

  const auto *verbExpr =
      dynamic_cast<const VerbExpr *>(conjunction.left().get());
  if (!verbExpr) refuse(kVerbUnsupported);
  const auto scalarVerb = scalarDyadicVerb(verbExpr->verb());
  if (!scalarVerb) refuse(kVerbUnsupported);

  const auto lSide = sideOf(ctx, lCell);
  const auto rSide = sideOf(ctx, rCell);

  const auto dRow      = ctx.core().findOrMintDimension("d.1");
  const auto dCol      = ctx.core().findOrMintDimension("d.2");
  const auto rowResult = beginResult(ctx, dRow);

  emit::walkAxis(
      ctx, lSide.origin, lSide.axis.dim, lSide.axis.dir, lSide.reversed,
      [&](CellRef lItem) {
        const auto rowHead =
            emit::mintAfter(ctx, rowResult.tail, dRow, DimVector::POS);
        const auto colTail = emit::aliasRegister(ctx, rowHead);

        const auto colIndex = ctx.core().arena().makeCell();
        const auto zeroOp   = ctx.emitOp(OpcodeKind::Add, "#OUTER_COL_ZERO");
        ctx.emitInputConstant(zeroOp, std::int64_t{0});
        ctx.emitInputConstant(zeroOp, std::int64_t{0});
        ctx.emitOutput(zeroOp, colIndex);

        emit::walkAxis(
            ctx, rSide.origin, rSide.axis.dim, rSide.axis.dir, rSide.reversed,
            [&](CellRef rItem) {
              const auto val = emitBinary(ctx, *scalarVerb, lItem, rItem);

              const auto isZero = ctx.core().arena().makeCell();
              const auto eqOp =
                  ctx.emitOp(OpcodeKind::Eq, "#OUTER_IS_FIRST_COL");
              ctx.emitInput(eqOp, colIndex);
              ctx.emitInputConstant(eqOp, std::int64_t{0});
              ctx.emitOutput(eqOp, isZero);

              const auto notFirstLabel =
                  emit::mintLabel(ctx, "#OUTER_NOT_FIRST_COL");
              const auto doneLabel = emit::mintLabel(ctx, "#OUTER_COL_DONE");

              emit::branchIfFalse(ctx, isZero, notFirstLabel,
                                  "#OUTER_BRANCH_NOT_FIRST");
              emit::copyValue(ctx, val, rowHead);
              emit::branchAlways(ctx, doneLabel, "#OUTER_GOTO_DONE");

              emit::placeLabel(ctx, notFirstLabel);
              const auto colCell =
                  emit::mintAfter(ctx, colTail, dCol, DimVector::POS);
              emit::copyValue(ctx, val, colCell);

              emit::placeLabel(ctx, doneLabel);

              const auto incOp = ctx.emitOp(OpcodeKind::Add, "#OUTER_COL_INC");
              ctx.emitInput(incOp, colIndex);
              ctx.emitInputConstant(incOp, std::int64_t{1});
              ctx.emitOutput(incOp, colIndex);
            });
      });

  const auto root      = endResult(ctx, rowResult);
  ctx.rankAxes()[root] = {
      DirectedDim{dRow, DimVector::POS},
      DirectedDim{dCol, DimVector::POS},
  };
  const std::array inputs{lCell, rCell};
  emit::mergeRankInputDependencies(ctx, root, inputs);
  return root;
}

} // namespace xanadu::vpl::lower
