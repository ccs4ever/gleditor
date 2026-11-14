/**
 * @file lower_hypertime.cpp
 * @brief Card V12: hypertime and transclusion.
 *
 * Monadic hypertime (⍟ / time.) emits OpcodeKind::Hypertime, which reads
 * the arena's high-water mark (operation count) as an Int64.
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
