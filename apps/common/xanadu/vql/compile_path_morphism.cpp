// Q10: PathMorphism steps (`$p/+d.comment => [ ... ]`), with or without a
// perspective qualifier. Nothing lowers them yet; the stub keeps the refusal
// the generic selector fallback used to give.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

std::vector<CellRef>
VQLCompiler::compilePathMorphismStep(const PathStep & /*step*/,
                                     const CellRef /*current*/,
                                     const bool /*hasContext*/) {
  throw QueryRefusal(
      QueryErrorCategory::Unsupported,
      "this VQL path selector does not have Vortex graph lowering yet");
}

} // namespace xanadu::vql
