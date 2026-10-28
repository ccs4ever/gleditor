#include "common/xanadu/vql/compiler.hpp"

#include <algorithm>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

void VQLCompiler::appendOpcode(CellRef opcode) {
  if (currentOp_)
    zigzag::expectWritten(
        core_.arena().link(*currentOp_, core_.dims().spin, false, opcode));
  else
    entryOp_ = opcode;
  currentOp_ = opcode;
  allOps_.push_back(opcode);
}

CellRef VQLCompiler::emitStreamClear() {
  const auto result = core_.arena().makeCell();
  const auto clear  = emitOp(OpcodeKind::StreamClear, "#STREAM_CLEAR");
  emitOutput(clear, result);
  dynamicStreams_.insert(result);
  return result;
}

void VQLCompiler::emitIteratorLoop(
    CellRef iterator, gleditor::cpp26::function_ref<void(CellRef)> body) {
  const auto next = emitOp(OpcodeKind::IteratorNext, "#ITERATOR_NEXT");
  emitInput(next, iterator);
  const auto item    = core_.arena().makeCell();
  const auto present = core_.arena().makeCell();
  emitOutput(next, item);
  emitOutput(next, present);
  const auto end = vm_.mintOpcode(OpcodeKind::Nop, "#ITERATOR_END");
  const auto branch =
      emitOp(OpcodeKind::BranchFalseCell, "#ITERATOR_EXHAUSTED");
  emitInput(branch, present);
  emitInput(branch, end);
  body(item);
  const auto repeat = emitOp(OpcodeKind::BranchFalseCell, "#ITERATOR_REPEAT");
  emitInputConstant(repeat, false);
  emitInput(repeat, next);
  appendOpcode(end);
}

CellRef VQLCompiler::emitReverse(std::span<const CellRef> inputs) {
  const auto result = emitStreamClear();
  const auto begin  = emitOp(OpcodeKind::StreamReverseBegin, "#REVERSE_BEGIN");
  for (const auto input : inputs) emitInput(begin, input);
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef item) {
    const auto append = emitOp(OpcodeKind::StreamAppend, "#REVERSE_YIELD");
    emitInput(append, item);
    emitOutput(append, result);
  });
  return result;
}

CellRef VQLCompiler::emitDereference(CellRef candidate) {
  const auto master =
      emitExtreme(candidate, core_.dims().clone, DimVector::NEG);
  const auto terminal = emitOp(OpcodeKind::Link, "#MASTER_TERMINAL");
  emitInput(terminal, master);
  emitInputConstant(terminal, static_cast<std::int64_t>(core_.dims().clone));
  emitInputConstant(terminal, std::int64_t{-1});
  const auto neighbor = core_.arena().makeCell();
  emitOutput(terminal, neighbor);
  const auto repeated = emitCellPresence(neighbor);
  const auto end      = vm_.mintOpcode(OpcodeKind::Nop, "#MASTER_END");
  const auto branch   = emitOp(OpcodeKind::BranchFalseCell, "#MASTER_CYCLE");
  emitInput(branch, repeated);
  emitInput(branch, end);
  const auto restore = emitOp(OpcodeKind::CopyCellRefs, "#MASTER_ORIGINAL");
  emitInput(restore, candidate);
  emitOutput(restore, master);
  appendOpcode(end);
  return master;
}

CellRef VQLCompiler::emitExtreme(CellRef anchor, DimRef dimension,
                                 DimVector direction) {
  const auto extreme = core_.arena().makeCell();
  const auto initial = emitOp(OpcodeKind::CopyCellRefs, "#EXTREME_START");
  emitInput(initial, anchor);
  emitOutput(initial, extreme);
  const auto begin = emitOp(OpcodeKind::RankBegin, "#EXTREME_BEGIN");
  emitInput(begin, anchor);
  emitInputConstant(begin, static_cast<std::int64_t>(dimension));
  emitInputConstant(begin, static_cast<std::int64_t>(toSign(direction)));
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef item) {
    const auto copy = emitOp(OpcodeKind::CopyCellRefs, "#EXTREME_NEXT");
    emitInput(copy, item);
    emitOutput(copy, extreme);
  });
  return extreme;
}

CellRef VQLCompiler::emitRank(CellRef anchor, DimRef dimension,
                              DimVector direction) {
  const auto result = emitStreamClear();
  const auto begin  = emitOp(OpcodeKind::RankBegin, "#RANK_BEGIN");
  emitInput(begin, anchor);
  emitInputConstant(begin, static_cast<std::int64_t>(dimension));
  emitInputConstant(begin, static_cast<std::int64_t>(toSign(direction)));
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef item) {
    const auto append = emitOp(OpcodeKind::StreamAppend, "#RANK_YIELD");
    emitInput(append, item);
    emitOutput(append, result);
  });
  return result;
}

std::vector<CellRef> VQLCompiler::emitMap(
    std::span<const CellRef> inputs,
    gleditor::cpp26::function_ref<std::vector<CellRef>(CellRef)> body) {
  const auto result = emitStreamClear();
  const auto begin  = emitOp(OpcodeKind::StreamBegin, "#STREAM_BEGIN");
  for (const auto input : inputs) emitInput(begin, input);
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef item) {
    struct RestoreContext {
      std::optional<CellRef> &context;
      std::optional<CellRef> saved;
      ~RestoreContext() { context = saved; }
    };
    const RestoreContext restore{ambientContext_, ambientContext_};
    ambientContext_   = item;
    const auto values = body(item);
    emitAppendStream(result, values);
  });
  return {result};
}

void VQLCompiler::emitAppendStream(CellRef output,
                                   std::span<const CellRef> inputs) {
  if (std::ranges::none_of(inputs, [&](CellRef input) {
        return dynamicStreams_.contains(input);
      })) {
    const auto append = emitOp(OpcodeKind::StreamAppend, "#STREAM_YIELD");
    for (const auto input : inputs) emitInput(append, input);
    emitOutput(append, output);
    return;
  }
  const auto begin = emitOp(OpcodeKind::StreamBegin, "#YIELD_BEGIN");
  for (const auto input : inputs) emitInput(begin, input);
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  emitIteratorLoop(iterator, [&](CellRef item) {
    const auto append = emitOp(OpcodeKind::StreamAppend, "#STREAM_YIELD");
    emitInput(append, item);
    emitOutput(append, output);
  });
}

CellRef VQLCompiler::emitFirstCell(CellRef input) {
  if (!dynamicStreams_.contains(input)) return input;
  const auto begin = emitOp(OpcodeKind::StreamBegin, "#SCALAR_BEGIN");
  emitInput(begin, input);
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  const auto next = emitOp(OpcodeKind::IteratorNext, "#SCALAR_FIRST");
  emitInput(next, iterator);
  const auto item    = core_.arena().makeCell();
  const auto present = core_.arena().makeCell();
  emitOutput(next, item);
  emitOutput(next, present);
  return item;
}

CellRef VQLCompiler::emitCellPresence(CellRef candidate) {
  return emitStreamPresence(std::span{&candidate, 1});
}

CellRef VQLCompiler::emitStreamPresence(std::span<const CellRef> inputs) {
  const auto begin = emitOp(OpcodeKind::StreamBegin, "#PRESENCE_BEGIN");
  for (const auto input : inputs) emitInput(begin, input);
  const auto iterator = core_.arena().makeCell();
  emitOutput(begin, iterator);
  const auto next = emitOp(OpcodeKind::IteratorNext, "#PRESENCE_NEXT");
  emitInput(next, iterator);
  const auto item    = core_.arena().makeCell();
  const auto present = core_.arena().makeCell();
  emitOutput(next, item);
  emitOutput(next, present);
  return present;
}
} // namespace xanadu::vql
