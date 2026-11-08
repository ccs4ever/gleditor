/**
 * @file vortex_bindings.cpp
 * @brief Snapshot, restore and native descriptor support for VM bindings.
 */
#include "common/xanadu/vortex/vortex_vm.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/zigzag/arena_savepoint.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

constexpr std::string_view descriptorTag = zigzag::vortexBindingsDescriptorTag;

BindingsDescriptorError error(std::string message) {
  return {.message = std::move(message)};
}

bool exhausted(const SequenceIteratorState &state, ArenaManifold &arena,
               const std::size_t limit, std::size_t &work) {
  if (state.reverse) {
    for (std::size_t cursor = state.chunkIndex; cursor > 0; --cursor) {
      if (work >= limit) return false;
      ++work;
      arena.consumeWork();
      const auto &values  = state.chunks[cursor - 1];
      const auto consumed = cursor == state.chunkIndex ? state.index : 0;
      if (values && consumed < values->size()) return false;
    }
    return true;
  }
  for (std::size_t chunk = state.chunkIndex; chunk < state.chunks.size();
       ++chunk) {
    if (work >= limit) return false;
    ++work;
    arena.consumeWork();
    const auto &values = state.chunks[chunk];
    if (!values) continue;
    const auto begin = chunk == state.chunkIndex ? state.index : 0;
    if (begin < values->size()) return false;
  }
  return true;
}

void validateSnapshot(const ArenaManifold &arena,
                      const RuntimeBindingSnapshot &snapshot,
                      const LogicalBindingsLimits &limits,
                      std::size_t *remaining = nullptr) {
  const auto slots = snapshot.cellReferences.size() + snapshot.streams.size() +
                     snapshot.iterators.size();
  if (slots > limits.maxSlots)
    throw std::length_error("binding snapshot exceeds the slot limit");
  if (slots > limits.maxReferences)
    throw std::length_error("binding snapshot exceeds the reference limit");
  std::size_t references = slots;
  std::size_t work       = 0;
  const auto inspect     = [&] {
    if (remaining) {
      if (*remaining == 0)
        throw std::length_error("binding descriptor inspection limit exceeded");
      --*remaining;
    } else {
      if (work >= limits.maxInspectionWork)
        throw std::length_error("binding validation inspection limit exceeded");
      ++work;
    }
    arena.consumeWork();
  };
  std::unordered_set<CellRef> typedSlots;
  typedSlots.reserve(slots);
  const auto addReference = [&] {
    if (references >= limits.maxReferences)
      throw std::length_error("binding snapshot exceeds the reference limit");
    ++references;
  };
  for (const auto &[slot, target] : snapshot.cellReferences) {
    inspect();
    if (!arena.contains(slot) || !typedSlots.insert(slot).second ||
        (target && !arena.contains(*target)))
      throw std::invalid_argument("invalid or overlapping cell binding");
    if (target) addReference();
  }
  for (const auto &[slot, members] : snapshot.streams) {
    inspect();
    if (!arena.contains(slot) || !typedSlots.insert(slot).second)
      throw std::invalid_argument("invalid or overlapping stream binding");
    if (members.size() >
        limits.maxReferences - std::min(references, limits.maxReferences))
      throw std::length_error("binding snapshot exceeds the reference limit");
    for (const auto member : members) {
      inspect();
      if (!arena.contains(member))
        throw std::invalid_argument("invalid stream member reference");
    }
    references += members.size();
  }
  for (const auto &[slot, state] : snapshot.iterators) {
    inspect();
    if (!arena.contains(slot) || !typedSlots.insert(slot).second)
      throw std::invalid_argument("invalid or overlapping iterator binding");
    if (state != ClosedIteratorState::Unavailable &&
        state != ClosedIteratorState::Exhausted)
      throw std::invalid_argument("unknown closed iterator state");
  }
}

void inspect(const ArenaManifold &arena, std::size_t *remaining,
             const std::size_t count = 1) {
  if (remaining) {
    if (count > *remaining)
      throw std::length_error("binding descriptor inspection limit exceeded");
    *remaining -= count;
  }
  for (std::size_t i = 0; i < count; ++i) arena.consumeWork();
}

std::optional<CellRef> uniqueLink(const ArenaManifold &arena, CellRef cell,
                                  DimRef dimension, std::size_t limit,
                                  bool &valid, std::size_t *remaining = nullptr,
                                  DimVector direction = DimVector::POS) {
  const auto links = arena.dimensionsOf(cell);
  if (links.size() > limit) {
    valid = false;
    return std::nullopt;
  }
  std::optional<CellRef> result;
  for (const auto &link : links) {
    inspect(arena, remaining);
    if (link.dim != dimension || !link.neighbor(direction)) continue;
    if (result) {
      valid = false;
      return std::nullopt;
    }
    result = *link.neighbor(direction);
  }
  return result;
}

std::optional<std::string> boundedText(const ArenaManifold &arena, CellRef cell,
                                       std::size_t limit,
                                       std::size_t *remaining = nullptr) {
  const auto spans = arena.contentOf(cell);
  if (spans.size() > limit) return std::nullopt;
  std::size_t bytes = 0;
  for (const auto &span : spans) {
    inspect(arena, remaining);
    if (span.length > limit - std::min(bytes, limit)) return std::nullopt;
    bytes += static_cast<std::size_t>(span.length);
  }
  inspect(arena, remaining, bytes);
  auto value = arena.textOf(cell);
  if (value.size() != bytes) return std::nullopt;
  return value;
}

} // namespace

VortexVM *VortexVM::setBindingsLimits(const LogicalBindingsLimits limits) {
  bindingsLimits_ = limits;
  return this;
}

ExecutionResult VortexVM::executeRestoreBindings(const CellRef opcode,
                                                 const CellRef cursor) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  if (!activeCursor_ || *activeCursor_ != cursor || !choiceStack_.empty() ||
      hasActiveFrames() || hasActiveGuards() || !pendingGuardChecks_.empty() ||
      !resumeValues_.empty() || !resumeTokenIds_.empty())
    return fail("RestoreBindings requires a cursor without live control state");
  for (const auto &[owner, scopes] : guardScopes_) {
    (void)owner;
    if (scopes && !scopes->empty())
      return fail("RestoreBindings cannot replace live guard scopes");
  }
  for (const auto &[owner, groups] : mutationGroups_) {
    (void)owner;
    if (groups && !groups->empty())
      return fail("RestoreBindings cannot replace live mutation groups");
  }
  for (const auto &[owner, pc] : cursorPC_)
    if (owner != cursor && pc && getOpcodeKind(*pc) != OpcodeKind::Halt)
      return fail(
          "RestoreBindings cannot replace another live cursor's bindings");

  auto &arena          = core_.arena();
  const auto limits    = bindingsLimits_;
  const auto relations = arena.dimensionsOf(opcode);
  if (relations.size() > limits.maxMetadataRelations)
    return fail("RestoreBindings operand relations exceed limit");
  for (const auto &relation : relations) {
    arena.consumeWork();
    if (relation.dim == core_.dims().contract &&
        (relation.neighbor(DimVector::POS) ||
         relation.neighbor(DimVector::NEG)))
      return fail("RestoreBindings cannot have contracts");
    if (relation.dim == core_.dims().grab && relation.neighbor(DimVector::NEG))
      return fail("RestoreBindings cannot have outputs");
  }
  if (isMemoized(opcode)) return fail("RestoreBindings cannot be memoized");
  bool valid         = true;
  const auto operand = uniqueLink(arena, opcode, core_.dims().grab,
                                  limits.maxMetadataRelations, valid);
  if (!valid || !operand || !arena.contains(*operand) ||
      arena.valueKindOf(*operand) != xanadu::ValueKind::Int64)
    return fail("RestoreBindings requires one literal descriptor operand");
  const auto extra = uniqueLink(arena, *operand, core_.dims().step,
                                limits.maxMetadataRelations, valid);
  const auto raw   = arena.asInt64(*operand);
  if (!valid || extra || !raw || *raw < 0 ||
      static_cast<std::uint64_t>(*raw) > std::numeric_limits<CellRef>::max())
    return fail("RestoreBindings requires exactly one descriptor operand");
  const auto root       = static_cast<CellRef>(*raw);
  const auto descriptor = bindingsDescriptor(root);
  if (!descriptor) return fail(descriptor.error().message);
  // A descriptor is code metadata. Its literal identity must never be resolved
  // through the register maps that it is about to replace.
  for (const auto &relation : arena.dimensionsOf(root)) {
    arena.consumeWork();
    if (relation.dim == core_.dims().spin &&
        !relation.neighbor(DimVector::NEG) && relation.neighbor(DimVector::POS))
      return fail("RestoreBindings cannot have a descriptor pipeline");
  }
  const auto next = uniqueLink(arena, opcode, core_.dims().spin,
                               limits.maxMetadataRelations, valid);
  if (!valid) return fail("RestoreBindings has invalid continuation metadata");
  if (!cursorPC_.contains(cursor))
    return fail("RestoreBindings requires a managed VM cursor");
  const auto previous          = cursorPC_.at(cursor);
  auto oldCellReferences       = cellReferences_;
  auto oldStreams              = streams_;
  auto oldIterators            = iterators_;
  auto oldStreamSlots          = streamSlots_;
  auto oldIteratorSlots        = iteratorSlots_;
  const auto oldStreamCursor   = streamCursor_;
  const auto oldRestoreCount   = bindingRestores_;
  const auto oldSlotCount      = bindingSlotsRestored_;
  const auto oldReferenceCount = bindingReferencesRestored_;
  ArenaSavepoint savepoint(arena);
  try {
    requireStreamCursor(cursor);
    restoreBindings(descriptor->snapshot);
    arena.consumeWork();
    expectWritten(arena.setValueBits(
        cursor, next ? xanadu::ValueKind::Int64 : xanadu::ValueKind::None,
        next ? static_cast<std::uint64_t>(*next) : 0));
    cursorPC_.at(cursor) = next;
    savepoint.commit();
  } catch (...) {
    cellReferences_.swap(oldCellReferences);
    streams_.swap(oldStreams);
    iterators_.swap(oldIterators);
    streamSlots_.swap(oldStreamSlots);
    iteratorSlots_.swap(oldIteratorSlots);
    streamCursor_              = oldStreamCursor;
    bindingRestores_           = oldRestoreCount;
    bindingSlotsRestored_      = oldSlotCount;
    bindingReferencesRestored_ = oldReferenceCount;
    cursorPC_.at(cursor)       = previous;
    throw;
  }
  return {.success = true};
}

RuntimeBindingSnapshot VortexVM::snapshotBindings(
    const std::optional<CellRef> allowedPendingEntry) const {
  const auto fail = [](std::string message) -> void {
    throw std::runtime_error(std::move(message));
  };
  if (!choiceStack_.empty() || hasActiveFrames() || hasActiveGuards() ||
      !pendingGuardChecks_.empty() || !resumeValues_.empty() ||
      !resumeTokenIds_.empty())
    fail("cannot snapshot bindings with live VM control state");
  if (activeCursor_) fail("cannot snapshot bindings during VM execution");
  for (const auto &[cursor, pc] : cursorPC_) {
    if (!pc) continue;
    const auto kind = getOpcodeKind(*pc);
    if (kind == OpcodeKind::Halt) continue;
    if (!allowedPendingEntry || *pc != *allowedPendingEntry)
      fail("cannot snapshot bindings with a live cursor");
  }
  for (const auto &[cursor, scopes] : guardScopes_) {
    (void)cursor;
    if (scopes && !scopes->empty())
      fail("cannot snapshot bindings with a live guard scope");
  }
  for (const auto &[cursor, groups] : mutationGroups_) {
    (void)cursor;
    if (groups && !groups->empty())
      fail("cannot snapshot bindings with a live mutation group");
  }
  if (streamCursor_) {
    const auto pc = getCursorOpcode(*streamCursor_);
    if (pc && getOpcodeKind(*pc) != OpcodeKind::Halt)
      fail("cannot snapshot bindings while a stream cursor is active");
  }

  RuntimeBindingSnapshot snapshot;
  const auto limits = bindingsLimits_;
  if (cellReferences_.size() > limits.maxSlots ||
      streamSlots_.size() >
          limits.maxSlots - std::min(cellReferences_.size(), limits.maxSlots) ||
      iteratorSlots_.size() >
          limits.maxSlots -
              std::min(cellReferences_.size() + streamSlots_.size(),
                       limits.maxSlots))
    fail("binding snapshot exceeds the slot limit");
  std::size_t plannedReferences =
      cellReferences_.size() + streamSlots_.size() + iteratorSlots_.size();
  for (const auto &[slot, target] : cellReferences_) {
    (void)slot;
    if (target) ++plannedReferences;
  }
  for (const auto slot : streamSlots_) {
    const auto found = streams_.find(slot);
    if (found != streams_.end() && found->second) {
      if (found->second->size() >
          limits.maxReferences -
              std::min(plannedReferences, limits.maxReferences))
        fail("binding snapshot exceeds the reference limit");
      plannedReferences += found->second->size();
    }
  }
  if (plannedReferences > limits.maxReferences)
    fail("binding snapshot exceeds the reference limit");
  std::size_t references = 0;
  std::size_t work       = 0;
  const auto addWork     = [&] {
    if (work >= limits.maxInspectionWork)
      fail("binding snapshot exceeds the inspection-work limit");
    ++work;
    core_.arena().consumeWork();
  };
  const auto addRef = [&] {
    if (references >= limits.maxReferences)
      fail("binding snapshot exceeds the reference limit");
    ++references;
    addWork();
  };
  snapshot.cellReferences.reserve(cellReferences_.size());
  for (const auto &[slot, target] : cellReferences_) {
    core_.arena().consumeWork();
    if (!core_.arena().contains(slot) ||
        (target && !core_.arena().contains(*target)))
      fail("binding snapshot contains an invalid or unavailable cell binding");
    if (streamSlots_.contains(slot) || iteratorSlots_.contains(slot))
      fail("binding slot has overlapping runtime types");
    addRef();
    if (target) addRef();
    snapshot.cellReferences.emplace(slot, target);
  }
  snapshot.streams.reserve(streamSlots_.size());
  for (const auto slot : streamSlots_) {
    addWork();
    if (!core_.arena().contains(slot) || iteratorSlots_.contains(slot) ||
        cellReferences_.contains(slot))
      fail("stream slot has an invalid or overlapping runtime type");
    const auto found = streams_.find(slot);
    const std::span<const CellRef> items =
        found == streams_.end() || !found->second
            ? std::span<const CellRef>{}
            : std::span<const CellRef>{*found->second};
    if (items.size() >
        limits.maxReferences - std::min(references + 1, limits.maxReferences))
      fail("binding snapshot exceeds the reference limit");
    addRef();
    for (const auto member : items) {
      addWork();
      if (!core_.arena().contains(member))
        fail("binding stream contains an invalid member reference");
      addRef();
    }
    auto &savedItems = snapshot.streams[slot];
    savedItems.reserve(items.size());
    savedItems.insert(savedItems.end(), items.begin(), items.end());
  }
  snapshot.iterators.reserve(iteratorSlots_.size());
  for (const auto slot : iteratorSlots_) {
    addWork();
    if (!core_.arena().contains(slot) || streamSlots_.contains(slot) ||
        cellReferences_.contains(slot))
      fail("iterator slot has an invalid or overlapping runtime type");
    const auto found          = iterators_.find(slot);
    ClosedIteratorState state = ClosedIteratorState::Unavailable;
    if (found != iterators_.end() && found->second) {
      if (const auto *rank =
              std::get_if<RankIteratorState>(found->second.get())) {
        if (!rank->done) fail("cannot snapshot a live rank iterator");
        state = ClosedIteratorState::Exhausted;
      } else if (exhausted(std::get<SequenceIteratorState>(*found->second),
                           core_.arena(), limits.maxInspectionWork, work)) {
        state = ClosedIteratorState::Exhausted;
      } else {
        fail("cannot snapshot a live sequence iterator");
      }
    }
    addRef();
    snapshot.iterators.emplace(slot, state);
  }
  return snapshot;
}

std::expected<CellRef, BindingsDescriptorError>
VortexVM::makeBindingsDescriptor(const RuntimeBindingSnapshot &snapshot) {
  const auto fail = [](std::string message)
      -> std::expected<CellRef, BindingsDescriptorError> {
    return std::unexpected(error(std::move(message)));
  };
  const auto limits    = bindingsLimits_;
  const auto slotCount = snapshot.cellReferences.size() +
                         snapshot.streams.size() + snapshot.iterators.size();
  if (slotCount > limits.maxSlots)
    return fail("binding descriptor exceeds the slot limit");
  if (!core_.dims().runtimeObjectClaim)
    return fail("Vortex runtime-object dimension is unavailable");
  try {
    validateSnapshot(core_.arena(), snapshot, limits);
    if (limits.maxDescriptorRecords < 1 ||
        limits.maxMetadataRelations < (slotCount == 0 ? 1u : 2u) ||
        descriptorTag.size() > limits.maxMetadataBytes ||
        zigzag::runtimeObjectProbeText.size() > limits.maxMetadataBytes)
      return fail("binding descriptor metadata limits are too small");
    ArenaSavepoint savepoint(core_.arena());
    auto &arena        = core_.arena();
    const auto probe   = arena.intern(zigzag::runtimeObjectProbeText);
    const CellRef root = arena.makeCell();
    expectWritten(arena.setContent(
        root, std::span<const xanadu::PrimediaSpan>{&probe, 1}));
    expectWritten(arena.setValueBits(root, xanadu::ValueKind::None, 0));
    const CellRef tag = arena.makeCell(descriptorTag);
    expectWritten(arena.setValueBits(tag, xanadu::ValueKind::None, 0));
    expectWritten(arena.link(root, *core_.dims().runtimeObjectClaim,
                             DimVector::POS, tag));
    std::optional<CellRef> tail;
    std::size_t records  = 0;
    const auto addRecord = [&](std::string_view role, std::int64_t value) {
      if (records >= limits.maxDescriptorRecords)
        throw std::length_error("binding descriptor record limit exceeded");
      if (role.size() > limits.maxMetadataBytes)
        throw std::length_error("binding descriptor metadata limit exceeded");
      const CellRef record = arena.makeCell(role);
      expectWritten(arena.setValueBits(record, xanadu::ValueKind::Int64,
                                       static_cast<std::uint64_t>(value)));
      expectWritten(arena.link(tail.value_or(root),
                               tail ? core_.dims().step : core_.dims().grab,
                               DimVector::POS, record));
      tail = record;
      ++records;
    };
    for (const auto &[slot, target] : snapshot.cellReferences) {
      if (!arena.contains(slot) || (target && !arena.contains(*target)))
        throw std::invalid_argument("invalid cell binding reference");
      addRecord("ref.cell.slot", static_cast<std::int64_t>(slot));
      addRecord("ref.cell.target",
                target ? static_cast<std::int64_t>(*target) : -1);
    }
    for (const auto &[slot, members] : snapshot.streams) {
      if (!arena.contains(slot))
        throw std::invalid_argument("invalid stream binding slot");
      addRecord("ref.stream.slot", static_cast<std::int64_t>(slot));
      for (const auto member : members) {
        if (!arena.contains(member))
          throw std::invalid_argument("invalid stream member reference");
        addRecord("ref.stream.member", static_cast<std::int64_t>(member));
      }
    }
    for (const auto &[slot, state] : snapshot.iterators) {
      if (!arena.contains(slot))
        throw std::invalid_argument("invalid iterator binding slot");
      if (state != ClosedIteratorState::Unavailable &&
          state != ClosedIteratorState::Exhausted)
        throw std::invalid_argument("unknown closed iterator state");
      addRecord(state == ClosedIteratorState::Unavailable
                    ? "ref.iterator.unavailable"
                    : "ref.iterator.exhausted",
                static_cast<std::int64_t>(slot));
    }
    savepoint.commit();
    return root;
  } catch (const std::exception &exception) {
    return fail(exception.what());
  }
}

std::expected<ParsedBindingsDescriptor, BindingsDescriptorError>
VortexVM::bindingsDescriptor(const CellRef root) const try {
  const auto fail = [](std::string message)
      -> std::expected<ParsedBindingsDescriptor, BindingsDescriptorError> {
    return std::unexpected(error(std::move(message)));
  };
  const auto &arena = core_.arena();
  const auto limits = bindingsLimits_;
  auto remaining    = limits.maxInspectionWork;
  if (!core_.dims().runtimeObjectClaim || !arena.contains(root) ||
      arena.isProxy(root))
    return fail("invalid Vortex bindings descriptor root");
  const auto rootText =
      boundedText(arena, root, limits.maxMetadataBytes, &remaining);
  if (!rootText || *rootText != zigzag::runtimeObjectProbeText ||
      arena.valueKindOf(root) != xanadu::ValueKind::None)
    return fail("invalid Vortex bindings descriptor claim");
  bool validLink = true;
  const auto tagCell =
      uniqueLink(arena, root, *core_.dims().runtimeObjectClaim,
                 limits.maxMetadataRelations, validLink, &remaining);
  const auto tag = tagCell ? boundedText(arena, *tagCell,
                                         limits.maxMetadataBytes, &remaining)
                           : std::nullopt;
  if (!validLink || !tag || *tag != descriptorTag)
    return fail("invalid Vortex bindings descriptor header or version");

  if (arena.isProxy(*tagCell) ||
      arena.valueKindOf(*tagCell) != xanadu::ValueKind::None)
    return fail("bindings descriptor has an invalid kind cell");
  const auto backlink = uniqueLink(
      arena, *tagCell, *core_.dims().runtimeObjectClaim,
      limits.maxMetadataRelations, validLink, &remaining, DimVector::NEG);
  if (!validLink || backlink != root)
    return fail("bindings descriptor kind cell has an invalid backlink");

  ParsedBindingsDescriptor parsed;
  std::unordered_set<CellRef> visited;
  std::optional<CellRef> pendingCellSlot;
  std::optional<CellRef> pendingStreamSlot;
  std::size_t references = 0;
  auto record            = uniqueLink(arena, root, core_.dims().grab,
                                      limits.maxMetadataRelations, validLink, &remaining);
  if (!validLink) return fail("malformed bindings descriptor header links");
  while (record) {
    inspect(arena, &remaining);
    if (visited.size() >= limits.maxDescriptorRecords ||
        !visited.insert(*record).second)
      return fail("bindings descriptor record limit or cycle exceeded");
    if (!arena.contains(*record) || arena.isProxy(*record) ||
        arena.valueKindOf(*record) != xanadu::ValueKind::Int64)
      return fail("bindings descriptor record has an invalid value type");
    const auto raw = arena.asInt64(*record);
    const auto role =
        boundedText(arena, *record, limits.maxMetadataBytes, &remaining);
    if (!raw || !role || *raw < -1)
      return fail("bindings descriptor record is malformed");
    const auto readTarget = [&]() -> std::optional<CellRef> {
      if (*raw < 0 || static_cast<std::uint64_t>(*raw) >
                          std::numeric_limits<CellRef>::max())
        return std::nullopt;
      const auto target = static_cast<CellRef>(*raw);
      if (!arena.contains(target)) return std::nullopt;
      return target;
    };
    if (*role != "ref.stream.member") pendingStreamSlot.reset();
    if (*role == "ref.cell.slot") {
      if (pendingCellSlot) return fail("misordered binding records");
      const auto slot = readTarget();
      if (!slot) return fail("invalid cell binding slot");
      pendingCellSlot = *slot;
      parsed.referenceCells.emplace_back(*record, *slot);
    } else if (*role == "ref.cell.target") {
      if (!pendingCellSlot) return fail("orphan cell target record");
      std::optional<CellRef> target;
      if (*raw >= 0) {
        target = readTarget();
        if (!target) return fail("invalid cell binding target");
        parsed.referenceCells.emplace_back(*record, *target);
      }
      if (!parsed.snapshot.cellReferences.emplace(*pendingCellSlot, target)
               .second)
        return fail("duplicate cell binding slot");
      pendingCellSlot.reset();
    } else if (*role == "ref.stream.slot") {
      if (pendingCellSlot) return fail("cell binding missing target record");
      const auto slot = readTarget();
      if (!slot || parsed.snapshot.streams.contains(*slot))
        return fail("invalid or duplicate stream binding slot");
      pendingStreamSlot = *slot;
      parsed.snapshot.streams.emplace(*slot, std::vector<CellRef>{});
      parsed.referenceCells.emplace_back(*record, *slot);
    } else if (*role == "ref.stream.member") {
      if (!pendingStreamSlot) return fail("orphan stream member record");
      const auto member = readTarget();
      if (!member) return fail("invalid stream member reference");
      parsed.snapshot.streams.at(*pendingStreamSlot).push_back(*member);
      parsed.referenceCells.emplace_back(*record, *member);
    } else if (*role == "ref.iterator.unavailable" ||
               *role == "ref.iterator.exhausted") {
      if (pendingCellSlot) return fail("cell binding missing target record");
      pendingStreamSlot.reset();
      const auto slot = readTarget();
      if (!slot || parsed.snapshot.iterators.contains(*slot))
        return fail("invalid or duplicate iterator slot");
      parsed.snapshot.iterators.emplace(*slot,
                                        *role == "ref.iterator.unavailable"
                                            ? ClosedIteratorState::Unavailable
                                            : ClosedIteratorState::Exhausted);
      parsed.referenceCells.emplace_back(*record, *slot);
    } else {
      return fail("bindings descriptor contains an unknown record role");
    }
    if (parsed.snapshot.cellReferences.size() + parsed.snapshot.streams.size() +
            parsed.snapshot.iterators.size() >
        limits.maxSlots)
      return fail("bindings descriptor exceeds the slot limit");
    references = parsed.referenceCells.size();
    if (references > limits.maxReferences)
      return fail("bindings descriptor exceeds the reference limit");
    record = uniqueLink(arena, *record, core_.dims().step,
                        limits.maxMetadataRelations, validLink, &remaining);
    if (!validLink) return fail("malformed bindings descriptor record links");
  }
  if (pendingCellSlot) return fail("cell binding missing target record");
  try {
    validateSnapshot(arena, parsed.snapshot, limits, &remaining);
  } catch (const std::exception &exception) {
    return fail(exception.what());
  }
  return parsed;
} catch (const std::exception &exception) {
  return std::unexpected(error(exception.what()));
}

void VortexVM::restoreBindings(const RuntimeBindingSnapshot &snapshot) {
  if (!choiceStack_.empty() || hasActiveFrames() || hasActiveGuards() ||
      !pendingGuardChecks_.empty() || !resumeValues_.empty() ||
      !resumeTokenIds_.empty())
    throw std::runtime_error(
        "binding restore cannot replace live control state");
  for (const auto &[owner, scopes] : guardScopes_) {
    (void)owner;
    if (scopes && !scopes->empty())
      throw std::runtime_error("binding restore cannot replace guard scopes");
  }
  for (const auto &[owner, groups] : mutationGroups_) {
    (void)owner;
    if (groups && !groups->empty())
      throw std::runtime_error(
          "binding restore cannot replace mutation groups");
  }
  for (const auto &[owner, pc] : cursorPC_)
    if (pc && getOpcodeKind(*pc) != OpcodeKind::Halt && owner != activeCursor_)
      throw std::runtime_error("binding restore cannot replace a live cursor");
  if (activeCursor_) {
    const auto pc = getCursorOpcode(*activeCursor_);
    if (!pc || getOpcodeKind(*pc) != OpcodeKind::RestoreBindings)
      throw std::runtime_error(
          "binding restore is unavailable during execution");
  }
  const auto limits = bindingsLimits_;
  validateSnapshot(core_.arena(), snapshot, limits);
  if (bindingRestores_ >= limits.maxRestores)
    throw std::length_error("binding restore count limit exceeded");
  const std::size_t slots = snapshot.cellReferences.size() +
                            snapshot.streams.size() + snapshot.iterators.size();
  if (slots > limits.maxSlots ||
      slots > limits.maxRestoredSlots -
                  std::min(bindingSlotsRestored_, limits.maxRestoredSlots))
    throw std::length_error("binding restore slot limit exceeded");
  std::size_t refs = slots;
  for (const auto &[slot, target] : snapshot.cellReferences) {
    (void)slot;
    if (target) ++refs;
  }
  for (const auto &[slot, members] : snapshot.streams) {
    (void)slot;
    if (members.size() >
        limits.maxReferences - std::min(refs, limits.maxReferences))
      throw std::length_error("binding restore reference limit exceeded");
    refs += members.size();
  }
  if (refs > limits.maxReferences ||
      refs >
          limits.maxRestoredReferences - std::min(bindingReferencesRestored_,
                                                  limits.maxRestoredReferences))
    throw std::length_error("binding restore reference limit exceeded");

  CellReferenceMap cellReferences;
  StreamMap streams;
  IteratorMap iterators;
  std::unordered_set<CellRef> streamSlots;
  std::unordered_set<CellRef> iteratorSlots;
  cellReferences.reserve(snapshot.cellReferences.size());
  streams.reserve(snapshot.streams.size());
  iterators.reserve(snapshot.iterators.size());
  streamSlots.reserve(snapshot.streams.size());
  iteratorSlots.reserve(snapshot.iterators.size());
  const auto checkSlot = [this, &cellReferences, &streamSlots,
                          &iteratorSlots](const CellRef slot) {
    if (!core_.arena().contains(slot) || cellReferences.contains(slot) ||
        streamSlots.contains(slot) || iteratorSlots.contains(slot))
      throw std::invalid_argument("invalid or overlapping binding slot");
  };
  for (const auto &[slot, target] : snapshot.cellReferences) {
    checkSlot(slot);
    if (target && !core_.arena().contains(*target))
      throw std::invalid_argument("invalid binding target cell");
    cellReferences.emplace(slot, target);
    core_.arena().consumeWork();
  }
  for (const auto &[slot, members] : snapshot.streams) {
    checkSlot(slot);
    auto values = std::make_shared<std::vector<CellRef>>();
    values->reserve(members.size());
    for (const auto member : members) {
      if (!core_.arena().contains(member))
        throw std::invalid_argument("invalid stream member cell");
      values->push_back(member);
      core_.arena().consumeWork();
    }
    streams.emplace(slot, std::move(values));
    streamSlots.insert(slot);
    core_.arena().consumeWork();
  }
  for (const auto &[slot, state] : snapshot.iterators) {
    checkSlot(slot);
    if (state != ClosedIteratorState::Unavailable &&
        state != ClosedIteratorState::Exhausted)
      throw std::invalid_argument("unknown closed iterator state");
    iteratorSlots.insert(slot);
    if (state == ClosedIteratorState::Exhausted) {
      auto state =
          std::make_shared<IteratorState>(RankIteratorState{.done = true});
      iterators.emplace(slot, std::move(state));
    }
    core_.arena().consumeWork();
  }
  cellReferences_.swap(cellReferences);
  streams_.swap(streams);
  iterators_.swap(iterators);
  streamSlots_.swap(streamSlots);
  iteratorSlots_.swap(iteratorSlots);
  if (!activeCursor_) streamCursor_.reset();
  ++bindingRestores_;
  bindingSlotsRestored_ += slots;
  bindingReferencesRestored_ += refs;
}

} // namespace zigzag::vortex
