/**
 * @file lower_hypertime.cpp
 * @brief Card V12: hypertime and transclusion.
 *
 * Monadic hypertime (⍟ / time.) emits OpcodeKind::Hypertime: the
 * evaluation-time operation high-water mark, as an Int64. It is NOT the
 * microversion the operand was read at, and it does not depend on the
 * operand's value: the operand is compiled and run for its effects (a
 * failing operand fails the program) and its result is dropped.
 *
 * What the VM reads when the opcode runs:
 *   - over a store (an arena with a base manifold): the base's folded-through
 *     operation index, which is the store's operation count. Ephemeral arena
 *     cells are not operations and do not move it, so it is the same for every
 *     program run over that base and changes only when the store gains an
 *     operation.
 *   - over a bare arena: its cell count at that moment, since there every cell
 *     is an operation. That includes the cells of the compiled program itself,
 *     so the number depends on the program's size and on the cells minted
 *     before it; compare two readings only within one arena.
 *
 * Monadic transclusion (⌺ / trans.) remains an explicit refusal until
 * transclusion link walks are implemented on the VM.
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
using zigzag::vortex::OpcodeKind;

constexpr std::string_view kHypertimeDimension =
    "VPL hypertime of a dimension value is unsupported by the VM";
constexpr std::string_view kHypertimeEnclosed =
    "VPL hypertime of an enclosed value is unsupported by the VM";
constexpr std::string_view kTranscludeUnsupported =
    "VPL transclusion (⌺) is unsupported by the VM";

} // namespace

Maybe lowerHypertimeMonadic(LowerContext &ctx, const MonadicExpr &,
                            CellRef rCell) {
  // rCell is compiled by the caller before this runs, so its operations are
  // already in the chain ahead of the Hypertime opcode; the value is unused.
  if (ctx.dimensionValuesOf(rCell) != nullptr) {
    refuse(kHypertimeDimension);
  }
  if (ctx.isEnclosed(rCell)) {
    refuse(kHypertimeEnclosed);
  }

  const auto op  = ctx.emitOp(OpcodeKind::Hypertime, "#HYPERTIME");
  const auto out = ctx.core().arena().makeCell();
  ctx.emitOutput(op, out);

  emit::mergeRankInputDependencies(ctx, out, std::array{rCell});
  return out;
}

Maybe lowerTranscludeMonadic(LowerContext &, const MonadicExpr &, CellRef) {
  refuse(kTranscludeUnsupported);
}

} // namespace xanadu::vpl::lower
