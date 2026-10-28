#include "common/xanadu/vortex/vortex_vm.hpp"

#include <limits>
#include <stdexcept>

namespace zigzag::vortex {
namespace {

ExecutionResult invalidStreamOperation(const std::string_view message) {
  return {.success = false, .errorMessage = std::string{message}};
}

} // namespace

bool VortexVM::streamStateOpcode(const OpcodeKind kind) const noexcept {
  switch (kind) {
  case OpcodeKind::StreamClear:
  case OpcodeKind::StreamAppend:
  case OpcodeKind::StreamBegin:
  case OpcodeKind::StreamReverseBegin:
  case OpcodeKind::RankBegin:
  case OpcodeKind::IteratorNext:
    return true;
  default:
    return false;
  }
}

void VortexVM::clearTypedRegister(const CellRef slot) {
  streamSlots_.erase(slot);
  iteratorSlots_.erase(slot);
  streams_.erase(slot);
  iterators_.erase(slot);
}

void VortexVM::requireStreamCursor(const CellRef cursor) {
  if (const auto owner = streamCursor_; owner && *owner != cursor) {
    const auto pc       = getCursorOpcode(*owner);
    const auto finished = !pc || getOpcodeKind(*pc) == OpcodeKind::Halt;
    if (!finished) {
      throw std::runtime_error(
          "Vortex VM supports one active stream cursor at a time");
    }
    if (!choiceStack_.empty()) {
      throw std::runtime_error(
          "stream cursor ownership cannot change while choice points remain");
    }
    streams_.clear();
    iterators_.clear();
  } else if (!owner && !choiceStack_.empty()) {
    throw std::runtime_error(
        "stream cursor ownership cannot start while choice points remain");
  }
  streamCursor_ = cursor;
}

std::vector<CellRef> VortexVM::resultCells(const CellRef slot) const {
  if (iteratorSlots_.contains(slot)) return {};
  if (const auto found = streams_.find(slot); found != streams_.end()) {
    return found->second ? *found->second : std::vector<CellRef>{};
  }
  if (streamSlots_.contains(slot)) return {};
  if (const auto found = cellReferences_.find(slot);
      found != cellReferences_.end()) {
    return found->second && core_.arena().contains(*found->second)
               ? std::vector<CellRef>{*found->second}
               : std::vector<CellRef>{};
  }
  return core_.arena().contains(slot) ? std::vector<CellRef>{slot}
                                      : std::vector<CellRef>{};
}

ExecutionResult VortexVM::executeCellRefCopy(const OpcodeKind kind,
                                             const CellRef opcode,
                                             std::vector<CellValue> &outputs) {
  const auto sourceSlots = core_.inputsOf(opcode);
  const auto targetSlots = core_.outputsOf(opcode);
  struct SourceSnapshot {
    enum class Kind { Scalar, Stream, Iterator } kind{Kind::Scalar};
    std::optional<CellRef> scalar;
    std::shared_ptr<std::vector<CellRef>> stream;
    std::shared_ptr<IteratorState> iterator;
  };
  std::vector<SourceSnapshot> sources;
  if (kind == OpcodeKind::CopyCellRefs) {
    sources.reserve(targetSlots.size());
    for (std::size_t index = 0; index < targetSlots.size(); ++index) {
      const CellRef source =
          index < sourceSlots.size() ? sourceSlots[index] : noCell;
      if (streamSlots_.contains(source)) {
        const auto found = streams_.find(source);
        sources.push_back(
            {.kind   = SourceSnapshot::Kind::Stream,
             .stream = found != streams_.end()
                           ? found->second
                           : std::make_shared<std::vector<CellRef>>()});
      } else if (iteratorSlots_.contains(source)) {
        const auto found = iterators_.find(source);
        sources.push_back(
            {.kind     = SourceSnapshot::Kind::Iterator,
             .iterator = found != iterators_.end() ? found->second : nullptr});
      } else {
        sources.push_back({.scalar = resultCell(source)});
      }
    }
  }
  outputs.reserve(targetSlots.size());
  for (std::size_t index = 0; index < targetSlots.size(); ++index) {
    const CellRef target = targetSlots[index];
    if (kind == OpcodeKind::ClearCellRefs) {
      cellReferences_.erase(target);
      if (streamSlots_.contains(target))
        streams_[target] = std::make_shared<std::vector<CellRef>>();
      if (iteratorSlots_.contains(target)) iterators_.erase(target);
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
      continue;
    }

    const auto &source = sources[index];
    if (source.kind == SourceSnapshot::Kind::Stream) {
      streamSlots_.insert(target);
      iteratorSlots_.erase(target);
      cellReferences_.erase(target);
      streams_.insert_or_assign(target, source.stream);
      iterators_.erase(target);
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
    } else if (source.kind == SourceSnapshot::Kind::Iterator) {
      iteratorSlots_.insert(target);
      streamSlots_.erase(target);
      cellReferences_.erase(target);
      if (source.iterator)
        iterators_.insert_or_assign(target, source.iterator);
      else
        iterators_.erase(target);
      streams_.erase(target);
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
    } else {
      clearTypedRegister(target);
      outputs.emplace_back(
          static_cast<std::int64_t>(source.scalar.value_or(noCell)));
    }
  }
  return {};
}

ExecutionResult
VortexVM::executeStreamOpcode(const OpcodeKind kind, const CellRef opcode,
                              std::vector<CellValue> &outputs,
                              const std::optional<CellRef> cursor) {
  if (!cursor) {
    return invalidStreamOperation(
        "stream and iterator opcodes require a VM cursor");
  }
  requireStreamCursor(*cursor);
  const auto inputs = core_.inputsOf(opcode);
  const auto slots  = core_.outputsOf(opcode);
  if (slots.empty())
    return invalidStreamOperation(
        "stream and iterator opcodes require output registers");

  switch (kind) {
  case OpcodeKind::StreamClear: {
    if (!inputs.empty() || slots.size() != 1)
      return invalidStreamOperation("StreamClear expects one output register");
    const CellRef slot = slots.front();
    streamSlots_.insert(slot);
    iteratorSlots_.erase(slot);
    cellReferences_.erase(slot);
    iterators_.erase(slot);
    streams_[slot] = std::make_shared<std::vector<CellRef>>();
    break;
  }
  case OpcodeKind::StreamAppend: {
    if (slots.size() != 1)
      return invalidStreamOperation("StreamAppend expects one output register");
    const CellRef target = slots.front();
    struct InputSnapshot {
      std::shared_ptr<const std::vector<CellRef>> cells;
      std::size_t selfCount{0};
    };
    std::vector<InputSnapshot> snapshots;
    snapshots.reserve(inputs.size());
    for (const CellRef input : inputs) {
      if (iteratorSlots_.contains(input))
        return invalidStreamOperation(
            "an iterator register cannot be appended as a stream");
      const auto found = streams_.find(input);
      if (found != streams_.end()) {
        if (input == target) {
          snapshots.push_back(
              {.selfCount = found->second ? found->second->size() : 0});
        } else if (found->second) {
          snapshots.push_back({.cells = found->second});
        } else {
          snapshots.emplace_back();
        }
        continue;
      }
      if (streamSlots_.contains(input)) {
        snapshots.emplace_back();
      } else if (const auto cell = resultCell(input)) {
        snapshots.push_back(
            {.cells = std::make_shared<const std::vector<CellRef>>(1, *cell)});
      } else {
        snapshots.emplace_back();
      }
    }

    for (const auto &snapshot : snapshots) {
      if (snapshot.cells) {
        for (std::size_t i = 0; i < snapshot.cells->size(); ++i)
          core_.arena().consumeWork();
      }
      for (std::size_t i = 0; i < snapshot.selfCount; ++i)
        core_.arena().consumeWork();
    }

    auto accumulated = streams_.find(target);
    std::shared_ptr<std::vector<CellRef>> values =
        accumulated == streams_.end() ? nullptr : accumulated->second;
    const bool sharedWithOtherState = values && values.use_count() > 2;
    if (!values) {
      values = std::make_shared<std::vector<CellRef>>();
    } else if (sharedWithOtherState) {
      values = std::make_shared<std::vector<CellRef>>(*values);
    }
    for (const auto &snapshot : snapshots) {
      if (snapshot.cells) {
        values->insert(values->end(), snapshot.cells->begin(),
                       snapshot.cells->end());
      }
      for (std::size_t i = 0; i < snapshot.selfCount; ++i) {
        const CellRef cell = (*values)[i];
        values->push_back(cell);
      }
    }
    streamSlots_.insert(target);
    iteratorSlots_.erase(target);
    cellReferences_.erase(target);
    iterators_.erase(target);
    streams_[target] = std::move(values);
    break;
  }
  case OpcodeKind::StreamBegin:
  case OpcodeKind::StreamReverseBegin: {
    if (slots.size() != 1)
      return invalidStreamOperation(
          "StreamBegin expects one iterator register");
    std::vector<std::shared_ptr<const std::vector<CellRef>>> chunks;
    for (const CellRef input : inputs) {
      if (iteratorSlots_.contains(input))
        return invalidStreamOperation(
            "StreamBegin cannot snapshot an iterator register");
      const auto found = streams_.find(input);
      if (found != streams_.end()) {
        if (found->second) chunks.push_back(found->second);
        continue;
      }
      if (streamSlots_.contains(input)) continue;
      if (const auto cell = resultCell(input)) {
        chunks.push_back(
            std::make_shared<const std::vector<CellRef>>(1, *cell));
      }
    }
    const CellRef target = slots.front();
    iteratorSlots_.insert(target);
    streamSlots_.erase(target);
    streams_.erase(target);
    cellReferences_.erase(target);
    SequenceIteratorState state{.chunks = std::move(chunks)};
    state.reverse = kind == OpcodeKind::StreamReverseBegin;
    if (state.reverse) state.chunkIndex = state.chunks.size();
    iterators_[target] = std::make_shared<IteratorState>(std::move(state));
    break;
  }
  case OpcodeKind::RankBegin: {
    if (inputs.size() != 3 || slots.size() != 1)
      return invalidStreamOperation(
          "RankBegin expects anchor, dimension, direction, and one iterator");
    const auto anchor   = resultCell(inputs[0]);
    const auto dimValue = inputValue(inputs[1]);
    const auto dirValue = inputValue(inputs[2]);
    if (!std::holds_alternative<std::int64_t>(dimValue) ||
        !std::holds_alternative<std::int64_t>(dirValue))
      return invalidStreamOperation(
          "RankBegin dimension and direction must be integers");
    const auto rawDimension = std::get<std::int64_t>(dimValue);
    const auto rawDirection = std::get<std::int64_t>(dirValue);
    if (rawDimension < 0 ||
        static_cast<std::uint64_t>(rawDimension) >
            std::numeric_limits<DimRef>::max() ||
        (rawDirection != -1 && rawDirection != 1))
      return invalidStreamOperation("RankBegin has an invalid dimension");
    RankIteratorState state{
        .current   = anchor,
        .start     = anchor,
        .dimension = static_cast<DimRef>(rawDimension),
        .direction = rawDirection < 0 ? DimVector::NEG : DimVector::POS,
        .done      = !anchor,
    };
    if (anchor) {
      if (!core_.arena().contains(*anchor))
        return invalidStreamOperation("RankBegin anchor is not a cell");
      state.seen.insert(*anchor);
    }
    const CellRef target = slots.front();
    iteratorSlots_.insert(target);
    streamSlots_.erase(target);
    streams_.erase(target);
    cellReferences_.erase(target);
    iterators_[target] = std::make_shared<IteratorState>(std::move(state));
    break;
  }
  case OpcodeKind::IteratorNext: {
    if (inputs.size() != 1 || slots.size() != 2)
      return invalidStreamOperation(
          "IteratorNext expects one iterator and item/presence outputs");
    const CellRef source = inputs.front();
    if (source == slots[0] || source == slots[1] || slots[0] == slots[1])
      return invalidStreamOperation(
          "IteratorNext requires distinct iterator, item, and presence slots");
    if (!iteratorSlots_.contains(source))
      return invalidStreamOperation("IteratorNext input is not an iterator");
    const auto found = iterators_.find(source);
    if (found == iterators_.end() || !found->second)
      return invalidStreamOperation("IteratorNext iterator is not initialized");
    if (!found->second.unique())
      found->second = std::make_shared<IteratorState>(*found->second);

    std::optional<CellRef> item;
    if (auto *sequence = std::get_if<SequenceIteratorState>(&*found->second)) {
      while (sequence->reverse
                 ? sequence->chunkIndex > 0
                 : sequence->chunkIndex < sequence->chunks.size()) {
        const auto chunkIndex =
            sequence->reverse ? sequence->chunkIndex - 1 : sequence->chunkIndex;
        const auto &chunk = sequence->chunks[chunkIndex];
        if (chunk && sequence->index < chunk->size()) {
          core_.arena().consumeWork();
          const auto index = sequence->reverse
                                 ? chunk->size() - 1 - sequence->index
                                 : sequence->index;
          item             = (*chunk)[index];
          ++sequence->index;
          break;
        }
        core_.arena().consumeWork();
        if (sequence->reverse)
          --sequence->chunkIndex;
        else
          ++sequence->chunkIndex;
        sequence->index = 0;
      }
    } else {
      auto &rank = std::get<RankIteratorState>(*found->second);
      if (!rank.done && rank.current && rank.dimension) {
        core_.arena().consumeWork();
        const auto next = core_.arena().linkedMinting(
            *rank.current, *rank.dimension, rank.direction);
        if (!next || rank.seen.contains(*next)) {
          rank.done = true;
        } else {
          item         = *next;
          rank.current = *next;
          rank.seen.insert(*next);
        }
      } else {
        rank.done = true;
      }
    }

    const CellRef itemSlot    = slots[0];
    const CellRef presentSlot = slots[1];
    streamSlots_.erase(itemSlot);
    iteratorSlots_.erase(itemSlot);
    streams_.erase(itemSlot);
    iterators_.erase(itemSlot);
    writeCellReferences(std::span<const CellRef>{&itemSlot, 1},
                        {static_cast<std::int64_t>(item.value_or(noCell))});
    core_.writeValue(presentSlot, 0, std::nullopt, CellValue{item.has_value()});
    cellReferences_.erase(presentSlot);
    streamSlots_.erase(presentSlot);
    iteratorSlots_.erase(presentSlot);
    streams_.erase(presentSlot);
    iterators_.erase(presentSlot);
    break;
  }
  default:
    return invalidStreamOperation("invalid stream opcode dispatch");
  }

  outputs.clear();
  return {};
}

} // namespace zigzag::vortex
