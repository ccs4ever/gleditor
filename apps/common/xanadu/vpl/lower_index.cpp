/**
 * @file lower_index.cpp
 * @brief Card V11: indexing, grade, where, match, member.
 *
 * Implements 0-based indexing (A[i]), grade up (⍋), grade down (⍒),
 * where (⍸), and monadic match / valence (≡).
 */
#include <array>
#include <cstdint>
#include <optional>
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
using zigzag::vortex::OpcodeKind;

constexpr std::string_view kIndexDimension =
    "VPL indexing with a dimension value is unsupported by the VM";
constexpr std::string_view kIndexTargetDimension =
    "VPL indexing on a dimension value is unsupported by the VM";
constexpr std::string_view kIndexEnclosed =
    "VPL indexing on an enclosed value is unsupported by the VM";
constexpr std::string_view kIndexIndexEnclosed =
    "VPL indexing with an enclosed value is unsupported by the VM";
constexpr std::string_view kIndexScalar =
    "VPL indexing on a scalar is unsupported by the VM";
constexpr std::string_view kIndexValence =
    "VPL indexing on a rank with more than one axis is unsupported by the VM";
constexpr std::string_view kIndexIndexValence =
    "VPL indexing with a rank of more than one axis is unsupported by the VM";
constexpr std::string_view kIndexNotNumber =
    "VPL index is not a number (DOMAIN ERROR)";
constexpr std::string_view kIndexMultiDim =
    "VPL multi-dimensional indexing is unsupported by the VM";
constexpr std::string_view kIndexNegativeLiteral =
    "VPL indexing with a negative index is a DOMAIN ERROR";
constexpr std::string_view kIndexOutOfRange = "VPL index out of range";
constexpr std::string_view kIndexNonIntegerLiteral =
    "VPL indexing with a non-integer index is a DOMAIN ERROR";

constexpr std::string_view kGradeDimension =
    "VPL grade of a dimension value is unsupported by the VM";
constexpr std::string_view kGradeEnclosed =
    "VPL grade of an enclosed value is unsupported by the VM";
constexpr std::string_view kGradeValence =
    "VPL grade on a rank with more than one axis is unsupported by the VM";

constexpr std::string_view kWhereDimension =
    "VPL where of a dimension value is unsupported by the VM";
constexpr std::string_view kWhereEnclosed =
    "VPL where of an enclosed value is unsupported by the VM";
constexpr std::string_view kWhereValence =
    "VPL where on a rank with more than one axis is unsupported by the VM";

constexpr std::string_view kMatchDimension =
    "VPL match of a dimension value is unsupported by the VM";
constexpr std::string_view kMatchEnclosed =
    "VPL match of an enclosed value is unsupported by the VM";

constexpr std::string_view kDyadicMemberUnsupported =
    "VPL dyadic member (∊) is unsupported by the VM";
constexpr std::string_view kDyadicMatchUnsupported =
    "VPL dyadic match (≡) is unsupported by the VM";

void refuseBadIndexLiteral(const AstNode *node) {
  const auto value = literalNumber(node);
  if (!value) return;
  if (std::trunc(*value) != *value) refuse(kIndexNonIntegerLiteral);
  if (*value < 0.0) refuse(kIndexNegativeLiteral);
}

} // namespace

Maybe lowerGradeMonadic(LowerContext &ctx, const MonadicExpr &expr,
                        CellRef rCell) {
  if (ctx.dimensionValuesOf(rCell) != nullptr) {
    refuse(kGradeDimension);
  }
  if (ctx.isEnclosed(rCell)) {
    refuse(kGradeEnclosed);
  }
  const auto *axes = ctx.axesOf(rCell);
  if (axes && axes->size() > 1) {
    refuse(kGradeValence);
  }

  const bool isDown = expr.verb() == TokenKind::GradeDown;
  const DirectedDim axis =
      (axes && !axes->empty())
          ? axes->front()
          : DirectedDim{ctx.core().dims().step, DimVector::POS};

  const auto dResult = ctx.core().findOrMintDimension("d.1");
  const auto result  = beginResult(ctx, dResult);

  walkView(ctx, rCell, axis, [&](CellRef) {
    static_cast<void>(
        emit::mintAfter(ctx, result.tail, dResult, DimVector::POS));
  });
  const auto firstResult = endResult(ctx, result);

  const auto iCell = ctx.core().arena().makeCell();
  zero(ctx, iCell, "#GRADE_I_ZERO");

  walkView(ctx, rCell, axis, [&](CellRef iItem) {
    const auto posCell = ctx.core().arena().makeCell();
    zero(ctx, posCell, "#GRADE_POS_ZERO");

    const auto jCell = ctx.core().arena().makeCell();
    zero(ctx, jCell, "#GRADE_J_ZERO");

    walkView(ctx, rCell, axis, [&](CellRef jItem) {
      const auto cmpOp =
          ctx.emitOp(isDown ? OpcodeKind::Gt : OpcodeKind::Lt, "#GRADE_CMP");
      ctx.emitInput(cmpOp, jItem);
      ctx.emitInput(cmpOp, iItem);
      const auto cmpVal = ctx.core().arena().makeCell();
      ctx.emitOutput(cmpOp, cmpVal);

      // Equal by value: the int 2 and the double 2.0 tie.
      const auto eqVal = emitEquals(ctx, jItem, iItem);

      const auto tieOp = ctx.emitOp(OpcodeKind::Lt, "#GRADE_TIE");
      ctx.emitInput(tieOp, jCell);
      ctx.emitInput(tieOp, iCell);
      const auto tieVal = ctx.core().arena().makeCell();
      ctx.emitOutput(tieOp, tieVal);

      const auto tieBreakOp = ctx.emitOp(OpcodeKind::And, "#GRADE_TIE_BREAK");
      ctx.emitInput(tieBreakOp, eqVal);
      ctx.emitInput(tieBreakOp, tieVal);
      const auto tieBreakVal = ctx.core().arena().makeCell();
      ctx.emitOutput(tieBreakOp, tieBreakVal);

      const auto advanceOp = ctx.emitOp(OpcodeKind::Or, "#GRADE_ADVANCE");
      ctx.emitInput(advanceOp, cmpVal);
      ctx.emitInput(advanceOp, tieBreakVal);
      const auto advanceVal = ctx.core().arena().makeCell();
      ctx.emitOutput(advanceOp, advanceVal);

      const auto addOp = ctx.emitOp(OpcodeKind::Add, "#GRADE_POS_ADD");
      ctx.emitInput(addOp, posCell);
      ctx.emitInput(addOp, advanceVal);
      ctx.emitOutput(addOp, posCell);

      increment(ctx, jCell, "#GRADE_J_INC");
    });

    const auto targetCell =
        hop(ctx, firstResult, posCell, dResult, DimVector::POS);
    emit::copyValue(ctx, iCell, targetCell);
    increment(ctx, iCell, "#GRADE_I_INC");
  });

  ctx.rankAxes()[firstResult] = {DirectedDim{dResult, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, firstResult, std::array{rCell});
  return firstResult;
}

Maybe lowerWhereMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  if (ctx.dimensionValuesOf(rCell) != nullptr) {
    refuse(kWhereDimension);
  }
  if (ctx.isEnclosed(rCell)) {
    refuse(kWhereEnclosed);
  }
  const auto *axes = ctx.axesOf(rCell);
  if (axes && axes->size() > 1) {
    refuse(kWhereValence);
  }
  const DirectedDim axis =
      (axes && !axes->empty())
          ? axes->front()
          : DirectedDim{ctx.core().dims().step, DimVector::POS};

  const auto dResult = ctx.core().findOrMintDimension("d.1");
  const auto result  = beginResult(ctx, dResult);

  const auto iCell = ctx.core().arena().makeCell();
  zero(ctx, iCell, "#WHERE_I_ZERO");

  walkView(ctx, rCell, axis, [&](CellRef item) {
    const auto neqOp = ctx.emitOp(OpcodeKind::Neq, "#WHERE_NEQ");
    ctx.emitInput(neqOp, item);
    ctx.emitInputConstant(neqOp, std::int64_t{0});
    const auto isNonZero = ctx.core().arena().makeCell();
    ctx.emitOutput(neqOp, isNonZero);

    const auto skipLabel = emit::mintLabel(ctx, "#WHERE_SKIP");
    emit::branchIfFalse(ctx, isNonZero, skipLabel, "#WHERE_BRANCH_SKIP");
    const auto fresh =
        emit::mintAfter(ctx, result.tail, dResult, DimVector::POS);
    emit::copyValue(ctx, iCell, fresh);
    emit::placeLabel(ctx, skipLabel);

    increment(ctx, iCell, "#WHERE_I_INC");
  });

  const auto firstResult      = endResult(ctx, result);
  ctx.rankAxes()[firstResult] = {DirectedDim{dResult, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, firstResult, std::array{rCell});
  return firstResult;
}

Maybe lowerMatchMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  if (ctx.dimensionValuesOf(rCell) != nullptr) {
    refuse(kMatchDimension);
  }
  if (ctx.isEnclosed(rCell)) {
    refuse(kMatchEnclosed);
  }
  const auto valence = ctx.valenceOf(rCell);
  const auto count   = ctx.core().arena().makeCell();
  const auto op      = ctx.emitOp(OpcodeKind::Add, "#MATCH_VALENCE");
  ctx.emitInputConstant(op, static_cast<std::int64_t>(valence));
  ctx.emitInputConstant(op, std::int64_t{0});
  ctx.emitOutput(op, count);
  return count;
}

CellRef lowerMemberDyadic(LowerContext &, const DyadicExpr &, CellRef,
                          CellRef) {
  refuse(kDyadicMemberUnsupported);
}

CellRef lowerMatchDyadic(LowerContext &, const DyadicExpr &, CellRef, CellRef) {
  refuse(kDyadicMatchUnsupported);
}

Maybe lowerIndexing(LowerContext &ctx, const IndexingExpr &expr) {
  if (!expr.target()) {
    throw std::invalid_argument("VPL indexing requires a target");
  }
  if (expr.indices().empty()) {
    throw std::invalid_argument("VPL indexing requires at least one index");
  }
  if (expr.indices().size() > 1) {
    refuse(kIndexMultiDim);
  }

  const auto target = ctx.compileNode(*expr.target());
  if (!target) {
    throw std::invalid_argument("VPL indexing target has no VM result");
  }
  if (ctx.dimensionValuesOf(*target) != nullptr) {
    refuse(kIndexTargetDimension);
  }
  if (ctx.isEnclosed(*target)) {
    refuse(kIndexEnclosed);
  }
  const auto *axes = ctx.axesOf(*target);
  if (axes == nullptr || axes->empty()) {
    refuse(kIndexScalar);
  }
  if (axes->size() > 1) {
    refuse(kIndexValence);
  }

  const auto indexNode = expr.indices().front();
  refuseBadIndexLiteral(indexNode.get());
  const auto indexCell = ctx.compileNode(*indexNode);
  if (!indexCell) {
    throw std::invalid_argument("VPL index has no VM result");
  }
  if (ctx.dimensionValuesOf(*indexCell) != nullptr) {
    refuse(kIndexDimension);
  }
  if (ctx.isEnclosed(*indexCell)) {
    refuse(kIndexIndexEnclosed);
  }

  // A matrix of positions would be read along its first axis only and answer
  // a wrong-shaped vector, so it is refused rather than half read.
  if (const auto *indexAxes = ctx.axesOf(*indexCell);
      indexAxes != nullptr && indexAxes->size() > 1) {
    refuse(kIndexIndexValence);
  }

  // Computed positions get the same whole-number test a literal count does,
  // so 1.0 and 4÷2 index and 3÷2 is the DOMAIN ERROR that [1.5] is.
  const auto checkedPosition = [&](CellRef position) {
    const auto whole = checkedWholeNumber(ctx, position, kIndexNotNumber,
                                          kIndexNonIntegerLiteral);
    assertNotNegative(ctx, whole, "#INDEX_NONNEGATIVE",
                      "VPL index out of range: negative index");
    return whole;
  };

  const auto walk = axisWalkOf(ctx, *target);
  // The cell at a position is found by a walk that stops at the view's bound,
  // and a position the walk does not reach fails by name: a hop would run on
  // past a bound, and past the rank's end it would answer an absent cell that
  // reads as 0.
  const auto at = [&](CellRef position) {
    const auto found = nthCell(ctx, walk, checkedPosition(position));
    assertCell(ctx, found.found, "#INDEX_IN_RANGE", kIndexOutOfRange);
    return found.cell;
  };

  if (!ctx.isRank(*indexCell)) {
    const auto cell = at(*indexCell);
    ctx.rankAxes().erase(cell);
    ctx.cellDependencies()[cell] = ctx.rankDependenciesOf(*target);
    emit::mergeRankInputDependencies(ctx, cell,
                                     std::array{*target, *indexCell});
    return cell;
  }

  // Vector indexing: walk indexCell and hop for each
  const auto dResult  = ctx.core().dims().step;
  const auto result   = beginResult(ctx, dResult);
  const auto *idxAxes = ctx.axesOf(*indexCell);
  const auto idxAxis =
      idxAxes ? idxAxes->front() : DirectedDim{dResult, DimVector::POS};

  walkView(ctx, *indexCell, idxAxis, [&](CellRef idx) {
    const auto item = at(idx);
    const auto fresh =
        emit::mintAfter(ctx, result.tail, dResult, DimVector::POS);
    emit::copyValue(ctx, item, fresh);
  });

  const auto firstResult      = endResult(ctx, result);
  ctx.rankAxes()[firstResult] = {DirectedDim{dResult, DimVector::POS}};
  emit::mergeRankInputDependencies(ctx, firstResult,
                                   std::array{*target, *indexCell});
  return firstResult;
}

} // namespace xanadu::vpl::lower
