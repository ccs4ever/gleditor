/**
 * @file vortex_frames.cpp
 * @brief Versioned frame descriptors and cursor-owned activation storage.
 */
#include "common/xanadu/vortex/vortex_vm.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#include "common/xanadu/zigzag/arena_savepoint.hpp"

namespace zigzag::vortex {
namespace {

constexpr std::string_view descriptorTag         = "VORTEX_FRAME/1";
constexpr std::string_view reservedDescriptorTag = "VORTEX_FRAME/RESERVED/1";

FrameDescriptorError descriptorError(std::string message) {
  return {.message = std::move(message)};
}

bool validPortKind(const FramePortKind kind) {
  return kind == FramePortKind::Cell || kind == FramePortKind::Stream;
}

std::expected<void, FrameDescriptorError>
validateFrameSpec(const VortexVM &vm, const FrameDescriptorSpec &spec) {
  const auto &arena = vm.core().arena();
  const auto fail =
      [](std::string message) -> std::expected<void, FrameDescriptorError> {
    return std::unexpected(descriptorError(std::move(message)));
  };
  if (!spec.entry || !arena.contains(*spec.entry) ||
      !vm.getOpcodeKind(*spec.entry))
    return fail("frame descriptor entry is not a valid opcode");
  const auto limits = vm.frameLimits();
  if (spec.locals.size() > limits.maxLocalsPerFrame)
    return fail("frame descriptor exceeds the local-slot limit");
  if (limits.maxDescriptorRecords == 0 ||
      spec.locals.size() > limits.maxDescriptorRecords - 1 ||
      spec.argumentPorts.size() >
          limits.maxDescriptorRecords - 1 - spec.locals.size() ||
      spec.resultPorts.size() > limits.maxDescriptorRecords - 1 -
                                    spec.locals.size() -
                                    spec.argumentPorts.size())
    return fail("frame descriptor exceeds the descriptor-record limit");

  std::unordered_set<CellRef> locals;
  locals.reserve(spec.locals.size());
  for (const auto local : spec.locals) {
    if (local == noCell || !arena.contains(local) ||
        !locals.insert(local).second || vm.getOpcodeKind(local))
      return fail("frame descriptor locals must be unique non-opcode cells");
  }
  const auto validPorts = [&locals](const std::vector<FramePort> &ports) {
    std::unordered_set<CellRef> seen;
    for (const auto &port : ports)
      if (!validPortKind(port.kind) || !locals.contains(port.slot) ||
          !seen.insert(port.slot).second)
        return false;
    return true;
  };
  if (!validPorts(spec.argumentPorts))
    return fail("frame argument ports must uniquely name local slots");
  if (!validPorts(spec.resultPorts))
    return fail("frame result ports must uniquely name local slots");
  return {};
}

void appendDescriptorRecords(VortexCore &core, const CellRef descriptor,
                             const FrameDescriptorSpec &spec) {
  auto &arena          = core.arena();
  CellRef tail         = descriptor;
  bool first           = true;
  const auto addRecord = [&](const std::string_view role, const CellRef target,
                             CellRef &recordTail, bool &isFirst) {
    const CellRef record = arena.makeCell(role);
    zigzag::expectWritten(arena.setValueBits(
        record, xanadu::ValueKind::Int64, static_cast<std::uint64_t>(target)));
    zigzag::expectWritten(
        arena.link(recordTail, isFirst ? core.dims().grab : core.dims().step,
                   DimVector::POS, record));
    isFirst    = false;
    recordTail = record;
  };
  addRecord("ref.entry", *spec.entry, tail, first);
  for (const auto local : spec.locals)
    addRecord("ref.local", local, tail, first);
  for (const auto &port : spec.argumentPorts)
    addRecord(port.kind == FramePortKind::Cell ? "ref.argument-cell"
                                               : "ref.argument-stream",
              port.slot, tail, first);
  for (const auto &port : spec.resultPorts)
    addRecord(port.kind == FramePortKind::Cell ? "ref.result-cell"
                                               : "ref.result-stream",
              port.slot, tail, first);
}

std::optional<std::string> boundedCellText(const zigzag::ArenaManifold &arena,
                                           const CellRef cell,
                                           const std::size_t limit) {
  std::size_t bytes = 0;
  const auto spans  = arena.contentOf(cell);
  if (spans.size() > limit) return std::nullopt;
  for (const auto &span : spans) {
    arena.consumeWork();
    if (span.length > limit - bytes) return std::nullopt;
    bytes += static_cast<std::size_t>(span.length);
    for (std::uint64_t index = 0; index < span.length; ++index)
      arena.consumeWork();
  }
  return arena.textOf(cell);
}

} // namespace

VortexVM *VortexVM::setFrameLimits(const FrameLimits limits) {
  frameLimits_ = limits;
  return this;
}

std::expected<CellRef, FrameDescriptorError>
VortexVM::makeFrameDescriptor(const FrameDescriptorSpec &spec) {
  auto &arena = core_.arena();
  const auto fail =
      [](std::string message) -> std::expected<CellRef, FrameDescriptorError> {
    return std::unexpected(descriptorError(std::move(message)));
  };
  if (const auto valid = validateFrameSpec(*this, spec); !valid)
    return fail(valid.error().message);

  try {
    zigzag::ArenaSavepoint savepoint(core_.arena());
    const CellRef descriptor = arena.makeCell(reservedDescriptorTag);
    appendDescriptorRecords(core_, descriptor, spec);
    const auto header = arena.intern(descriptorTag);
    zigzag::expectWritten(arena.setContent(
        descriptor, std::span<const xanadu::PrimediaSpan>{&header, 1}));
    savepoint.commit();
    return descriptor;
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

std::expected<CellRef, FrameDescriptorError>
VortexVM::reserveFrameDescriptor() {
  try {
    zigzag::ArenaSavepoint savepoint(core_.arena());
    const CellRef descriptor = core_.arena().makeCell(reservedDescriptorTag);
    savepoint.commit();
    return descriptor;
  } catch (const std::exception &error) {
    return std::unexpected(descriptorError(error.what()));
  }
}

std::expected<VortexVM *, FrameDescriptorError>
VortexVM::finalizeFrameDescriptor(const CellRef reserved,
                                  const FrameDescriptorSpec &spec) {
  auto &arena     = core_.arena();
  const auto fail = [](std::string message)
      -> std::expected<VortexVM *, FrameDescriptorError> {
    return std::unexpected(descriptorError(std::move(message)));
  };
  if (!arena.contains(reserved))
    return fail("frame descriptor root is not reserved or was finalized");
  const auto header =
      boundedCellText(arena, reserved, reservedDescriptorTag.size());
  if (!header || *header != reservedDescriptorTag ||
      arena.linked(reserved, core_.dims().grab, DimVector::POS))
    return fail("frame descriptor root is not reserved or was finalized");
  if (const auto valid = validateFrameSpec(*this, spec); !valid)
    return fail(valid.error().message);
  if (std::ranges::find(spec.locals, reserved) != spec.locals.end())
    return fail("frame descriptor cannot list its own root as a local");
  try {
    zigzag::ArenaSavepoint savepoint(arena);
    appendDescriptorRecords(core_, reserved, spec);
    const auto header = arena.intern(descriptorTag);
    zigzag::expectWritten(arena.setContent(
        reserved, std::span<const xanadu::PrimediaSpan>{&header, 1}));
    savepoint.commit();
    return this;
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

std::expected<FrameDescriptor, FrameDescriptorError>
VortexVM::frameDescriptor(const CellRef descriptor) const {
  const auto &arena = core_.arena();
  const auto fail   = [](std::string message)
      -> std::expected<FrameDescriptor, FrameDescriptorError> {
    return std::unexpected(descriptorError(std::move(message)));
  };
  if (!arena.contains(descriptor))
    return fail("invalid Vortex frame descriptor header or version");
  const auto header = boundedCellText(arena, descriptor, descriptorTag.size());
  if (!header || *header != descriptorTag)
    return fail("invalid Vortex frame descriptor header or version");

  FrameDescriptor result;
  std::unordered_set<CellRef> records;
  std::unordered_set<CellRef> locals;
  std::unordered_set<CellRef> argumentSlots;
  std::unordered_set<CellRef> resultSlots;
  bool foundEntry = false;
  auto record     = arena.linked(descriptor, core_.dims().grab, DimVector::POS);
  while (record) {
    arena.consumeWork();
    if (records.size() >= frameLimits_.maxDescriptorRecords)
      return fail("frame descriptor exceeds the descriptor-record limit");
    if (!records.insert(*record).second)
      return fail("frame descriptor contains a record cycle");
    if (!arena.contains(*record))
      return fail("frame descriptor refers to a missing record");
    const auto targetValue = arena.asInt64(*record);
    if (!targetValue || *targetValue < 0 ||
        static_cast<std::uint64_t>(*targetValue) >
            std::numeric_limits<CellRef>::max() ||
        !arena.contains(static_cast<CellRef>(*targetValue)) ||
        arena.valueKindOf(*record) != xanadu::ValueKind::Int64)
      return fail("frame descriptor record has no valid target");
    const CellRef target = static_cast<CellRef>(*targetValue);
    const auto label     = boundedCellText(
        arena, *record, std::string_view("ref.argument-stream").size());
    if (!label)
      return fail("frame descriptor record label exceeds its byte limit");
    const auto &role = *label;
    if (role == "ref.entry") {
      if (foundEntry || !getOpcodeKind(target))
        return fail("frame descriptor entry record is invalid");
      result.entry = target;
      foundEntry   = true;
    } else if (role == "ref.local") {
      if (!locals.insert(target).second || getOpcodeKind(target))
        return fail("frame descriptor local record is invalid or duplicated");
      result.locals.push_back(target);
      result.referenceCells.emplace_back(*record, target);
    } else if (role == "ref.argument-cell" || role == "ref.argument-stream") {
      if (!argumentSlots.insert(target).second)
        return fail("frame descriptor argument slot is duplicated");
      result.argumentPorts.emplace_back(target, role == "ref.argument-cell"
                                                    ? FramePortKind::Cell
                                                    : FramePortKind::Stream);
      result.referenceCells.emplace_back(*record, target);
    } else if (role == "ref.result-cell" || role == "ref.result-stream") {
      if (!resultSlots.insert(target).second)
        return fail("frame descriptor result slot is duplicated");
      result.resultPorts.emplace_back(target, role == "ref.result-cell"
                                                  ? FramePortKind::Cell
                                                  : FramePortKind::Stream);
      result.referenceCells.emplace_back(*record, target);
    } else {
      return fail("frame descriptor contains an unknown record role");
    }
    if (role == "ref.entry")
      result.referenceCells.emplace_back(*record, target);
    record = arena.linked(*record, core_.dims().step, DimVector::POS);
  }
  if (!foundEntry) return fail("frame descriptor has no entry record");
  if (result.locals.size() > frameLimits_.maxLocalsPerFrame)
    return fail("frame descriptor exceeds the local-slot limit");
  const auto portLocals = [&locals](const std::vector<FramePort> &ports) {
    return std::ranges::all_of(ports, [&locals](const FramePort &port) {
      return locals.contains(port.slot);
    });
  };
  if (!portLocals(result.argumentPorts) || !portLocals(result.resultPorts))
    return fail("frame descriptor port does not refer to a local slot");
  return result;
}

std::size_t VortexVM::activationDepth(const CellRef cursor) const noexcept {
  const auto found = activationFrames_.find(cursor);
  return found == activationFrames_.end() ? 0 : found->second.size();
}

bool VortexVM::hasActiveFrames() const noexcept {
  return std::ranges::any_of(activationFrames_, [](const auto &entry) {
    return !entry.second.empty();
  });
}

std::optional<CellRef> VortexVM::resultCell(const CellRef cursor,
                                            const CellRef slot) const {
  const auto found = activationFrames_.find(cursor);
  if (found != activationFrames_.end()) {
    for (auto frame = found->second.rbegin(); frame != found->second.rend();
         ++frame) {
      const auto backing = (*frame)->localBacking.find(slot);
      if (backing != (*frame)->localBacking.end())
        return resultCell(backing->second);
    }
  }
  return resultCell(slot);
}

std::vector<CellRef> VortexVM::resultCells(const CellRef cursor,
                                           const CellRef slot) const {
  const auto found = activationFrames_.find(cursor);
  if (found != activationFrames_.end()) {
    for (auto frame = found->second.rbegin(); frame != found->second.rend();
         ++frame) {
      const auto backing = (*frame)->localBacking.find(slot);
      if (backing != (*frame)->localBacking.end())
        return resultCells(backing->second);
    }
  }
  return resultCells(slot);
}

CellRef VortexVM::runtimeSlot(const CellRef slot) const {
  if (!activeCursor_) return slot;
  const auto frames = activationFrames_.find(*activeCursor_);
  if (frames == activationFrames_.end() || frames->second.empty()) return slot;
  const auto found = frames->second.back()->localBacking.find(slot);
  return found == frames->second.back()->localBacking.end() ? slot
                                                            : found->second;
}

std::vector<CellRef> VortexVM::inputsOf(const CellRef opcode) const {
  auto slots = core_.inputsOf(opcode);
  for (auto &slot : slots) slot = runtimeSlot(slot);
  return slots;
}

std::vector<CellRef> VortexVM::outputsOf(const CellRef opcode) const {
  auto slots = core_.outputsOf(opcode);
  for (auto &slot : slots) slot = runtimeSlot(slot);
  return slots;
}

ExecutionResult VortexVM::invokeFramePrepared(
    const CellRef descriptorCell,
    const std::span<const std::vector<CellRef>> arguments,
    const std::span<const CellRef> resultSlots, const CellRef cursor,
    const std::optional<CellRef> continuation,
    const std::optional<CellRef> failureOp) {
  const auto fail = [failureOp](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = failureOp,
                           .errorMessage  = std::move(message)};
  };
  auto &arena       = core_.arena();
  const auto parsed = frameDescriptor(descriptorCell);
  if (!parsed) return fail(parsed.error().message);
  const auto &descriptor = *parsed;
  const auto framesIt    = activationFrames_.find(cursor);
  const std::size_t depth =
      framesIt == activationFrames_.end() ? 0 : framesIt->second.size();
  if (depth >= frameLimits_.maxDepth)
    return fail("activation frame depth limit exceeded");
  if (frameBackingCellsAllocated_ > frameLimits_.maxBackingCells ||
      descriptor.locals.size() >
          frameLimits_.maxBackingCells - frameBackingCellsAllocated_)
    return fail("activation frame backing-cell limit exceeded");
  if (arguments.size() != descriptor.argumentPorts.size() ||
      resultSlots.size() != descriptor.resultPorts.size())
    return fail("InvokeFrame port arity does not match its descriptor");
  std::unordered_set<CellRef> uniqueDestinations;
  for (const auto slot : resultSlots)
    if (!arena.contains(slot) || getOpcodeKind(slot) ||
        !uniqueDestinations.insert(slot).second || slot == descriptorCell)
      return fail("InvokeFrame result destinations must be caller slots");
  for (const auto local : descriptor.locals)
    if (core_.hasPipeline(local))
      return fail("frame descriptor locals cannot have pipelines");
  if (depth > 0 && !continuation)
    return fail("nested InvokeFrame requires a caller continuation");
  if (nextActivationId_ == std::numeric_limits<std::uint64_t>::max())
    return fail("activation identity limit exceeded");
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (descriptor.argumentPorts[index].kind == FramePortKind::Cell &&
        arguments[index].size() > 1)
      return fail("InvokeFrame cell argument resolved to multiple cells");
  }
  const auto guards = guardStacks_.find(cursor);
  const std::size_t guardBaseline =
      guards == guardStacks_.end() || !guards->second ? 0
                                                      : guards->second->size();
  if (!continuation && guardBaseline != 0)
    return fail("InvokeFrame requires a continuation for active caller guards");
  if (!continuation) {
    const auto scopes = guardScopes_.find(cursor);
    if ((scopes != guardScopes_.end() && scopes->second &&
         !scopes->second->empty()) ||
        mutationGroupActive(cursor))
      return fail(
          "InvokeFrame requires a continuation for caller scopes/groups");
  }
  ActivationFrame frame(descriptorCell, nextActivationId_, guardBaseline);
  frame.returnOpcode = continuation;
  frame.parentResultSlots.assign(resultSlots.begin(), resultSlots.end());
  frame.resultPorts               = descriptor.resultPorts;
  bool committed                  = false;
  auto previousReferences         = cellReferences_;
  auto previousStreams            = streams_;
  auto previousIterators          = iterators_;
  auto previousStreamSlots        = streamSlots_;
  auto previousIteratorSlots      = iteratorSlots_;
  auto previousFrames             = activationFrames_;
  auto previousCursorPC           = cursorPC_;
  const auto previousStreamCursor = streamCursor_;
  if (std::ranges::any_of(descriptor.argumentPorts,
                          [](const FramePort &port) {
                            return port.kind == FramePortKind::Stream;
                          }) ||
      std::ranges::any_of(descriptor.resultPorts, [](const FramePort &port) {
        return port.kind == FramePortKind::Stream;
      })) {
    try {
      requireStreamCursor(cursor);
    } catch (const std::exception &error) {
      return fail(error.what());
    }
  }
  zigzag::Mark mark;
  try {
    mark = arena.mark();
  } catch (const std::exception &error) {
    cellReferences_.swap(previousReferences);
    streams_.swap(previousStreams);
    iterators_.swap(previousIterators);
    streamSlots_.swap(previousStreamSlots);
    iteratorSlots_.swap(previousIteratorSlots);
    activationFrames_.swap(previousFrames);
    cursorPC_.swap(previousCursorPC);
    streamCursor_ = previousStreamCursor;
    return fail(error.what());
  }
  try {
    frame.localBacking.reserve(descriptor.locals.size());
    for (const auto logical : descriptor.locals) {
      arena.consumeWork();
      const CellRef backing = arena.makeCell();
      frame.localBacking.emplace(logical, backing);
      cellReferences_[backing] = std::nullopt;
    }
    for (std::size_t index = 0; index < descriptor.argumentPorts.size();
         ++index) {
      const CellRef target =
          frame.localBacking.at(descriptor.argumentPorts[index].slot);
      auto values = std::make_shared<std::vector<CellRef>>();
      values->reserve(arguments[index].size());
      for (const CellRef cell : arguments[index]) {
        arena.consumeWork();
        if (!arena.contains(cell))
          throw std::runtime_error(
              "InvokeFrame argument contains a missing cell");
        values->push_back(cell);
      }
      if (descriptor.argumentPorts[index].kind == FramePortKind::Stream) {
        streams_[target] = std::move(values);
        streamSlots_.insert(target);
      } else {
        if (values->size() > 1)
          throw std::runtime_error(
              "InvokeFrame cell argument resolved to multiple cells");
        cellReferences_[target] =
            values->empty() ? std::nullopt : std::optional{values->front()};
      }
    }
    activationFrames_[cursor].push_back(
        std::make_shared<const ActivationFrame>(std::move(frame)));
    setCursorOpcode(cursor, descriptor.entry.value());
    arena.discard(mark);
    frameBackingCellsAllocated_ += descriptor.locals.size();
    ++nextActivationId_;
    committed = true;
    return {};
  } catch (const std::exception &error) {
    if (!committed) {
      cellReferences_.swap(previousReferences);
      streams_.swap(previousStreams);
      iterators_.swap(previousIterators);
      streamSlots_.swap(previousStreamSlots);
      iteratorSlots_.swap(previousIteratorSlots);
      activationFrames_.swap(previousFrames);
      cursorPC_.swap(previousCursorPC);
      streamCursor_ = previousStreamCursor;
      arena.release(mark);
      return fail(error.what());
    }
    return fail(error.what());
  }
}

ExecutionResult VortexVM::executeFrameOpcode(const OpcodeKind kind,
                                             const CellRef opcode,
                                             const CellRef cursor) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  const auto rejectEffects = [this, opcode]() -> std::optional<std::string> {
    if (isMemoized(opcode)) return "frame opcodes cannot be memoized";
    if (!core_.preconditionsOf(opcode).empty() ||
        !core_.postconditionsOf(opcode).empty())
      return "frame opcodes do not support pre/postconditions";
    for (const auto slot : core_.inputsOf(opcode))
      if (core_.hasPipeline(slot))
        return "frame opcodes do not support parameter pipelines";
    for (const auto slot : core_.outputsOf(opcode))
      if (core_.hasPipeline(slot))
        return "frame opcodes do not support parameter pipelines";
    return std::nullopt;
  };
  if (const auto error = rejectEffects()) return fail(*error);
  auto &arena = core_.arena();
  if (kind == OpcodeKind::InvokeFrame) {
    auto graphInputs        = core_.inputsOf(opcode);
    const auto graphOutputs = outputsOf(opcode);
    for (std::size_t index = 1; index < graphInputs.size(); ++index)
      graphInputs[index] = runtimeSlot(graphInputs[index]);
    if (graphInputs.empty()) return fail("InvokeFrame requires a descriptor");
    if (!graphInputs.empty() && cellReferences_.contains(graphInputs.front()))
      return fail("InvokeFrame descriptor must be a direct graph reference");
    std::vector<std::vector<CellRef>> arguments;
    arguments.reserve(graphInputs.size() - 1);
    for (std::size_t index = 1; index < graphInputs.size(); ++index) {
      if (iteratorSlots_.contains(graphInputs[index]))
        return fail("InvokeFrame iterator arguments are unsupported");
      auto &values = arguments.emplace_back();
      forEachResultCell(graphInputs[index], [&](const CellRef cell) {
        arena.consumeWork();
        values.push_back(cell);
      });
    }
    return invokeFramePrepared(
        graphInputs.front(), arguments, graphOutputs, cursor,
        arena.linked(opcode, core_.dims().spin, DimVector::POS), opcode);
  }

  const auto graphInputs = core_.inputsOf(opcode);
  if (graphInputs.size() != 1 || !activationDepth(cursor))
    return fail("ReturnFrame requires an active frame and descriptor");
  const auto frameOwner = activationFrames_.at(cursor).back();
  const auto &frame     = *frameOwner;
  if (graphInputs.front() != frame.descriptor)
    return fail("ReturnFrame descriptor does not match active frame");
  if (frame.parentResultSlots.size() != frame.resultPorts.size())
    return fail("ReturnFrame result arity does not match its descriptor");

  if (hasOwnedMutationGroup(cursor, frame.activationId))
    return fail("ReturnFrame cannot leave an owned mutation group open");
  if (!frame.returnOpcode) {
    const auto scopes = guardScopes_.find(cursor);
    if (scopes != guardScopes_.end() && scopes->second &&
        std::ranges::any_of(*scopes->second, [&](const GuardScope &scope) {
          return scope.ownerActivation != frame.activationId;
        }))
      return fail("ReturnFrame ended with an active caller guard scope");
  }

  auto participating = ownedParticipatingGuards(cursor, frame.activationId);
  const auto pending = pendingGuardChecks_.find(cursor);
  const bool returningValidation =
      pending != pendingGuardChecks_.end() && pending->second &&
      pending->second->checkOpcode == opcode &&
      pending->second->ownerActivation == frame.activationId;
  if (!participating.empty() || returningValidation) {
    const auto validation = driveGuardValidation(
        cursor, opcode,
        std::make_shared<const std::vector<RuntimeGuardRef>>(
            std::move(participating)));
    if (!validation.execution.success ||
        validation.state == GuardDriveState::Suspended)
      return validation.execution;
  }

  const auto guards     = guardStacks_.find(cursor);
  const auto guardCount = guards == guardStacks_.end() || !guards->second
                              ? 0
                              : guards->second->size();
  if (guardCount < frame.guardBaseline)
    return fail("ReturnFrame lost caller guard state");
  if (!frame.returnOpcode && frame.guardBaseline != 0)
    return fail("ReturnFrame ended with active caller guards");
  std::optional<RuntimeGuardStack> retainedGuards;
  if (guardCount > frame.guardBaseline) {
    for (std::size_t index = frame.guardBaseline; index < guardCount; ++index)
      if ((*guards->second)[index]->ownerActivation != frame.activationId)
        return fail("ReturnFrame cannot remove another activation's guards");
    retainedGuards = std::make_shared<const std::vector<RuntimeGuardRef>>(
        guards->second->begin(),
        guards->second->begin() +
            static_cast<std::ptrdiff_t>(frame.guardBaseline));
  }

  struct ResultSnapshot {
    FramePortKind kind;
    std::vector<CellRef> cells;
  };
  std::vector<ResultSnapshot> results;
  results.reserve(frame.resultPorts.size());
  for (const auto &port : frame.resultPorts) {
    const CellRef backing = frame.localBacking.at(port.slot);
    if (iteratorSlots_.contains(backing))
      return fail("ReturnFrame iterator results are unsupported");
    std::vector<CellRef> cells;
    forEachResultCell(backing, [&](const CellRef cell) {
      arena.consumeWork();
      cells.push_back(cell);
    });
    if (port.kind == FramePortKind::Cell && cells.size() > 1)
      return fail("ReturnFrame cell result contains multiple cells");
    results.push_back({port.kind, std::move(cells)});
  }

  auto previousGuardScopes        = guardScopes_;
  auto previousGuards             = guardStacks_;
  auto previousReferences         = cellReferences_;
  auto previousStreams            = streams_;
  auto previousIterators          = iterators_;
  auto previousStreamSlots        = streamSlots_;
  auto previousIteratorSlots      = iteratorSlots_;
  auto previousFrames             = activationFrames_;
  auto previousCursorPC           = cursorPC_;
  const auto previousStreamCursor = streamCursor_;
  zigzag::Mark mark;
  try {
    mark = arena.mark();
  } catch (const std::exception &error) {
    return fail(error.what());
  }
  try {
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto destination = frame.parentResultSlots[i];
      if (results[i].kind == FramePortKind::Stream) {
        auto stream = std::make_shared<std::vector<CellRef>>(results[i].cells);
        streams_[destination] = std::move(stream);
        streamSlots_.insert(destination);
        iteratorSlots_.erase(destination);
        iterators_.erase(destination);
        cellReferences_.erase(destination);
      } else {
        cellReferences_[destination] =
            results[i].cells.empty() ? std::nullopt
                                     : std::optional{results[i].cells.front()};
        clearTypedRegister(destination);
      }
    }

    trimGuardScopes(cursor, frame.activationId);
    if (retainedGuards) {
      if ((*retainedGuards)->empty())
        guardStacks_.erase(cursor);
      else
        guardStacks_[cursor] = *retainedGuards;
    }
    auto &frames            = activationFrames_.at(cursor);
    const auto returnOpcode = frame.returnOpcode;
    frames.pop_back();
    for (const auto &[logical, backing] : frame.localBacking) {
      (void)logical;
      cellReferences_.erase(backing);
      streams_.erase(backing);
      iterators_.erase(backing);
      streamSlots_.erase(backing);
      iteratorSlots_.erase(backing);
    }
    if (frames.empty()) activationFrames_.erase(cursor);
    cursorPC_[cursor] = returnOpcode;
    if (arena.contains(cursor)) {
      if (returnOpcode) {
        zigzag::expectWritten(
            arena.setValueBits(cursor, xanadu::ValueKind::Int64,
                               static_cast<std::uint64_t>(*returnOpcode)));
      } else {
        zigzag::expectWritten(
            arena.setValueBits(cursor, xanadu::ValueKind::None, 0));
      }
    }
    arena.discard(mark);
  } catch (const std::exception &error) {
    arena.release(mark);
    guardScopes_.swap(previousGuardScopes);
    guardStacks_.swap(previousGuards);
    cellReferences_.swap(previousReferences);
    streams_.swap(previousStreams);
    iterators_.swap(previousIterators);
    streamSlots_.swap(previousStreamSlots);
    iteratorSlots_.swap(previousIteratorSlots);
    activationFrames_.swap(previousFrames);
    cursorPC_.swap(previousCursorPC);
    streamCursor_ = previousStreamCursor;
    return fail(error.what());
  }
  return {};
}

} // namespace zigzag::vortex
