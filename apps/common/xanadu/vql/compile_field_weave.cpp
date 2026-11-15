// Q9: field weaving in return clauses.
#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

void VQLCompiler::compileFieldWeave(const FieldWeave &weave,
                                    const CellRef resultStream) {
  std::vector<CellRef> stream;
  if (ambientContext_) {
    stream = {*ambientContext_};
  } else if (!contextInputs_.empty()) {
    stream = {contextInputs_.back()};
  } else {
    stream = {core_.home()};
  }
  stream = compileSteps(weave.steps, std::move(stream),
                        ambientContext_.has_value() || !contextInputs_.empty());
  emitAppendStream(resultStream, stream);
}

} // namespace xanadu::vql
