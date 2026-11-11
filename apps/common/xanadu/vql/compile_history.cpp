// Q3: version pins, pin arithmetic, version values, historical anchors.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef>
VQLCompiler::compileVersionPinnedStep(const PathStep & /*step*/,
                                      std::vector<CellRef> /*stream*/,
                                      const bool /*hasContext*/) {
  throw std::invalid_argument("version pins require historical VM lowering");
}

std::optional<CellRef>
VQLCompiler::compileVersionPinnedPath(const PathExpression & /*path*/,
                                      const CellRef /*ctxCell*/) {
  throw std::invalid_argument("version pins require historical VM lowering");
}

std::optional<CellRef>
VQLCompiler::compileHistoricalAnchor(const AnchorNode & /*anchor*/) {
  throw std::invalid_argument(
      "store generators and historical anchors require VM selector lowering");
}

std::optional<CellRef>
VQLCompiler::compileVersionValue(const VersionExpr & /*version*/,
                                 const CellRef /*ctxCell*/) {
  throw std::invalid_argument(
      "value expression requires Vortex graph lowering");
}

} // namespace xanadu::vql
