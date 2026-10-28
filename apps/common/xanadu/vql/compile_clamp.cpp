#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

std::vector<CellRef> VQLCompiler::emitClamp(std::span<const CellRef> inputs,
                                            const RangeClamp &clamp) {
  const auto result     = emitStreamClear();
  const auto zero       = emitConstant(std::int64_t{0});
  const auto one        = emitConstant(std::int64_t{1});
  const auto arithmetic = [&](OpcodeKind kind, CellRef left, CellRef right,
                              std::optional<CellRef> destination =
                                  std::nullopt) {
    const auto opcode = emitOp(kind, "#CLAMP_ARITHMETIC");
    emitInput(opcode, left);
    emitInput(opcode, right);
    const auto output = destination ? *destination : core_.arena().makeCell();
    emitOutput(opcode, output);
    return output;
  };
  const auto iteratorFor = [&] {
    const auto begin = emitOp(OpcodeKind::StreamBegin, "#CLAMP_BEGIN");
    for (const auto input : inputs) emitInput(begin, input);
    const auto iterator = core_.arena().makeCell();
    emitOutput(begin, iterator);
    return iterator;
  };
  // Count the post-predicate stream, including repeated identities. Counters
  // are runtime resets so the same compiled call can be invoked repeatedly.
  const auto count = arithmetic(OpcodeKind::Add, zero, zero);
  emitIteratorLoop(iteratorFor(), [&](CellRef) {
    arithmetic(OpcodeKind::Add, count, one, count);
  });
  const auto end      = vm_.mintOpcode(OpcodeKind::Nop, "#CLAMP_END");
  const auto nonempty = arithmetic(OpcodeKind::Gt, count, zero);
  const auto empty    = emitOp(OpcodeKind::BranchFalseCell, "#CLAMP_EMPTY");
  emitInput(empty, nonempty);
  emitInput(empty, end);
  const auto last      = arithmetic(OpcodeKind::Sub, count, one);
  const auto normalize = [&](std::int64_t index) {
    const auto raw =
        index < 0 ? arithmetic(OpcodeKind::Add, count, emitConstant(index))
                  : emitConstant(index - 1);
    const auto opcode = emitOp(OpcodeKind::Clamp, "#CLAMP_BOUND");
    emitInput(opcode, raw);
    emitInput(opcode, zero);
    emitInput(opcode, last);
    const auto bound = core_.arena().makeCell();
    emitOutput(opcode, bound);
    return bound;
  };
  const auto from  = normalize(clamp.start);
  const auto to    = normalize(clamp.end.value_or(clamp.start));
  const auto index = arithmetic(OpcodeKind::Sub, zero, one);
  emitIteratorLoop(iteratorFor(), [&](CellRef item) {
    arithmetic(OpcodeKind::Add, index, one, index);
    const auto lower    = arithmetic(OpcodeKind::Gte, index, from);
    const auto upper    = arithmetic(OpcodeKind::Lte, index, to);
    const auto included = arithmetic(OpcodeKind::And, lower, upper);
    const auto skip     = vm_.mintOpcode(OpcodeKind::Nop, "#CLAMP_SKIP");
    const auto branch   = emitOp(OpcodeKind::BranchFalseCell, "#CLAMP_FILTER");
    emitInput(branch, included);
    emitInput(branch, skip);
    const auto append = emitOp(OpcodeKind::StreamAppend, "#CLAMP_YIELD");
    emitInput(append, item);
    emitOutput(append, result);
    appendOpcode(skip);
  });
  appendOpcode(end);
  return {result};
}
} // namespace xanadu::vql
