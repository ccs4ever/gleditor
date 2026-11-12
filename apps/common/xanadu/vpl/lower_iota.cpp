/**
 * @file lower_iota.cpp
 * @brief Card V3: iota (monadic and dyadic) and monadic rho.
 *
 * Both are lowered as ordinary graph operations, so the work happens when the
 * VM runs the program and N may be a run-time number:
 *
 *  - `n ⍳ d.k` appends N cells (values 1..N) after the tail of H's rank along
 *    d.k and yields a register holding the first of them; the oracle did the
 *    same. `⍳ n` does the same behind a private anchor cell on d.1, so each
 *    site owns a chain of its own.
 *  - `⍴ A` mints a rank of valence(A) cells on d.1 whose values are the
 *    extents of A along its axes; a valence is static, an extent is walked.
 *
 * An empty result is a register that holds no cell at run time; the executor
 * turns it into a zero-length array that keeps its axes.
 */
#include <cstdint>

#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {
namespace {

using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::DirectedDim;
using zigzag::vortex::OpcodeKind;

// The dimension a rank-minting verb uses when the program names none.
constexpr std::string_view kDefaultDimension = "d.1";
constexpr std::int64_t kIotaOrigin           = 1;

constexpr std::string_view kIotaCountOnRank =
    "VPL iota of a rank argument is unsupported by the VM";
constexpr std::string_view kIotaDimension =
    "VPL dyadic iota requires a single dimension (d.1) as its right operand";
constexpr std::string_view kRhoEnclosed =
    "VPL monadic shape of an enclosed value is unsupported by the VM";
constexpr std::string_view kRhoDimension =
    "VPL monadic shape of a dimension value is unsupported by the VM";

std::int64_t asValue(DimRef dimension) {
  return static_cast<std::int64_t>(dimension);
}

std::int64_t asValue(DimVector direction) {
  return static_cast<std::int64_t>(zigzag::toSign(direction));
}

// Link read form: the cell after `from`, or an absent register.
CellRef readNext(LowerContext &ctx, CellRef from, DimRef dimension) {
  const auto read = ctx.emitOp(OpcodeKind::Link, "#IOTA_FIRST");
  ctx.emitInput(read, from);
  ctx.emitInputConstant(read, asValue(dimension));
  ctx.emitInputConstant(read, asValue(DimVector::POS));
  const auto out = ctx.core().arena().makeCell();
  ctx.emitOutput(read, out);
  return out;
}

// Appends `count` cells valued 1..count after the tail of the rank that
// starts at `anchor` on `dimension`; returns a register for the first one.
CellRef mintIota(LowerContext &ctx, CellRef count, CellRef anchor,
                 DimRef dimension) {
  auto &arena     = ctx.core().arena();
  const auto tail = emit::extreme(ctx, anchor, dimension, DimVector::POS);
  // The register `tail` is advanced by every mint, so the old tail is kept
  // to find the first new cell afterwards.
  const auto before = emit::aliasRegister(ctx, tail);
  emit::countedLoop(ctx, count, [&](CellRef index) {
    const auto fresh = emit::mintAfter(ctx, tail, dimension, DimVector::POS);
    // An Add writes a plain integer; a Value/Link output could turn the number
    // into a cell reference once it equals a live cell id.
    const auto number = arena.makeCell();
    const auto step   = ctx.emitOp(OpcodeKind::Add, "#IOTA_VALUE");
    ctx.emitInput(step, index);
    ctx.emitInputConstant(step, kIotaOrigin);
    ctx.emitOutput(step, number);
    emit::copyValue(ctx, number, fresh);
  });
  return readNext(ctx, before, dimension);
}

CellRef finishIota(LowerContext &ctx, CellRef first, DimRef dimension,
                   CellRef count) {
  ctx.rankAxes()[first] = {DirectedDim{dimension, DimVector::POS}};
  const std::array inputs{count};
  emit::mergeRankInputDependencies(ctx, first, inputs);
  return first;
}

CellRef compileCount(LowerContext &ctx, const std::shared_ptr<AstNode> &node) {
  if (!node) throw std::invalid_argument("VPL iota requires an argument");
  const auto count = requireCompiledCell(ctx.compileNode(*node));
  if (ctx.isRank(count) || ctx.dimensionValuesOf(count) != nullptr ||
      ctx.isEnclosed(count)) {
    refuse(kIotaCountOnRank);
  }
  return count;
}

} // namespace

Maybe lowerIotaMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  const auto count     = compileCount(ctx, expr.right());
  const auto dimension = ctx.resolveDimension(kDefaultDimension);
  const auto anchor    = ctx.core().arena().makeCell();
  return finishIota(ctx, mintIota(ctx, count, anchor, dimension), dimension,
                    count);
}

Maybe lowerRhoMonadic(LowerContext &ctx, const MonadicExpr &expr) {
  if (!expr.right()) {
    throw std::invalid_argument("VPL shape requires an argument");
  }
  const auto operand = requireCompiledCell(ctx.compileNode(*expr.right()));
  if (ctx.dimensionValuesOf(operand) != nullptr) refuse(kRhoDimension);
  if (ctx.isEnclosed(operand)) refuse(kRhoEnclosed);

  const auto *axes = ctx.axesOf(operand);
  // The shape of a scalar has no axes to measure; the oracle answered 0.
  if (axes == nullptr || axes->empty()) {
    return ctx.emitConstant(std::int64_t{0});
  }

  const auto dimension = ctx.resolveDimension(kDefaultDimension);
  const auto measured  = *axes;
  std::vector<CellRef> cells;
  cells.reserve(measured.size());
  for (const auto &axis : measured) {
    // Each extent is measured from the origin, which is how the first row's
    // length stands for every row of a rectangular array.
    const auto extent = emit::extentOf(ctx, operand, axis.dim, axis.dir);
    const auto cell   = ctx.core().arena().makeCell();
    // The cell needs a value before the first Value op can replace it.
    ctx.core().writeValue(cell, 0, std::nullopt, std::int64_t{0});
    emit::copyValue(ctx, extent, cell);
    cells.push_back(cell);
  }
  zigzag::expectWritten(
      ctx.core().arena().linkChain(cells, dimension, zigzag::DimVector::POS));
  const auto head      = cells.front();
  ctx.rankAxes()[head] = {DirectedDim{dimension, DimVector::POS}};
  const std::array inputs{operand};
  emit::mergeRankInputDependencies(ctx, head, inputs);
  return head;
}

CellRef lowerIotaDyadic(LowerContext &ctx, const DyadicExpr &expr) {
  const auto *dimExpr =
      expr.right() ? dynamic_cast<const DimensionExpr *>(expr.right().get())
                   : nullptr;
  if (dimExpr == nullptr) refuse(kIotaDimension);
  const auto count     = compileCount(ctx, expr.left());
  const auto dimension = ctx.resolveDimension(dimExpr->name());
  return finishIota(ctx, mintIota(ctx, count, ctx.core().home(), dimension),
                    dimension, count);
}

} // namespace xanadu::vpl::lower
