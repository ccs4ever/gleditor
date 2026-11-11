/**
 * @file vortex_vm.cpp
 * @brief Implementation of Spin-Head Virtual Machine for Vortex.
 */
#include "common/xanadu/vortex/vortex_vm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>

#include <gleditor/logging.hpp>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/contract_codec.hpp"
#include "common/xanadu/zigzag/arena_savepoint.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

namespace zigzag::vortex {

namespace {

constexpr std::size_t maxParameterPipelineInstructions = 1000;

bool sourceMutatingPrimitive(const OpcodeKind kind,
                             const std::size_t arity) noexcept {
  return kind == OpcodeKind::VqlMaskLinks ||
         ((kind == OpcodeKind::Link || kind == OpcodeKind::Value) &&
          arity >= 4) ||
         ((kind == OpcodeKind::New || kind == OpcodeKind::Break) &&
          arity >= 3) ||
         (kind == OpcodeKind::Splice && arity >= 4) ||
         (kind == OpcodeKind::Clone && arity >= 1);
}

// Views borrow snapshot collections so resource preflight never allocates them.
struct ChoiceSnapshotView {
  std::span<const std::uint16_t> deweyPath;
  const CellReferenceMap &cellReferences;
  const StreamMap &streams;
  const IteratorMap &iterators;
  const std::unordered_set<CellRef> &streamSlots;
  const std::unordered_set<CellRef> &iteratorSlots;
  const std::unordered_map<CellRef, ActivationStack> &activationFrames;
  const GuardStackMap &guardStacks;
  const PendingGuardMap &pendingGuardChecks;
  const GuardScopeMap &guardScopes;
  const MutationGroupMap &mutationGroups;
};

template <class Snapshot>
std::size_t countSnapshotEntries(const Snapshot &snapshot, ArenaManifold &arena,
                                 const std::size_t limit) {
  std::size_t count = 1;
  const auto add    = [&](const std::size_t amount) {
    if (count > limit || amount > limit - count) {
      throw std::length_error("scoped choice snapshot entry limit exceeded");
    }
    for (std::size_t index = 0; index < amount; ++index) {
      arena.consumeWork();
    }
    count += amount;
  };
  add(0);
  arena.consumeWork();
  add(snapshot.deweyPath.size());
  add(snapshot.cellReferences.size());
  add(snapshot.streams.size());
  for (const auto &[slot, stream] : snapshot.streams) {
    (void)slot;
    if (stream) add(stream->size());
  }
  add(snapshot.iterators.size());
  for (const auto &[slot, iterator] : snapshot.iterators) {
    (void)slot;
    if (!iterator) continue;
    if (const auto *rank = std::get_if<RankIteratorState>(iterator.get())) {
      add(rank->seen.size());
    } else if (const auto *sequence =
                   std::get_if<SequenceIteratorState>(iterator.get())) {
      add(sequence->chunks.size());
      for (const auto &chunk : sequence->chunks) {
        if (chunk) add(chunk->size());
      }
    }
  }
  add(snapshot.streamSlots.size());
  add(snapshot.iteratorSlots.size());
  add(snapshot.activationFrames.size());
  for (const auto &[cursor, stack] : snapshot.activationFrames) {
    (void)cursor;
    add(stack.size());
    for (const auto &frame : stack) {
      if (!frame) continue;
      add(frame->parentResultSlots.size());
      add(frame->localBacking.size());
      add(frame->resultPorts.size());
    }
  }
  add(snapshot.guardStacks.size());
  for (const auto &[cursor, guards] : snapshot.guardStacks) {
    (void)cursor;
    if (!guards) continue;
    add(guards->size());
    for (const auto &guard : *guards) {
      add(guard->diagnostic.size());
      add(guard->arguments.size());
      for (const auto &argument : guard->arguments) add(argument.size());
    }
  }
  add(snapshot.pendingGuardChecks.size());
  for (const auto &[cursor, pending] : snapshot.pendingGuardChecks) {
    (void)cursor;
    if (!pending) continue;
    if (!pending->guards) continue;
    add(pending->guards->size());
    for (const auto &guard : *pending->guards) {
      add(guard->diagnostic.size());
      add(guard->arguments.size());
      for (const auto &argument : guard->arguments) add(argument.size());
    }
  }
  add(snapshot.guardScopes.size());
  for (const auto &[cursor, scopes] : snapshot.guardScopes) {
    (void)cursor;
    if (scopes) add(scopes->size());
  }
  add(snapshot.mutationGroups.size());
  for (const auto &[cursor, groups] : snapshot.mutationGroups) {
    (void)cursor;
    if (!groups) continue;
    add(groups->size());
    for (const auto &group : *groups) add(group.participants.size());
  }
  return count;
}

double toDouble(const CellValue &val) {
  if (std::holds_alternative<double>(val)) return std::get<double>(val);
  if (std::holds_alternative<std::int64_t>(val))
    return static_cast<double>(std::get<std::int64_t>(val));
  if (std::holds_alternative<bool>(val)) return std::get<bool>(val) ? 1.0 : 0.0;
  try {
    return std::stod(std::get<std::string>(val));
  } catch (...) {
    return 0.0;
  }
}

std::int64_t toInt(const CellValue &val) {
  if (std::holds_alternative<std::int64_t>(val))
    return std::get<std::int64_t>(val);
  if (std::holds_alternative<double>(val))
    return static_cast<std::int64_t>(std::get<double>(val));
  if (std::holds_alternative<bool>(val)) return std::get<bool>(val) ? 1 : 0;
  try {
    return std::stoll(std::get<std::string>(val));
  } catch (...) {
    return 0;
  }
}

std::optional<CellRef> resolveCell(
    const std::vector<CellRef> &inCells, const std::vector<CellValue> &inputs,
    const std::unordered_map<CellRef, std::optional<CellRef>> &cellReferences,
    const std::size_t idx, const ArenaManifold &arena) {
  if (idx < inCells.size()) {
    CellRef c = inCells[idx];
    if (const auto ref = cellReferences.find(c); ref != cellReferences.end()) {
      if (!ref->second || !arena.contains(*ref->second)) {
        return std::nullopt;
      }
      return *ref->second;
    }
    if (arena.contains(c)) {
      return c;
    }
  }
  if (idx < inputs.size()) {
    auto c = static_cast<CellRef>(toInt(inputs[idx]));
    if (arena.contains(c)) {
      return c;
    }
  }
  return std::nullopt;
}

std::optional<std::string> boundedText(const ArenaManifold &arena,
                                       const CellRef cell,
                                       const std::size_t limit) {
  std::size_t bytes = 0;
  const auto spans  = arena.contentOf(cell);
  if (spans.size() > limit) return std::nullopt;
  for (const auto &span : spans) {
    arena.consumeWork();
    if (span.length > limit - std::min(bytes, limit)) return std::nullopt;
    bytes += static_cast<std::size_t>(span.length);
    for (std::uint64_t index = 0; index < span.length; ++index)
      arena.consumeWork();
  }
  return arena.textOf(cell);
}

std::size_t guardCapturedCells(const RuntimeGuardStack &guards,
                               ArenaManifold &arena) {
  std::size_t total = 0;
  if (!guards) return total;
  for (const auto &guard : *guards) {
    arena.consumeWork();
    for (const auto &argument : guard->arguments) {
      arena.consumeWork();
      for (const auto cell : argument) {
        (void)cell;
        arena.consumeWork();
      }
      if (argument.size() > std::numeric_limits<std::size_t>::max() - total)
        throw std::length_error("guard captured-cell budget exceeded");
      total += argument.size();
    }
  }
  return total;
}

} // namespace

VortexVM::VortexVM(VortexCore &core) : core_(core) { installWriteGuard(); }

VortexVM::~VortexVM() {
  // The installed predicate captures this VM — releasing the slot keeps
  // a longer-lived core from invoking a dangling guard.
  if (guardInstalled_) {
    core_.setWriteGuard(nullptr);
  }
}

VortexVM *VortexVM::installWriteGuard() {
  // P15 (spec §6.5/M10): the funnel's kind predicate tests *this*
  // context's opcodeMap_ membership of the resolved clone master —
  // masterOf rather than the raw target, because a clone of a
  // registered op is unregistered yet writes the registered master's
  // bits. Fresh cells are never claimed: an unregistered cell's
  // content is its kind and cannot desync the map.
  core_.setWriteGuard(
      [this](const CellRef master) { return hasOpcodeKind(master); });
  guardInstalled_ = true;
  return this;
}

std::expected<std::size_t, std::string>
VortexVM::registerProgramOps(std::span<const CellRef> enumeration) {
  std::size_t registered = 0;
  for (const CellRef opCell : enumeration) {
    if (!core_.arena().contains(opCell)) {
      return std::unexpected("registerProgramOps: recorded op cell " +
                             std::to_string(opCell) + " is not in the arena");
    }
    // The kind reads off the recorded cell's own content — safe for a
    // *recorded* cell (spec :2326). Never a value scan: an
    // asInt64 ≤ Find sweep of the band would register dimConstant
    // operand cells and every other small-int data cell too.
    const auto kind = core_.arena().asInt64(opCell);
    // The bound is the enum's last ordinal — RunResume while §6.2's
    // ops hold the append-last tail; a new kind landing past it must
    // move this ceiling the way linkedCeiling's does (meta.cpp).
    if (!kind || *kind < 0 ||
        static_cast<std::uint64_t>(*kind) >
            static_cast<std::uint64_t>(OpcodeKind::RunResume)) {
      return std::unexpected("registerProgramOps: recorded cell " +
                             std::to_string(opCell) +
                             " does not decode to an opcode kind");
    }
    setOpcodeKind(opCell, static_cast<OpcodeKind>(*kind));
    ++registered;
  }
  return registered;
}

std::expected<std::size_t, std::string>
VortexVM::registerProgramImage(CellRef imageRecord) {
  if (!core_.arena().contains(imageRecord)) {
    return std::unexpected(
        "registerProgramImage: image record is not an arena cell");
  }
  const auto &dims = core_.dims();
  // The "ops" +d.vars record — fragment.cpp's varsRecord shape: record
  // content is the key verbatim, +d.values the rank head.
  std::optional<CellRef> opsHead;
  for (const auto record :
       zigzag::rankAfter(core_.arena(), imageRecord, dims.vars)) {
    if (core_.arena().textOf(record) == "ops") {
      opsHead = core_.arena().linked(record, dims.values, DimVector::POS);
      break;
    }
  }
  if (!opsHead) {
    return std::unexpected(
        "registerProgramImage: image record carries no \"ops\" rank");
  }
  std::vector<CellRef> enumeration;
  for (const auto member :
       zigzag::rankAfter(core_.arena(), *opsHead, dims.step)) {
    const auto position = core_.arena().asInt64(member);
    if (!position || *position <= 0 ||
        static_cast<std::uint64_t>(*position) >
            std::numeric_limits<CellRef>::max()) {
      return std::unexpected(
          "registerProgramImage: ops member does not name a cell");
    }
    enumeration.push_back(static_cast<CellRef>(*position));
  }
  return registerProgramOps(enumeration);
}

VortexVM *VortexVM::setGuardLimits(const GuardLimits limits) {
  guardLimits_ = limits;
  return this;
}

VortexVM *VortexVM::setProjectionLimits(const ProjectionLimits limits) {
  projectionLimits_ = limits;
  return this;
}

bool VortexVM::hasActiveGuards() const noexcept {
  return !guardStacks_.empty() || !pendingGuardChecks_.empty() ||
         !guardScopes_.empty() || !mutationGroups_.empty() ||
         std::ranges::any_of(choiceStack_, [](const ChoicePoint &choice) {
           return !choice.guardStacks.empty() ||
                  !choice.pendingGuardChecks.empty() ||
                  !choice.guardScopes.empty() || !choice.mutationGroups.empty();
         });
}

std::vector<RuntimeGuardRef>
VortexVM::activeGuards(const CellRef cursor) const {
  const auto found = guardStacks_.find(cursor);
  return found == guardStacks_.end() || !found->second
             ? std::vector<RuntimeGuardRef>{}
             : *found->second;
}

std::vector<RuntimeGuardRef>
VortexVM::ownedParticipatingGuards(const CellRef cursor,
                                   const std::uint64_t activationId) const {
  std::vector<RuntimeGuardRef> result;
  const auto found = mutationGroups_.find(cursor);
  if (found == mutationGroups_.end() || !found->second) return result;
  std::unordered_set<CellRef> seen;
  for (const auto &group : *found->second) {
    for (const auto &guard : group.participants) {
      core_.arena().consumeWork();
      if (guard && guard->ownerActivation == activationId &&
          seen.insert(guard->token).second)
        result.push_back(guard);
    }
  }
  return result;
}

bool VortexVM::hasOwnedMutationGroup(const CellRef cursor,
                                     const std::uint64_t activationId) const {
  const auto found = mutationGroups_.find(cursor);
  return found != mutationGroups_.end() && found->second &&
         std::ranges::any_of(*found->second, [activationId](const auto &group) {
           return group.ownerActivation == activationId;
         });
}

bool VortexVM::mutationGroupActive(const CellRef cursor) const noexcept {
  const auto found = mutationGroups_.find(cursor);
  return found != mutationGroups_.end() && found->second &&
         !found->second->empty();
}

bool VortexVM::hasPendingGuardValidation(const CellRef cursor) const noexcept {
  return pendingGuardChecks_.contains(cursor);
}

void VortexVM::removeValidatedGuardObligations(
    const CellRef cursor, const std::span<const RuntimeGuardRef> guards) {
  if (guards.empty()) return;
  std::unordered_set<CellRef> tokens;
  tokens.reserve(guards.size());
  for (const auto &guard : guards) {
    core_.arena().consumeWork();
    if (guard) tokens.insert(guard->token);
  }
  const auto found = mutationGroups_.find(cursor);
  if (found == mutationGroups_.end() || !found->second) return;
  auto nextGroups =
      std::make_shared<std::vector<MutationGroup>>(*found->second);
  for (auto &group : *nextGroups) {
    std::erase_if(group.participants, [&](const RuntimeGuardRef &guard) {
      return guard && tokens.contains(guard->token);
    });
  }
  auto nextMap = mutationGroups_;
  if (nextGroups->empty())
    nextMap.erase(cursor);
  else
    nextMap[cursor] = std::move(nextGroups);
  mutationGroups_.swap(nextMap);
}

void VortexVM::trimGuardScopes(const CellRef cursor,
                               const std::uint64_t ownerActivation) {
  const auto found = guardScopes_.find(cursor);
  if (found == guardScopes_.end() || !found->second) return;
  auto next = std::make_shared<std::vector<GuardScope>>(*found->second);
  while (!next->empty() && next->back().ownerActivation == ownerActivation)
    next->pop_back();
  auto nextMap = guardScopes_;
  if (next->empty())
    nextMap.erase(cursor);
  else
    nextMap[cursor] = std::move(next);
  guardScopes_.swap(nextMap);
}

void VortexVM::abortOwnedMutationGroups(
    const CellRef cursor, const std::optional<std::uint64_t> owner) noexcept {
  const auto found = mutationGroups_.find(cursor);
  if (found == mutationGroups_.end() || !found->second ||
      found->second->empty())
    return;
  const auto groupStack = found->second;
  std::optional<std::size_t> groupIndex;
  for (std::size_t index = 0; index < groupStack->size(); ++index) {
    if (!owner || groupStack->at(index).ownerActivation == owner) {
      groupIndex = index;
      break;
    }
  }
  if (!groupIndex) return;
  const auto &group = groupStack->at(*groupIndex);
  if (group.checkpointIndex >= choiceStack_.size() ||
      choiceStack_[group.checkpointIndex].choiceId != group.choiceId)
    return;
  while (choiceStack_.size() > group.checkpointIndex + 1) {
    core_.arena().discard(choiceStack_.back().mark);
    choiceStack_.pop_back();
  }
  ChoicePoint saved = std::move(choiceStack_.back());
  choiceStack_.pop_back();
  core_.arena().release(saved.mark);
  if (const auto pc = cursorPC_.find(cursor); pc != cursorPC_.end())
    pc->second = saved.altOp;
  cellReferences_.swap(saved.cellReferences);
  streams_.swap(saved.streams);
  iterators_.swap(saved.iterators);
  streamSlots_.swap(saved.streamSlots);
  iteratorSlots_.swap(saved.iteratorSlots);
  activationFrames_.swap(saved.activationFrames);
  guardStacks_.swap(saved.guardStacks);
  pendingGuardChecks_.swap(saved.pendingGuardChecks);
  guardScopes_.swap(saved.guardScopes);
  mutationGroups_.swap(saved.mutationGroups);
  currentDeweyPath_.swap(saved.deweyPath);
  streamCursor_ = saved.streamCursor;
  pruneResumeState();
}

std::optional<std::uint64_t>
VortexVM::currentActivationId(const CellRef cursor) const {
  const auto found = activationFrames_.find(cursor);
  if (found == activationFrames_.end() || found->second.empty())
    return std::nullopt;
  return found->second.back()->activationId;
}

CellRef VortexVM::mintOpcode(OpcodeKind op, std::string_view label) {
  CellRef opCell = core_.arena().makeScalarCell(static_cast<std::int64_t>(op));
  opcodeMap_[opCell] = op;
  if (!label.empty()) {
    auto span = core_.arena().intern(label);
    zigzag::expectWritten(core_.arena().setContent(
        opCell, std::span<const xanadu::PrimediaSpan>{&span, 1}));
  }
  return opCell;
}

std::optional<CellRef>
VortexVM::assembleSequence(std::span<const OpcodeKind> ops) {
  if (ops.empty()) return std::nullopt;
  CellRef head = mintOpcode(ops[0]);
  CellRef cur  = head;
  for (std::size_t i = 1; i < ops.size(); ++i) {
    CellRef next = mintOpcode(ops[i]);
    zigzag::expectWritten(
        core_.arena().link(cur, core_.dims().spin, false, next));
    cur = next;
  }
  return head;
}

VortexVM *VortexVM::pruneAssemblyState() {
  std::erase_if(opcodeMap_, [this](const auto &entry) {
    return !core_.arena().contains(entry.first);
  });
  std::erase_if(cursorPC_, [this](const auto &entry) {
    return !core_.arena().contains(entry.first);
  });
  pruneResumeState();
  return this;
}

void VortexVM::setOpcodeKind(CellRef opCell, OpcodeKind op) {
  opcodeMap_[opCell] = op;
}

std::optional<OpcodeKind> VortexVM::getOpcodeKind(CellRef opCell) const {
  if (cellReferences_.contains(opCell) || streamSlots_.contains(opCell) ||
      iteratorSlots_.contains(opCell))
    return std::nullopt;
  auto it = opcodeMap_.find(opCell);
  if (it != opcodeMap_.end()) {
    return it->second;
  }
  auto val = core_.arena().asInt64(opCell);
  if (val && *val >= 0 &&
      static_cast<std::uint64_t>(*val) <=
          static_cast<std::uint64_t>(OpcodeKind::RunResume)) {
    return static_cast<OpcodeKind>(*val);
  }
  return std::nullopt;
}

std::optional<CellRef> VortexVM::getCursorOpcode(CellRef cursor) const {
  if (const auto it = cursorPC_.find(cursor); it != cursorPC_.end()) {
    return it->second;
  }
  // Cursor cells can arrive through an arena snapshot rather than this VM's
  // cache. Only representable, nonzero operation references name a program.
  if (const auto value = core_.arena().asInt64(cursor);
      value && *value > 0 &&
      static_cast<std::uint64_t>(*value) <=
          std::numeric_limits<CellRef>::max()) {
    return static_cast<CellRef>(*value);
  }
  return std::nullopt;
}

std::optional<CellRef>
VortexVM::cellReferenceOf(const CellRef outputSlot) const {
  const auto found = cellReferences_.find(outputSlot);
  if (found == cellReferences_.end()) return std::nullopt;
  return found->second;
}

std::optional<CellRef> VortexVM::resultCell(const CellRef slot) const {
  if (streamSlots_.contains(slot) || iteratorSlots_.contains(slot))
    return std::nullopt;
  const auto found = cellReferences_.find(slot);
  if (found != cellReferences_.end()) return found->second;
  return core_.arena().contains(slot) ? std::optional{slot} : std::nullopt;
}

void VortexVM::setCursorOpcode(CellRef cursor, std::optional<CellRef> op) {
  if (op && *op == noCell) {
    throw std::invalid_argument(
        "an opcode reference must be a cell or nullopt");
  }
  cursorPC_[cursor] = op;
  if (!op) {
    abortOwnedMutationGroups(cursor, std::nullopt);
    cursorPC_[cursor] = std::nullopt;
    clearResumeState(cursor);
    guardStacks_.erase(cursor);
    guardScopes_.erase(cursor);
    if (const auto pending = pendingGuardChecks_.find(cursor);
        pending != pendingGuardChecks_.end() && pending->second) {
      cellReferences_.erase(pending->second->resultSlot);
      clearTypedRegister(pending->second->resultSlot);
    }
    pendingGuardChecks_.erase(cursor);
    if (const auto found = activationFrames_.find(cursor);
        found != activationFrames_.end()) {
      for (const auto &frame : found->second) {
        for (const auto &[logical, backing] : frame->localBacking) {
          (void)logical;
          cellReferences_.erase(backing);
          streams_.erase(backing);
          iterators_.erase(backing);
          streamSlots_.erase(backing);
          iteratorSlots_.erase(backing);
        }
      }
      activationFrames_.erase(found);
    }
  }
  if (core_.arena().contains(cursor)) {
    if (op) {
      zigzag::expectWritten(core_.arena().setValueBits(
          cursor, xanadu::ValueKind::Int64, static_cast<std::uint64_t>(*op)));
    } else {
      zigzag::expectWritten(
          core_.arena().setValueBits(cursor, xanadu::ValueKind::None, 0));
    }
  }
}

CellRef VortexVM::spawnCursor(CellRef entryOpcode, std::string_view name) {
  CellRef cursor = core_.arena().makeCell();
  setCursorOpcode(cursor, entryOpcode);
  if (!name.empty()) {
    CellRef nameCell = core_.arena().makeCell(name);
    zigzag::expectWritten(
        core_.arena().link(cursor, core_.dims().name, false, nameCell));
  }

  // Onto the tail of home's +d.cursors rank.
  zigzag::expectWritten(core_.arena().link(
      zigzag::rankTail(core_.arena(), core_.home(), core_.dims().cursors),
      core_.dims().cursors, false, cursor));
  return cursor;
}

std::vector<CellRef> VortexVM::activeCursors() const {
  return zigzag::rankAfter(core_.arena(), core_.home(), core_.dims().cursors) |
         std::ranges::to<std::vector>();
}

void VortexVM::pruneCursors(CellRef keep) {
  const DimRef dim = core_.dims().cursors;
  for (const CellRef member : activeCursors()) {
    if (member == keep) continue;
    // Same-dim rank unlink: forward edge first so the successor's backlink
    // frees before the predecessor repair rewires it.
    const auto pred = core_.arena().linked(member, dim, zigzag::Negward);
    const auto succ = core_.arena().linked(member, dim, zigzag::Posward);
    if (succ)
      zigzag::expectWritten(core_.arena().unlink(member, dim, zigzag::Posward));
    if (pred) {
      if (succ)
        zigzag::expectWritten(
            core_.arena().link(*pred, dim, zigzag::Posward, *succ));
      else
        zigzag::expectWritten(
            core_.arena().unlink(*pred, dim, zigzag::Posward));
    }
  }
}

void VortexVM::enableMemoization(CellRef opcode, std::string_view memoKey) {
  memoizedOps_[opcode] = std::string(memoKey);
}

void VortexVM::writeCellReferences(const std::span<const CellRef> outputSlots,
                                   const std::vector<CellValue> &values,
                                   const bool preserveTypedRegisters) {
  for (std::size_t i = 0; i < outputSlots.size(); ++i) {
    const CellRef slot = outputSlots[i];
    if (preserveTypedRegisters &&
        (streamSlots_.contains(slot) || iteratorSlots_.contains(slot)))
      continue;
    clearTypedRegister(slot);
    std::optional<CellRef> target;
    if (i < values.size()) {
      if (const auto *const raw = std::get_if<std::int64_t>(&values[i]);
          raw != nullptr && *raw >= 0 &&
          static_cast<std::uint64_t>(*raw) <=
              std::numeric_limits<CellRef>::max()) {
        const auto candidate = static_cast<CellRef>(*raw);
        if (core_.arena().contains(candidate)) target = candidate;
      }
    }
    cellReferences_[slot] = target;
    // Keep the reference visible in the output slot for the VM's normal
    // snapshot and postcondition paths; cellReferences_ preserves its handle
    // identity when a later structural opcode consumes the slot.
    core_.writeValue(slot, 0, std::nullopt,
                     static_cast<std::int64_t>(target.value_or(noCell)));
  }
}

CellValue VortexVM::inputValue(const CellRef input) const {
  if (streamSlots_.contains(input))
    throw std::invalid_argument("a stream register is not a scalar input");
  if (iteratorSlots_.contains(input))
    throw std::invalid_argument("an iterator register is not a scalar input");
  if (const auto resume = resumeValueForInput(input)) return *resume;
  if (const auto reference = cellReferences_.find(input);
      reference != cellReferences_.end()) {
    if (reference->second && core_.arena().contains(*reference->second)) {
      return core_.render(*reference->second)
          .value_or(CellValue{static_cast<std::int64_t>(noCell)});
    }
    return static_cast<std::int64_t>(noCell);
  }
  return core_.render(input).value_or(
      CellValue{static_cast<std::int64_t>(noCell)});
}

ExecutionResult VortexVM::executeProbeLink(const CellRef opcode,
                                           std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  try {
    const auto inputs = inputsOf(opcode);
    const auto slots  = outputsOf(opcode);
    if (inputs.size() != 3 || slots.size() != 1)
      return fail("ProbeLink expects three inputs and one output");
    if (std::ranges::find(inputs, slots.front()) != inputs.end())
      return fail("ProbeLink output must not alias an input");
    if (isMemoized(opcode)) return fail("ProbeLink cannot be memoized");
    if (!core_.preconditionsOf(opcode).empty() ||
        !core_.postconditionsOf(opcode).empty())
      return fail("ProbeLink does not support pre/postconditions");

    const auto typed = [this](const CellRef slot) {
      return streamSlots_.contains(slot) || iteratorSlots_.contains(slot);
    };
    if (std::ranges::any_of(inputs, typed) || typed(slots.front()))
      return fail("ProbeLink requires scalar reference slots");
    if (std::ranges::any_of(
            inputs,
            [&](const CellRef slot) { return core_.hasPipeline(slot); }) ||
        core_.hasPipeline(slots.front()))
      return fail("ProbeLink does not support parameter pipelines");

    for (const CellRef slot : inputs)
      if (!core_.arena().contains(slot))
        return fail("ProbeLink input slot is missing");
    if (!core_.arena().contains(slots.front()))
      return fail("ProbeLink output slot is missing");

    if (cellReferences_.contains(inputs[1]) ||
        cellReferences_.contains(inputs[2]))
      return fail("ProbeLink dimension and direction must be scalar values");
    const auto dimensionValue = core_.arena().asInt64(inputs[1]);
    if (!dimensionValue || *dimensionValue < 0 ||
        static_cast<std::uint64_t>(*dimensionValue) >
            std::numeric_limits<DimRef>::max() ||
        !core_.arena().contains(static_cast<DimRef>(*dimensionValue)))
      return fail("ProbeLink dimension must be a valid 32-bit dimension");
    const auto directionValue = core_.arena().asInt64(inputs[2]);
    if (!directionValue || (*directionValue != 1 && *directionValue != -1))
      return fail("ProbeLink direction must be exactly +1 or -1");

    const auto anchor =
        resolveCell(inputs, {}, cellReferences_, 0, core_.arena());
    core_.arena().consumeWork();
    const auto neighbor =
        anchor ? core_.arena().linkedMinting(
                     *anchor, static_cast<DimRef>(*dimensionValue),
                     *directionValue > 0 ? DimVector::POS : DimVector::NEG)
               : std::nullopt;
    outputs.emplace_back(static_cast<std::int64_t>(neighbor.value_or(noCell)));
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

ExecutionResult VortexVM::executeVqlMaskLinks(const CellRef opcode,
                                              std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  try {
    auto &arena       = core_.arena();
    const auto inputs = inputsOf(opcode);
    if (inputs.size() < 2 || (inputs.size() - 2) % 2 != 0 ||
        !outputsOf(opcode).empty() || !outputs.empty())
      return fail("VqlMaskLinks expects owner, default, and dimension/mask "
                  "pairs with no outputs");
    if (isMemoized(opcode)) return fail("VqlMaskLinks cannot be memoized");
    if (!core_.preconditionsOf(opcode).empty() ||
        !core_.postconditionsOf(opcode).empty())
      return fail("VqlMaskLinks does not support pre/postconditions");
    if (std::ranges::any_of(inputs, [&](const CellRef slot) {
          return streamSlots_.contains(slot) || iteratorSlots_.contains(slot);
        }))
      return fail("VqlMaskLinks requires scalar reference slots");
    if (std::ranges::any_of(inputs, [&](const CellRef slot) {
          return core_.hasPipeline(slot);
        }))
      return fail("VqlMaskLinks does not support parameter pipelines");
    if ((inputs.size() - 2) / 2 > projectionLimits_.maxRules)
      return fail("VqlMaskLinks rule limit exceeded");
    for (const auto slot : inputs)
      if (!arena.contains(slot))
        return fail("VqlMaskLinks input slot is missing");

    const auto owner = resolveCell(inputs, {}, cellReferences_, 0, arena);
    if (!owner) return fail("VqlMaskLinks owner is not a cell reference");
    if (arena.isQuoteOccurrence(*owner))
      return fail("VqlMaskLinks does not support quote occurrences");
    const auto foreignOwner = arena.foreignOf(*owner);
    if (cellReferences_.contains(inputs[1]))
      return fail("VqlMaskLinks default must be a Boolean scalar");
    const auto defaultKeep = arena.asBool(inputs[1]);
    if (!defaultKeep) return fail("VqlMaskLinks default must be Boolean");

    std::unordered_map<DimRef, std::uint8_t> rules;
    rules.reserve((inputs.size() - 2) / 2);
    for (std::size_t index = 2; index < inputs.size(); index += 2) {
      arena.consumeWork();
      if (cellReferences_.contains(inputs[index]) ||
          cellReferences_.contains(inputs[index + 1]))
        return fail("VqlMaskLinks dimension and mask must be scalar values");
      const auto rawDimension = arena.asInt64(inputs[index]);
      if (!rawDimension || *rawDimension < 0 ||
          static_cast<std::uint64_t>(*rawDimension) >
              std::numeric_limits<DimRef>::max())
        return fail("VqlMaskLinks dimension must be a valid 32-bit reference");
      const auto dimension = static_cast<DimRef>(*rawDimension);
      if (dimension == noCell || !arena.contains(dimension))
        return fail("VqlMaskLinks dimension must be a valid local dimension");
      const auto rawBits = arena.asInt64(inputs[index + 1]);
      if (!rawBits || *rawBits < 0 || *rawBits > 3)
        return fail("VqlMaskLinks keep bits must be in the range 0..3");
      if (!rules.emplace(dimension, static_cast<std::uint8_t>(*rawBits)).second)
        return fail("VqlMaskLinks dimensions must be unique");
    }

    struct Removal {
      DimRef dimension;
      DimVector direction;
    };
    std::vector<Removal> removals;
    const auto planRow = [&](const DimRef dimension,
                             const std::optional<EffectiveLinkNeighbor> &pos,
                             const std::optional<EffectiveLinkNeighbor> &neg) {
      arena.consumeWork();
      if (!arena.contains(dimension)) {
        throw std::logic_error(
            "VqlMaskLinks encountered an unmapped local dimension");
      }
      const auto rule          = rules.find(dimension);
      const auto planDirection = [&](const DimVector direction,
                                     const auto &neighbor,
                                     const std::uint8_t bit) {
        if (!neighbor) return;
        arena.consumeWork();
        const bool keep =
            rule != rules.end() ? (rule->second & bit) != 0 : *defaultKeep;
        if (!keep) removals.push_back({dimension, direction});
      };
      planDirection(DimVector::POS, pos, 1);
      planDirection(DimVector::NEG, neg, 2);
    };

    if (foreignOwner.has_value()) {
      if (projectionLimits_.maxLinksPerOwner == 0 ||
          projectionLimits_.maxLinksPerOwner >
              std::numeric_limits<std::size_t>::max() / 2) {
        return fail("VqlMaskLinks directed endpoint limit is invalid");
      }
      const auto projected = arena.snapshotLinks(
          *owner,
          zigzag::EffectiveLinkLimits{
              .maxCandidates = projectionLimits_.maxSnapshotCandidates,
              .maxLinks      = 2 * projectionLimits_.maxLinksPerOwner,
              .maxWork       = projectionLimits_.maxSnapshotWork},
          zigzag::DimensionDirectory{.head = core_.dims().dims,
                                     .axis = core_.dims().dims});
      if (!projected) {
        switch (projected.error()) {
        case zigzag::EffectiveLinkRefusal::UnknownOwner:
          return fail("VqlMaskLinks effective-link owner is unavailable");
        case zigzag::EffectiveLinkRefusal::UnmappedDimension:
          return fail("VqlMaskLinks encountered an unmapped foreign dimension");
        case zigzag::EffectiveLinkRefusal::AmbiguousDimension:
          return fail("VqlMaskLinks foreign dimension mapping is ambiguous");
        case zigzag::EffectiveLinkRefusal::NameResolutionRequired:
          return fail(
              "VqlMaskLinks requires explicit foreign dimension mappings");
        case zigzag::EffectiveLinkRefusal::LimitExceeded:
          return fail("VqlMaskLinks effective-link snapshot limit exceeded");
        case zigzag::EffectiveLinkRefusal::InvalidDimensionDirectory:
          return fail("VqlMaskLinks Vortex dimension directory is invalid");
        }
      }
      if (projected->size() > projectionLimits_.maxLinksPerOwner)
        return fail("VqlMaskLinks owner link limit exceeded");
      for (const auto &row : *projected) {
        planRow(row.dim, row.pos, row.neg);
      }
    } else {
      const auto links = arena.dimensionsOf(*owner);
      if (links.size() > projectionLimits_.maxLinksPerOwner)
        return fail("VqlMaskLinks owner link limit exceeded");
      const std::vector<DimLink> snapshot(links.begin(), links.end());
      for (const auto &link : snapshot) {
        const auto asNeighbor = [](const std::optional<CellRef> neighbor)
            -> std::optional<EffectiveLinkNeighbor> {
          if (!neighbor) return std::nullopt;
          return EffectiveLinkNeighbor{*neighbor};
        };
        planRow(link.dim, asNeighbor(link.neighbor(DimVector::POS)),
                asNeighbor(link.neighbor(DimVector::NEG)));
      }
    }

    // The proxy map and this rank's tail cache must remain in agreement. Until
    // unlink maintains that cache, reject the exact index axis before writes.
    if (foreignOwner.has_value() && !removals.empty()) {
      std::size_t preflightWork = 0;
      if (preflightWork >= projectionLimits_.maxSnapshotWork) {
        return fail("VqlMaskLinks store-ref index preflight limit exceeded");
      }
      preflightWork++;
      arena.consumeWork();
      const auto indexDimension = arena.storeRefsDimension(foreignOwner->space);
      if (!indexDimension) {
        return fail("VqlMaskLinks store-ref index identity is unavailable");
      }
      for (const auto &removal : removals) {
        if (preflightWork >= projectionLimits_.maxSnapshotWork) {
          return fail("VqlMaskLinks store-ref index preflight limit exceeded");
        }
        preflightWork++;
        arena.consumeWork();
        if (removal.dimension == *indexDimension) {
          return fail(
              "VqlMaskLinks cannot remove an internal store-ref index link");
        }
      }
    }

    // Fuel is charged during the complete read/preflight pass above. The
    // savepoint then makes reciprocal edge changes all-or-nothing on refusal.
    zigzag::ArenaSavepoint savepoint(arena);
    for (const auto &removal : removals)
      zigzag::expectWritten(
          arena.unlink(*owner, removal.dimension, removal.direction));
    savepoint.commit();
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

ExecutionResult
VortexVM::executeVqlFieldProbe(const CellRef opcode,
                               std::vector<CellValue> &outputs) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  try {
    const auto inputs = inputsOf(opcode);
    const auto slots  = outputsOf(opcode);
    if (inputs.size() != 3 || slots.size() != 1)
      return fail("VqlFieldProbe expects owner, dimension name, direction, and "
                  "one output");
    if (std::ranges::find(inputs, slots.front()) != inputs.end())
      return fail("VqlFieldProbe output must not alias an input");
    if (isMemoized(opcode)) return fail("VqlFieldProbe cannot be memoized");
    if (!core_.preconditionsOf(opcode).empty() ||
        !core_.postconditionsOf(opcode).empty())
      return fail("VqlFieldProbe does not support pre/postconditions");

    const auto typed = [this](const CellRef slot) {
      return streamSlots_.contains(slot) || iteratorSlots_.contains(slot);
    };
    if (std::ranges::any_of(inputs, typed) || typed(slots.front()))
      return fail("VqlFieldProbe requires scalar reference slots");
    if (std::ranges::any_of(
            inputs,
            [&](const CellRef slot) { return core_.hasPipeline(slot); }) ||
        core_.hasPipeline(slots.front()))
      return fail("VqlFieldProbe does not support parameter pipelines");
    for (const CellRef slot : inputs) {
      if (!core_.arena().contains(slot))
        return fail("VqlFieldProbe input slot is missing");
    }
    if (!core_.arena().contains(slots.front()))
      return fail("VqlFieldProbe output slot is missing");

    if (cellReferences_.contains(inputs[1]) ||
        cellReferences_.contains(inputs[2]))
      return fail("VqlFieldProbe name and direction must be scalar values");
    if (core_.arena().valueKindOf(inputs[1]) != xanadu::ValueKind::None)
      return fail("VqlFieldProbe dimension name must be text");
    const auto direction = core_.arena().asInt64(inputs[2]);
    if (!direction || (*direction != 1 && *direction != -1))
      return fail("VqlFieldProbe direction must be exactly +1 or -1");

    const xanadu::vql::ContractDescriptorLimits limits{};
    const auto fragments = core_.arena().contentOf(inputs[1]);
    if (fragments.size() > limits.maxBytes)
      return fail("VqlFieldProbe dimension name has too many text fragments");
    std::size_t nameBytes = 0;
    for (const auto &fragment : fragments) {
      core_.arena().consumeWork();
      if (fragment.length > limits.maxBytes - nameBytes)
        return fail("VqlFieldProbe dimension name exceeds the text limit");
      nameBytes += static_cast<std::size_t>(fragment.length);
    }
    for (std::size_t byte = 0; byte < nameBytes; ++byte)
      core_.arena().consumeWork();
    const std::string name = core_.arena().textOf(inputs[1]);
    if (name.empty()) return fail("VqlFieldProbe dimension name is empty");

    const auto owner =
        resolveCell(inputs, {}, cellReferences_, 0, core_.arena());
    zigzag::ArenaSavepoint savepoint(core_.arena());
    xanadu::MultiStoreCoordinator fallback(core_);
    auto &coordinator = contractCoordinator_ ? *contractCoordinator_ : fallback;
    const auto dimension = coordinator.resolveDimension(name);
    const auto neighbor =
        owner ? core_.arena().linkedMinting(*owner, dimension,
                                            *direction > 0 ? DimVector::POS
                                                           : DimVector::NEG)
              : std::nullopt;
    const std::vector<CellValue> reference{
        static_cast<std::int64_t>(neighbor.value_or(noCell))};
    const auto outputSlot = slots.front();
    const auto prior      = cellReferences_.find(outputSlot);
    const bool hadPrior   = prior != cellReferences_.end();
    const std::optional<CellRef> priorReference =
        hadPrior ? prior->second : std::nullopt;
    try {
      writeCellReferences(slots, reference);
      outputs.emplace_back(reference.front());
    } catch (...) {
      if (hadPrior) {
        if (const auto found = cellReferences_.find(outputSlot);
            found != cellReferences_.end()) {
          found->second = priorReference;
        }
      } else {
        cellReferences_.erase(outputSlot);
      }
      throw;
    }
    savepoint.commit();
    return {};
  } catch (const std::exception &error) {
    return fail(error.what());
  }
}

bool VortexVM::isMemoized(CellRef opcode) const {
  return memoizedOps_.contains(opcode);
}

std::optional<std::string> VortexVM::getMemoKey(CellRef opcode) const {
  const auto it = memoizedOps_.find(opcode);
  if (it == memoizedOps_.end()) return std::nullopt;
  return it->second;
}

std::string formatDeweyPath(std::span<const std::uint16_t> path) {
  std::string out;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (i > 0) {
      out.push_back('.');
    }
    out += std::to_string(path[i]);
  }
  return out;
}

void VortexVM::pushChoicePoint(const std::optional<CellRef> cursor,
                               const CellRef altOp) {
  pushChoicePointImpl(cursor, altOp, false);
}

bool VortexVM::hasScopedChoices() const noexcept {
  return std::ranges::any_of(
      choiceStack_, [](const ChoicePoint &choice) { return choice.scoped; });
}

bool VortexVM::scopedChoicesOwnedBy(
    const std::optional<CellRef> cursor) const noexcept {
  return std::ranges::all_of(choiceStack_, [&](const ChoicePoint &choice) {
    return !choice.scoped || choice.cursor == cursor;
  });
}

void VortexVM::enforceScopedChoiceBudget(const bool addScoped) {
  const auto scopes = static_cast<std::size_t>(std::ranges::count_if(
      choiceStack_, [](const ChoicePoint &choice) { return choice.scoped; }));
  if ((addScoped || scopes != 0) &&
      choiceStack_.size() >= scopedChoiceLimits_.maxDepth) {
    throw std::length_error("scoped choice total depth limit exceeded");
  }
  const ChoiceSnapshotView current{
      currentDeweyPath_,   cellReferences_, streams_,          iterators_,
      streamSlots_,        iteratorSlots_,  activationFrames_, guardStacks_,
      pendingGuardChecks_, guardScopes_,    mutationGroups_};
  // The current view is the prospective snapshot, not an additional copy.
  std::size_t total = countSnapshotEntries(
      current, core_.arena(), scopedChoiceLimits_.maxSnapshotEntries);
  if (!addScoped && (!oraclePath_.empty() || cutCallback_)) {
    if (total == scopedChoiceLimits_.maxSnapshotEntries)
      throw std::length_error("scoped choice snapshot entry limit exceeded");
    core_.arena().consumeWork();
    ++total;
  }
  for (const auto &choice : choiceStack_) {
    const auto entries = countSnapshotEntries(
        choice, core_.arena(), scopedChoiceLimits_.maxSnapshotEntries);
    if (entries > scopedChoiceLimits_.maxSnapshotEntries -
                      std::min(total, scopedChoiceLimits_.maxSnapshotEntries)) {
      throw std::length_error("scoped choice snapshot entry limit exceeded");
    }
    total += entries;
  }
  if (total > scopedChoiceLimits_.maxSnapshotEntries) {
    throw std::length_error("scoped choice snapshot entry limit exceeded");
  }
}

void VortexVM::pushChoicePointImpl(const std::optional<CellRef> cursor,
                                   const CellRef altOp, const bool scoped) {
  if (cursor && *cursor == noCell) {
    throw std::invalid_argument(
        "choice-point cursor must be a cell or nullopt");
  }
  if (altOp == noCell || !core_.arena().contains(altOp) ||
      !getOpcodeKind(altOp)) {
    throw std::invalid_argument("choice point requires a valid alternative");
  }
  if ((scoped || hasScopedChoices()) &&
      (!cursor || !scopedChoicesOwnedBy(cursor))) {
    throw std::invalid_argument(
        "choice point cursor does not match the active scoped choice");
  }
  if (const auto active = std::ranges::find_if(mutationGroups_,
                                               [](const auto &entry) {
                                                 return entry.second &&
                                                        !entry.second->empty();
                                               });
      active != mutationGroups_.end() && (!cursor || active->first != *cursor))
    throw std::invalid_argument(
        "choice point cursor does not match the active mutation group");
  if (scoped || hasScopedChoices()) enforceScopedChoiceBudget(scoped);
  std::optional<CellRef> stackFrame;
  if (cursor && core_.arena().contains(*cursor)) {
    stackFrame = core_.arena().linked(*cursor, core_.dims().stack, false);
  }
  std::vector<std::uint16_t> altDewey;
  auto nextDewey = currentDeweyPath_;
  if (scoped) {
    altDewey = currentDeweyPath_;
  } else if (!oraclePath_.empty() || cutCallback_) {
    altDewey = currentDeweyPath_;
    altDewey.push_back(1);
    nextDewey.push_back(0);
  }
  if (nextChoiceId_ == 0)
    throw std::overflow_error("choice-point identity space exhausted");
  choiceStack_.push_back(ChoicePoint{
      .cursor             = cursor,
      .altOp              = altOp,
      .mark               = {},
      .stackFrame         = stackFrame,
      .cutBarrier         = choiceStack_.size(),
      .deweyPath          = std::move(altDewey),
      .nextAltIndex       = 1,
      .cyclesSinceRoot    = totalCycles_,
      .cellReferences     = cellReferences_,
      .streams            = streams_,
      .iterators          = iterators_,
      .streamSlots        = streamSlots_,
      .iteratorSlots      = iteratorSlots_,
      .streamCursor       = streamCursor_,
      .activationFrames   = activationFrames_,
      .guardStacks        = guardStacks_,
      .pendingGuardChecks = pendingGuardChecks_,
      .guardScopes        = guardScopes_,
      .mutationGroups     = mutationGroups_,
      .choiceId           = nextChoiceId_++,
      .scoped             = scoped,
  });
  // Finish all allocating snapshots before taking a LIFO arena mark.
  try {
    choiceStack_.back().mark = core_.arena().mark();
  } catch (...) {
    choiceStack_.pop_back();
    throw;
  }
  currentDeweyPath_ = std::move(nextDewey);
}

bool VortexVM::backtrack(const std::optional<CellRef> cursor) {
  if (cursor && *cursor == noCell) {
    throw std::invalid_argument("backtrack cursor must be a cell or nullopt");
  }
  if (hasScopedChoices() && (!cursor || !scopedChoicesOwnedBy(cursor))) {
    throw std::invalid_argument(
        "backtrack cursor does not match the active scoped choice");
  }
  if (const auto active = std::ranges::find_if(mutationGroups_,
                                               [](const auto &entry) {
                                                 return entry.second &&
                                                        !entry.second->empty();
                                               });
      active != mutationGroups_.end() && (!cursor || active->first != *cursor))
    throw std::invalid_argument(
        "backtrack cursor does not match the active mutation group");
  while (!choiceStack_.empty()) {
    if (choiceStack_.back().mutationCheckpointId) return false;
    // A choice point owned by a different activation is never this cursor's
    // to unwind: consuming it would drop its mark (and the rollback it
    // guards) outright. Stop instead -- the caller's own points sit below it.
    if (cursor && choiceStack_.back().cursor &&
        *choiceStack_.back().cursor != *cursor) {
      return false;
    }
    ChoicePoint cp = choiceStack_.back();
    choiceStack_.pop_back();
    if (!cursor || cp.cursor == cursor || !cp.cursor) {
      core_.arena().release(cp.mark);
      cellReferences_     = std::move(cp.cellReferences);
      streams_            = std::move(cp.streams);
      iterators_          = std::move(cp.iterators);
      streamSlots_        = std::move(cp.streamSlots);
      iteratorSlots_      = std::move(cp.iteratorSlots);
      streamCursor_       = cp.streamCursor;
      activationFrames_   = std::move(cp.activationFrames);
      guardStacks_        = std::move(cp.guardStacks);
      pendingGuardChecks_ = std::move(cp.pendingGuardChecks);
      guardScopes_        = std::move(cp.guardScopes);
      mutationGroups_     = std::move(cp.mutationGroups);
      pruneResumeState();
      if (cp.scoped || !oraclePath_.empty() || cutCallback_) {
        currentDeweyPath_ = cp.deweyPath;
      }
      if (cursor) {
        setCursorOpcode(*cursor, cp.altOp);
        if (cp.stackFrame && core_.arena().contains(*cursor)) {
          zigzag::expectWritten(core_.arena().link(*cursor, core_.dims().stack,
                                                   false, *cp.stackFrame));
        }
      }
      return true;
    }
  }
  return false;
}

void VortexVM::cut(std::size_t cutBarrier) {
  if (activeCursor_) {
    const auto active =
        std::ranges::find_if(mutationGroups_, [](const auto &entry) {
          return entry.second && !entry.second->empty();
        });
    if (active != mutationGroups_.end() && active->first != *activeCursor_)
      throw std::invalid_argument(
          "cut cursor does not match the active mutation group");
  }
  if (activeCursor_ &&
      std::ranges::any_of(
          choiceStack_.begin() + static_cast<std::ptrdiff_t>(
                                     std::min(cutBarrier, choiceStack_.size())),
          choiceStack_.end(), [&](const ChoicePoint &choice) {
            return choice.scoped && choice.cursor != activeCursor_;
          })) {
    throw std::invalid_argument(
        "cut cursor does not match the active scoped choice");
  }
  const auto firstRemoved =
      choiceStack_.begin() +
      static_cast<std::ptrdiff_t>(std::min(cutBarrier, choiceStack_.size()));
  if (std::ranges::any_of(firstRemoved, choiceStack_.end(),
                          [](const ChoicePoint &choice) {
                            return choice.mutationCheckpointId.has_value();
                          }))
    throw std::invalid_argument("cut cannot cross an active mutation group");
  const auto firstScope = std::ranges::find_if(
      choiceStack_, [](const ChoicePoint &choice) { return choice.scoped; });
  const auto scopeBoundary =
      static_cast<std::size_t>(std::distance(choiceStack_.begin(), firstScope));
  bool removedOrdinary = false;
  while (choiceStack_.size() > cutBarrier) {
    const bool internal = choiceStack_.size() > scopeBoundary;
    ChoicePoint cp      = choiceStack_.back();
    choiceStack_.pop_back();
    core_.arena().discard(cp.mark);
    removedOrdinary |= !internal;
    if (!internal && cutCallback_ && !cp.deweyPath.empty()) {
      const auto parentSpan =
          std::span<const std::uint16_t>{cp.deweyPath}.subspan(
              0, cp.deweyPath.size() - 1);
      cutCallback_(formatDeweyPath(parentSpan));
    }
  }
  if (removedOrdinary && cutCallback_ && choiceStack_.empty() &&
      !currentDeweyPath_.empty()) {
    const auto parentSpan =
        std::span<const std::uint16_t>{currentDeweyPath_}.subspan(
            0, currentDeweyPath_.size() - 1);
    cutCallback_(formatDeweyPath(parentSpan));
  }
}

VortexVM *VortexVM::abortScopedChoices(const CellRef cursor) noexcept {
  const auto outer =
      std::ranges::find_if(choiceStack_, [cursor](const ChoicePoint &choice) {
        return choice.scoped && choice.cursor == cursor;
      });
  if (outer == choiceStack_.end()) return this;

  const auto outerIndex =
      static_cast<std::size_t>(std::distance(choiceStack_.begin(), outer));
  ChoicePoint saved = std::move(*outer);
  while (choiceStack_.size() > outerIndex + 1) {
    core_.arena().discard(choiceStack_.back().mark);
    choiceStack_.pop_back();
  }
  choiceStack_.pop_back();
  core_.arena().release(saved.mark);

  cellReferences_.swap(saved.cellReferences);
  streams_.swap(saved.streams);
  iterators_.swap(saved.iterators);
  streamSlots_.swap(saved.streamSlots);
  iteratorSlots_.swap(saved.iteratorSlots);
  activationFrames_.swap(saved.activationFrames);
  guardStacks_.swap(saved.guardStacks);
  pendingGuardChecks_.swap(saved.pendingGuardChecks);
  guardScopes_.swap(saved.guardScopes);
  mutationGroups_.swap(saved.mutationGroups);
  currentDeweyPath_.swap(saved.deweyPath);
  streamCursor_ = saved.streamCursor;
  pruneResumeState();
  return this;
}

std::size_t VortexVM::choiceDepth() const noexcept {
  return choiceStack_.size();
}

void VortexVM::clearChoicePoints() { cut(0); }

VortexVM *VortexVM::setScopedChoiceLimits(const ScopedChoiceLimits limits) {
  scopedChoiceLimits_ = limits;
  return this;
}

VortexVM *VortexVM::setOraclePath(std::span<const std::uint16_t> path) {
  oraclePath_.assign(path.begin(), path.end());
  oracleCursor_ = 0;
  currentDeweyPath_.clear();
  return this;
}

VortexVM *VortexVM::setCurrentDeweyPath(std::span<const std::uint16_t> path) {
  currentDeweyPath_.assign(path.begin(), path.end());
  return this;
}

std::optional<ChoicePoint> VortexVM::stealNearRootChoicePoint() {
  if (choiceStack_.empty() || hasScopedChoices() || !streams_.empty() ||
      !iterators_.empty() || hasActiveGuards()) {
    return std::nullopt;
  }
  ChoicePoint stolen = std::move(choiceStack_.front());
  choiceStack_.erase(choiceStack_.begin());
  core_.arena().evictOldestMark();
  for (auto &cp : choiceStack_) {
    if (cp.cutBarrier > 0) {
      cp.cutBarrier--;
    }
  }
  return stolen;
}

VortexVM *
VortexVM::setCutCallback(std::function<void(std::string_view deweyPrefix)> cb) {
  cutCallback_ = std::move(cb);
  return this;
}

ExecutionResult VortexVM::runPipeline(CellRef paramCell) {
  // The funnel's refusal mapped to the per-site typed failure — the
  // executing pipeline op is the failing clause, like the numeric
  // contract faults' convention.
  const auto kindImmutabilityFault = [](CellRef executing) {
    return ExecutionResult{
        .success           = false,
        .contractViolation = ContractViolationKind::KindImmutability,
        .failingClause     = executing,
        .errorMessage      = "P15: pipeline write to a registered "
                             "opcode cell refused (kind immutability)"};
  };
  auto pipeOp = core_.getPipelineHead(paramCell);
  std::unordered_set<CellRef> preflightSeen;
  std::size_t preflightLimit = maxParameterPipelineInstructions;
  auto preflightOp           = pipeOp;
  while (preflightOp && preflightLimit-- > 0) {
    core_.arena().consumeWork();
    if (!preflightSeen.insert(*preflightOp).second)
      throw std::invalid_argument("cyclic parameter pipeline");
    const auto kind = getOpcodeKind(*preflightOp).value_or(OpcodeKind::Nop);
    if (kind == OpcodeKind::InvokeFrame || kind == OpcodeKind::ReturnFrame ||
        kind == OpcodeKind::CreateClosure || kind == OpcodeKind::ApplyClosure ||
        kind == OpcodeKind::RestoreBindings)
      throw std::invalid_argument(
          "frame and closure opcodes cannot run in parameter pipelines");
    const auto pipelineInputs = core_.inputsOf(*preflightOp);
    const bool mutatingPrimitive =
        ((kind == OpcodeKind::Link || kind == OpcodeKind::Value) &&
         pipelineInputs.size() >= 4) ||
        (kind == OpcodeKind::New && pipelineInputs.size() >= 3) ||
        (kind == OpcodeKind::Break && pipelineInputs.size() >= 3) ||
        (kind == OpcodeKind::Splice && pipelineInputs.size() >= 4) ||
        (kind == OpcodeKind::Clone && !pipelineInputs.empty());
    if (mutatingPrimitive && activeCursor_) {
      const auto active  = guardStacks_.find(*activeCursor_);
      const bool guarded = active != guardStacks_.end() && active->second &&
                           !active->second->empty();
      if (guarded && hasPendingGuardValidation(*activeCursor_))
        throw std::invalid_argument(
            "source mutation refused during pending guard validation");
      if (guarded && !mutationGroupActive(*activeCursor_))
        throw std::invalid_argument(
            "guarded source mutation requires an atomic mutation group");
    }
    if (kind == OpcodeKind::Halt || kind == OpcodeKind::Return) {
      preflightOp.reset();
      break;
    }
    if (kind == OpcodeKind::PushGuard || kind == OpcodeKind::PopGuard ||
        kind == OpcodeKind::CheckGuards || kind == OpcodeKind::OpenGuardScope ||
        kind == OpcodeKind::CloseGuardScope ||
        kind == OpcodeKind::BeginMutationGroup ||
        kind == OpcodeKind::EndMutationGroup)
      throw std::invalid_argument(
          "guard and mutation-scope opcodes cannot run in parameter pipelines");
    preflightOp =
        core_.arena().linked(*preflightOp, core_.dims().spin, DimVector::POS);
  }
  if (preflightOp)
    throw std::length_error("parameter pipeline length limit exceeded");
  std::size_t limit = maxParameterPipelineInstructions;
  while (pipeOp && limit-- > 0) {
    auto kindOpt    = getOpcodeKind(*pipeOp);
    OpcodeKind kind = kindOpt ? *kindOpt : OpcodeKind::Nop;
    if (kind == OpcodeKind::Halt || kind == OpcodeKind::Return) {
      break;
    }
    if (kind == OpcodeKind::MatchContract ||
        kind == OpcodeKind::RequireContract) {
      throw std::invalid_argument(
          "contract opcodes cannot run in parameter pipelines");
    }
    if (kind == OpcodeKind::ResumeInit || kind == OpcodeKind::ResumeSet) {
      throw std::invalid_argument(
          "resume-state opcodes cannot run in parameter pipelines");
    }
    if (kind == OpcodeKind::ScopedChoice) {
      throw std::invalid_argument(
          "ScopedChoice cannot run in a parameter pipeline");
    }
    if (kind == OpcodeKind::ProbeLink || kind == OpcodeKind::VqlFieldProbe ||
        kind == OpcodeKind::VqlMaskLinks) {
      throw std::invalid_argument("ProbeLink/VqlFieldProbe/VqlMaskLinks cannot "
                                  "run in a parameter pipeline");
    }
    if (kind == OpcodeKind::VqlString || kind == OpcodeKind::VqlAdd ||
        kind == OpcodeKind::VqlBoxScalar ||
        kind == OpcodeKind::CurrentTimestamp)
      throw std::invalid_argument(
          "VQL scalar opcodes cannot run in a parameter pipeline");
    if (kind == OpcodeKind::InvokeFrame || kind == OpcodeKind::ReturnFrame ||
        kind == OpcodeKind::CreateClosure || kind == OpcodeKind::ApplyClosure ||
        kind == OpcodeKind::RestoreBindings)
      throw std::invalid_argument(
          "frame and closure opcodes cannot run in parameter pipelines");
    if (kind == OpcodeKind::PushGuard || kind == OpcodeKind::PopGuard ||
        kind == OpcodeKind::CheckGuards)
      throw std::invalid_argument(
          "guard opcodes cannot run in parameter pipelines");
    if (streamStateOpcode(kind)) {
      throw std::invalid_argument(
          "stream and iterator opcodes cannot run in parameter pipelines");
    }
    if ((kind == OpcodeKind::CopyCellRefs ||
         kind == OpcodeKind::ClearCellRefs) &&
        std::ranges::any_of(inputsOf(*pipeOp), [&](const CellRef input) {
          return streamSlots_.contains(input) || iteratorSlots_.contains(input);
        })) {
      throw std::invalid_argument(
          "stream and iterator registers cannot run in parameter pipelines");
    }

    std::vector<CellRef> inCells = inputsOf(*pipeOp);
    std::vector<CellValue> inVals;
    if (kind == OpcodeKind::VqlCompare || kind == OpcodeKind::VqlString ||
        kind == OpcodeKind::VqlAdd || kind == OpcodeKind::VqlBoxScalar ||
        kind == OpcodeKind::CurrentTimestamp) {
      // These VQL scalar leaves resolve raw VM slots themselves.
    } else if (inCells.empty()) {
      inVals.push_back(inputValue(paramCell));
    } else {
      for (CellRef c : inCells) {
        inVals.push_back(inputValue(c));
      }
    }

    std::vector<CellValue> outVals;
    bool jumped = false;
    const auto executed =
        executeOpcodeBody(kind, *pipeOp, inVals, outVals, std::nullopt, jumped);
    // The P15 refusal -- a Value/Splice body inside the chain aiming at a
    // registered op cell -- surfaces per site as a typed fault.
    if (executed.contractViolation == ContractViolationKind::KindImmutability) {
      return executed;
    }
    if (!executed.success) throw std::runtime_error(executed.errorMessage);

    if (activeCursor_) {
      const auto pipelineInputs = core_.inputsOf(*pipeOp);
      const bool mutatingPrimitive =
          ((kind == OpcodeKind::Link || kind == OpcodeKind::Value) &&
           pipelineInputs.size() >= 4) ||
          (kind == OpcodeKind::New && pipelineInputs.size() >= 3) ||
          (kind == OpcodeKind::Break && pipelineInputs.size() >= 3) ||
          (kind == OpcodeKind::Splice && pipelineInputs.size() >= 4) ||
          (kind == OpcodeKind::Clone && !pipelineInputs.empty());
      const auto subject = mutatingPrimitive
                               ? resolveCell(pipelineInputs, {},
                                             cellReferences_, 0, core_.arena())
                               : std::nullopt;
      const auto active  = guardStacks_.find(*activeCursor_);
      if (subject && active != guardStacks_.end() && active->second &&
          !active->second->empty()) {
        auto nextGroups = mutationGroups_;
        auto groups     = std::make_shared<std::vector<MutationGroup>>(
            *nextGroups.at(*activeCursor_));
        for (auto &group : *groups) {
          std::unordered_set<CellRef> known;
          for (const auto &participant : group.participants)
            if (participant) known.insert(participant->token);
          for (const auto &guard : *active->second) {
            core_.arena().consumeWork();
            if (!guard || !known.insert(guard->token).second) continue;
            if (group.participants.size() >= guardLimits_.maxGroupParticipants)
              throw std::length_error(
                  "mutation-group participant limit exceeded");
            group.participants.push_back(guard);
          }
        }
        nextGroups[*activeCursor_] = std::move(groups);
        mutationGroups_.swap(nextGroups);
      }
    }

    std::vector<CellRef> outCells = outputsOf(*pipeOp);
    if ((kind == OpcodeKind::Link || kind == OpcodeKind::Break ||
         kind == OpcodeKind::Value || kind == OpcodeKind::CopyCellRefs ||
         kind == OpcodeKind::ClearCellRefs) &&
        !outCells.empty()) {
      writeCellReferences(outCells, outVals);
    } else if (outCells.empty()) {
      if (!outVals.empty()) {
        WriteFault fault = WriteFault::None;
        std::ignore =
            core_.writeValue(paramCell, 0, std::nullopt, outVals[0], &fault);
        if (fault == WriteFault::OpcodeKind) {
          return kindImmutabilityFault(*pipeOp);
        }
      }
    } else {
      if (kind != OpcodeKind::New && kind != OpcodeKind::Clone &&
          kind != OpcodeKind::Linked) {
        for (std::size_t i = 0; i < std::min(outCells.size(), outVals.size());
             ++i) {
          cellReferences_.erase(outCells[i]);
          WriteFault fault = WriteFault::None;
          std::ignore      = core_.writeValue(outCells[i], 0, std::nullopt,
                                              outVals[i], &fault);
          if (fault == WriteFault::OpcodeKind) {
            return kindImmutabilityFault(*pipeOp);
          }
        }
      }
    }

    pipeOp = core_.arena().linked(*pipeOp, core_.dims().spin, false);
  }
  return ExecutionResult{.success           = true,
                         .contractViolation = ContractViolationKind::None,
                         .failingClause     = std::nullopt,
                         .errorMessage      = ""};
}

bool VortexVM::evaluateCondition(CellRef clause,
                                 const std::vector<CellValue> &oldInputs,
                                 const std::vector<CellValue> &outputs) {
  const CellRef actualClause =
      core_.arena().cloneMaster(clause, core_.dims().clone).value_or(clause);
  auto kindOpt = getOpcodeKind(actualClause);
  if (!kindOpt) {
    return core_.isTruthy(actualClause).value_or(false);
  }

  OpcodeKind kind = *kindOpt;
  if (hasActiveGuards() &&
      sourceMutatingPrimitive(kind, inputsOf(actualClause).size()))
    return false;
  if (kind == OpcodeKind::MatchContract ||
      kind == OpcodeKind::RequireContract) {
    return false;
  }
  if (kind == OpcodeKind::ResumeInit || kind == OpcodeKind::ResumeSet) {
    return false;
  }
  if (kind == OpcodeKind::ScopedChoice)
    throw std::invalid_argument("ScopedChoice cannot run as a condition");
  if (kind == OpcodeKind::ProbeLink || kind == OpcodeKind::VqlFieldProbe ||
      kind == OpcodeKind::VqlMaskLinks)
    throw std::invalid_argument(
        "ProbeLink/VqlFieldProbe/VqlMaskLinks cannot run as conditions");
  if (kind == OpcodeKind::VqlString || kind == OpcodeKind::VqlAdd ||
      kind == OpcodeKind::VqlBoxScalar || kind == OpcodeKind::CurrentTimestamp)
    throw std::invalid_argument("scalar opcodes cannot run as conditions");
  if (kind == OpcodeKind::InvokeFrame || kind == OpcodeKind::ReturnFrame)
    throw std::invalid_argument(
        "activation-frame opcodes cannot run as conditions");
  if (kind == OpcodeKind::CreateClosure || kind == OpcodeKind::ApplyClosure ||
      kind == OpcodeKind::RestoreBindings)
    throw std::invalid_argument("closure opcodes cannot run as conditions");
  if (kind == OpcodeKind::PushGuard || kind == OpcodeKind::PopGuard ||
      kind == OpcodeKind::CheckGuards || kind == OpcodeKind::OpenGuardScope ||
      kind == OpcodeKind::CloseGuardScope ||
      kind == OpcodeKind::BeginMutationGroup ||
      kind == OpcodeKind::EndMutationGroup)
    throw std::invalid_argument("guard opcodes cannot run as conditions");
  std::vector<CellRef> inCells = inputsOf(actualClause);
  std::vector<CellValue> inVals;
  if (kind == OpcodeKind::VqlCompare || kind == OpcodeKind::VqlString ||
      kind == OpcodeKind::VqlAdd || kind == OpcodeKind::VqlBoxScalar ||
      kind == OpcodeKind::CurrentTimestamp) {
    // Their handlers resolve raw slots and must observe missing references.
  } else if (!inCells.empty()) {
    for (CellRef c : inCells) {
      inVals.push_back(inputValue(c));
    }
  } else {
    if (!outputs.empty()) {
      inVals.push_back(outputs[0]);
    }
    if (!oldInputs.empty()) {
      inVals.push_back(oldInputs[0]);
    }
  }

  std::vector<CellValue> outVals;
  bool jumped       = false;
  const auto result = executeOpcodeBody(kind, actualClause, inVals, outVals,
                                        std::nullopt, jumped);
  if (!result.success) return false;
  if (!outVals.empty()) {
    return VortexCore::evaluateTruthiness(outVals[0]);
  }
  return core_.isTruthy(actualClause).value_or(false);
}

ExecutionResult VortexVM::executeGuardOpcode(const OpcodeKind kind,
                                             const CellRef opcode,
                                             const CellRef cursor) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  auto &arena              = core_.arena();
  const auto advanceCursor = [this, &arena,
                              cursor](const std::optional<CellRef> next) {
    auto nextCursorPC    = cursorPC_;
    nextCursorPC[cursor] = next;
    zigzag::ArenaSavepoint savepoint(arena);
    if (arena.contains(cursor)) {
      if (next) {
        zigzag::expectWritten(
            arena.setValueBits(cursor, xanadu::ValueKind::Int64,
                               static_cast<std::uint64_t>(*next)));
      } else {
        zigzag::expectWritten(
            arena.setValueBits(cursor, xanadu::ValueKind::None, 0));
      }
    }
    cursorPC_.swap(nextCursorPC);
    savepoint.commit();
  };
  const auto inputs  = core_.inputsOf(opcode);
  const auto outputs = core_.outputsOf(opcode);
  if (kind == OpcodeKind::PushGuard) {
    if (hasPendingGuardValidation(cursor))
      return fail("PushGuard refused during pending guard validation");
    if (inputs.size() < 2 || outputs.size() != 1)
      return fail("PushGuard requires descriptor, diagnostic, captures, and "
                  "one token output");
    const CellRef descriptorCell = runtimeSlot(inputs[0]);
    if (cellReferences_.contains(inputs[0]) || !arena.contains(descriptorCell))
      return fail("PushGuard descriptor must be a direct graph reference");
    const auto descriptor = frameDescriptor(descriptorCell);
    if (!descriptor) return fail(descriptor.error().message);
    if (descriptor->resultPorts.size() != 1 ||
        descriptor->resultPorts.front().kind != FramePortKind::Cell)
      return fail("PushGuard validator must have exactly one Cell result");
    if (descriptor->argumentPorts.size() != inputs.size() - 2)
      return fail(
          "PushGuard capture arity does not match its frame descriptor");
    if (isMemoized(opcode) || !core_.preconditionsOf(opcode).empty() ||
        !core_.postconditionsOf(opcode).empty())
      return fail("guard opcodes do not support conditions or memoization");
    for (const auto slot : inputs)
      if (core_.hasPipeline(slot))
        return fail("guard opcodes do not support parameter pipelines");
    for (const auto slot : outputs)
      if (core_.hasPipeline(slot))
        return fail("guard opcodes do not support parameter pipelines");
    if (guardLimits_.maxGuards == 0 || guardLimits_.maxDiagnosticBytes == 0)
      return fail("guard budget limit is zero");
    const auto current = guardStacks_.find(cursor);
    const RuntimeGuardStack currentStack =
        current == guardStacks_.end()
            ? std::make_shared<const std::vector<RuntimeGuardRef>>()
            : current->second;
    if (!currentStack || currentStack->size() >= guardLimits_.maxGuards)
      return fail("guard count budget limit exceeded");
    const CellRef output = runtimeSlot(outputs.front());
    if (!arena.contains(output) || getOpcodeKind(output) ||
        streamSlots_.contains(output) || iteratorSlots_.contains(output) ||
        std::ranges::any_of(inputs, [this, output](const CellRef slot) {
          return runtimeSlot(slot) == output;
        }))
      return fail(
          "PushGuard token output must be a distinct scalar caller slot");
    const auto continuation = arena.linked(opcode, core_.dims().spin, false);
    if (!continuation)
      return fail(
          "PushGuard requires a continuation so its token can be popped");
    const auto diagnosticCell = runtimeSlot(inputs[1]);
    if (!arena.contains(diagnosticCell) || getOpcodeKind(diagnosticCell))
      return fail("PushGuard diagnostic must be a direct text cell");
    const auto diagnostic =
        boundedText(arena, diagnosticCell, guardLimits_.maxDiagnosticBytes);
    if (!diagnostic || diagnostic->empty())
      return fail("PushGuard diagnostic exceeds its text budget or is empty");

    auto guards = std::make_shared<std::vector<RuntimeGuardRef>>(*currentStack);
    std::size_t captured = guardCapturedCells(currentStack, arena);
    if (const auto pending = pendingGuardChecks_.find(cursor);
        pending != pendingGuardChecks_.end() && pending->second &&
        pending->second->guards) {
      const auto pendingCells =
          guardCapturedCells(pending->second->guards, arena);
      if (pendingCells > guardLimits_.maxCapturedCells -
                             std::min(captured, guardLimits_.maxCapturedCells))
        return fail("guard captured-cell budget limit exceeded");
      captured += pendingCells;
    }
    if (captured > guardLimits_.maxCapturedCells)
      return fail("guard captured-cell budget limit exceeded");
    std::vector<std::vector<CellRef>> arguments;
    arguments.reserve(descriptor->argumentPorts.size());
    for (std::size_t index = 0; index < descriptor->argumentPorts.size();
         ++index) {
      const auto argumentSlot = runtimeSlot(inputs[index + 2]);
      if (iteratorSlots_.contains(argumentSlot))
        return fail("PushGuard iterator captures are unsupported");
      std::vector<CellRef> cells;
      forEachResultCell(argumentSlot, [&](const CellRef cell) {
        arena.consumeWork();
        const auto remaining = guardLimits_.maxCapturedCells - captured;
        if (cells.size() >= remaining)
          throw std::length_error("guard captured-cell budget limit exceeded");
        if (descriptor->argumentPorts[index].kind == FramePortKind::Cell &&
            !cells.empty())
          throw std::runtime_error(
              "PushGuard Cell capture resolved to multiple cells");
        cells.push_back(cell);
      });
      if (descriptor->argumentPorts[index].kind == FramePortKind::Cell &&
          cells.size() > 1)
        return fail("PushGuard Cell capture resolved to multiple cells");
      if (cells.size() > guardLimits_.maxCapturedCells -
                             std::min(captured, guardLimits_.maxCapturedCells))
        return fail("guard captured-cell budget limit exceeded");
      captured += cells.size();
      arguments.push_back(std::move(cells));
    }
    zigzag::ArenaSavepoint savepoint(arena);
    try {
      const CellRef token   = arena.makeCell("VORTEX_GUARD/1");
      const auto scopeStack = guardScopes_.find(cursor);
      std::optional<CellRef> ownerScope;
      if (scopeStack != guardScopes_.end() && scopeStack->second &&
          !scopeStack->second->empty() &&
          scopeStack->second->back().ownerActivation ==
              currentActivationId(cursor))
        ownerScope = scopeStack->second->back().token;
      guards->push_back(std::make_shared<const RuntimeGuard>(
          token, descriptorCell, *diagnostic, std::move(arguments),
          currentActivationId(cursor), ownerScope));
      auto nextGuardStacks    = guardStacks_;
      nextGuardStacks[cursor] = std::move(guards);
      auto nextReferences     = cellReferences_;
      nextReferences[output]  = token;
      advanceCursor(continuation);
      guardStacks_.swap(nextGuardStacks);
      cellReferences_.swap(nextReferences);
      savepoint.commit();
      return {};
    } catch (const std::exception &error) {
      return fail(error.what());
    }
  }

  if (kind == OpcodeKind::PopGuard) {
    if (inputs.size() != 1 || !outputs.empty())
      return fail("PopGuard requires one token and no outputs");
    if (isMemoized(opcode) || !core_.preconditionsOf(opcode).empty() ||
        !core_.postconditionsOf(opcode).empty())
      return fail("guard opcodes do not support conditions or memoization");
    for (const auto slot : inputs)
      if (core_.hasPipeline(slot))
        return fail("guard opcodes do not support parameter pipelines");
    const auto found = guardStacks_.find(cursor);
    if (found == guardStacks_.end() || !found->second || found->second->empty())
      return fail("PopGuard has no active guard");
    const auto token = resultCell(cursor, runtimeSlot(inputs.front()));
    const auto &top  = found->second->back();
    if (!token || *token != top->token ||
        top->ownerActivation != currentActivationId(cursor))
      return fail("PopGuard token is not the current activation's top guard");
    if (const auto groups = mutationGroups_.find(cursor);
        groups != mutationGroups_.end() && groups->second &&
        std::ranges::any_of(*groups->second, [&](const MutationGroup &group) {
          return std::ranges::any_of(
              group.participants, [&](const RuntimeGuardRef &participant) {
                return participant && participant->token == top->token;
              });
        }))
      return fail("PopGuard cannot remove a dirty guard before validation");
    auto nextGuards =
        std::make_shared<std::vector<RuntimeGuardRef>>(*found->second);
    nextGuards->pop_back();
    const auto continuation = arena.linked(opcode, core_.dims().spin, false);
    auto nextStacks         = guardStacks_;
    if (nextGuards->empty())
      nextStacks.erase(cursor);
    else
      nextStacks[cursor] = std::move(nextGuards);
    try {
      advanceCursor(continuation);
    } catch (const std::exception &error) {
      return fail(error.what());
    }
    guardStacks_.swap(nextStacks);
    return {};
  }

  if (!inputs.empty() || !outputs.empty())
    return fail("CheckGuards takes no inputs or outputs");
  if (isMemoized(opcode) || !core_.preconditionsOf(opcode).empty() ||
      !core_.postconditionsOf(opcode).empty())
    return fail("guard opcodes do not support conditions or memoization");
  if (const auto pending = pendingGuardChecks_.find(cursor);
      pending != pendingGuardChecks_.end() &&
      (!pending->second || pending->second->checkOpcode != opcode ||
       pending->second->ownerActivation != currentActivationId(cursor)))
    return fail("recursive CheckGuards is not supported");
  const auto stackIt = guardStacks_.find(cursor);
  const RuntimeGuardStack guards =
      stackIt == guardStacks_.end() || !stackIt->second
          ? std::make_shared<const std::vector<RuntimeGuardRef>>()
          : stackIt->second;
  const auto driven = driveGuardValidation(cursor, opcode, guards);
  if (!driven.execution.success || driven.state == GuardDriveState::Suspended)
    return driven.execution;
  if (guards && !guards->empty() &&
      !arena.linked(opcode, core_.dims().spin, false))
    return fail("cursor finished with an unpopped guard");
  advanceCursor(arena.linked(opcode, core_.dims().spin, false));
  return {};
}

ExecutionResult VortexVM::executeMutationScopeOpcode(const OpcodeKind kind,
                                                     const CellRef opcode,
                                                     const CellRef cursor) {
  const auto fail = [opcode](std::string message) {
    return ExecutionResult{.success       = false,
                           .failingClause = opcode,
                           .errorMessage  = std::move(message)};
  };
  auto &arena             = core_.arena();
  const auto inputs       = inputsOf(opcode);
  const auto outputs      = outputsOf(opcode);
  const auto continuation = arena.linked(opcode, core_.dims().spin, false);
  if (isMemoized(opcode) || !core_.preconditionsOf(opcode).empty() ||
      !core_.postconditionsOf(opcode).empty())
    return fail(
        "mutation-scope opcodes do not support conditions or memoization");
  for (const auto slot : inputs)
    if (core_.hasPipeline(slot))
      return fail("mutation-scope opcodes do not support parameter pipelines");
  for (const auto slot : outputs)
    if (core_.hasPipeline(slot))
      return fail("mutation-scope opcodes do not support parameter pipelines");

  const auto advance = [&] {
    auto nextPC    = cursorPC_;
    nextPC[cursor] = continuation;
    zigzag::ArenaSavepoint savepoint(arena);
    if (arena.contains(cursor)) {
      if (continuation)
        zigzag::expectWritten(
            arena.setValueBits(cursor, xanadu::ValueKind::Int64,
                               static_cast<std::uint64_t>(*continuation)));
      else
        zigzag::expectWritten(
            arena.setValueBits(cursor, xanadu::ValueKind::None, 0));
    }
    cursorPC_.swap(nextPC);
    savepoint.commit();
  };
  const auto validOutput = [&](const CellRef output) {
    return arena.contains(output) && !getOpcodeKind(output) &&
           !streamSlots_.contains(output) && !iteratorSlots_.contains(output);
  };

  if (kind == OpcodeKind::OpenGuardScope) {
    if (!inputs.empty() || outputs.size() != 1 || !continuation)
      return fail("OpenGuardScope requires no inputs, one token output, and a "
                  "continuation");
    const CellRef output = runtimeSlot(outputs.front());
    if (!validOutput(output))
      return fail("OpenGuardScope token output must be a distinct scalar slot");
    const auto scopes = guardScopes_.find(cursor);
    if (guardLimits_.maxGuardScopes == 0 ||
        (scopes != guardScopes_.end() && scopes->second &&
         scopes->second->size() >= guardLimits_.maxGuardScopes))
      return fail("guard-scope depth limit exceeded");
    const auto guards          = guardStacks_.find(cursor);
    const std::size_t baseline = guards == guardStacks_.end() || !guards->second
                                     ? 0
                                     : guards->second->size();
    zigzag::ArenaSavepoint savepoint(arena);
    try {
      const CellRef token = arena.makeCell("VORTEX_GUARD_SCOPE/1");
      auto nextScopes     = guardScopes_;
      auto scopes         = std::make_shared<std::vector<GuardScope>>();
      if (const auto current = guardScopes_.find(cursor);
          current != guardScopes_.end() && current->second)
        *scopes = *current->second;
      scopes->emplace_back(token, baseline, currentActivationId(cursor));
      nextScopes[cursor] = std::move(scopes);
      auto nextRefs      = cellReferences_;
      nextRefs[output]   = token;
      advance();
      guardScopes_.swap(nextScopes);
      cellReferences_.swap(nextRefs);
      savepoint.commit();
      return {};
    } catch (const std::exception &error) {
      return fail(error.what());
    }
  }

  if (kind == OpcodeKind::CloseGuardScope) {
    if (inputs.size() != 1 || !outputs.empty())
      return fail("CloseGuardScope requires one scope token and no outputs");
    const auto scopesIt = guardScopes_.find(cursor);
    if (scopesIt == guardScopes_.end() || !scopesIt->second ||
        scopesIt->second->empty())
      return fail("CloseGuardScope has no active scope");
    const auto scope = scopesIt->second->back();
    const auto token = resultCell(cursor, runtimeSlot(inputs.front()));
    if (!token || *token != scope.token ||
        scope.ownerActivation != currentActivationId(cursor))
      return fail(
          "CloseGuardScope token is not the current activation's top scope");
    if (const auto groups = mutationGroups_.find(cursor);
        groups != mutationGroups_.end() && groups->second &&
        std::ranges::any_of(*groups->second, [&](const MutationGroup &group) {
          return group.ownerScope == scope.token;
        }))
      return fail(
          "CloseGuardScope cannot leave a group owned by that scope open");
    if (hasPendingGuardValidation(cursor) &&
        pendingGuardChecks_.at(cursor)->checkOpcode != opcode)
      return fail(
          "CloseGuardScope cannot consume another validation continuation");
    std::vector<RuntimeGuardRef> participants;
    if (const auto groups = mutationGroups_.find(cursor);
        groups != mutationGroups_.end() && groups->second) {
      std::unordered_set<CellRef> seen;
      for (const auto &group : *groups->second)
        for (const auto &guard : group.participants)
          if (guard && guard->ownerScope == scope.token &&
              seen.insert(guard->token).second)
            participants.push_back(guard);
    }
    if (!participants.empty()) {
      auto snapshot =
          std::make_shared<const std::vector<RuntimeGuardRef>>(participants);
      const auto driven = driveGuardValidation(cursor, opcode, snapshot);
      if (!driven.execution.success ||
          driven.state == GuardDriveState::Suspended)
        return driven.execution;
    }
    const auto guards     = guardStacks_.find(cursor);
    const auto guardCount = guards == guardStacks_.end() || !guards->second
                                ? 0
                                : guards->second->size();
    if (guardCount < scope.guardBaseline)
      return fail("CloseGuardScope lost its guard baseline");
    if (guardCount > scope.guardBaseline) {
      for (std::size_t index = scope.guardBaseline; index < guardCount;
           ++index) {
        const auto &guard = guards->second->at(index);
        if (!guard || guard->ownerScope != scope.token ||
            guard->ownerActivation != scope.ownerActivation)
          return fail(
              "CloseGuardScope cannot trim a guard owned by another scope");
      }
    }
    auto nextGuards = guardStacks_;
    if (scope.guardBaseline == 0)
      nextGuards.erase(cursor);
    else {
      auto prefix = std::make_shared<std::vector<RuntimeGuardRef>>(
          guards->second->begin(),
          guards->second->begin() +
              static_cast<std::ptrdiff_t>(scope.guardBaseline));
      nextGuards[cursor] = std::move(prefix);
    }
    auto nextScopes = guardScopes_;
    auto remaining  = std::make_shared<std::vector<GuardScope>>(
        scopesIt->second->begin(), scopesIt->second->end() - 1);
    if (remaining->empty())
      nextScopes.erase(cursor);
    else
      nextScopes[cursor] = std::move(remaining);
    try {
      advance();
    } catch (const std::exception &error) {
      return fail(error.what());
    }
    guardStacks_.swap(nextGuards);
    guardScopes_.swap(nextScopes);
    return {};
  }

  if (kind == OpcodeKind::BeginMutationGroup) {
    if (!inputs.empty() || outputs.size() != 1 || !continuation)
      return fail("BeginMutationGroup requires no inputs, one token output, "
                  "and a continuation");
    if (hasPendingGuardValidation(cursor))
      return fail("BeginMutationGroup refused during pending guard validation");
    if (guardLimits_.maxMutationGroupCycles == 0)
      return fail("mutation-group cycle limit is zero");
    const auto groups = mutationGroups_.find(cursor);
    const auto depth  = groups == mutationGroups_.end() || !groups->second
                            ? 0
                            : groups->second->size();
    if (depth >= guardLimits_.maxMutationGroupDepth)
      return fail("mutation-group depth limit exceeded");
    const CellRef output = runtimeSlot(outputs.front());
    if (!validOutput(output))
      return fail(
          "BeginMutationGroup token output must be a distinct scalar slot");
    if (nextMutationGroupId_ == 0)
      return fail("mutation-group identity space exhausted");
    std::size_t checkpointIndex = choiceStack_.size();
    std::optional<std::uint64_t> checkpointId;
    bool checkpointCreated = false;
    std::optional<CellRef> ownerScope;
    if (const auto scopes = guardScopes_.find(cursor);
        scopes != guardScopes_.end() && scopes->second &&
        !scopes->second->empty() &&
        scopes->second->back().ownerActivation == currentActivationId(cursor))
      ownerScope = scopes->second->back().token;
    try {
      enforceScopedChoiceBudget(true);
      pushChoicePointImpl(cursor, *continuation, false);
      checkpointIndex                          = choiceStack_.size() - 1;
      checkpointId                             = choiceStack_.back().choiceId;
      checkpointCreated                        = true;
      const auto groupId                       = nextMutationGroupId_++;
      choiceStack_.back().mutationCheckpointId = groupId;
      const CellRef token = arena.makeCell("VORTEX_MUTATION_GROUP/1");
      auto nextGroups     = mutationGroups_;
      auto stack          = std::make_shared<std::vector<MutationGroup>>();
      if (const auto current = mutationGroups_.find(cursor);
          current != mutationGroups_.end() && current->second)
        *stack = *current->second;
      stack->emplace_back(groupId, *checkpointId, token, checkpointIndex,
                          currentActivationId(cursor), ownerScope,
                          totalCycles_);
      nextGroups[cursor] = std::move(stack);
      auto nextRefs      = cellReferences_;
      nextRefs[output]   = token;
      advance();
      mutationGroups_.swap(nextGroups);
      cellReferences_.swap(nextRefs);
      return {};
    } catch (const std::exception &error) {
      if (checkpointCreated && choiceStack_.size() == checkpointIndex + 1 &&
          checkpointId && choiceStack_.back().choiceId == *checkpointId) {
        core_.arena().release(choiceStack_.back().mark);
        choiceStack_.pop_back();
      }
      return fail(error.what());
    }
  }

  if (inputs.size() != 1 || !outputs.empty())
    return fail("EndMutationGroup requires one token and no outputs");
  const auto groupsIt = mutationGroups_.find(cursor);
  if (groupsIt == mutationGroups_.end() || !groupsIt->second ||
      groupsIt->second->empty())
    return fail("EndMutationGroup has no active group");
  const auto group      = groupsIt->second->back();
  const auto groupToken = resultCell(cursor, runtimeSlot(inputs.front()));
  if (!groupToken || *groupToken != group.token ||
      group.ownerActivation != currentActivationId(cursor))
    return fail(
        "EndMutationGroup token is not the current activation's top group");
  if (hasPendingGuardValidation(cursor) &&
      pendingGuardChecks_.at(cursor)->checkOpcode != opcode)
    return fail(
        "EndMutationGroup cannot consume another validation continuation");
  if (groupsIt->second->size() == 1 && !group.participants.empty()) {
    const auto snapshot = std::make_shared<const std::vector<RuntimeGuardRef>>(
        group.participants);
    const auto driven = driveGuardValidation(cursor, opcode, snapshot);
    if (!driven.execution.success || driven.state == GuardDriveState::Suspended)
      return driven.execution;
  }
  const auto currentGroups = mutationGroups_.find(cursor);
  if (currentGroups == mutationGroups_.end() || !currentGroups->second ||
      currentGroups->second->empty() ||
      currentGroups->second->back().id != group.id)
    return fail("EndMutationGroup lost its live group state");
  if (group.checkpointIndex >= choiceStack_.size() ||
      choiceStack_[group.checkpointIndex].choiceId != group.choiceId ||
      choiceStack_[group.checkpointIndex].mutationCheckpointId != group.id)
    return fail("EndMutationGroup lost its exact rollback checkpoint");
  if (std::ranges::any_of(choiceStack_.begin() + static_cast<std::ptrdiff_t>(
                                                     group.checkpointIndex + 1),
                          choiceStack_.end(), [](const ChoicePoint &choice) {
                            return choice.scoped;
                          }))
    return fail("EndMutationGroup cannot close across an active scoped choice");
  auto nextGroups = mutationGroups_;
  auto nextStack =
      std::make_shared<std::vector<MutationGroup>>(*currentGroups->second);
  nextStack->pop_back();
  if (!nextStack->empty()) {
    auto &parent = nextStack->back();
    std::unordered_set<CellRef> known;
    for (const auto &participant : parent.participants)
      if (participant) known.insert(participant->token);
    for (const auto &participant : group.participants) {
      arena.consumeWork();
      if (!participant || !known.insert(participant->token).second) continue;
      if (parent.participants.size() >= guardLimits_.maxGroupParticipants)
        return fail("mutation-group participant limit exceeded");
      parent.participants.push_back(participant);
    }
  }
  if (nextStack->empty())
    nextGroups.erase(cursor);
  else
    nextGroups[cursor] = std::move(nextStack);
  try {
    advance();
  } catch (const std::exception &error) {
    return fail(error.what());
  }
  while (choiceStack_.size() > group.checkpointIndex + 1) {
    core_.arena().discard(choiceStack_.back().mark);
    choiceStack_.pop_back();
  }
  ChoicePoint checkpoint = std::move(choiceStack_.back());
  choiceStack_.pop_back();
  core_.arena().discard(checkpoint.mark);
  mutationGroups_.swap(nextGroups);
  return {};
}

GuardDriveResult VortexVM::driveGuardValidation(const CellRef cursor,
                                                const CellRef resumeOpcode,
                                                RuntimeGuardStack guards) {
  const auto fail = [resumeOpcode](std::string message,
                                   const bool violation = false) {
    return GuardDriveResult{
        .execution =
            ExecutionResult{.success = false,
                            .contractViolation =
                                violation ? ContractViolationKind::Postcondition
                                          : ContractViolationKind::None,
                            .failingClause = resumeOpcode,
                            .errorMessage  = std::move(message)},
        .state = GuardDriveState::Complete};
  };
  auto &arena           = core_.arena();
  const auto owner      = currentActivationId(cursor);
  const auto pending    = pendingGuardChecks_.find(cursor);
  std::size_t nextIndex = 0;
  if (pending != pendingGuardChecks_.end()) {
    const auto prior = pending->second;
    if (!prior || prior->checkOpcode != resumeOpcode ||
        prior->ownerActivation != owner)
      return fail("recursive guard validation is not supported");
    pendingGuardChecks_.erase(pending);
    guards            = prior->guards;
    nextIndex         = prior->nextIndex;
    const auto actual = resultCell(cursor, prior->resultSlot);
    const auto isBool = actual && arena.contains(*actual) &&
                        arena.valueKindOf(*actual) == xanadu::ValueKind::Bool;
    const auto value = isBool ? arena.asBool(*actual) : std::nullopt;
    cellReferences_.erase(prior->resultSlot);
    if (!value) {
      const auto diagnostic =
          nextIndex > 0 && guards && nextIndex <= guards->size()
              ? guards->at(nextIndex - 1)->diagnostic
              : std::string("validator did not return one Bool Cell");
      return fail("TypeContractError: " + diagnostic, true);
    }
    if (!*value) {
      const auto diagnostic =
          nextIndex > 0 && guards && nextIndex <= guards->size()
              ? guards->at(nextIndex - 1)->diagnostic
              : std::string("guard validator returned false");
      return fail("TypeContractError: " + diagnostic, true);
    }
  }
  if (!guards || nextIndex >= guards->size()) {
    std::span<const RuntimeGuardRef> validated;
    if (guards) validated = *guards;
    removeValidatedGuardObligations(cursor, validated);
    return {.execution = {}, .state = GuardDriveState::Complete};
  }
  if (!arena.linked(resumeOpcode, core_.dims().spin, false))
    return fail("guard validation requires a continuation");
  if (guards->size() > guardLimits_.maxGuards)
    return fail("guard count budget limit exceeded");
  const auto captured = guardCapturedCells(guards, arena);
  if (captured > guardLimits_.maxCapturedCells)
    return fail("guard captured-cell budget limit exceeded");
  zigzag::ArenaSavepoint scratchSavepoint(arena);
  try {
    const CellRef resultSlot    = arena.makeCell();
    const RuntimeGuardRef guard = guards->at(nextIndex);
    std::vector<CellRef> destinations{resultSlot};
    auto stagedPending    = pendingGuardChecks_;
    stagedPending[cursor] = std::make_shared<const PendingGuardCheck>(
        resumeOpcode, resultSlot, guards, nextIndex + 1, owner);
    const auto invoked =
        invokeFramePrepared(guard->descriptor, guard->arguments, destinations,
                            cursor, resumeOpcode, resumeOpcode);
    if (!invoked.success) {
      cellReferences_.erase(resultSlot);
      return {.execution = invoked, .state = GuardDriveState::Complete};
    }
    pendingGuardChecks_.swap(stagedPending);
    scratchSavepoint.commit();
    return {.execution = {}, .state = GuardDriveState::Suspended};
  } catch (const std::exception &error) {
    pendingGuardChecks_.erase(cursor);
    return fail(error.what());
  }
}

ExecutionResult
VortexVM::executeOpcodeBody(OpcodeKind kind, CellRef opcode,
                            const std::vector<CellValue> &inputs,
                            std::vector<CellValue> &outputs,
                            const std::optional<CellRef> cursor, bool &jumped) {
  if (kind == OpcodeKind::InvokeFrame || kind == OpcodeKind::ReturnFrame)
    return {.success      = false,
            .errorMessage = "activation-frame opcodes require a VM cursor"};
  if (kind == OpcodeKind::CreateClosure || kind == OpcodeKind::ApplyClosure ||
      kind == OpcodeKind::RestoreBindings)
    return {.success      = false,
            .errorMessage = "closure opcodes require a VM cursor"};
  if (kind == OpcodeKind::PushGuard || kind == OpcodeKind::PopGuard ||
      kind == OpcodeKind::CheckGuards || kind == OpcodeKind::OpenGuardScope ||
      kind == OpcodeKind::CloseGuardScope ||
      kind == OpcodeKind::BeginMutationGroup ||
      kind == OpcodeKind::EndMutationGroup)
    return {.success = false,
            .errorMessage =
                "guard and mutation-scope opcodes require a VM cursor"};
  const auto overflowFailure = [opcode] {
    return ExecutionResult{.success = false,
                           .contractViolation =
                               ContractViolationKind::Precondition,
                           .failingClause = opcode,
                           .errorMessage = "Signed integer overflow in opcode"};
  };
  // P15's typed refusal (spec §6.5/M10): the funnel reports
  // WriteFault::OpcodeKind when a program-value write aims at an
  // opcodeMap_-registered cell's clone master — surfaced per site as
  // `success=false`, classified like the numeric contract faults.
  const auto kindImmutabilityFault = [opcode] {
    return ExecutionResult{
        .success           = false,
        .contractViolation = ContractViolationKind::KindImmutability,
        .failingClause     = opcode,
        .errorMessage      = "P15: write to a registered opcode cell "
                             "refused (kind immutability)"};
  };
  constexpr auto minInt = std::numeric_limits<std::int64_t>::min();
  switch (kind) {
  case OpcodeKind::ResumeInit:
    return executeResumeInit(opcode, inputs, cursor);
  case OpcodeKind::ResumeSet:
    return executeResumeSet(opcode, inputs, cursor);
  case OpcodeKind::VqlCompare:
    return executeVqlCompare(opcode, outputs);
  case OpcodeKind::VqlString:
    return executeVqlString(opcode, outputs);
  case OpcodeKind::VqlAdd:
    return executeVqlAdd(opcode, outputs);
  case OpcodeKind::VqlBoxScalar:
    return executeVqlBoxScalar(opcode, outputs);
  case OpcodeKind::CurrentTimestamp:
    return executeCurrentTimestamp(opcode, outputs);
  case OpcodeKind::ProbeLink:
    return executeProbeLink(opcode, outputs);
  case OpcodeKind::VqlMaskLinks:
    return executeVqlMaskLinks(opcode, outputs);
  case OpcodeKind::CreateClosure:
  case OpcodeKind::ApplyClosure:
  case OpcodeKind::RestoreBindings:
    return {.success       = false,
            .failingClause = opcode,
            .errorMessage  = "closure opcodes require a VM cursor"};
  case OpcodeKind::VqlFieldProbe:
    return executeVqlFieldProbe(opcode, outputs);
  case OpcodeKind::Add: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.emplace_back(std::get<std::string>(inputs[0]) +
                             std::get<std::string>(inputs[1]));
      } else if (std::holds_alternative<double>(inputs[0]) ||
                 std::holds_alternative<double>(inputs[1])) {
        outputs.emplace_back(toDouble(inputs[0]) + toDouble(inputs[1]));
      } else {
        std::int64_t result;
        if (__builtin_add_overflow(toInt(inputs[0]), toInt(inputs[1]),
                                   &result)) {
          return overflowFailure();
        }
        outputs.emplace_back(result);
      }
    } else if (inputs.size() == 1) {
      outputs.push_back(inputs[0]);
    }
    break;
  }
  case OpcodeKind::Sub: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.emplace_back(toDouble(inputs[0]) - toDouble(inputs[1]));
      } else {
        std::int64_t result;
        if (__builtin_sub_overflow(toInt(inputs[0]), toInt(inputs[1]),
                                   &result)) {
          return overflowFailure();
        }
        outputs.emplace_back(result);
      }
    } else if (inputs.size() == 1) {
      if (std::holds_alternative<double>(inputs[0])) {
        outputs.emplace_back(-std::get<double>(inputs[0]));
      } else {
        const auto value = toInt(inputs[0]);
        if (value == minInt) return overflowFailure();
        outputs.emplace_back(-value);
      }
    }
    break;
  }
  case OpcodeKind::Mul: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.emplace_back(toDouble(inputs[0]) * toDouble(inputs[1]));
      } else {
        std::int64_t result;
        if (__builtin_mul_overflow(toInt(inputs[0]), toInt(inputs[1]),
                                   &result)) {
          return overflowFailure();
        }
        outputs.emplace_back(result);
      }
    }
    break;
  }
  case OpcodeKind::Div: {
    if (inputs.size() >= 2) {
      double b = toDouble(inputs[1]);
      if (b == 0.0) {
        outputs.emplace_back(0.0);
      } else if (std::holds_alternative<double>(inputs[0]) ||
                 std::holds_alternative<double>(inputs[1])) {
        outputs.emplace_back(toDouble(inputs[0]) / b);
      } else {
        std::int64_t ib = toInt(inputs[1]);
        const auto a    = toInt(inputs[0]);
        if (a == minInt && ib == -1) return overflowFailure();
        outputs.emplace_back(ib != 0 ? a / ib : 0);
      }
    }
    break;
  }
  case OpcodeKind::DivMod: {
    if (inputs.size() >= 2) {
      std::int64_t a = toInt(inputs[0]);
      std::int64_t b = toInt(inputs[1]);
      if (a == minInt && b == -1) return overflowFailure();
      if (b != 0) {
        outputs.emplace_back(a / b);
        outputs.emplace_back(a % b);
      } else {
        outputs.emplace_back(static_cast<std::int64_t>(0));
        outputs.emplace_back(static_cast<std::int64_t>(0));
      }
    }
    break;
  }
  case OpcodeKind::Mod: {
    if (inputs.size() >= 2) {
      std::int64_t b = toInt(inputs[1]);
      const auto a   = toInt(inputs[0]);
      if (a == minInt && b == -1) return overflowFailure();
      outputs.emplace_back(b != 0 ? a % b : 0);
    }
    break;
  }
  case OpcodeKind::Neg: {
    if (!inputs.empty()) {
      if (std::holds_alternative<double>(inputs[0])) {
        outputs.emplace_back(-toDouble(inputs[0]));
      } else {
        const auto value = toInt(inputs[0]);
        if (value == minInt) return overflowFailure();
        outputs.emplace_back(-value);
      }
    }
    break;
  }
  case OpcodeKind::Eq: {
    if (inputs.size() >= 2) {
      outputs.emplace_back(inputs[0] == inputs[1]);
    }
    break;
  }
  case OpcodeKind::Neq: {
    if (inputs.size() >= 2) {
      outputs.emplace_back(inputs[0] != inputs[1]);
    }
    break;
  }
  case OpcodeKind::Lt: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.emplace_back(std::get<std::string>(inputs[0]) <
                             std::get<std::string>(inputs[1]));
      } else if (std::holds_alternative<std::int64_t>(inputs[0]) &&
                 std::holds_alternative<std::int64_t>(inputs[1])) {
        outputs.emplace_back(std::get<std::int64_t>(inputs[0]) <
                             std::get<std::int64_t>(inputs[1]));
      } else {
        outputs.emplace_back(toDouble(inputs[0]) < toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Lte: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.emplace_back(std::get<std::string>(inputs[0]) <=
                             std::get<std::string>(inputs[1]));
      } else if (std::holds_alternative<std::int64_t>(inputs[0]) &&
                 std::holds_alternative<std::int64_t>(inputs[1])) {
        outputs.emplace_back(std::get<std::int64_t>(inputs[0]) <=
                             std::get<std::int64_t>(inputs[1]));
      } else {
        outputs.emplace_back(toDouble(inputs[0]) <= toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Gt: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.emplace_back(std::get<std::string>(inputs[0]) >
                             std::get<std::string>(inputs[1]));
      } else if (std::holds_alternative<std::int64_t>(inputs[0]) &&
                 std::holds_alternative<std::int64_t>(inputs[1])) {
        outputs.emplace_back(std::get<std::int64_t>(inputs[0]) >
                             std::get<std::int64_t>(inputs[1]));
      } else {
        outputs.emplace_back(toDouble(inputs[0]) > toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Gte: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.emplace_back(std::get<std::string>(inputs[0]) >=
                             std::get<std::string>(inputs[1]));
      } else if (std::holds_alternative<std::int64_t>(inputs[0]) &&
                 std::holds_alternative<std::int64_t>(inputs[1])) {
        outputs.emplace_back(std::get<std::int64_t>(inputs[0]) >=
                             std::get<std::int64_t>(inputs[1]));
      } else {
        outputs.emplace_back(toDouble(inputs[0]) >= toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::And: {
    if (inputs.size() >= 2) {
      outputs.emplace_back(VortexCore::evaluateTruthiness(inputs[0]) &&
                           VortexCore::evaluateTruthiness(inputs[1]));
    }
    break;
  }
  case OpcodeKind::Or: {
    if (inputs.size() >= 2) {
      outputs.emplace_back(VortexCore::evaluateTruthiness(inputs[0]) ||
                           VortexCore::evaluateTruthiness(inputs[1]));
    }
    break;
  }
  case OpcodeKind::Not: {
    if (!inputs.empty()) {
      outputs.emplace_back(!VortexCore::evaluateTruthiness(inputs[0]));
    }
    break;
  }
  case OpcodeKind::Link: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      const auto c =
          resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      DimRef d                   = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg                   = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      std::optional<CellRef> tgt = std::nullopt;
      if (inCells.size() >= 4 || inputs.size() >= 4) {
        const auto t =
            resolveCell(inCells, inputs, cellReferences_, 3, core_.arena());
        if (t) {
          tgt = *t;
        }
      }
      if (c) {
        const auto res =
            tgt ? core_.setLink(*c, d, neg, *tgt) : core_.readLink(*c, d, neg);
        outputs.emplace_back(res ? *res : noCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Break: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      const auto c =
          resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      DimRef d = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      if (c) {
        auto res = core_.breakLink(*c, d, neg);
        outputs.emplace_back(res ? *res : noCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Linked: {
    // #LINKED subject dim neg [follow] [filterDim filterNeg]: walks the whole
    // rank from the subject and rebuilds, under the bound output head, a
    // +d.grab/+d.step list of ref-boxes (int content holding each member's
    // CellRef). With follow, the subject box's int content names the real
    // subject, which is how a stored traversal result feeds a later step.
    // With a filter pair, a member must have its own link on filterDim to be
    // included.
    std::vector<CellRef> inCells  = core_.inputsOf(opcode);
    std::vector<CellRef> outCells = core_.outputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      auto subject =
          resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      if (subject && inputs.size() >= 4 &&
          VortexCore::evaluateTruthiness(inputs[3])) {
        const auto referenced = static_cast<CellRef>(toInt(inputs[0]));
        subject               = core_.arena().contains(referenced)
                                    ? std::optional<CellRef>(referenced)
                                    : std::nullopt;
      }
      const DimRef dim       = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      const bool neg         = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      const bool filter      = inputs.size() >= 6;
      const DimRef filterDim = filter ? toInt(inputs[4]) : 0;
      const bool filterNeg   = filter && toInt(inputs[5]) < 0;
      if (!outCells.empty()) {
        const CellRef head = outCells.front();
        // A previous materialization is replaced: unlink the old ref-boxes.
        // Runs even when the subject is gone, or a re-execution would keep a
        // stale rank instead of yielding an empty list.
        if (const auto first =
                core_.arena().linked(head, core_.dims().grab, false)) {
          for (auto box = std::optional<CellRef>(*first); box;) {
            core_.arena().consumeWork();
            // The successor is read before the unlink: breaking first would
            // orphan the rest of the rank and leak every box after it.
            const auto next =
                core_.arena().linked(*box, core_.dims().step, false);
            core_.breakLink(*box, core_.dims().step, false);
            box = next;
          }
          core_.breakLink(head, core_.dims().grab, false);
        }
        if (subject) {
          CellRef tail = head;
          bool first   = true;
          std::unordered_set<CellRef> seen{*subject};
          for (auto member = core_.arena().linked(*subject, dim, neg);
               member && seen.insert(*member).second;
               member = core_.arena().linked(*member, dim, neg)) {
            core_.arena().consumeWork();
            if (filter && !core_.arena().linked(*member, filterDim, filterNeg))
              continue;
            const CellRef box = core_.arena().makeScalarCell(
                static_cast<std::int64_t>(*member));
            zigzag::expectWritten(core_.arena().link(
                tail, first ? core_.dims().grab : core_.dims().step, false,
                box));
            tail  = box;
            first = false;
          }
          outputs.emplace_back(static_cast<std::int64_t>(head));
        } else {
          outputs.emplace_back(noCell);
        }
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Find: {
    // #FIND subject name: the subject operand's int names a term cell;
    // walk its +d.step rank for the member whose +d.name text matches
    // and yield that member's rendered value — `term.field` in one op.
    // A missing member or unbound subject yields false so a guard's
    // `a.missing > n` fails the clause instead of faulting the turn.
    if (inputs.size() >= 2) {
      const auto referenced    = static_cast<CellRef>(toInt(inputs[0]));
      const std::string wanted = std::holds_alternative<std::string>(inputs[1])
                                     ? std::get<std::string>(inputs[1])
                                     : std::string{};
      CellValue found{false};
      if (core_.arena().contains(referenced)) {
        const auto &dims = core_.dims();
        for (const CellRef member :
             zigzag::rankAfter(core_.arena(), referenced, dims.step)) {
          core_.arena().consumeWork();
          const auto label =
              core_.arena().linked(member, dims.name, DimVector::POS);
          if (label && core_.arena().textOf(*label) == wanted) {
            if (const auto rendered = core_.render(member)) found = *rendered;
            break;
          }
        }
      }
      outputs.push_back(found);
    }
    break;
  }
  case OpcodeKind::FieldSet: {
    // #SET subject name value: Find's write twin — the subject operand's
    // int names a term cell; walk its +d.step rank for the member whose
    // +d.name text matches and replace that member's content. A missing
    // member or unbound subject yields false so the write's caller can
    // fail the clause honestly rather than silently dropping state.
    if (inputs.size() >= 3) {
      const auto referenced    = static_cast<CellRef>(toInt(inputs[0]));
      const std::string wanted = std::holds_alternative<std::string>(inputs[1])
                                     ? std::get<std::string>(inputs[1])
                                     : std::string{};
      bool wrote               = false;
      if (core_.arena().contains(referenced)) {
        const auto &dims = core_.dims();
        for (const CellRef member :
             zigzag::rankAfter(core_.arena(), referenced, dims.step)) {
          core_.arena().consumeWork();
          const auto label =
              core_.arena().linked(member, dims.name, DimVector::POS);
          if (label && core_.arena().textOf(*label) == wanted) {
            WriteFault fault = WriteFault::None;
            const auto res =
                core_.writeValue(member, 0, std::nullopt, inputs[2], &fault);
            if (fault == WriteFault::OpcodeKind) return kindImmutabilityFault();
            wrote = res.has_value();
            break;
          }
        }
      }
      outputs.push_back(wrote);
    }
    break;
  }
  case OpcodeKind::RunCall: {
    // #RUNCALL box entryDim failTarget argsRank — §6.2's dynamic run:
    // the box's int names a fragment head; follow its +d.entry chain
    // to the entry op, descriptor-check the args rank's arity, bind
    // each value by the parameter's +d.name label on the cursor's
    // +d.vars (the #BIND/#RESOLVE channel), push the frame whose
    // successor is the +d.spin target (the RunResume op), and jump.
    // Every §6.2 refusal lands on the fail target instead.
    if (inputs.size() >= 4 && cursor) {
      const auto &dims     = core_.dims();
      const CellRef failOp = static_cast<CellRef>(toInt(inputs[2]));
      const auto failJump  = [&]() {
        setCursorOpcode(*cursor, failOp);
        jumped = true;
      };
      CellRef head           = static_cast<CellRef>(toInt(inputs[0]));
      const DimRef entryDim  = toInt(inputs[1]);
      const CellRef argsRank = static_cast<CellRef>(toInt(inputs[3]));

      // A member-`def` callsite names the module *symbol*, not the
      // head — the symbol's +d.values lands on the head whenever the
      // method's own walk finished, so a caller lowered before its
      // callee still resolves. Heads are discriminated by +d.entry: a
      // cell carrying one IS the head (dynamic-run shape); a cell
      // lacking one gets the single +d.values hop.
      if (core_.arena().contains(head) &&
          !core_.arena().linked(head, entryDim, zigzag::Posward))
        if (const auto via =
                core_.arena().linked(head, dims.values, zigzag::Posward))
          head = *via;

      // Op first, head second — the same discriminator runFragment
      // applies (meta.cpp): an operand naming an op is the headless-run
      // refusal; a head's +d.entry chain resolves to the graph root,
      // bounded against link cycles like graphRoot's.
      const auto resolved = [&]() -> std::optional<CellRef> {
        if (!core_.arena().contains(head) ||
            opcodeMap_.find(head) != opcodeMap_.end())
          return std::nullopt;
        auto current = head;
        for (std::size_t hops = 0; hops < 16; ++hops) {
          if (opcodeMap_.find(current) != opcodeMap_.end()) return current;
          const auto next =
              core_.arena().linked(current, entryDim, zigzag::Posward);
          if (!next || !core_.arena().contains(*next)) return std::nullopt;
          current = *next;
        }
        return std::nullopt;
      }();
      if (!resolved) {
        failJump();
        break;
      }
      const auto descriptor =
          core_.arena().linked(head, dims.values, zigzag::Posward);
      if (!descriptor || !core_.arena().contains(*descriptor)) {
        failJump();
        break;
      }

      // Ordered signature: the descriptor's +d.grab chain via +d.step —
      // parameterCells' convention — each member +d.name-labelled.
      std::vector<CellRef> params;
      for (auto cell =
               core_.arena().linked(*descriptor, dims.grab, zigzag::Posward);
           cell && core_.arena().contains(*cell);
           cell = core_.arena().linked(*cell, dims.step, zigzag::Posward))
        params.push_back(*cell);

      // §5.8's member convention: a `$owner` param is not an argument —
      // the callee's #RESOLVE prologue finds the caller's own $owner
      // binding on the shared vars rank. Arity and positional binding
      // count the caller-supplied params only.
      std::vector<CellRef> boundParams;
      for (const auto param : params) {
        const auto label =
            core_.arena().linked(param, dims.name, zigzag::Posward);
        if (label && core_.arena().textOf(*label) == "$owner") continue;
        boundParams.push_back(param);
      }

      // The caller-staged values ride the same convention: argsRank's
      // +d.grab chain via +d.step, each member the evaluated cell
      // itself.
      std::vector<CellValue> argValues;
      for (auto cell =
               core_.arena().linked(argsRank, dims.grab, zigzag::Posward);
           cell && core_.arena().contains(*cell);
           cell = core_.arena().linked(*cell, dims.step, zigzag::Posward)) {
        core_.arena().consumeWork();
        argValues.push_back(core_.render(*cell).value_or(CellValue{false}));
      }
      if (argValues.size() != boundParams.size()) {
        failJump();
        break;
      }

      // The §6.2 frame: an Int64 cell holding the successor op, pushed
      // on the cursor's +d.stack — Call's shape, so the callee's tail
      // Return lands on the RunResume op.
      const auto nextOp =
          core_.arena().linked(opcode, dims.spin, zigzag::Posward);
      if (!nextOp) {
        failJump();
        break;
      }
      CellRef frame = core_.arena().makeCell();
      zigzag::expectWritten(
          core_.arena().setValueBits(frame, xanadu::ValueKind::Int64,
                                     static_cast<std::uint64_t>(*nextOp)));
      const auto oldStack =
          core_.arena().linked(*cursor, dims.stack, zigzag::Posward);
      zigzag::expectWritten(
          core_.arena().link(*cursor, dims.stack, zigzag::Posward, frame));
      if (oldStack) {
        zigzag::expectWritten(
            core_.arena().link(frame, dims.stack, zigzag::Posward, *oldStack));
      }

      // Bind each value by its parameter's +d.name label — #BIND's
      // name-keyed +d.vars/+d.values channel so the callee's #RESOLVE
      // prologue reads them verbatim; labelOf's argN fallback stands
      // for an unlabelled parameter.
      for (std::size_t i = 0; i < boundParams.size(); ++i) {
        core_.arena().consumeWork();
        const auto label =
            core_.arena().linked(boundParams[i], dims.name, zigzag::Posward);
        const auto name =
            label ? core_.arena().textOf(*label) : "arg" + std::to_string(i);
        CellRef varCell  = core_.arena().makeCell(name);
        CellRef valCell  = core_.arena().makeCell();
        WriteFault fault = WriteFault::None;
        core_.writeValue(valCell, 0, std::nullopt, argValues[i], &fault);
        if (fault == WriteFault::OpcodeKind) return kindImmutabilityFault();
        zigzag::expectWritten(
            core_.arena().link(varCell, dims.values, zigzag::Posward, valCell));
        if (const auto varsHead =
                core_.arena().linked(*cursor, dims.vars, zigzag::Posward))
          zigzag::expectWritten(core_.arena().link(varCell, dims.vars,
                                                   zigzag::Posward, *varsHead));
        zigzag::expectWritten(
            core_.arena().link(*cursor, dims.vars, zigzag::Posward, varCell));
      }

      setCursorOpcode(*cursor, *resolved);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::RunResume: {
    // #RUNRESUME box failTarget → out: the frame successor a RunCall
    // pushes. Read the head's descriptor produced/result cells — the
    // descriptor's +d.vars records textOf-keyed — produced writes the
    // result into the bound output and spins on; unproduced lands on
    // the fail target — §6.2's post-Return gate.
    if (inputs.size() >= 2 && cursor) {
      const auto &dims     = core_.dims();
      const CellRef head   = static_cast<CellRef>(toInt(inputs[0]));
      const CellRef failOp = static_cast<CellRef>(toInt(inputs[1]));
      bool produced        = false;
      CellValue result{false};
      // The box may name a member-`def` *symbol*: its +d.values lands
      // on the head, and only the head's own +d.values is the
      // descriptor. Hop the chain — the first cell whose +d.vars
      // carries produced/result records is the descriptor.
      auto at = head;
      for (std::size_t hops = 0; hops < 2 && !produced; ++hops) {
        if (!core_.arena().contains(at)) break;
        const auto descriptor =
            core_.arena().linked(at, dims.values, zigzag::Posward);
        if (!descriptor || !core_.arena().contains(*descriptor)) break;
        CellRef producedCell{noCell}, resultCell{noCell};
        for (const CellRef record :
             zigzag::rankAfter(core_.arena(), *descriptor, dims.vars)) {
          core_.arena().consumeWork();
          const auto key = core_.arena().textOf(record);
          if (key == "produced")
            producedCell = core_.arena()
                               .linked(record, dims.values, zigzag::Posward)
                               .value_or(noCell);
          else if (key == "result")
            resultCell = core_.arena()
                             .linked(record, dims.values, zigzag::Posward)
                             .value_or(noCell);
        }
        if (core_.arena().contains(producedCell) &&
            core_.arena().contains(resultCell)) {
          produced = VortexCore::evaluateTruthiness(
              core_.render(producedCell).value_or(CellValue{false}));
          result = core_.render(resultCell).value_or(CellValue{false});
        } else {
          at = *descriptor;
        }
      }
      if (produced)
        outputs.emplace_back(result);
      else {
        setCursorOpcode(*cursor, failOp);
        jumped = true;
      }
    }
    break;
  }
  case OpcodeKind::New: {
    std::vector<CellRef> inCells  = inputsOf(opcode);
    std::vector<CellRef> outCells = outputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      auto c = resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      DimRef d = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      if (c) {
        const bool append = inputs.size() == 4 &&
                            std::holds_alternative<bool>(inputs[3]) &&
                            std::get<bool>(inputs[3]);
        if (append) {
          if (!outCells.empty() &&
              (core_.arena().linked(outCells.front(), d, false) ||
               core_.arena().linked(outCells.front(), d, true))) {
            return ExecutionResult{
                .success = false,
                .errorMessage =
                    "Compiled creation output was already used; compile a "
                    "fresh executable"};
          }
          std::unordered_set<CellRef> seen{*c};
          while (const auto next = core_.arena().linked(*c, d, neg)) {
            core_.arena().consumeWork();
            if (!seen.insert(*next).second) {
              return ExecutionResult{
                  .success = false,
                  .errorMessage =
                      "Compiled append requires an open rank, not a cycle"};
            }
            c = *next;
          }
        }
        if (!outCells.empty()) {
          CellRef fresh = outCells[0];
          core_.setLink(*c, d, neg, fresh);
          outputs.emplace_back(fresh);
        } else {
          auto res = core_.newCell(*c, d, neg);
          outputs.emplace_back(res ? *res : noCell);
        }
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Value: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    if (!inCells.empty() || !inputs.empty()) {
      const auto c =
          resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      std::int64_t off = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      std::optional<std::int64_t> len =
          inputs.size() >= 3 ? std::optional<std::int64_t>{toInt(inputs[2])}
                             : std::nullopt;
      std::optional<CellValue> repl = inputs.size() >= 4
                                          ? std::optional<CellValue>(inputs[3])
                                          : std::nullopt;
      if (c) {
        if (repl) {
          WriteFault fault = WriteFault::None;
          const auto res   = core_.writeValue(*c, off, len, *repl, &fault);
          if (fault == WriteFault::OpcodeKind) {
            return kindImmutabilityFault();
          }
          outputs.emplace_back(res ? *res : noCell);
        } else {
          const auto res = core_.readValue(*c, off, len);
          outputs.emplace_back(res ? *res : noCell);
        }
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Splice: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    if (inCells.size() >= 4 || inputs.size() >= 4) {
      const auto c =
          resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      std::int64_t off = toInt(inputs[1]);
      std::int64_t len = toInt(inputs[2]);
      if (c) {
        WriteFault fault = WriteFault::None;
        const auto res   = core_.splice(*c, off, len, inputs[3], &fault);
        if (fault == WriteFault::OpcodeKind) {
          return kindImmutabilityFault();
        }
        outputs.emplace_back(res ? *res : noCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Clone: {
    std::vector<CellRef> inCells  = inputsOf(opcode);
    std::vector<CellRef> outCells = outputsOf(opcode);
    if (!inCells.empty() || !inputs.empty()) {
      const auto c =
          resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
      if (c) {
        CellRef cloneCell =
            !outCells.empty() ? outCells[0] : core_.arena().makeCell();
        CellRef cloneTail  = *c;
        std::size_t cLimit = core_.arena().cellCount() + 1;
        while (cLimit-- > 0) {
          auto next =
              core_.arena().linked(cloneTail, core_.dims().clone, false);
          if (!next) {
            zigzag::expectWritten(core_.arena().link(
                cloneTail, core_.dims().clone, false, cloneCell));
            break;
          }
          if (*next == cloneCell) {
            break;
          }
          cloneTail = *next;
        }
        outputs.emplace_back(cloneCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::MatchContract:
  case OpcodeKind::RequireContract:
    return executeTypeContract(kind, opcode, outputs);
  case OpcodeKind::CopyCellRefs:
  case OpcodeKind::ClearCellRefs: {
    return executeCellRefCopy(kind, opcode, outputs);
  }
  case OpcodeKind::BranchFalseCell: {
    const auto inCells = inputsOf(opcode);
    if (inputs.empty() || inCells.size() != 2 || !cursor ||
        !core_.arena().contains(inCells[1])) {
      return {.success = false, .errorMessage = "invalid cell branch operands"};
    }
    if (!VortexCore::evaluateTruthiness(inputs.front())) {
      setCursorOpcode(*cursor, inCells[1]);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::StreamClear:
  case OpcodeKind::StreamAppend:
  case OpcodeKind::StreamBegin:
  case OpcodeKind::StreamReverseBegin:
  case OpcodeKind::RankBegin:
  case OpcodeKind::IteratorNext:
    return executeStreamOpcode(kind, opcode, outputs, cursor);
  case OpcodeKind::Jump: {
    if (!inputs.empty() && cursor) {
      CellRef targetOp = toInt(inputs[0]);
      setCursorOpcode(*cursor, targetOp);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::Branch: {
    if (inputs.size() >= 2 && cursor &&
        VortexCore::evaluateTruthiness(inputs[0])) {
      CellRef targetOp = toInt(inputs[1]);
      setCursorOpcode(*cursor, targetOp);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::Call: {
    if (!inputs.empty() && cursor) {
      CellRef targetOp = toInt(inputs[0]);
      const auto nextOp =
          core_.arena().linked(opcode, core_.dims().spin, false);
      if (nextOp) {
        CellRef frame = core_.arena().makeCell();
        zigzag::expectWritten(
            core_.arena().setValueBits(frame, xanadu::ValueKind::Int64,
                                       static_cast<std::uint64_t>(*nextOp)));
        const auto oldStack =
            core_.arena().linked(*cursor, core_.dims().stack, false);
        zigzag::expectWritten(
            core_.arena().link(*cursor, core_.dims().stack, false, frame));
        if (oldStack) {
          zigzag::expectWritten(
              core_.arena().link(frame, core_.dims().stack, false, *oldStack));
        }
      }
      setCursorOpcode(*cursor, targetOp);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::Return: {
    if (cursor) {
      const auto frame =
          core_.arena().linked(*cursor, core_.dims().stack, false);
      if (frame) {
        auto returnOpVal = core_.arena().asInt64(*frame);
        CellRef returnOp =
            returnOpVal ? static_cast<CellRef>(*returnOpVal) : noCell;
        const auto nextStack =
            core_.arena().linked(*frame, core_.dims().stack, false);
        zigzag::expectWritten(
            core_.arena().link(*cursor, core_.dims().stack, false, nextStack));
        setCursorOpcode(*cursor, returnOp);
        jumped = true;
      }
    }
    break;
  }
  case OpcodeKind::Bind: {
    // #BIND name val: binds var in cursor's scope (+d.vars and +d.values)
    if (inputs.size() >= 2 && cursor) {
      std::string varName = std::holds_alternative<std::string>(inputs[0])
                                ? std::get<std::string>(inputs[0])
                                : std::to_string(toInt(inputs[0]));
      CellRef varCell     = core_.arena().makeCell(varName);
      CellRef valCell     = core_.arena().makeCell();
      core_.writeValue(valCell, 0, std::nullopt, inputs[1]);
      zigzag::expectWritten(
          core_.arena().link(varCell, core_.dims().values, false, valCell));

      const auto firstVar =
          core_.arena().linked(*cursor, core_.dims().vars, false);
      if (!firstVar) {
        zigzag::expectWritten(
            core_.arena().link(*cursor, core_.dims().vars, false, varCell));
      } else {
        zigzag::expectWritten(
            core_.arena().link(varCell, core_.dims().vars, false, *firstVar));
        zigzag::expectWritten(
            core_.arena().link(*cursor, core_.dims().vars, false, varCell));
      }
      outputs.emplace_back(valCell);
    }
    break;
  }
  case OpcodeKind::Resolve: {
    // #RESOLVE name: finds var in cursor's scope
    if (!inputs.empty() && cursor) {
      std::string varName = std::holds_alternative<std::string>(inputs[0])
                                ? std::get<std::string>(inputs[0])
                                : std::to_string(toInt(inputs[0]));
      // The first variable in scope with this name and a value; a name
      // declared but never bound is passed over.
      const auto &arena = core_.arena();
      const auto bound  = [&](const CellRef var) {
        return arena.textOf(var) == varName &&
               zigzag::step(arena, var, core_.dims().values).has_value();
      };
      outputs.push_back(
          zigzag::firstOf(zigzag::rankAfter(arena, *cursor, core_.dims().vars) |
                          std::views::filter(bound))
              .and_then(zigzag::hop(arena, core_.dims().values))
              .transform([this](const CellRef value) {
                return core_.render(value).value();
              })
              .value_or(CellValue{false}));
    }
    break;
  }
  case OpcodeKind::Assert: {
    if (inputs.size() > 2 ||
        (inputs.size() == 2 &&
         !std::holds_alternative<std::string>(inputs[1]))) {
      return ExecutionResult{.success           = false,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = opcode,
                             .errorMessage =
                                 "Assert accepts an optional text diagnostic"};
    }
    if (inputs.empty() || !VortexCore::evaluateTruthiness(inputs[0])) {
      return ExecutionResult{
          .success           = false,
          .contractViolation = ContractViolationKind::None,
          .failingClause     = opcode,
          .errorMessage = inputs.size() == 2 ? std::get<std::string>(inputs[1])
                                             : "Assertion failed"};
    }
    break;
  }
  case OpcodeKind::Abs: {
    if (!inputs.empty()) {
      if (std::holds_alternative<double>(inputs[0])) {
        outputs.emplace_back(std::abs(std::get<double>(inputs[0])));
      } else {
        std::int64_t v = toInt(inputs[0]);
        if (v == minInt) return overflowFailure();
        outputs.emplace_back(v < 0 ? -v : v);
      }
    }
    break;
  }
  case OpcodeKind::Min: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.emplace_back(
            std::min(toDouble(inputs[0]), toDouble(inputs[1])));
      } else {
        outputs.emplace_back(std::min(toInt(inputs[0]), toInt(inputs[1])));
      }
    }
    break;
  }
  case OpcodeKind::Max: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.emplace_back(
            std::max(toDouble(inputs[0]), toDouble(inputs[1])));
      } else {
        outputs.emplace_back(std::max(toInt(inputs[0]), toInt(inputs[1])));
      }
    }
    break;
  }
  case OpcodeKind::Clamp: {
    if (inputs.size() >= 3) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1]) ||
          std::holds_alternative<double>(inputs[2])) {
        double v  = toDouble(inputs[0]);
        double lo = toDouble(inputs[1]);
        double hi = toDouble(inputs[2]);
        outputs.emplace_back(std::clamp(v, lo, hi));
      } else {
        std::int64_t v  = toInt(inputs[0]);
        std::int64_t lo = toInt(inputs[1]);
        std::int64_t hi = toInt(inputs[2]);
        outputs.emplace_back(std::clamp(v, lo, hi));
      }
    }
    break;
  }
  case OpcodeKind::Trim: {
    if (!inputs.empty()) {
      std::string s     = std::holds_alternative<std::string>(inputs[0])
                              ? std::get<std::string>(inputs[0])
                              : std::to_string(toInt(inputs[0]));
      std::size_t start = s.find_first_not_of(" \t\n\r");
      if (start == std::string::npos) {
        outputs.emplace_back(std::string{});
      } else {
        std::size_t end = s.find_last_not_of(" \t\n\r");
        outputs.emplace_back(s.substr(start, end - start + 1));
      }
    }
    break;
  }
  case OpcodeKind::ToLower: {
    if (!inputs.empty()) {
      std::string s = std::holds_alternative<std::string>(inputs[0])
                          ? std::get<std::string>(inputs[0])
                          : std::to_string(toInt(inputs[0]));
      for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      outputs.emplace_back(s);
    }
    break;
  }
  case OpcodeKind::ToUpper: {
    if (!inputs.empty()) {
      std::string s = std::holds_alternative<std::string>(inputs[0])
                          ? std::get<std::string>(inputs[0])
                          : std::to_string(toInt(inputs[0]));
      for (char &c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      outputs.emplace_back(s);
    }
    break;
  }
  case OpcodeKind::Unify: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    const auto a =
        resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
    const auto b =
        resolveCell(inCells, inputs, cellReferences_, 1, core_.arena());
    if (a && b) {
      zigzag::Vlog vlog{.m     = core_.arena(),
                        .clone = core_.dims().clone,
                        .grab  = core_.dims().grab,
                        .step  = core_.dims().step,
                        .vars  = core_.dims().vars};
      outputs.emplace_back(vlog.unify(*a, *b).has_value());
    } else {
      outputs.emplace_back(false);
    }
    break;
  }
  case OpcodeKind::IsVar: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    const auto c =
        resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
    if (c) {
      zigzag::Vlog vlog{.m     = core_.arena(),
                        .clone = core_.dims().clone,
                        .grab  = core_.dims().grab,
                        .step  = core_.dims().step,
                        .vars  = core_.dims().vars};
      outputs.emplace_back(vlog.isUnbound(*c));
    } else {
      outputs.emplace_back(false);
    }
    break;
  }
  case OpcodeKind::MakeVar: {
    zigzag::Vlog vlog{.m     = core_.arena(),
                      .clone = core_.dims().clone,
                      .grab  = core_.dims().grab,
                      .step  = core_.dims().step,
                      .vars  = core_.dims().vars};
    CellRef var = vlog.makeVar();
    outputs.emplace_back(static_cast<std::int64_t>(var));
    break;
  }
  case OpcodeKind::MakeTerm: {
    std::string functor;
    if (!inputs.empty()) {
      if (std::holds_alternative<std::string>(inputs[0])) {
        functor = std::get<std::string>(inputs[0]);
      } else {
        functor = std::to_string(toInt(inputs[0]));
      }
    }
    std::vector<CellRef> inCells = inputsOf(opcode);
    std::vector<CellRef> args;
    std::size_t maxArgs = std::max(inCells.size(), inputs.size());
    for (std::size_t i = 1; i < maxArgs; ++i) {
      const auto argCell =
          resolveCell(inCells, inputs, cellReferences_, i, core_.arena());
      if (argCell) {
        args.push_back(*argCell);
      }
    }
    CellRef term = core_.arena().makeCell(functor);
    CellRef prev = noCell;
    for (CellRef arg : args) {
      if (prev == noCell) {
        zigzag::expectWritten(
            core_.arena().link(term, core_.dims().grab, false, arg));
      } else {
        zigzag::expectWritten(
            core_.arena().link(prev, core_.dims().step, false, arg));
      }
      prev = arg;
    }
    outputs.emplace_back(static_cast<std::int64_t>(term));
    break;
  }
  case OpcodeKind::Deref: {
    std::vector<CellRef> inCells = inputsOf(opcode);
    const auto c =
        resolveCell(inCells, inputs, cellReferences_, 0, core_.arena());
    if (c) {
      zigzag::Vlog vlog{.m     = core_.arena(),
                        .clone = core_.dims().clone,
                        .grab  = core_.dims().grab,
                        .step  = core_.dims().step,
                        .vars  = core_.dims().vars};
      outputs.emplace_back(static_cast<std::int64_t>(vlog.deref(*c)));
    } else {
      outputs.emplace_back(static_cast<std::int64_t>(noCell));
    }
    break;
  }
  case OpcodeKind::Choice: {
    const auto invalidChoice = [opcode] {
      return ExecutionResult{.success           = false,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = opcode,
                             .errorMessage =
                                 "Choice requires a valid alternative opcode"};
    };
    if (inputs.size() != 1 ||
        !std::holds_alternative<std::int64_t>(inputs.front())) {
      return invalidChoice();
    }
    const auto rawAlt = std::get<std::int64_t>(inputs.front());
    if (rawAlt < 0 || static_cast<std::uint64_t>(rawAlt) >
                          std::numeric_limits<CellRef>::max()) {
      return invalidChoice();
    }
    const auto altOp = static_cast<CellRef>(rawAlt);
    if (altOp == noCell || !core_.arena().contains(altOp) ||
        !getOpcodeKind(altOp)) {
      return invalidChoice();
    }
    if (oracleCursor_ < oraclePath_.size()) {
      const std::uint16_t directive = oraclePath_[oracleCursor_++];
      currentDeweyPath_.push_back(directive);
      if (directive > 0 && cursor) {
        setCursorOpcode(*cursor, altOp);
        jumped = true;
      }
    } else {
      pushChoicePoint(cursor, altOp);
    }
    outputs.emplace_back(true);
    break;
  }
  case OpcodeKind::Fail: {
    if (backtrack(cursor)) {
      jumped = true;
    } else {
      return ExecutionResult{.success           = false,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = opcode,
                             .errorMessage =
                                 "Logic failure: no remaining choice points"};
    }
    break;
  }
  case OpcodeKind::ScopedChoice: {
    const auto targets = inputsOf(opcode);
    if (!cursor || targets.size() != 1 ||
        !core_.arena().contains(targets.front()) ||
        !getOpcodeKind(targets.front())) {
      return {.success       = false,
              .failingClause = opcode,
              .errorMessage  = "ScopedChoice requires one direct opcode target "
                               "and a live cursor"};
    }
    try {
      pushChoicePointImpl(cursor, targets.front(), true);
    } catch (const std::exception &error) {
      return {.success       = false,
              .failingClause = opcode,
              .errorMessage  = error.what()};
    }
    break;
  }
  case OpcodeKind::Cut: {
    std::size_t barrier = 0;
    if (!inputs.empty()) {
      barrier =
          static_cast<std::size_t>(std::max<std::int64_t>(0, toInt(inputs[0])));
    }
    cut(barrier);
    outputs.emplace_back(true);
    break;
  }
  case OpcodeKind::ChoiceDepth:
    outputs.emplace_back(static_cast<std::int64_t>(choiceDepth()));
    break;
  case OpcodeKind::Halt:
  case OpcodeKind::Nop:
  default:
    break;
  }
  return ExecutionResult{.success           = true,
                         .contractViolation = ContractViolationKind::None,
                         .failingClause     = std::nullopt,
                         .errorMessage      = ""};
}

ExecutionResult VortexVM::step(CellRef cursor) {
  if (const auto groups = mutationGroups_.find(cursor);
      groups != mutationGroups_.end() && groups->second &&
      !groups->second->empty() && guardLimits_.maxMutationGroupCycles == 0) {
    abortOwnedMutationGroups(cursor, std::nullopt);
    return {.success      = false,
            .errorMessage = "mutation-group cycle limit exceeded"};
  }
  if (const auto groups = mutationGroups_.find(cursor);
      groups != mutationGroups_.end() && groups->second &&
      std::ranges::any_of(*groups->second, [&](const MutationGroup &group) {
        return totalCycles_ >= group.startTotalCycles &&
               totalCycles_ - group.startTotalCycles >=
                   guardLimits_.maxMutationGroupCycles;
      })) {
    abortOwnedMutationGroups(cursor, std::nullopt);
    return {.success      = false,
            .errorMessage = "mutation-group cycle limit exceeded"};
  }
  ExecutionResult result;
  try {
    result = stepImpl(cursor);
  } catch (const std::exception &error) {
    result = {.success = false, .errorMessage = error.what()};
  }
  if (!result.success) abortOwnedMutationGroups(cursor, std::nullopt);
  return result;
}

ExecutionResult VortexVM::stepImpl(CellRef cursor) {
  const auto activeGroup =
      std::ranges::find_if(mutationGroups_, [](const auto &entry) {
        return entry.second && !entry.second->empty();
      });
  if (activeGroup != mutationGroups_.end() && activeGroup->first != cursor)
    return {.success = false,
            .errorMessage =
                "execution cursor does not match the active mutation group"};
  const auto foreignStack = [cursor](const auto &entry) {
    return entry.first != cursor && entry.second && !entry.second->empty();
  };
  if (std::ranges::any_of(guardStacks_, foreignStack) ||
      std::ranges::any_of(guardScopes_, foreignStack) ||
      std::ranges::any_of(pendingGuardChecks_, [cursor](const auto &entry) {
        return entry.first != cursor && entry.second;
      }))
    return {.success = false,
            .errorMessage =
                "Vortex VM supports one active guarded cursor at a time"};
  if (hasScopedChoices() && !scopedChoicesOwnedBy(cursor)) {
    return {.success = false,
            .errorMessage =
                "execution cursor does not match the active scoped choice"};
  }
  const auto op = getCursorOpcode(cursor);
  if (!op) {
    if (hasScopedChoices()) {
      return {.success      = false,
              .errorMessage = "cursor finished with an active scoped choice"};
    }
    if (activationDepth(cursor) != 0)
      return {.success      = false,
              .errorMessage = "activation frame lost its cursor continuation"};
    if (const auto guards = guardStacks_.find(cursor);
        guards != guardStacks_.end() && guards->second &&
        !guards->second->empty())
      return {.success      = false,
              .errorMessage = "cursor finished with an active guard"};
    if (pendingGuardChecks_.contains(cursor))
      return {.success      = false,
              .errorMessage = "cursor finished with pending guard validation"};
    if (mutationGroupActive(cursor))
      return {.success      = false,
              .errorMessage = "cursor finished with an active mutation group"};
    if (guardScopes_.contains(cursor))
      return {.success      = false,
              .errorMessage = "cursor finished with an active guard scope"};
    return ExecutionResult{.success           = true,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage      = "Finished"};
  }

  auto kindOpt    = getOpcodeKind(*op);
  OpcodeKind kind = kindOpt ? *kindOpt : OpcodeKind::Nop;
  if (kind == OpcodeKind::Halt) {
    if (hasScopedChoices()) {
      return {.success      = false,
              .errorMessage = "Halt reached with an active scoped choice"};
    }
    if (activationDepth(cursor) != 0)
      return {.success      = false,
              .errorMessage = "Halt reached with an active activation frame"};
    if (const auto guards = guardStacks_.find(cursor);
        guards != guardStacks_.end() && guards->second &&
        !guards->second->empty())
      return {.success       = false,
              .failingClause = *op,
              .errorMessage  = "Halt reached with an active guard"};
    if (pendingGuardChecks_.contains(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage  = "Halt reached with pending guard validation"};
    if (mutationGroupActive(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage  = "Halt reached with an active mutation group"};
    if (guardScopes_.contains(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage  = "Halt reached with an active guard scope"};
    return ExecutionResult{.success           = true,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage      = "Halted"};
  }

  if (activationDepth(cursor) != 0 && kind != OpcodeKind::ReturnFrame &&
      kind != OpcodeKind::Jump && kind != OpcodeKind::Branch &&
      kind != OpcodeKind::BranchFalseCell && kind != OpcodeKind::Call &&
      !core_.arena().linked(*op, core_.dims().spin, DimVector::POS)) {
    return {.success      = false,
            .errorMessage = "activation-frame body ended without ReturnFrame"};
  }

  struct ActiveCursorScope {
    std::optional<CellRef> &active;
    std::optional<CellRef> prior;
    ~ActiveCursorScope() { active = prior; }
  } activeScope{activeCursor_, activeCursor_};
  activeCursor_ = cursor;

  if (streamCursor_ && *streamCursor_ != cursor) {
    const auto ownerOpcode = getCursorOpcode(*streamCursor_);
    const bool ownerActive =
        ownerOpcode && getOpcodeKind(*ownerOpcode) != OpcodeKind::Halt;
    if (ownerActive) {
      return {.success = false,
              .errorMessage =
                  "Vortex VM supports one active stream cursor at a time"};
    }
  }

  if (kind == OpcodeKind::InvokeFrame || kind == OpcodeKind::ReturnFrame) {
    ++totalCycles_;
    try {
      core_.arena().consumeWork();
      return executeFrameOpcode(kind, *op, cursor);
    } catch (const std::exception &error) {
      return {.success = false, .errorMessage = error.what()};
    }
  }

  if (kind == OpcodeKind::CreateClosure || kind == OpcodeKind::ApplyClosure) {
    ++totalCycles_;
    try {
      core_.arena().consumeWork();
      return executeClosureOpcode(kind, *op, cursor);
    } catch (const std::exception &error) {
      return {
          .success = false, .failingClause = *op, .errorMessage = error.what()};
    }
  }
  if (kind == OpcodeKind::RestoreBindings) {
    ++totalCycles_;
    try {
      core_.arena().consumeWork();
      return executeRestoreBindings(*op, cursor);
    } catch (const std::exception &error) {
      return {
          .success = false, .failingClause = *op, .errorMessage = error.what()};
    }
  }

  if (kind == OpcodeKind::PushGuard || kind == OpcodeKind::PopGuard ||
      kind == OpcodeKind::CheckGuards || kind == OpcodeKind::OpenGuardScope ||
      kind == OpcodeKind::CloseGuardScope ||
      kind == OpcodeKind::BeginMutationGroup ||
      kind == OpcodeKind::EndMutationGroup) {
    ++totalCycles_;
    try {
      core_.arena().consumeWork();
      if (kind == OpcodeKind::OpenGuardScope ||
          kind == OpcodeKind::CloseGuardScope ||
          kind == OpcodeKind::BeginMutationGroup ||
          kind == OpcodeKind::EndMutationGroup)
        return executeMutationScopeOpcode(kind, *op, cursor);
      return executeGuardOpcode(kind, *op, cursor);
    } catch (const std::exception &error) {
      return {
          .success = false, .failingClause = *op, .errorMessage = error.what()};
    }
  }

  const bool mayTransferControl =
      kind == OpcodeKind::Jump || kind == OpcodeKind::Branch ||
      kind == OpcodeKind::BranchFalseCell || kind == OpcodeKind::Call ||
      kind == OpcodeKind::Return || kind == OpcodeKind::Choice ||
      kind == OpcodeKind::Fail || kind == OpcodeKind::ScopedChoice;
  if (!mayTransferControl &&
      !core_.arena().linked(*op, core_.dims().spin, DimVector::POS)) {
    const auto guards = guardStacks_.find(cursor);
    if (guards != guardStacks_.end() && guards->second &&
        !guards->second->empty())
      return {.success       = false,
              .failingClause = *op,
              .errorMessage  = "opcode would finish with an active guard"};
  }

  const auto inputCells = inputsOf(*op);
  const auto outCells   = outputsOf(*op);
  if (kind == OpcodeKind::VqlMaskLinks) {
    if (inputCells.size() < 2 || (inputCells.size() - 2) % 2 != 0 ||
        !outCells.empty())
      return {.success       = false,
              .failingClause = *op,
              .errorMessage  = "VqlMaskLinks expects owner, default, and "
                               "dimension/mask pairs with no outputs"};
    if (isMemoized(*op) || !core_.preconditionsOf(*op).empty() ||
        !core_.postconditionsOf(*op).empty())
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "VqlMaskLinks does not support conditions or memoization"};
    if (std::ranges::any_of(inputCells, [&](const CellRef input) {
          return core_.hasPipeline(input);
        }))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "VqlMaskLinks does not support parameter pipelines"};
    const auto active  = guardStacks_.find(cursor);
    const bool guarded = active != guardStacks_.end() && active->second &&
                         !active->second->empty();
    if (guarded && hasPendingGuardValidation(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "source mutation refused during pending guard validation"};
    if (guarded && !mutationGroupActive(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "guarded source mutation requires an atomic mutation group"};
  }
  const auto hasMutatingCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      const auto conditionKind = getOpcodeKind(actual);
      return conditionKind &&
             sourceMutatingPrimitive(*conditionKind, inputsOf(actual).size());
    });
  };
  if (hasActiveGuards() && (hasMutatingCondition(core_.preconditionsOf(*op)) ||
                            hasMutatingCondition(core_.postconditionsOf(*op))))
    return {.success       = false,
            .failingClause = *op,
            .errorMessage  = "source-mutating conditions are refused while "
                             "guard state is active"};
  const bool sourceMutation =
      kind == OpcodeKind::VqlMaskLinks ||
      ((kind == OpcodeKind::Link || kind == OpcodeKind::Value) &&
       inputCells.size() >= 4) ||
      (kind == OpcodeKind::New && inputCells.size() >= 3) ||
      (kind == OpcodeKind::Break && inputCells.size() >= 3) ||
      (kind == OpcodeKind::Splice && inputCells.size() >= 4) ||
      (kind == OpcodeKind::Clone && !inputCells.empty());
  const auto mutationSubject =
      sourceMutation
          ? resolveCell(inputCells, {}, cellReferences_, 0, core_.arena())
          : std::nullopt;
  const bool executesSourceMutation = sourceMutation && mutationSubject;
  std::optional<MutationGroupMap> stagedParticipants;
  if (executesSourceMutation) {
    const auto active  = guardStacks_.find(cursor);
    const bool guarded = active != guardStacks_.end() && active->second &&
                         !active->second->empty();
    if (guarded && hasPendingGuardValidation(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "source mutation refused during pending guard validation"};
    if (guarded && !mutationGroupActive(cursor))
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "guarded source mutation requires an atomic mutation group"};
    if (guarded) {
      const auto guards  = active->second;
      stagedParticipants = mutationGroups_;
      auto groups        = std::make_shared<std::vector<MutationGroup>>(
          *stagedParticipants->at(cursor));
      for (auto &group : *groups) {
        std::unordered_set<CellRef> known;
        for (const auto &participant : group.participants)
          if (participant) known.insert(participant->token);
        for (const auto &guard : *guards) {
          if (!guard || !known.insert(guard->token).second) continue;
          if (group.participants.size() >= guardLimits_.maxGroupParticipants)
            return {.success       = false,
                    .failingClause = *op,
                    .errorMessage =
                        "mutation-group participant limit exceeded"};
          group.participants.push_back(guard);
        }
      }
      (*stagedParticipants)[cursor] = std::move(groups);
    }
  }
  if (kind == OpcodeKind::ScopedChoice) {
    const auto failScoped = [op](const std::string_view message) {
      return ExecutionResult{.success       = false,
                             .failingClause = *op,
                             .errorMessage  = std::string{message}};
    };
    if (inputCells.size() != 1 || !outCells.empty()) {
      return failScoped(
          "ScopedChoice requires one direct opcode target and no outputs");
    }
    if (!core_.preconditionsOf(*op).empty() ||
        !core_.postconditionsOf(*op).empty() || isMemoized(*op)) {
      return failScoped(
          "ScopedChoice does not support conditions or memoization");
    }
    if (!core_.arena().contains(inputCells.front()) ||
        !getOpcodeKind(inputCells.front())) {
      return failScoped("ScopedChoice target must be a direct opcode cell");
    }
    if (streamSlots_.contains(inputCells.front()) ||
        iteratorSlots_.contains(inputCells.front()) ||
        cellReferences_.contains(inputCells.front())) {
      return failScoped("ScopedChoice target cannot be a register");
    }
  }
  const bool stateful = streamStateOpcode(kind);
  const bool contractOp =
      kind == OpcodeKind::MatchContract || kind == OpcodeKind::RequireContract;
  const bool resumeStateOp =
      kind == OpcodeKind::ResumeInit || kind == OpcodeKind::ResumeSet;
  const bool probeLinkOp           = kind == OpcodeKind::ProbeLink;
  const bool fieldProbeOp          = kind == OpcodeKind::VqlFieldProbe;
  const bool maskLinksOp           = kind == OpcodeKind::VqlMaskLinks;
  const auto hasProbeLinkCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      const auto clauseKind = getOpcodeKind(actual);
      return clauseKind == OpcodeKind::ProbeLink ||
             clauseKind == OpcodeKind::VqlFieldProbe ||
             clauseKind == OpcodeKind::VqlMaskLinks;
    });
  };
  const auto hasVqlScalarCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      const auto kind = getOpcodeKind(actual);
      return kind == OpcodeKind::VqlString || kind == OpcodeKind::VqlAdd ||
             kind == OpcodeKind::VqlBoxScalar ||
             kind == OpcodeKind::CurrentTimestamp;
    });
  };
  const auto hasFrameCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      const auto conditionKind = getOpcodeKind(actual);
      return conditionKind == OpcodeKind::InvokeFrame ||
             conditionKind == OpcodeKind::ReturnFrame;
    });
  };
  const auto hasClosureCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      const auto conditionKind = getOpcodeKind(actual);
      return conditionKind == OpcodeKind::CreateClosure ||
             conditionKind == OpcodeKind::ApplyClosure ||
             conditionKind == OpcodeKind::RestoreBindings;
    });
  };
  const auto hasScopedChoiceCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      return getOpcodeKind(actual) == OpcodeKind::ScopedChoice;
    });
  };
  const auto hasGuardCondition = [this](const auto &clauses) {
    return std::ranges::any_of(clauses, [this](const CellRef clause) {
      const CellRef actual = core_.arena()
                                 .cloneMaster(clause, core_.dims().clone)
                                 .value_or(clause);
      const auto conditionKind = getOpcodeKind(actual);
      return conditionKind == OpcodeKind::PushGuard ||
             conditionKind == OpcodeKind::PopGuard ||
             conditionKind == OpcodeKind::CheckGuards ||
             conditionKind == OpcodeKind::OpenGuardScope ||
             conditionKind == OpcodeKind::CloseGuardScope ||
             conditionKind == OpcodeKind::BeginMutationGroup ||
             conditionKind == OpcodeKind::EndMutationGroup;
    });
  };
  if (hasGuardCondition(core_.preconditionsOf(*op)) ||
      hasGuardCondition(core_.postconditionsOf(*op))) {
    return {.success       = false,
            .failingClause = *op,
            .errorMessage  = "guard opcodes cannot run as conditions"};
  }
  if (hasProbeLinkCondition(core_.preconditionsOf(*op)) ||
      hasProbeLinkCondition(core_.postconditionsOf(*op))) {
    return {
        .success = false,
        .errorMessage =
            "ProbeLink/VqlFieldProbe/VqlMaskLinks cannot run as conditions"};
  }
  if (hasVqlScalarCondition(core_.preconditionsOf(*op)) ||
      hasVqlScalarCondition(core_.postconditionsOf(*op))) {
    return {.success      = false,
            .errorMessage = "VQL scalar opcodes cannot run as conditions"};
  }
  if (hasFrameCondition(core_.preconditionsOf(*op)) ||
      hasFrameCondition(core_.postconditionsOf(*op))) {
    return {.success = false,
            .errorMessage =
                "activation-frame opcodes cannot run as conditions"};
  }
  if (hasClosureCondition(core_.preconditionsOf(*op)) ||
      hasClosureCondition(core_.postconditionsOf(*op))) {
    return {.success      = false,
            .errorMessage = "closure opcodes cannot run as conditions"};
  }
  if (hasScopedChoiceCondition(core_.preconditionsOf(*op)) ||
      hasScopedChoiceCondition(core_.postconditionsOf(*op))) {
    return {.success      = false,
            .errorMessage = "ScopedChoice cannot run as a condition"};
  }
  if (probeLinkOp || fieldProbeOp) {
    if (!core_.preconditionsOf(*op).empty() ||
        !core_.postconditionsOf(*op).empty()) {
      return {.success = false,
              .errorMessage =
                  "ProbeLink/VqlFieldProbe do not support pre/postconditions"};
    }
    if (std::ranges::any_of(
            inputCells,
            [&](const CellRef input) { return core_.hasPipeline(input); }) ||
        std::ranges::any_of(outCells, [&](const CellRef output) {
          return core_.hasPipeline(output);
        })) {
      return {.success = false,
              .errorMessage =
                  "ProbeLink/VqlFieldProbe do not support parameter pipelines"};
    }
    const auto isTypedSlot = [this](const CellRef slot) {
      return streamSlots_.contains(slot) || iteratorSlots_.contains(slot);
    };
    if (std::ranges::any_of(inputCells, isTypedSlot) ||
        std::ranges::any_of(outCells, isTypedSlot)) {
      return {.success = false,
              .errorMessage =
                  "ProbeLink/VqlFieldProbe require scalar reference slots"};
    }
    if (isMemoized(*op))
      return {.success      = false,
              .errorMessage = "ProbeLink/VqlFieldProbe cannot be memoized"};
  }
  if (maskLinksOp && std::ranges::any_of(inputCells, [&](const CellRef input) {
        return streamSlots_.contains(input) || iteratorSlots_.contains(input);
      }))
    return {.success       = false,
            .failingClause = *op,
            .errorMessage  = "VqlMaskLinks requires scalar reference slots"};
  if (kind == OpcodeKind::VqlString || kind == OpcodeKind::VqlAdd ||
      kind == OpcodeKind::VqlBoxScalar ||
      kind == OpcodeKind::CurrentTimestamp) {
    if (!core_.preconditionsOf(*op).empty() ||
        !core_.postconditionsOf(*op).empty()) {
      return {.success = false,
              .errorMessage =
                  "VQL scalar opcodes do not support pre/postconditions"};
    }
    if (std::ranges::any_of(
            inputCells,
            [&](const CellRef input) { return core_.hasPipeline(input); }) ||
        std::ranges::any_of(outCells, [&](const CellRef output) {
          return core_.hasPipeline(output);
        })) {
      return {.success      = false,
              .errorMessage = "VQL scalar opcodes do not support pipelines"};
    }
    if (isMemoized(*op))
      return {.success      = false,
              .errorMessage = "VQL scalar opcodes cannot be memoized"};
  }
  if (contractOp) {
    if (!core_.preconditionsOf(*op).empty() ||
        !core_.postconditionsOf(*op).empty()) {
      return {.success = false,
              .errorMessage =
                  "contract opcodes do not support pre/postconditions"};
    }
    if (std::ranges::any_of(
            inputCells,
            [&](const CellRef input) { return core_.hasPipeline(input); }) ||
        std::ranges::any_of(outCells, [&](const CellRef output) {
          return core_.hasPipeline(output);
        })) {
      return {.success      = false,
              .errorMessage = "contract opcodes do not support pipelines"};
    }
  }
  if (resumeStateOp) {
    if (!core_.preconditionsOf(*op).empty() ||
        !core_.postconditionsOf(*op).empty()) {
      return {.success = false,
              .errorMessage =
                  "resume-state opcodes do not support pre/postconditions"};
    }
    if (std::ranges::any_of(
            inputCells,
            [&](const CellRef input) { return core_.hasPipeline(input); }) ||
        std::ranges::any_of(outCells, [&](const CellRef output) {
          return core_.hasPipeline(output);
        })) {
      return {.success      = false,
              .errorMessage = "resume-state opcodes do not support pipelines"};
    }
    const auto isTypedSlot = [this](const CellRef slot) {
      return streamSlots_.contains(slot) || iteratorSlots_.contains(slot);
    };
    if (std::ranges::any_of(inputCells, isTypedSlot) ||
        std::ranges::any_of(outCells, isTypedSlot)) {
      return {.success = false,
              .errorMessage =
                  "resume-state opcodes require scalar arena slots"};
    }
    if (isMemoized(*op)) {
      return {.success      = false,
              .errorMessage = "resume-state opcodes cannot be memoized"};
    }
  }
  totalCycles_++;
  const bool streamRefOp =
      kind == OpcodeKind::CopyCellRefs || kind == OpcodeKind::ClearCellRefs ||
      kind == OpcodeKind::MatchContract || kind == OpcodeKind::RequireContract;
  const auto hasStateSlot = [this](const CellRef slot) {
    return streamSlots_.contains(slot) || iteratorSlots_.contains(slot);
  };
  const bool typedStreamUse =
      streamRefOp && (std::ranges::any_of(inputCells, hasStateSlot) ||
                      std::ranges::any_of(outCells, hasStateSlot));
  if (stateful && (!core_.preconditionsOf(*op).empty() ||
                   !core_.postconditionsOf(*op).empty())) {
    return {.success = false,
            .errorMessage =
                "stream and iterator opcodes do not support contracts"};
  }
  if (stateful || typedStreamUse) {
    if (std::ranges::any_of(
            inputCells,
            [&](const CellRef input) { return core_.hasPipeline(input); }) ||
        std::ranges::any_of(outCells, [&](const CellRef output) {
          return core_.hasPipeline(output);
        })) {
      return {.success = false,
              .errorMessage =
                  "stream and iterator opcodes do not support pipelines"};
    }
  }
  if (typedStreamUse) {
    try {
      requireStreamCursor(cursor);
    } catch (const std::exception &error) {
      return {.success = false, .errorMessage = error.what()};
    }
  }

  // 1. Precondition Evaluation (-d.contract)
  std::vector<CellRef> preconds = core_.preconditionsOf(*op);
  for (CellRef clause : preconds) {
    if (!evaluateCondition(clause, {}, {})) {
      return ExecutionResult{
          .success           = false,
          .contractViolation = ContractViolationKind::Precondition,
          .failingClause     = clause,
          .errorMessage      = "Precondition violation: assertion failed"};
    }
  }

  // 2. Input Parameter Preprocessing (+d.grab -> +d.spin)
  std::unordered_set<CellRef> processedIn;
  for (std::size_t index = 0; index < inputCells.size(); ++index) {
    const CellRef paramCell = inputCells[index];
    if (kind == OpcodeKind::ScopedChoice) continue;
    if (kind == OpcodeKind::BranchFalseCell && index == 1) continue;
    if (processedIn.insert(paramCell).second && core_.hasPipeline(paramCell)) {
      try {
        const auto pipelineResult = runPipeline(paramCell);
        if (!pipelineResult.success) {
          return pipelineResult;
        }
      } catch (const std::exception &error) {
        return {.success = false, .errorMessage = error.what()};
      }
    }
  }

  // 3. Snapshot-Before-Write
  std::vector<CellValue> oldInputs;
  oldInputs.reserve(inputCells.size());
  try {
    for (CellRef paramCell : inputCells) {
      if (stateful || kind == OpcodeKind::CopyCellRefs ||
          kind == OpcodeKind::ClearCellRefs ||
          kind == OpcodeKind::MatchContract ||
          kind == OpcodeKind::RequireContract ||
          kind == OpcodeKind::VqlCompare || kind == OpcodeKind::VqlString ||
          kind == OpcodeKind::VqlAdd || kind == OpcodeKind::VqlBoxScalar ||
          kind == OpcodeKind::CurrentTimestamp ||
          kind == OpcodeKind::ProbeLink || kind == OpcodeKind::VqlMaskLinks ||
          kind == OpcodeKind::ScopedChoice ||
          kind == OpcodeKind::VqlFieldProbe) {
        oldInputs.emplace_back(false);
      } else {
        oldInputs.push_back(inputValue(paramCell));
      }
    }
  } catch (const std::exception &error) {
    return {.success = false, .errorMessage = error.what()};
  }

  // 4. Memoization Check (d.cache)
  bool memoHit = false;
  std::vector<CellValue> newOutputs;

  // P15's typed refusal (spec §6.5/M10): a funnel write aimed at an
  // opcodeMap_-registered cell's clone master fails the step — the
  // executing op is the failing clause, matching the numeric faults.
  const auto kindImmutabilityFault = [](CellRef executing) {
    return ExecutionResult{
        .success           = false,
        .contractViolation = ContractViolationKind::KindImmutability,
        .failingClause     = executing,
        .errorMessage      = "P15: write to a registered opcode cell "
                             "refused (kind immutability)"};
  };

  // These operations return cell identities. Content-only memo keys cannot
  // preserve the identity of a current neighbor or an ephemeral value slice.
  const bool memoizable =
      kind != OpcodeKind::Link && kind != OpcodeKind::Break &&
      kind != OpcodeKind::Value && kind != OpcodeKind::MatchContract &&
      kind != OpcodeKind::RequireContract && kind != OpcodeKind::CopyCellRefs &&
      kind != OpcodeKind::ClearCellRefs &&
      kind != OpcodeKind::BranchFalseCell && kind != OpcodeKind::VqlCompare &&
      kind != OpcodeKind::VqlString && kind != OpcodeKind::VqlAdd &&
      kind != OpcodeKind::VqlBoxScalar &&
      kind != OpcodeKind::CurrentTimestamp &&
      kind != OpcodeKind::ScopedChoice && kind != OpcodeKind::ResumeInit &&
      kind != OpcodeKind::ResumeSet && kind != OpcodeKind::ProbeLink &&
      kind != OpcodeKind::VqlMaskLinks && kind != OpcodeKind::VqlFieldProbe &&
      !stateful;
  if (memoizable && isMemoized(*op)) {
    if (const auto key = getMemoKey(*op)) {
      CellRef pin = core_.getOrCreateMemoPin(*key);
      auto cached = core_.lookupMemo(pin, oldInputs);
      if (cached.has_value()) {
        memoHit    = true;
        newOutputs = *cached;
        if (kind == OpcodeKind::Link || kind == OpcodeKind::Break ||
            kind == OpcodeKind::Value || kind == OpcodeKind::CopyCellRefs ||
            kind == OpcodeKind::ClearCellRefs) {
          writeCellReferences(outCells, newOutputs);
        } else {
          for (std::size_t i = 0;
               i < std::min(outCells.size(), newOutputs.size()); ++i) {
            clearTypedRegister(outCells[i]);
            cellReferences_.erase(outCells[i]);
            WriteFault fault = WriteFault::None;
            std::ignore      = core_.writeValue(outCells[i], 0, std::nullopt,
                                                newOutputs[i], &fault);
            if (fault == WriteFault::OpcodeKind) {
              return kindImmutabilityFault(*op);
            }
          }
        }
      }
    }
  }

  // 5. Opcode Body Execution & Output Write
  std::optional<Mark> boxScalarMark;
  std::optional<CellRef> boxScalarOutput;
  bool boxReferenceSnapshotTaken = false;
  bool hadBoxOutputReference     = false;
  std::optional<CellRef> priorBoxOutputReference;
  if (kind == OpcodeKind::VqlBoxScalar ||
      kind == OpcodeKind::CurrentTimestamp) {
    if (outCells.size() != 1 || !core_.arena().contains(outCells.front()) ||
        streamSlots_.contains(outCells.front()) ||
        iteratorSlots_.contains(outCells.front())) {
      return {.success       = false,
              .failingClause = *op,
              .errorMessage =
                  "scalar-producing opcode output must be one arena slot"};
    }
    boxScalarOutput = outCells.front();
    try {
      boxScalarMark = core_.arena().mark();
    } catch (const std::exception &error) {
      return {
          .success = false, .failingClause = *op, .errorMessage = error.what()};
    }
  }
  const auto rollbackBoxScalar = [&] {
    if (!boxScalarMark) return;
    core_.arena().release(*boxScalarMark);
    boxScalarMark.reset();
    if (boxReferenceSnapshotTaken) {
      if (hadBoxOutputReference) {
        if (const auto found = cellReferences_.find(*boxScalarOutput);
            found != cellReferences_.end()) {
          found->second = priorBoxOutputReference;
        }
      } else {
        cellReferences_.erase(*boxScalarOutput);
      }
    }
  };
  bool jumped = false;
  if (!memoHit) {
    ExecutionResult execRes;
    try {
      execRes =
          executeOpcodeBody(kind, *op, oldInputs, newOutputs, cursor, jumped);
    } catch (const std::exception &error) {
      rollbackBoxScalar();
      return {.success = false, .errorMessage = error.what()};
    }
    if (!execRes.success) {
      rollbackBoxScalar();
      return execRes;
    }

    // Structural reads and value reads return cell handles. Preserve identity
    // so later operations consume the target cell, not the result slot.
    if (kind == OpcodeKind::MatchContract || kind == OpcodeKind::ResumeInit ||
        kind == OpcodeKind::ResumeSet || kind == OpcodeKind::VqlFieldProbe ||
        kind == OpcodeKind::VqlMaskLinks) {
      // These handlers atomically publish their reference or scalar outputs.
    } else if (stateful) {
      // Stateful registers write their runtime state and output refs directly.
    } else if (kind == OpcodeKind::Link || kind == OpcodeKind::Break ||
               kind == OpcodeKind::Value || kind == OpcodeKind::CopyCellRefs ||
               kind == OpcodeKind::ClearCellRefs ||
               kind == OpcodeKind::ProbeLink ||
               kind == OpcodeKind::VqlBoxScalar ||
               kind == OpcodeKind::CurrentTimestamp) {
      if (kind == OpcodeKind::VqlBoxScalar ||
          kind == OpcodeKind::CurrentTimestamp) {
        if (const auto found = cellReferences_.find(*boxScalarOutput);
            found != cellReferences_.end()) {
          hadBoxOutputReference   = true;
          priorBoxOutputReference = found->second;
        }
        boxReferenceSnapshotTaken = true;
        try {
          writeCellReferences(outCells, newOutputs);
          core_.arena().discard(*boxScalarMark);
          boxScalarMark.reset();
        } catch (const std::exception &error) {
          rollbackBoxScalar();
          return {.success       = false,
                  .failingClause = *op,
                  .errorMessage  = error.what()};
        }
      } else {
        writeCellReferences(outCells, newOutputs,
                            kind == OpcodeKind::CopyCellRefs ||
                                kind == OpcodeKind::ClearCellRefs);
      }
    } else if (kind != OpcodeKind::New && kind != OpcodeKind::Clone &&
               kind != OpcodeKind::Linked) {
      for (std::size_t i = 0; i < std::min(outCells.size(), newOutputs.size());
           ++i) {
        clearTypedRegister(outCells[i]);
        cellReferences_.erase(outCells[i]);
        WriteFault fault = WriteFault::None;
        std::ignore      = core_.writeValue(outCells[i], 0, std::nullopt,
                                            newOutputs[i], &fault);
        if (fault == WriteFault::OpcodeKind) {
          return kindImmutabilityFault(*op);
        }
      }
    }

    // 6. Output Parameter Postprocessing (-d.grab -> +d.spin)
    if (!stateful) {
      std::unordered_set<CellRef> processedOut;
      for (CellRef outCell : outCells) {
        if (processedOut.insert(outCell).second && core_.hasPipeline(outCell)) {
          try {
            const auto pipelineResult = runPipeline(outCell);
            if (!pipelineResult.success) {
              return pipelineResult;
            }
          } catch (const std::exception &error) {
            return {.success = false, .errorMessage = error.what()};
          }
        }
      }
    }
  }

  // 7. Postcondition Evaluation (+d.contract)
  std::vector<CellValue> finalOutputs;
  if (!stateful) {
    finalOutputs.reserve(outCells.size());
    for (CellRef outCell : outCells) {
      finalOutputs.push_back(core_.render(outCell).value());
    }
  }

  std::vector<CellRef> postconds = core_.postconditionsOf(*op);
  for (CellRef clause : postconds) {
    if (!evaluateCondition(clause, oldInputs, finalOutputs)) {
      return ExecutionResult{
          .success           = false,
          .contractViolation = ContractViolationKind::Postcondition,
          .failingClause     = clause,
          .errorMessage      = "Postcondition violation: assertion failed"};
    }
  }

  if (stagedParticipants) mutationGroups_.swap(*stagedParticipants);

  // 8. Memoization Record (on Miss)
  if (!memoHit && memoizable) {
    if (const auto key = getMemoKey(*op)) {
      CellRef pin = core_.getOrCreateMemoPin(*key);
      core_.recordMemo(pin, oldInputs, finalOutputs);
    }
  }

  // 9. Advance Cursor
  if (!jumped) {
    const auto nextOp = core_.arena().linked(*op, core_.dims().spin, false);
    if (!nextOp && activationDepth(cursor) != 0)
      return {.success = false,
              .errorMessage =
                  "activation-frame body ended without ReturnFrame"};
    if (!nextOp) {
      const auto guards = guardStacks_.find(cursor);
      if (guards != guardStacks_.end() && guards->second &&
          !guards->second->empty())
        return {.success       = false,
                .failingClause = *op,
                .errorMessage  = "cursor finished with an active guard"};
      if (mutationGroupActive(cursor))
        return {.success       = false,
                .failingClause = *op,
                .errorMessage =
                    "cursor finished with an active mutation group"};
      if (guardScopes_.contains(cursor))
        return {.success       = false,
                .failingClause = *op,
                .errorMessage  = "cursor finished with an active guard scope"};
    }
    setCursorOpcode(cursor, nextOp);
  }

  return ExecutionResult{.success           = true,
                         .contractViolation = ContractViolationKind::None,
                         .failingClause     = std::nullopt,
                         .errorMessage      = ""};
}

ExecutionResult VortexVM::prepareTypeContract(OpcodeKind kind, CellRef opcode) {
  if (kind != OpcodeKind::MatchContract) return {.success = true};
  const auto slots = outputsOf(opcode);
  if (slots.empty()) {
    return {.success = false, .errorMessage = "invalid contract output slots"};
  }
  try {
    for (const auto slot : std::span(slots).subspan(1)) {
      if (const auto found = cellReferences_.find(slot);
          found != cellReferences_.end()) {
        found->second.reset();
      } else {
        cellReferences_.emplace(slot, std::nullopt);
      }
    }
    const bool invalidOutput =
        std::ranges::any_of(slots, [this](const CellRef slot) {
          return !core_.arena().contains(slot) || streamSlots_.contains(slot) ||
                 iteratorSlots_.contains(slot);
        });
    if (invalidOutput) {
      return {.success = false,
              .errorMessage =
                  "contract output slots must be scalar arena cells"};
    }
    cellReferences_.erase(slots.front());
    clearTypedRegister(slots.front());
    core_.writeValue(slots.front(), 0, std::nullopt, CellValue{false});
  } catch (const std::exception &error) {
    for (const auto slot : std::span(slots).subspan(1)) {
      if (const auto found = cellReferences_.find(slot);
          found != cellReferences_.end())
        found->second.reset();
    }
    return {.success = false, .errorMessage = error.what()};
  }
  return {.success = true};
}

ExecutionResult VortexVM::run(CellRef cursor, std::size_t maxCycles,
                              std::stop_token stopToken) {
  if (hasScopedChoices() && !scopedChoicesOwnedBy(cursor)) {
    return {.success = false,
            .errorMessage =
                "execution cursor does not match the active scoped choice"};
  }
  for (std::size_t i = 0; i < maxCycles; ++i) {
    if (stopToken.stop_requested()) {
      abortOwnedMutationGroups(cursor, std::nullopt);
      return ExecutionResult{.success           = false,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = std::nullopt,
                             .errorMessage      = "Cancelled by cut"};
    }
    const auto op = getCursorOpcode(cursor);
    if (!op) {
      if (mutationGroupActive(cursor)) {
        abortOwnedMutationGroups(cursor, std::nullopt);
        return {.success = false,
                .errorMessage =
                    "cursor finished with an active mutation group"};
      }
      if (hasScopedChoices()) {
        return {.success      = false,
                .errorMessage = "cursor finished with an active scoped choice"};
      }
      if (activationDepth(cursor) != 0)
        return {.success = false,
                .errorMessage =
                    "activation frame lost its cursor continuation"};
      if (const auto guards = guardStacks_.find(cursor);
          guards != guardStacks_.end() && guards->second &&
          !guards->second->empty())
        return {.success      = false,
                .errorMessage = "cursor finished with an active guard"};
      if (pendingGuardChecks_.contains(cursor))
        return {.success = false,
                .errorMessage =
                    "cursor finished with pending guard validation"};
      if (guardScopes_.contains(cursor))
        return {.success      = false,
                .errorMessage = "cursor finished with an active guard scope"};
      return ExecutionResult{.success           = true,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = std::nullopt,
                             .errorMessage      = "Finished"};
    }
    auto kind = getOpcodeKind(*op);
    if (kind && *kind == OpcodeKind::Halt) {
      if (mutationGroupActive(cursor)) {
        abortOwnedMutationGroups(cursor, std::nullopt);
        return {.success      = false,
                .errorMessage = "Halt reached with an active mutation group"};
      }
      if (hasScopedChoices()) {
        return {.success      = false,
                .errorMessage = "Halt reached with an active scoped choice"};
      }
      if (activationDepth(cursor) != 0)
        return {.success      = false,
                .errorMessage = "Halt reached with an active activation frame"};
      if (const auto guards = guardStacks_.find(cursor);
          guards != guardStacks_.end() && guards->second &&
          !guards->second->empty())
        return {.success      = false,
                .errorMessage = "Halt reached with an active guard"};
      if (pendingGuardChecks_.contains(cursor))
        return {.success      = false,
                .errorMessage = "Halt reached with pending guard validation"};
      if (guardScopes_.contains(cursor))
        return {.success      = false,
                .errorMessage = "Halt reached with an active guard scope"};
      return ExecutionResult{.success           = true,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = std::nullopt,
                             .errorMessage      = "Halted"};
    }
    auto res = step(cursor);
    if (!res.success) {
      return res;
    }
  }
  return ExecutionResult{.success           = false,
                         .contractViolation = ContractViolationKind::None,
                         .failingClause     = std::nullopt,
                         .errorMessage      = "Exceeded max cycles"};
}

std::size_t VortexVM::stepScheduler(std::stop_token stopToken) {
  if (stopToken.stop_requested()) {
    for (const auto cursor : activeCursors())
      abortOwnedMutationGroups(cursor, std::nullopt);
    return 0;
  }
  std::size_t executed         = 0;
  std::vector<CellRef> cursors = activeCursors();

  // Stage 0 instrumentation (vortex-multithreading-study.md §3.6): time one
  // cooperative scheduler tick over all active cursors. Because every cursor
  // steps serially over the one shared arena, this wall time grows linearly
  // with the cursor count -- the measurement that proves the single arena is
  // the serialization point. The guard skips the clock reads when the
  // category is disabled, so an instrumented build pays nothing by default.
  static const auto schedLogger = gleditor::logging::category("vortex.sched");
  const bool trace              = schedLogger->should_log(spdlog::level::trace);
  const auto tickStart          = trace ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};

  for (CellRef cursor : cursors) {
    if (stopToken.stop_requested()) {
      for (const auto active : cursors)
        abortOwnedMutationGroups(active, std::nullopt);
      break;
    }
    const auto op = getCursorOpcode(cursor);
    if (op) {
      auto res = step(cursor);
      if (res.success) {
        executed++;
      }
    }
  }

  if (trace) {
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - tickStart)
                               .count();
    schedLogger->trace(
        "tick: cursors={} executed={} elapsed_ns={} ns_per_cursor={}",
        cursors.size(), executed, elapsedNs,
        cursors.empty() ? 0
                        : elapsedNs / static_cast<long long>(cursors.size()));
  }

  return executed;
}

} // namespace zigzag::vortex
