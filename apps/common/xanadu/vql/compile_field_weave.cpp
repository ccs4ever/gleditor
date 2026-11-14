// Q9: field weaving in return clauses.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

void VQLCompiler::compileFieldWeave(const FieldWeave &weave,
                                    const CellRef resultStream) {
  // A FieldWeave is a sequence of PathSteps rooted at resultStream (or current
  // walk/context). In the engine, evaluateStep evaluates each step over current
  // cells. In the compiler, we compile the steps starting from resultStream as
  // the stream.
  std::vector<CellRef> stream{resultStream};
  stream = compileSteps(weave.steps, std::move(stream),
                        ambientContext_.has_value() || !contextInputs_.empty());
  emitAppendStream(resultStream, stream);
}

} // namespace xanadu::vql
