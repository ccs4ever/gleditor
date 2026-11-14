/**
 * @file lower_misc.cpp
 * @brief Card V13: print marker, compress, key, rank operator and named
 * refusals.
 *
 * Provides named refusals for VPL features not yet lowered to the VM:
 * scrub (⍫), compress (⌿), rank conjunction (⍤), key conjunction (⌸),
 * and quad functions (⎕READ, ⎕SPLIT, ⎕).
 */
#include <string_view>

#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::lower {
namespace {

constexpr std::string_view kScrubUnsupported =
    "VPL scrub (⍫) is unsupported by the VM";
constexpr std::string_view kCompressUnsupported =
    "VPL compress (⌿) is unsupported by the VM";
constexpr std::string_view kRankUnsupported =
    "VPL rank conjunction (⍤) is unsupported by the VM";
constexpr std::string_view kKeyUnsupported =
    "VPL key conjunction (⌸) is unsupported by the VM";
constexpr std::string_view kConjunctionUnsupported =
    "unsupported VPL conjunction in VM compiler";
constexpr std::string_view kQuadReadUnsupported =
    "VPL ⎕READ is unsupported by the VM (requires store reader)";
constexpr std::string_view kQuadSplitUnsupported =
    "VPL ⎕SPLIT is unsupported by the VM (requires string split)";
constexpr std::string_view kQuadUnsupported =
    "VPL quad IO is unsupported by the VM";

[[noreturn]] void refuseConjunctionKind(TokenKind kind) {
  if (kind == TokenKind::RankOp) {
    refuse(kRankUnsupported);
  }
  if (kind == TokenKind::Key) {
    refuse(kKeyUnsupported);
  }
  refuse(kConjunctionUnsupported);
}

} // namespace

Maybe lowerScrubMonadic(LowerContext &, const MonadicExpr &, CellRef) {
  refuse(kScrubUnsupported);
}

CellRef lowerScrubDyadic(LowerContext &, const DyadicExpr &) {
  refuse(kScrubUnsupported);
}

CellRef lowerCompressDyadic(LowerContext &, const DyadicExpr &, CellRef,
                            CellRef) {
  refuse(kCompressUnsupported);
}

Maybe lowerConjunctionMonadic(LowerContext &, const MonadicExpr &,
                              const ConjunctionExpr &conjunction) {
  refuseConjunctionKind(conjunction.conjunction());
}

CellRef lowerConjunctionDyadic(LowerContext &, const DyadicExpr &,
                               const ConjunctionExpr &conjunction, CellRef,
                               CellRef) {
  refuseConjunctionKind(conjunction.conjunction());
}

CellRef lowerConjunctionExpr(LowerContext &, const ConjunctionExpr &expr) {
  refuseConjunctionKind(expr.conjunction());
}

Maybe lowerQuad(LowerContext &, const QuadExpr &expr) {
  if (expr.quadKind() == TokenKind::QuadRead) {
    refuse(kQuadReadUnsupported);
  }
  if (expr.quadKind() == TokenKind::QuadSplit) {
    refuse(kQuadSplitUnsupported);
  }
  refuse(kQuadUnsupported);
}

} // namespace xanadu::vpl::lower
