// Q9: field weaving in return clauses.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;

void VQLCompiler::compileFieldWeave(const FieldWeave &weave,
                                    const CellRef resultStream) {
  std::vector<CellRef> stream{resultStream};
  stream = compileSteps(weave.steps, std::move(stream),
                        ambientContext_.has_value() || !contextInputs_.empty());
  emitAppendStream(resultStream, stream);
}

} // namespace xanadu::vql
