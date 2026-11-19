#include "common/xanadu/vortex/vortex_vm.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_savepoint.hpp"
#include "common/xanadu/zigzag/arena_topology_copy.hpp"

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
  case OpcodeKind::GroupBegin:
  case OpcodeKind::GroupNext:
  case OpcodeKind::PinHistorical:
  case OpcodeKind::ProjectTopology:
    return true;
  default:
    return false;
  }
}

void VortexVM::clearTypedRegister(const CellRef slot) {
  streamSlots_.erase(slot);
  iteratorSlots_.erase(slot);
  groupIteratorSlots_.erase(slot);
  streams_.erase(slot);
  iterators_.erase(slot);
  groupIterators_.erase(slot);
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
    groupIterators_.clear();
    groupIteratorSlots_.clear();
  } else if (!owner && !choiceStack_.empty()) {
    const bool scopedOwner =
        hasScopedChoices() &&
        std::ranges::all_of(choiceStack_, [cursor](const ChoicePoint &choice) {
          return choice.cursor == cursor;
        });
    if (!scopedOwner) {
      throw std::runtime_error("stream cursor ownership cannot start while "
                               "ordinary choice points remain");
    }
  }
  streamCursor_ = cursor;
}

std::vector<CellRef> VortexVM::resultCells(const CellRef slot) const {
  std::vector<CellRef> result;
  if (!iteratorSlots_.contains(slot)) {
    if (const auto found = streams_.find(slot);
        found != streams_.end() && found->second)
      result.reserve(found->second->size());
  }
  forEachResultCell(slot, [&](const CellRef cell) { result.push_back(cell); });
  return result;
}

void VortexVM::forEachResultCell(
    const CellRef slot,
    gleditor::cpp26::function_ref<void(CellRef)> visitor) const {
  if (isGroupsPort(slot))
    throw std::runtime_error("private Groups port cannot be read as a cell");
  if (iteratorSlots_.contains(slot)) return;
  if (const auto found = streams_.find(slot); found != streams_.end()) {
    if (found->second)
      for (const CellRef cell : *found->second) visitor(cell);
    return;
  }
  if (streamSlots_.contains(slot)) return;
  if (const auto found = cellReferences_.find(slot);
      found != cellReferences_.end()) {
    if (found->second && core_.arena().contains(*found->second))
      visitor(*found->second);
    return;
  }
  if (core_.arena().contains(slot)) visitor(slot);
}

ExecutionResult VortexVM::executeCellRefCopy(const OpcodeKind kind,
                                             const CellRef opcode,
                                             std::vector<CellValue> &outputs) {
  const auto sourceSlots = inputsOf(opcode);
  const auto targetSlots = outputsOf(opcode);
  const auto refuse      = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                                .failingClause = opcode,
                                .errorMessage  = std::move(message)};
  };
  for (const CellRef target : targetSlots)
    if (isGroupsPort(target))
      return refuse("generic copy output cannot target a private Groups port");
  struct SourceSnapshot {
    enum class Kind {
      Scalar,
      Stream,
      Iterator,
      GroupIterator
    } kind{Kind::Scalar};
    std::optional<CellRef> scalar;
    std::shared_ptr<std::vector<CellRef>> stream;
    std::shared_ptr<IteratorState> iterator;
    std::shared_ptr<GroupIteratorState> groupIterator;
  };
  std::vector<SourceSnapshot> sources;
  if (kind == OpcodeKind::CopyCellRefs) {
    sources.reserve(targetSlots.size());
    for (std::size_t index = 0; index < targetSlots.size(); ++index) {
      const CellRef source =
          index < sourceSlots.size() ? sourceSlots[index] : noCell;
      if (isGroupsPort(source))
        return refuse("generic copy cannot read a private Groups port");
      if (groupIteratorSlots_.contains(source)) {
        const auto found = groupIterators_.find(source);
        sources.push_back({.kind          = SourceSnapshot::Kind::GroupIterator,
                           .groupIterator = found != groupIterators_.end()
                                                ? found->second
                                                : nullptr});
      } else if (streamSlots_.contains(source)) {
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
  } else {
    for (const CellRef source : sourceSlots)
      if (isGroupsPort(source))
        return refuse("generic clear cannot read a private Groups port");
  }
  outputs.reserve(targetSlots.size());
  for (std::size_t index = 0; index < targetSlots.size(); ++index) {
    const CellRef target = targetSlots[index];
    if (kind == OpcodeKind::ClearCellRefs) {
      cellReferences_.erase(target);
      if (streamSlots_.contains(target))
        streams_[target] = std::make_shared<std::vector<CellRef>>();
      if (iteratorSlots_.contains(target)) iterators_.erase(target);
      if (groupIteratorSlots_.contains(target)) groupIterators_.erase(target);
      groupIteratorSlots_.erase(target);
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
      continue;
    }

    const auto &source = sources[index];
    if (source.kind == SourceSnapshot::Kind::Stream) {
      streamSlots_.insert(target);
      iteratorSlots_.erase(target);
      groupIteratorSlots_.erase(target);
      groupIterators_.erase(target);
      cellReferences_.erase(target);
      streams_.insert_or_assign(target, source.stream);
      iterators_.erase(target);
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
    } else if (source.kind == SourceSnapshot::Kind::GroupIterator) {
      iteratorSlots_.insert(target);
      groupIteratorSlots_.insert(target);
      groupIterators_.erase(target);
      if (source.groupIterator)
        groupIterators_.insert_or_assign(target, source.groupIterator);
      cellReferences_.erase(target);
      streamSlots_.erase(target);
      streams_.erase(target);
      iterators_.erase(target);
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
    } else if (source.kind == SourceSnapshot::Kind::Iterator) {
      iteratorSlots_.insert(target);
      groupIteratorSlots_.erase(target);
      groupIterators_.erase(target);
      cellReferences_.erase(target);
      if (source.iterator)
        iterators_.insert_or_assign(target, source.iterator);
      else
        iterators_.erase(target);
      streamSlots_.erase(target);
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
  if (kind != OpcodeKind::GroupBegin && kind != OpcodeKind::GroupNext)
    requireStreamCursor(*cursor);
  const auto inputs = inputsOf(opcode);
  const auto slots  = outputsOf(opcode);
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
      // Each input can create a chunk even when it carries no stream members.
      // Finish this charged preparation before replacing the target iterator.
      core_.arena().consumeWork();
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
  case OpcodeKind::GroupBegin: {
    if (inputs.size() != 1 || slots.size() != 1)
      return invalidStreamOperation(
          "GroupBegin expects one Groups port and one iterator output");
    const CellRef source = inputs.front();
    const CellRef target = slots.front();
    if (target == source || isGroupsPort(target) ||
        !core_.arena().contains(target) || getOpcodeKind(target))
      return invalidStreamOperation(
          "GroupBegin output must be a distinct public iterator slot");
    const auto ownerFound = groupOwners_.find(source);
    if (ownerFound == groupOwners_.end())
      return invalidStreamOperation("GroupBegin input is not a Groups port");
    const auto owner  = ownerFound->second.lock();
    const auto frames = activationFrames_.find(*cursor);
    if (!owner || frames == activationFrames_.end() || frames->second.empty() ||
        frames->second.back() != owner)
      return invalidStreamOperation(
          "GroupBegin requires the Groups port of the top activation");
    const auto payload = owner->groupsByBacking.find(source);
    if (payload == owner->groupsByBacking.end() || !payload->second)
      return invalidStreamOperation("GroupBegin Groups payload is unavailable");
    try {
      requireStreamCursor(*cursor);
    } catch (const std::exception &error) {
      return invalidStreamOperation(error.what());
    }
    auto state = std::make_shared<GroupIteratorState>(
        owner, payload->second, owner->activationId, *cursor, source);
    // The call touches one slot in seven tables. Journalling those keys costs
    // what the call changes; the whole-table copies it replaces cost the size
    // of every binding the VM holds.
    SetUndo groupIteratorSlots(groupIteratorSlots_, frameProbe_);
    SetUndo iteratorSlots(iteratorSlots_, frameProbe_);
    MapUndo<GroupIteratorMap> groupIterators(groupIterators_, frameProbe_);
    MapUndo<IteratorMap> iterators(iterators_, frameProbe_);
    SetUndo streamSlots(streamSlots_, frameProbe_);
    MapUndo<StreamMap> streams(streams_, frameProbe_);
    MapUndo<CellReferenceMap> references(cellReferences_, frameProbe_);
    try {
      groupIteratorSlots.insert(target);
      iteratorSlots.insert(target);
      groupIterators.set(target, std::move(state));
      iterators.erase(target);
      streamSlots.erase(target);
      streams.erase(target);
      references.erase(target);
    } catch (...) {
      groupIteratorSlots.rollback();
      iteratorSlots.rollback();
      groupIterators.rollback();
      iterators.rollback();
      streamSlots.rollback();
      streams.rollback();
      references.rollback();
      throw;
    }
    break;
  }
  case OpcodeKind::GroupNext: {
    if (inputs.size() != 1 || slots.size() != 2)
      return invalidStreamOperation(
          "GroupNext expects one group iterator and stream/presence outputs");
    const CellRef source         = inputs.front();
    const CellRef streamOutput   = slots[0];
    const CellRef presenceOutput = slots[1];
    if (!groupIteratorSlots_.contains(source))
      return invalidStreamOperation("GroupNext input is not a group iterator");
    if (source == streamOutput || source == presenceOutput ||
        streamOutput == presenceOutput || groupOwners_.contains(streamOutput) ||
        groupOwners_.contains(presenceOutput))
      return invalidStreamOperation(
          "GroupNext requires distinct public output slots");
    const auto iteratorFound = groupIterators_.find(source);
    if (iteratorFound == groupIterators_.end() || !iteratorFound->second)
      return invalidStreamOperation("GroupNext iterator is unavailable");
    const auto &current = *iteratorFound->second;
    const auto owner    = current.owner.lock();
    const auto payload  = current.payload.lock();
    const auto frames   = activationFrames_.find(*cursor);
    if (!owner || !payload || current.cursor != *cursor ||
        current.activationId != owner->activationId ||
        frames == activationFrames_.end() || frames->second.empty() ||
        frames->second.back() != owner)
      return invalidStreamOperation(
          "GroupNext iterator does not belong to the top activation");
    const auto backing = owner->groupsByBacking.find(current.backingSlot);
    if (backing == owner->groupsByBacking.end() || backing->second != payload)
      return invalidStreamOperation("GroupNext Groups payload is unavailable");

    auto nextState     = std::make_shared<GroupIteratorState>(current);
    const bool present = nextState->index < payload->groups.size();
    std::vector<CellRef> selected;
    if (present) {
      const auto &group = payload->groups[nextState->index];
      for (const CellRef cell : group) {
        core_.arena().consumeWork();
        if (!core_.arena().contains(cell))
          return invalidStreamOperation(
              "GroupNext group contains a missing cell");
      }
      selected.reserve(group.size());
      selected.insert(selected.end(), group.begin(), group.end());
      ++nextState->index;
    }
    core_.arena().consumeWork();
    if (!core_.arena().contains(streamOutput) ||
        !core_.arena().contains(presenceOutput) ||
        getOpcodeKind(streamOutput) || getOpcodeKind(presenceOutput))
      return invalidStreamOperation("GroupNext output slot is invalid");

    // requireStreamCursor wipes four tables when another, finished cursor owns
    // the streams. The code this replaced took its table copies first and put
    // them back at the end, so a handover left those tables as they were on
    // success and wiped them on failure. Keep exactly that, copying only in the
    // handover case, which is the one cost that does grow with the tables.
    struct Handover {
      StreamMap streams;
      IteratorMap iterators;
      GroupIteratorMap groupIterators;
      std::unordered_set<CellRef> groupIteratorSlots;
    };
    std::optional<Handover> handover;
    if (streamCursor_ && *streamCursor_ != *cursor) {
      handover =
          Handover{streams_, iterators_, groupIterators_, groupIteratorSlots_};
      frameProbe_.entries += streams_.size() + iterators_.size() +
                             groupIterators_.size() +
                             groupIteratorSlots_.size();
    }
    try {
      requireStreamCursor(*cursor);
    } catch (const std::exception &error) {
      return invalidStreamOperation(error.what());
    }
    if (handover) {
      streams_.swap(handover->streams);
      iterators_.swap(handover->iterators);
      groupIterators_.swap(handover->groupIterators);
      groupIteratorSlots_.swap(handover->groupIteratorSlots);
    }
    MapUndo<StreamMap> streams(streams_, frameProbe_);
    MapUndo<CellReferenceMap> references(cellReferences_, frameProbe_);
    MapUndo<GroupIteratorMap> groupIterators(groupIterators_, frameProbe_);
    SetUndo iteratorSlots(iteratorSlots_, frameProbe_);
    SetUndo groupIteratorSlots(groupIteratorSlots_, frameProbe_);
    SetUndo streamSlots(streamSlots_, frameProbe_);
    MapUndo<IteratorMap> ordinaryIterators(iterators_, frameProbe_);
    zigzag::ArenaSavepoint savepoint(core_.arena());
    try {
      groupIterators.set(source, std::move(nextState));
      streams.set(streamOutput,
                  std::make_shared<std::vector<CellRef>>(std::move(selected)));
      references.erase(streamOutput);
      iteratorSlots.erase(streamOutput);
      groupIteratorSlots.erase(streamOutput);
      ordinaryIterators.erase(streamOutput);
      streamSlots.insert(streamOutput);
      references.erase(presenceOutput);
      streams.erase(presenceOutput);
      iteratorSlots.erase(presenceOutput);
      groupIteratorSlots.erase(presenceOutput);
      ordinaryIterators.erase(presenceOutput);
      streamSlots.erase(presenceOutput);
      core_.writeValue(presenceOutput, 0, std::nullopt, CellValue{present});
    } catch (...) {
      streams.rollback();
      references.rollback();
      groupIterators.rollback();
      iteratorSlots.rollback();
      groupIteratorSlots.rollback();
      streamSlots.rollback();
      ordinaryIterators.rollback();
      if (handover) {
        streams_.clear();
        iterators_.clear();
        groupIterators_.clear();
        groupIteratorSlots_.clear();
      }
      throw;
    }
    savepoint.commit();
    break;
  }
  case OpcodeKind::PinHistorical: {
    if (inputs.size() < 2 || slots.size() != 1) {
      return invalidStreamOperation(
          "PinHistorical expects an input stream, a fold constant, and one "
          "output register");
    }
    const CellRef target = slots.front();
    const auto foldInput = inputValue(inputs.back());
    std::optional<std::uint32_t> spaceId;
    if (std::holds_alternative<std::int64_t>(foldInput)) {
      const auto id = std::get<std::int64_t>(foldInput);
      if (id > 0 &&
          static_cast<std::uint64_t>(id) <= core_.arena().spaceCount()) {
        spaceId = static_cast<std::uint32_t>(id);
      }
    } else if (std::holds_alternative<std::string>(foldInput)) {
      const auto &label = std::get<std::string>(foldInput);
      core_.arena().forEachSpace([&](const std::uint32_t id, const Space &sp) {
        if (sp.label == label) spaceId = id;
      });
    }
    if (!spaceId) {
      return invalidStreamOperation(
          "PinHistorical fold constant does not name a valid attached space");
    }
    const auto attached = core_.arena().spaceAt(*spaceId);
    if (!attached || !attached->manifold) {
      return invalidStreamOperation("PinHistorical space manifold is missing");
    }

    std::vector<CellRef> inputCells;
    for (std::size_t i = 0; i + 1 < inputs.size(); ++i) {
      const CellRef in = inputs[i];
      if (iteratorSlots_.contains(in)) {
        return invalidStreamOperation(
            "an iterator register cannot be pinned as a stream");
      }
      if (const auto found = streams_.find(in); found != streams_.end()) {
        if (found->second) {
          inputCells.insert(inputCells.end(), found->second->begin(),
                            found->second->end());
        }
      } else if (streamSlots_.contains(in)) {
        // empty stream
      } else if (const auto cell = resultCell(in)) {
        inputCells.push_back(*cell);
      }
    }

    std::vector<CellRef> pinned;
    for (const CellRef cell : inputCells) {
      core_.arena().consumeWork();
      auto source = core_.arena().resolveSourceCell(cell);
      if (!source) {
        if (const auto master =
                core_.arena().cloneMaster(cell, core_.dims().clone)) {
          source = core_.arena().resolveSourceCell(*master);
        }
      }
      if (source && source->first == attached->store) {
        if (attached->manifold->contains(source->second)) {
          pinned.push_back(core_.arena().proxyFor(*spaceId, source->second));
        }
      } else if (attached->store &&
                 (attached->store->homeCell() == cell ||
                  core_.arena().homeCell() == cell || core_.home() == cell)) {
        if (const auto home = attached->manifold->home()) {
          pinned.push_back(core_.arena().proxyFor(*spaceId, *home));
        }
      }
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
    values->insert(values->end(), pinned.begin(), pinned.end());
    streamSlots_.insert(target);
    iteratorSlots_.erase(target);
    cellReferences_.erase(target);
    iterators_.erase(target);
    streams_[target] = std::move(values);
    break;
  }
  case OpcodeKind::ProjectTopology: {
    // Inputs: the subject stream, the wildcard flag, the count of added edges,
    // then (dimension, sign) pairs -- the added edges, then the removed ones
    // to the end. The compiler folded the shape's rules, so none is read here.
    constexpr std::size_t headerInputs = 3;
    if (inputs.size() < headerInputs || slots.size() != 1) {
      return invalidStreamOperation(
          "ProjectTopology expects a subject stream, a wildcard flag, an "
          "added-edge count, edge pairs and one output register");
    }
    const auto integerAt =
        [&](const std::size_t at) -> std::optional<std::int64_t> {
      const auto value = inputValue(inputs[at]);
      if (const auto *held = std::get_if<std::int64_t>(&value)) return *held;
      return std::nullopt;
    };
    const auto wildcard = integerAt(1);
    const auto added    = integerAt(2);
    const auto pairs    = inputs.size() - headerInputs;
    if (!wildcard || !added || *added < 0 || pairs % 2 != 0 ||
        static_cast<std::uint64_t>(*added) > pairs / 2) {
      return invalidStreamOperation(
          "ProjectTopology edge operands are malformed");
    }
    EdgeSelection selection{.everything = *wildcard != 0};
    for (std::size_t pair = 0; pair < pairs / 2; ++pair) {
      const auto dim  = integerAt(headerInputs + 2 * pair);
      const auto sign = integerAt(headerInputs + 2 * pair + 1);
      if (!dim || !sign || *dim <= 0 ||
          !core_.arena().contains(static_cast<CellRef>(*dim))) {
        return invalidStreamOperation(
            "ProjectTopology edge operand does not name a dimension");
      }
      const DirectedDim edge{static_cast<DimRef>(*dim), fromSign(*sign)};
      (static_cast<std::int64_t>(pair) < *added ? selection.add
                                                : selection.remove)
          .push_back(edge);
    }

    std::vector<CellRef> subjects;
    const CellRef in = inputs.front();
    if (iteratorSlots_.contains(in)) {
      return invalidStreamOperation(
          "an iterator register cannot be projected as a stream");
    }
    if (const auto found = streams_.find(in); found != streams_.end()) {
      if (found->second)
        subjects.insert(subjects.end(), found->second->begin(),
                        found->second->end());
    } else if (streamSlots_.contains(in)) {
      // empty stream
    } else if (const auto cell = resultCell(in)) {
      subjects.push_back(*cell);
    }

    auto copied =
        copyTopology(core_.arena(), subjects, selection, core_.dims().clone,
                     core_.dims().runtimeObjectClaim);
    if (!copied) {
      return invalidStreamOperation("ProjectTopology refused: " +
                                    toString(copied.error()));
    }
    const CellRef target = slots.front();
    streamSlots_.insert(target);
    iteratorSlots_.erase(target);
    cellReferences_.erase(target);
    iterators_.erase(target);
    streams_[target] =
        std::make_shared<std::vector<CellRef>>(std::move(*copied));
    break;
  }
  default:
    return invalidStreamOperation("invalid stream opcode dispatch");
  }

  outputs.clear();
  return {};
}

} // namespace zigzag::vortex
