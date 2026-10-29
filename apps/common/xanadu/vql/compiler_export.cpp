#include "common/xanadu/vortex/program_export.hpp"
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {

xanadu::MicroversionId
VQLCompiler::exportToStore(xanadu::Store &store,
                           std::optional<xanadu::MicroversionId> parent) const {
  return zigzag::vortex::exportProgram(core_, vm_, allOps_, store, parent);
}

} // namespace xanadu::vql
