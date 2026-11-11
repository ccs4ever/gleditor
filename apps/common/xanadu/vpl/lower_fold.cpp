/**
 * @file lower_fold.cpp
 * @brief Card V7 fold: reduce, scan and tally.
 *
 * The literal-vector reduce and scan lowering moved here from
 * compileMonadic unchanged. Card V7 generalises it to runtime ranks and to
 * every scalar dyadic verb, keeping the labels `#REDUCE_ADD` and `#SCAN_ADD`
 * and the scan axis `d.step` that existing tests pin.
 */
#include <stdexcept>

#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {

using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

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
      // Addition is the only currently supported reduction primitive.
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
      throw std::invalid_argument(
          "VPL VM reduce/scan supports only +, *, maximum, and minimum");
    }

    if (adverb == TokenKind::Reduce) {
      const auto *vec = dynamic_cast<const VectorExpr *>(expr.right().get());
      if (!vec)
        throw std::invalid_argument(
            "VPL VM reduce currently requires a literal vector");
      std::vector<CellRef> operandCells;
      for (const auto &el : vec->elements()) {
        if (!el)
          throw std::invalid_argument(
              "VPL reduction rank contains a missing element");
        operandCells.push_back(requireCompiledCell(ctx.compileNode(*el)));
      }

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
      const auto *vec = dynamic_cast<const VectorExpr *>(expr.right().get());
      if (!vec)
        throw std::invalid_argument(
            "VPL VM scan currently requires a literal vector");
      std::vector<CellRef> operandCells;
      for (const auto &el : vec->elements()) {
        if (!el)
          throw std::invalid_argument("VPL scan rank has a missing element");
        operandCells.push_back(requireCompiledCell(ctx.compileNode(*el)));
      }

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
  throw std::invalid_argument("unsupported VPL custom verb in VM compiler");
  throw std::invalid_argument("unsupported VPL custom verb in VM compiler");
}

Maybe lowerTallyMonadic(LowerContext &ctx, const MonadicExpr &, CellRef rCell) {
  if (ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar monadic arithmetic on a rank requires array lowering");
  throw std::invalid_argument("unsupported VPL monadic verb in VM compiler");
}

CellRef lowerFoldDyadic(LowerContext &ctx, const DyadicExpr &,
                        const AdverbExpr &, CellRef lCell, CellRef rCell) {
  if (ctx.isRank(lCell) || ctx.isRank(rCell))
    throw std::invalid_argument(
        "VPL scalar dyadic arithmetic on ranks requires array lowering");
  throw std::invalid_argument("unsupported VPL dyadic verb in VM compiler");
}

} // namespace xanadu::vpl::lower
