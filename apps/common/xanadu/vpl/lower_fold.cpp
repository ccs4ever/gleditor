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
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
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
constexpr std::int64_t kSumIdentity     = 0;
constexpr std::int64_t kProductIdentity = 1;

std::string noIdentityMessage(std::string_view verbName) {
  return "VPL reduce of an empty rank has no identity for " +
         std::string{verbName};
}

/// Emits `target` := constant, by arithmetic only so it is a plain integer.
void emitConstant(LowerContext &ctx, CellRef target, std::int64_t value) {
  const auto op = ctx.emitOp(OpcodeKind::Add, "#REDUCE_IDENTITY");
  ctx.emitInputConstant(op, value);
  ctx.emitInputConstant(op, std::int64_t{0});
  ctx.emitOutput(op, target);
}

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
    // Owner 2026-11-18: an empty reduce is the APL identity for the two verbs
    // that have one (`+/⍬` is 0, `×/⍬` is 1); maximum and minimum have none and
    // are refused by name, at run time for a rank and at compile time for an
    // empty literal vector.
    std::optional<std::int64_t> identity;
    std::string_view verbName = "sum";
    std::string_view opLabel =
        adverb == TokenKind::Reduce ? "#REDUCE_ADD" : "#SCAN_ADD";
    if (innerVerb == TokenKind::Plus) {
      opKind   = OpcodeKind::Add;
      opLabel  = (adverb == TokenKind::Reduce) ? "#REDUCE_ADD" : "#SCAN_ADD";
      identity = kSumIdentity;
    } else if (innerVerb == TokenKind::Times) {
      opKind   = OpcodeKind::Mul;
      opLabel  = (adverb == TokenKind::Reduce) ? "#REDUCE_MUL" : "#SCAN_MUL";
      identity = kProductIdentity;
      verbName = "product";
    } else if (innerVerb == TokenKind::Ceiling) {
      opKind   = OpcodeKind::Max;
      opLabel  = (adverb == TokenKind::Reduce) ? "#REDUCE_MAX" : "#SCAN_MAX";
      verbName = "maximum";
    } else if (innerVerb == TokenKind::Floor) {
      opKind   = OpcodeKind::Min;
      opLabel  = (adverb == TokenKind::Reduce) ? "#REDUCE_MIN" : "#SCAN_MIN";
      verbName = "minimum";
    } else if (innerVerb == TokenKind::And) {
      // Associative, so the left-to-right chain agrees with APL's right fold.
      opKind   = OpcodeKind::And;
      opLabel  = (adverb == TokenKind::Reduce) ? "#REDUCE_AND" : "#SCAN_AND";
      identity = kProductIdentity;
      verbName = "and";
    } else if (innerVerb == TokenKind::Or) {
      opKind   = OpcodeKind::Or;
      opLabel  = (adverb == TokenKind::Reduce) ? "#REDUCE_OR" : "#SCAN_OR";
      identity = kSumIdentity;
      verbName = "or";
    } else {
      // APL folds from the right, so `-/` and `÷/` are not the left-to-right
      // chain this lowering emits; only the associative verbs agree in both
      // orders (`=/` and `≠/` among those refused: `=` is not associative, so
      // the two orders differ). Anything else is refused rather than answered
      // differently.
      refuse(msg::reduceScanVerbs);
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
        if (operandCells.empty()) {
          if (!identity)
            throw std::invalid_argument(noIdentityMessage(verbName));
          const auto result = ctx.core().arena().makeCell();
          emitConstant(ctx, result, *identity);
          return result;
        }
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
    const auto axis      = axes->front();
    const auto walk      = axisWalkOf(ctx, *rCell);
    const CellRef anchor = walk.anchor;
    const DimVector dir  = walk.direction;
    const auto stop      = walk.stop;

    // 1 when the walk has a first cell, 0 for an empty rank (an absent
    // anchor): the anchor is its own stop, so the count cannot pass one.
    const auto present = emit::extentOf(ctx, anchor, axis.dim, dir, anchor);

    if (adverb == TokenKind::Reduce) {
      const auto acc = ctx.core().arena().makeCell();
      std::optional<CellRef> emptyLabel;
      std::optional<CellRef> endLabel;
      if (identity) {
        emptyLabel = emit::mintLabel(ctx, "#REDUCE_EMPTY");
        endLabel   = emit::mintLabel(ctx, "#REDUCE_END");
        emit::branchIfFalse(ctx, present, *emptyLabel, "#REDUCE_IF_EMPTY");
      } else {
        assertCell(ctx, present, "#REDUCE_NONEMPTY",
                   noIdentityMessage(verbName));
      }
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

      if (identity) {
        emit::branchAlways(ctx, *endLabel, "#REDUCE_DONE");
        emit::placeLabel(ctx, *emptyLabel);
        emitConstant(ctx, acc, *identity);
        emit::placeLabel(ctx, *endLabel);
      }

      emit::mergeRankInputDependencies(ctx, acc, std::array{*rCell});
      mergeDependencies(ctx.cellDependencies()[acc],
                        ctx.rankDependenciesOf(*rCell));
      return acc;
    }

    if (adverb == TokenKind::Scan) {
      const auto resultDim = ctx.core().dims().step;
      const auto result    = beginResult(ctx, resultDim);
      const auto acc       = ctx.core().arena().makeCell();
      // An empty rank scans to an empty rank: nothing is minted, and the
      // result's first-cell read below finds nothing.
      const auto skip = emit::mintLabel(ctx, "#SCAN_EMPTY");
      emit::branchIfFalse(ctx, present, skip, "#SCAN_IF_EMPTY");
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
      emit::placeLabel(ctx, skip);

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
  const auto axis = axes->front();
  const auto walk = axisWalkOf(ctx, rCell);
  return emit::extentOf(ctx, walk.anchor, axis.dim, walk.direction, walk.stop);
}

CellRef lowerFoldDyadic(LowerContext &ctx, const DyadicExpr &,
                        const AdverbExpr &, CellRef lCell, CellRef rCell) {
  refuseDyadicOperands(ctx, lCell, rCell);
}

} // namespace xanadu::vpl::lower
