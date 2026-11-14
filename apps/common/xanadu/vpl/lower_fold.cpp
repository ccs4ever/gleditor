/**
 * @file lower_fold.cpp
 * @brief Card V7 fold: reduce, scan and tally.
 *
 * Reduce and scan execute on the Vortex VM for both literal vectors and
 * runtime ranks. Scalar reduction is identity; runtime ranks are reduced
 * using extreme/walkRankAfter so that single-cell ranks produce their value
 * without iterations, and multi-cell ranks accumulate with the appropriate
 * opcode. Labels #REDUCE_ADD, #SCAN_ADD etc. are preserved.
 *
 * Tally computes the length of runtime ranks via extentOf, or 1 for scalars.
 */
#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/vpl/lower_support.hpp"

namespace xanadu::vpl::lower {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::DirectedDim;
using zigzag::vortex::OpcodeKind;

namespace {

constexpr std::string_view kValence =
    "VPL reduction on a rank with more than one axis is unsupported by the VM";
constexpr std::string_view kEnclosed =
    "VPL reduction on an enclosed value is unsupported by the VM";
constexpr std::string_view kDimension =
    "VPL reduction on a dimension value is unsupported by the VM";

} // namespace

Maybe lowerFoldMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  if (const auto *adv =
          dynamic_cast<const AdverbExpr *>(expr.customVerb().get())) {
    TokenKind adverb    = adv->adverb();
    TokenKind innerVerb = TokenKind::Plus;
    if (const auto *v = dynamic_cast<const VerbExpr *>(adv->operand().get())) {
      innerVerb = v->verb();
    }

    OpcodeKind opKind = OpcodeKind::Add;
    std::string_view opLabel =
        adverb == TokenKind::Reduce ? "#REDUCE_ADD" : "#SCAN_ADD";
    if (innerVerb == TokenKind::Plus) {
      opKind  = OpcodeKind::Add;
      opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_ADD" : "#SCAN_ADD";
    } else if (innerVerb == TokenKind::Times) {
      opKind  = OpcodeKind::Mul;
      opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MUL" : "#SCAN_MUL";
    } else if (innerVerb == TokenKind::Ceiling) {
      opKind  = OpcodeKind::Max;
      opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MAX" : "#SCAN_MAX";
    } else if (innerVerb == TokenKind::Floor) {
      opKind  = OpcodeKind::Min;
      opLabel = (adverb == TokenKind::Reduce) ? "#REDUCE_MIN" : "#SCAN_MIN";
    } else {
      const auto v = scalarDyadicVerb(innerVerb);
      if (!v) refuse(msg::reduceScanVerbs);
      opKind  = v->opcode;
      opLabel = v->label;
    }

    if (const auto *vec =
            dynamic_cast<const VectorExpr *>(expr.right().get())) {
      std::vector<CellRef> operandCells;
      for (const auto &el : vec->elements()) {
        if (!el)
          throw std::invalid_argument(
              "VPL reduction rank contains a missing element");
        operandCells.push_back(requireCompiledCell(ctx.compileNode(*el)));
      }

      if (adverb == TokenKind::Reduce) {
        if (operandCells.empty())
          throw std::invalid_argument("VPL reduce requires a nonempty rank");
        if (operandCells.size() == 1) return operandCells.front();

        CellRef acc = operandCells[0];
        for (std::size_t i = 1; i < operandCells.size(); ++i) {
          CellRef op = ctx.emitOp(opKind, opLabel);
          ctx.emitInput(op, acc);
          ctx.emitInput(op, operandCells[i]);
          CellRef out = ctx.core().arena().makeCell();
          ctx.emitOutput(op, out);
          acc = out;
        }
        return acc;
      }

      if (adverb == TokenKind::Scan) {
        if (operandCells.empty())
          throw std::invalid_argument("VPL scan requires a nonempty rank");

        std::vector<CellRef> scanCells;
        CellRef acc = operandCells[0];
        scanCells.push_back(acc);

        for (std::size_t i = 1; i < operandCells.size(); ++i) {
          CellRef op = ctx.emitOp(opKind, opLabel);
          ctx.emitInput(op, acc);
          ctx.emitInput(op, operandCells[i]);
          CellRef out = ctx.core().arena().makeCell();
          ctx.emitOutput(op, out);
          scanCells.push_back(out);
          acc = out;
        }

        const auto axis = ctx.core().dims().step;
        for (std::size_t i = 0; i + 1 < scanCells.size(); ++i) {
          zigzag::expectWritten(ctx.core().arena().link(
              scanCells[i], axis, DimVector::POS, scanCells[i + 1]));
        }
        ctx.rankAxes()[scanCells.front()] = {
            zigzag::DirectedDim{axis, DimVector::POS}};
        auto &dependencies = ctx.rankDependencies()[scanCells.front()];
        for (const auto cell : scanCells) {
          ctx.core().arena().consumeWork();
          mergeDependencies(dependencies, ctx.dependenciesOf(cell));
        }
        return scanCells.front();
      }
    }

    // Dynamic right operand (bound rank, expression result, scalar)
    if (!expr.right()) {
      throw std::invalid_argument("VPL fold requires an operand");
    }
    const auto rCell = ctx.compileNode(*expr.right());
    if (!rCell) {
      throw std::invalid_argument("VPL monadic operand has no VM result");
    }

    if (ctx.dimensionValuesOf(*rCell) != nullptr) {
      refuse(kDimension);
    }
    if (ctx.isEnclosed(*rCell)) {
      refuse(kEnclosed);
    }

    if (!ctx.isRank(*rCell)) {
      if (adverb == TokenKind::Reduce) {
        return *rCell;
      }
      // Scan of a scalar: single-cell rank on d.step
      const auto axis   = ctx.core().dims().step;
      const auto result = beginResult(ctx, axis);
      mintValue(ctx, result, *rCell);
      const auto first      = endResult(ctx, result);
      ctx.rankAxes()[first] = {DirectedDim{axis, DimVector::POS}};
      emit::mergeRankInputDependencies(ctx, first, std::array{*rCell});
      return first;
    }

    const auto *axes = ctx.axesOf(*rCell);
    if (axes == nullptr || axes->size() != 1) {
      refuse(kValence);
    }
    const auto axis     = axes->front();
    const bool reversed = ctx.isReversed(*rCell, 0);

    CellRef anchor              = *rCell;
    DimVector dir               = axis.dir;
    std::optional<CellRef> stop = std::nullopt;
    if (reversed) {
      anchor = emit::extreme(ctx, *rCell, axis.dim, axis.dir, std::nullopt);
      dir    = -axis.dir;
      stop   = *rCell;
    }

    if (adverb == TokenKind::Reduce) {
      const auto acc = ctx.core().arena().makeCell();
      const auto initial =
          ctx.emitOp(OpcodeKind::CopyCellRefs, "#REDUCE_START");
      ctx.emitInput(initial, anchor);
      ctx.emitOutput(initial, acc);

      emit::walkRankAfter(
          ctx, anchor, axis.dim, dir,
          [&](CellRef item) {
            const auto op = ctx.emitOp(opKind, opLabel);
            ctx.emitInput(op, acc);
            ctx.emitInput(op, item);
            ctx.emitOutput(op, acc);
          },
          stop);

      emit::mergeRankInputDependencies(ctx, acc, std::array{*rCell});
      mergeDependencies(ctx.cellDependencies()[acc],
                        ctx.rankDependenciesOf(*rCell));
      return acc;
    }

    if (adverb == TokenKind::Scan) {
      const auto resultDim = ctx.core().dims().step;
      const auto result    = beginResult(ctx, resultDim);
      const auto acc       = ctx.core().arena().makeCell();
      const auto initial = ctx.emitOp(OpcodeKind::CopyCellRefs, "#SCAN_START");
      ctx.emitInput(initial, anchor);
      ctx.emitOutput(initial, acc);
      mintValue(ctx, result, acc);

      emit::walkRankAfter(
          ctx, anchor, axis.dim, dir,
          [&](CellRef item) {
            const auto op = ctx.emitOp(opKind, opLabel);
            ctx.emitInput(op, acc);
            ctx.emitInput(op, item);
            ctx.emitOutput(op, acc);
            mintValue(ctx, result, acc);
          },
          stop);

      const auto first      = endResult(ctx, result);
      ctx.rankAxes()[first] = {DirectedDim{resultDim, DimVector::POS}};
      emit::mergeRankInputDependencies(ctx, first, std::array{*rCell});
      return first;
    }
  }
  refuse(msg::customUnsupported);
}

Maybe lowerTallyMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  if (ctx.dimensionValuesOf(rCell) != nullptr) {
    refuse(kDimension);
  }
  if (ctx.isEnclosed(rCell)) {
    refuse(kEnclosed);
  }

  if (!ctx.isRank(rCell)) {
    const auto count = ctx.core().arena().makeCell();
    const auto op    = ctx.emitOp(OpcodeKind::Add, "#TALLY_SCALAR");
    ctx.emitInputConstant(op, std::int64_t{1});
    ctx.emitInputConstant(op, std::int64_t{0});
    ctx.emitOutput(op, count);
    return count;
  }

  const auto *axes = ctx.axesOf(rCell);
  if (axes == nullptr || axes->size() != 1) {
    refuse(kValence);
  }
  const auto axis     = axes->front();
  const bool reversed = ctx.isReversed(rCell, 0);

  CellRef anchor              = rCell;
  DimVector dir               = axis.dir;
  std::optional<CellRef> stop = std::nullopt;
  if (reversed) {
    anchor = emit::extreme(ctx, rCell, axis.dim, axis.dir, std::nullopt);
    dir    = -axis.dir;
    stop   = rCell;
  }

  return emit::extentOf(ctx, anchor, axis.dim, dir, stop);
}

CellRef lowerFoldDyadic(LowerContext &ctx, const DyadicExpr &,
                        const AdverbExpr &, CellRef lCell, CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

} // namespace xanadu::vpl::lower
