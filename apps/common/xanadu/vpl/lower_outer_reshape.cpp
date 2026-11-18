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
constexpr std::string_view kEmptyOperand =
    "VPL outer product with an empty operand is unsupported by the VM: the "
    "empty matrix would lose the extent of its other axis";
constexpr std::string_view kVerbNamed = "VPL outer product of ";
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

/// An empty operand would make an empty matrix, and a view with no cells has
/// no origin to carry the extent of its other axis: `⍴` of it would answer 0 0
/// where APL answers 0 n or m 0. So it is refused when the program runs.
void assertNotEmpty(LowerContext &ctx, const Side &side) {
  const auto count = ctx.core().arena().makeCell();
  zero(ctx, count, "#OUTER_COUNT_ZERO");
  walkView(ctx, side.origin, side.axis,
           [&](CellRef) { increment(ctx, count, "#OUTER_COUNT_STEP"); });
  assertCell(
      ctx,
      emitBinaryOp(ctx, OpcodeKind::Gt, "#OUTER_NOT_EMPTY", count, kNoneYet),
      "#OUTER_NONEMPTY", kEmptyOperand);
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
  if (!scalarVerb) {
    refuse(std::string(kVerbNamed) +
           std::string(tokenKindToString(verbExpr->verb())) +
           " is unsupported: it requires a supported scalar dyadic verb");
  }

  const auto lSide = sideOf(ctx, lCell);
  const auto rSide = sideOf(ctx, rCell);

  assertNotEmpty(ctx, lSide);
  assertNotEmpty(ctx, rSide);

  const auto dRow      = ctx.core().findOrMintDimension("d.1");
  const auto dCol      = ctx.core().findOrMintDimension("d.2");
  const auto rowResult = beginResult(ctx, dRow);

  // The grid is a lattice: every cell is linked along d.2 to its row
  // neighbour and along d.1 to the cell below it, so a transposed or
  // reversed view reads columns as well as it reads rows. A row is built
  // left to right while a cursor walks the row above in step with it.
  auto &arena            = ctx.core().arena();
  const auto rowIndex    = arena.makeCell();
  const auto aboveHead   = arena.makeCell();
  const auto aboveCursor = arena.makeCell();
  zero(ctx, rowIndex, "#OUTER_ROW_ZERO");

  walkView(ctx, lSide.origin, lSide.axis, [&](CellRef lItem) {
    const auto rowHead =
        emit::mintAfter(ctx, rowResult.tail, dRow, DimVector::POS);
    const auto colTail  = emit::aliasRegister(ctx, rowHead);
    const auto hasAbove = emitBinaryOp(ctx, OpcodeKind::Gt, "#OUTER_HAS_ABOVE",
                                       rowIndex, kNoneYet);

    const auto colIndex = arena.makeCell();
    zero(ctx, colIndex, "#OUTER_COL_ZERO");

    walkView(ctx, rSide.origin, rSide.axis, [&](CellRef rItem) {
      const auto val =
          emitScalarDyadic(ctx, verbExpr->verb(), *scalarVerb, lItem, rItem);

      const auto isFirst = emitBinaryOp(
          ctx, OpcodeKind::Eq, "#OUTER_IS_FIRST_COL", colIndex, kNoneYet);

      const auto notFirstLabel = emit::mintLabel(ctx, "#OUTER_NOT_FIRST_COL");
      const auto doneLabel     = emit::mintLabel(ctx, "#OUTER_COL_DONE");

      emit::branchIfFalse(ctx, isFirst, notFirstLabel,
                          "#OUTER_BRANCH_NOT_FIRST");
      emit::copyValue(ctx, val, rowHead);
      const auto firstDone = emit::mintLabel(ctx, "#OUTER_FIRST_DONE");
      emit::branchIfFalse(ctx, hasAbove, firstDone, "#OUTER_FIRST_NO_ABOVE");
      copyRegister(ctx, aboveHead, aboveCursor, "#OUTER_CURSOR_START");
      emit::placeLabel(ctx, firstDone);
      emit::branchAlways(ctx, doneLabel, "#OUTER_GOTO_DONE");

      emit::placeLabel(ctx, notFirstLabel);
      const auto colCell = emit::mintAfter(ctx, colTail, dCol, DimVector::POS);
      emit::copyValue(ctx, val, colCell);
      const auto linkDone = emit::mintLabel(ctx, "#OUTER_LINK_DONE");
      emit::branchIfFalse(ctx, hasAbove, linkDone, "#OUTER_NO_ABOVE");
      const auto step = ctx.emitOp(OpcodeKind::Link, "#OUTER_ABOVE_STEP");
      ctx.emitInput(step, aboveCursor);
      ctx.emitInputConstant(step, static_cast<std::int64_t>(dCol));
      ctx.emitInputConstant(
          step, static_cast<std::int64_t>(zigzag::toSign(DimVector::POS)));
      const auto above = arena.makeCell();
      ctx.emitOutput(step, above);
      copyRegister(ctx, above, aboveCursor, "#OUTER_ABOVE_ADVANCE");
      const auto join = ctx.emitOp(OpcodeKind::Link, "#OUTER_COLUMN_LINK");
      ctx.emitInput(join, aboveCursor);
      ctx.emitInputConstant(join, static_cast<std::int64_t>(dRow));
      ctx.emitInputConstant(
          join, static_cast<std::int64_t>(zigzag::toSign(DimVector::POS)));
      ctx.emitInput(join, colCell);
      emit::placeLabel(ctx, linkDone);

      emit::placeLabel(ctx, doneLabel);
      increment(ctx, colIndex, "#OUTER_COL_INC");
    });

    copyRegister(ctx, rowHead, aboveHead, "#OUTER_ROW_ABOVE");
    increment(ctx, rowIndex, "#OUTER_ROW_INC");
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
