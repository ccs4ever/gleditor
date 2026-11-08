#include "common/xanadu/vortex/program_export.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

std::optional<std::string> boundedArenaText(const ArenaManifold &arena,
                                            const CellRef cell,
                                            const std::size_t limit) {
  const auto spans = arena.contentOf(cell);
  if (spans.size() > limit) return std::nullopt;
  std::size_t bytes = 0;
  for (const auto &span : spans) {
    if (span.length > limit - std::min(bytes, limit)) return std::nullopt;
    bytes += static_cast<std::size_t>(span.length);
  }
  const auto text = arena.textOf(cell);
  if (text.size() != bytes) return std::nullopt;
  return text;
}

void addFrameReferences(const VortexVM &vm, const CellRef descriptor,
                        std::map<CellRef, CellRef> &references,
                        std::vector<CellRef> &instructions) {
  const auto parsed = vm.frameDescriptor(descriptor);
  if (!parsed) throw std::runtime_error(parsed.error().message);
  for (const auto &[record, target] : parsed->referenceCells)
    references.emplace(record, target);
  if (parsed->entry) instructions.push_back(*parsed->entry);
}

void collectClosureReferences(const VortexCore &core, const VortexVM &vm,
                              const CellRef root,
                              std::map<CellRef, CellRef> &references,
                              std::vector<CellRef> &instructions) {
  const auto dimension = core.dims().runtimeObjectClaim;
  if (!dimension)
    throw std::runtime_error("Vortex runtime-object dimension is unavailable");
  const auto metadataBytes = vm.closureLimits().maxMetadataBytes;
  const auto claim         = zigzag::classifyRuntimeObjectRoot(
      core.arena(), root, *dimension,
      {.maxRootRelations = 64,
               .maxMappingWork   = metadataBytes > SIZE_MAX / 4
                                       ? SIZE_MAX
                                       : std::max<std::size_t>(4096, metadataBytes * 4)});
  if (claim != zigzag::RuntimeObjectClaimResult::Claimed)
    throw std::runtime_error("export found a malformed claimed runtime object");
  const auto tagCell = core.arena().linked(root, *dimension, DimVector::POS);
  const auto tag     = tagCell
                           ? boundedArenaText(core.arena(), *tagCell, metadataBytes)
                           : std::nullopt;
  if (!tag)
    throw std::runtime_error("export found an unreadable runtime-object tag");
  if (*tag == "VORTEX_CLOSURE_DESCRIPTOR/1") {
    const auto parsed = vm.closureDescriptor(root);
    if (!parsed) throw std::runtime_error(parsed.error().message);
    for (const auto &[record, target] : parsed->referenceCells)
      references.emplace(record, target);
    if (parsed->frameDescriptor)
      addFrameReferences(vm, *parsed->frameDescriptor, references,
                         instructions);
    return;
  }
  if (*tag == "VORTEX_CLOSURE_VALUE/1") {
    const auto parsed = vm.closureValue(root);
    if (!parsed) throw std::runtime_error(parsed.error().message);
    for (const auto &[record, target] : parsed->referenceCells)
      references.emplace(record, target);
    const auto descriptor = vm.closureDescriptor(*parsed->descriptor);
    if (!descriptor) throw std::runtime_error(descriptor.error().message);
    if (descriptor->frameDescriptor)
      addFrameReferences(vm, *descriptor->frameDescriptor, references,
                         instructions);
    return;
  }
  if (*tag == "VORTEX_BINDINGS/1") {
    const auto parsed = vm.bindingsDescriptor(root);
    if (!parsed) throw std::runtime_error(parsed.error().message);
    for (const auto &[record, target] : parsed->referenceCells)
      references.emplace(record, target);
    return;
  }
  throw std::runtime_error(
      "export found an unknown claimed runtime-object tag");
}

} // namespace

xanadu::MicroversionId
exportProgram(const VortexCore &core, const VortexVM &vm,
              std::span<const CellRef> opcodes, xanadu::Store &store,
              std::optional<xanadu::MicroversionId> parent) {
  using zigzag::CellRef;
  using zigzag::DimRef;
  using zigzag::DimVector;
  using zigzag::vortex::OpcodeKind;
  const auto &arena = core.arena();
  if (arena.base())
    throw std::runtime_error(
        "export of a base-backed query requires a source binding");
  if (vm.hasActiveGuards() ||
      std::ranges::any_of(vm.choiceStack(), [](const auto &choice) {
        return !choice.guardStacks.empty() ||
               !choice.pendingGuardChecks.empty() ||
               !choice.guardScopes.empty() || !choice.mutationGroups.empty();
      }))
    throw std::runtime_error("export of live or restorable guards/groups "
                             "requires persisted runtime state");
  if (vm.hasActiveFrames())
    throw std::runtime_error(
        "export of a live activation frame requires persisted runtime state");

  const auto originalEntry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  const auto bindings    = vm.snapshotBindings(originalEntry);
  const bool hasBindings = !bindings.cellReferences.empty() ||
                           !bindings.streams.empty() ||
                           !bindings.iterators.empty();
  if (hasBindings && !originalEntry)
    throw std::runtime_error(
        "runtime bindings require an executable home.spin entry");
  if (hasBindings && !core.dims().runtimeObjectClaim)
    throw std::runtime_error("Vortex runtime-object dimension is unavailable");

  if (std::ranges::any_of(vm.choiceStack(),
                          [](const auto &choice) { return choice.scoped; }))
    throw std::runtime_error(
        "export of a live scoped choice requires persisted runtime state");

  // Validate before appending operations. A dimension operand is a reference
  // encoded as a scalar, whereas ordinary integer payloads must stay integers.
  std::map<CellRef, DimRef> dimensionOperands;
  std::map<CellRef, CellRef> referenceOperands;
  std::vector<CellRef> instructions(opcodes.begin(), opcodes.end());
  for (const auto &cell : arena.cells()) {
    if (const auto claimDimension = core.dims().runtimeObjectClaim;
        claimDimension &&
        arena.linked(cell.birthOp, *claimDimension, DimVector::POS))
      collectClosureReferences(core, vm, cell.birthOp, referenceOperands,
                               instructions);
  }
  std::set<CellRef> visitedInstructions;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto opcode = instructions[index];
    arena.consumeWork();
    if (!visitedInstructions.insert(opcode).second) continue;
    const auto kind = vm.getOpcodeKind(opcode);
    if (!kind) throw std::runtime_error("cannot export an unknown opcode");
    // Frame entries may be disconnected from the submitted main chain. Only
    // declared code edges extend the walk; ordinary input values are data.
    if (const auto next =
            arena.linked(opcode, core.dims().spin, DimVector::POS))
      instructions.push_back(*next);
    for (const auto condition : core.preconditionsOf(opcode))
      instructions.push_back(condition);
    for (const auto condition : core.postconditionsOf(opcode))
      instructions.push_back(condition);
    if (*kind == OpcodeKind::ScopedChoice) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.size() != 1 || !core.outputsOf(opcode).empty() ||
          !vm.getOpcodeKind(inputs.front()))
        throw std::runtime_error("invalid scoped choice graph target");
      instructions.push_back(inputs.front());
    }
    if (*kind == OpcodeKind::BranchFalseCell) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.size() != 2 || !vm.getOpcodeKind(inputs[1]))
        throw std::runtime_error("invalid cell branch graph target");
      instructions.push_back(inputs[1]);
    }
    if (*kind == OpcodeKind::Jump || *kind == OpcodeKind::Branch ||
        *kind == OpcodeKind::Call || *kind == OpcodeKind::Choice)
      throw std::runtime_error(
          "numeric control targets require relocation before export");
    for (const auto side : {DimVector::POS, DimVector::NEG}) {
      auto slot = arena.linked(opcode, core.dims().grab, side);
      std::set<CellRef> seen;
      while (slot) {
        if (!seen.insert(*slot).second)
          throw std::runtime_error("cyclic opcode parameter wing");
        const auto target = arena.asInt64(*slot);
        if (!target || *target < 0 ||
            static_cast<std::uint64_t>(*target) >
                std::numeric_limits<CellRef>::max() ||
            !arena.contains(static_cast<CellRef>(*target)))
          throw std::runtime_error(
              "opcode parameter slot has no relocatable target");
        referenceOperands.emplace(*slot, static_cast<CellRef>(*target));
        if (const auto pipeline =
                core.getPipelineHead(static_cast<CellRef>(*target)))
          instructions.push_back(*pipeline);
        slot = arena.linked(*slot, core.dims().step, DimVector::POS);
      }
    }
    if (*kind == OpcodeKind::InvokeFrame || *kind == OpcodeKind::ReturnFrame ||
        *kind == OpcodeKind::PushGuard) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.empty())
        throw std::runtime_error(
            "frame or guard opcode has no descriptor operand");
      addFrameReferences(vm, inputs.front(), referenceOperands, instructions);
    }
    if (*kind == OpcodeKind::Link || *kind == OpcodeKind::New ||
        *kind == OpcodeKind::Break || *kind == OpcodeKind::RankBegin ||
        *kind == OpcodeKind::ProbeLink) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.size() < 2)
        throw std::runtime_error("structural opcode has no dimension operand");
      const auto value = arena.asInt64(inputs[1]);
      if (!value || *value < 0 ||
          static_cast<std::uint64_t>(*value) >
              std::numeric_limits<DimRef>::max() ||
          !arena.contains(static_cast<DimRef>(*value)))
        throw std::runtime_error(
            "structural dimension operand is not relocatable");
      dimensionOperands.emplace(inputs[1], static_cast<DimRef>(*value));
    }
    if (*kind == OpcodeKind::VqlMaskLinks) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.size() < 2 || (inputs.size() - 2) % 2 != 0 ||
          !core.outputsOf(opcode).empty())
        throw std::runtime_error("invalid VqlMaskLinks operand layout");
      for (std::size_t index = 2; index < inputs.size(); index += 2) {
        const auto value = arena.asInt64(inputs[index]);
        if (!value || *value < 0 ||
            static_cast<std::uint64_t>(*value) >=
                std::numeric_limits<DimRef>::max() ||
            !arena.contains(static_cast<DimRef>(*value)))
          throw std::runtime_error(
              "VqlMaskLinks dimension operand is not relocatable");
        const auto bits = arena.asInt64(inputs[index + 1]);
        if (!bits || *bits < 0 || *bits > 3)
          throw std::runtime_error("invalid VqlMaskLinks keep bits");
        dimensionOperands.emplace(inputs[index], static_cast<DimRef>(*value));
      }
    }
  }
  for (const auto &[operand, axis] : dimensionOperands)
    referenceOperands.emplace(operand, axis);
  for (const auto cursor : vm.activeCursors())
    if (const auto target = vm.getCursorOpcode(cursor))
      referenceOperands.emplace(cursor, *target);
  std::set<DimRef> axes;
  for (const auto &cell : arena.cells()) {
    switch (static_cast<xanadu::ValueKind>(cell.valueKind)) {
    case xanadu::ValueKind::None:
    case xanadu::ValueKind::Double:
    case xanadu::ValueKind::Bool:
    case xanadu::ValueKind::Int64:
    case xanadu::ValueKind::Timestamp:
      break;
    default:
      throw std::runtime_error("export of handle values requires relocation");
    }
    if (arena.isProxy(cell.birthOp))
      throw std::runtime_error(
          "export of foreign query cells requires a source binding");
    for (const auto &link : arena.dimensionsOf(cell.birthOp))
      axes.insert(link.dim);
  }
  for (const auto &[operand, axis] : dimensionOperands) {
    (void)operand;
    axes.insert(axis);
  }
  axes.insert(core.dims().grab);
  axes.insert(core.dims().step);
  axes.insert(core.dims().spin);
  if (core.dims().runtimeObjectClaim)
    axes.insert(*core.dims().runtimeObjectClaim);
  for (const auto axis : axes)
    if (!arena.contains(axis) || arena.textOf(axis).empty())
      throw std::runtime_error(
          "export dimension has no local identity or name");

  // Check every reference and edge before the first Store mutation. Runtime
  // register values are explicit descriptor records; integer payloads remain
  // ordinary data unless the graph schema declares them as references.
  for (const auto &[source, target] : referenceOperands)
    if (!arena.contains(source) || !arena.contains(target))
      throw std::runtime_error("export reference has an unmapped endpoint");
  for (const auto &[source, target] : bindings.cellReferences) {
    if (!arena.contains(source) || (target && !arena.contains(*target)))
      throw std::runtime_error("runtime binding has an unmapped endpoint");
  }
  for (const auto &[slot, members] : bindings.streams) {
    if (!arena.contains(slot) ||
        !std::ranges::all_of(members, [&](const CellRef value) {
          return arena.contains(value);
        }))
      throw std::runtime_error("runtime stream has an unmapped member");
  }
  for (const auto &[slot, _] : bindings.iterators)
    if (!arena.contains(slot))
      throw std::runtime_error("runtime iterator has an unmapped slot");
  if (hasBindings) {
    const auto limits     = vm.bindingsLimits();
    std::size_t records   = 0;
    const auto addRecords = [&](const std::size_t count) {
      if (count > limits.maxDescriptorRecords -
                      std::min(records, limits.maxDescriptorRecords))
        throw std::runtime_error(
            "runtime binding descriptor exceeds the record limit");
      records += count;
    };
    addRecords(bindings.cellReferences.size());
    addRecords(bindings.streams.size());
    addRecords(bindings.iterators.size());
    addRecords(bindings.cellReferences.size());
    for (const auto &[_, members] : bindings.streams) {
      addRecords(members.size());
    }
    if (records > limits.maxDescriptorRecords ||
        limits.maxMetadataRelations < 2 ||
        limits.maxMetadataBytes < zigzag::runtimeObjectProbeText.size() ||
        limits.maxMetadataBytes < std::string_view{"VORTEX_BINDINGS/1"}.size())
      throw std::runtime_error(
          "runtime binding descriptor exceeds metadata limits");
    for (const auto role :
         {"ref.cell.slot", "ref.cell.target", "ref.stream.slot",
          "ref.stream.member", "ref.iterator.unavailable",
          "ref.iterator.exhausted"})
      if (std::string_view{role}.size() > limits.maxMetadataBytes)
        throw std::runtime_error(
            "runtime binding descriptor role exceeds the metadata limit");
  }
  for (const auto &cell : arena.cells())
    for (const auto &edge : arena.dimensionsOf(cell.birthOp))
      if (const auto target = edge.neighbor(DimVector::POS);
          target && !arena.contains(*target))
        throw std::runtime_error("export edge has an unmapped endpoint");

  auto version =
      parent.value_or(store.structureHead().value_or(store.latest()));
  if (parent && store.homeCell() && !store.rebuildManifold(*parent).home())
    throw std::runtime_error("export parent does not contain the store origin");
  if (!store.homeCell()) version = store.sliceGenesis(version);
  auto manifold = store.rebuildManifold(version);
  std::map<CellRef, CellRef> cells{{core.home(), *store.homeCell()}};
  std::map<DimRef, DimRef> dimensions;
  for (const auto axis : axes) {
    const auto persisted = zigzag::DimensionRegistry::instance().getOrCreate(
        store, version, manifold, arena.textOf(axis));
    if (!persisted)
      throw std::runtime_error("cannot persist an export dimension");
    dimensions.emplace(axis, *persisted);
    cells.emplace(axis, *persisted);
  }
  // Arena slots carry full CellRefs; dense slot positions are never cell IDs.
  for (const auto &slot : arena.cells()) {
    const auto source = slot.birthOp;
    if (cells.contains(source)) continue;
    version = store.makeCell(version, std::string_view{});
    cells.emplace(source, *store.cellRefOf(version));
  }
  // Content and scalar bits are independent halves. Minting from text infers
  // numbers, while scalar minting canonicalizes display text; neither preserves
  // an arena slot exactly. Restate both halves even for the mapped root/axes.
  for (const auto &slot : arena.cells()) {
    const auto source  = slot.birthOp;
    const auto content = store.userPermascroll().append(arena.textOf(source));
    version            = store.setValue(version, cells.at(source), content,
                                        static_cast<xanadu::ValueKind>(slot.valueKind),
                                        slot.valueBits);
  }
  // Parameter wings and cursor PCs use explicit reference slots. Relocate
  // them only after every target has a persistent birth, including cycles.
  for (const auto &[source, target] : referenceOperands) {
    const auto content = store.userPermascroll().append(arena.textOf(source));
    version            = store.setValue(version, cells.at(source), content,
                                        xanadu::ValueKind::Int64,
                                        static_cast<std::uint64_t>(cells.at(target)));
  }

  std::optional<CellRef> persistedBindings;
  if (hasBindings) {
    const auto claimAxis = dimensions.at(*core.dims().runtimeObjectClaim);
    const auto grabAxis  = dimensions.at(core.dims().grab);
    const auto stepAxis  = dimensions.at(core.dims().step);
    const auto rootText =
        store.userPermascroll().append(zigzag::runtimeObjectProbeText);
    version            = store.makeCell(version, rootText);
    persistedBindings  = *store.cellRefOf(version);
    version            = store.setValue(version, *persistedBindings, rootText,
                                        xanadu::ValueKind::None, 0);
    const auto tagText = store.userPermascroll().append("VORTEX_BINDINGS/1");
    version            = store.makeCell(version, tagText);
    const auto tagCell = *store.cellRefOf(version);
    version =
        store.setValue(version, tagCell, tagText, xanadu::ValueKind::None, 0);
    version = store.setLink(version, *persistedBindings, claimAxis,
                            DimVector::POS, tagCell);

    std::optional<CellRef> lastRecord;
    auto addRecord = [&](const std::string_view role,
                         const std::int64_t value) {
      const auto roleText = store.userPermascroll().append(role);
      version             = store.makeCell(version, roleText);
      const auto record   = *store.cellRefOf(version);
      version =
          store.setValue(version, record, roleText, xanadu::ValueKind::Int64,
                         static_cast<std::uint64_t>(value));
      version = store.setLink(version, lastRecord.value_or(*persistedBindings),
                              lastRecord ? stepAxis : grabAxis, DimVector::POS,
                              record);
      lastRecord = record;
    };
    for (const auto &[slot, target] : bindings.cellReferences) {
      addRecord("ref.cell.slot", static_cast<std::int64_t>(cells.at(slot)));
      addRecord("ref.cell.target",
                target ? static_cast<std::int64_t>(cells.at(*target)) : -1);
    }
    for (const auto &[slot, members] : bindings.streams) {
      addRecord("ref.stream.slot", static_cast<std::int64_t>(cells.at(slot)));
      for (const auto member : members)
        addRecord("ref.stream.member",
                  static_cast<std::int64_t>(cells.at(member)));
    }
    for (const auto &[slot, state] : bindings.iterators)
      addRecord(state == ClosedIteratorState::Unavailable
                    ? "ref.iterator.unavailable"
                    : "ref.iterator.exhausted",
                static_cast<std::int64_t>(cells.at(slot)));

    const auto emptyText     = store.userPermascroll().append("");
    version                  = store.makeCell(version, emptyText);
    const auto restoreOpcode = *store.cellRefOf(version);
    version                  = store.setValue(
        version, restoreOpcode, emptyText, xanadu::ValueKind::Int64,
        static_cast<std::uint64_t>(OpcodeKind::RestoreBindings));
    version            = store.makeCell(version, emptyText);
    const auto operand = *store.cellRefOf(version);
    version =
        store.setValue(version, operand, emptyText, xanadu::ValueKind::Int64,
                       static_cast<std::uint64_t>(*persistedBindings));
    version = store.setLink(version, restoreOpcode, grabAxis, DimVector::POS,
                            operand);
    version = store.setLink(version, *store.homeCell(),
                            dimensions.at(core.dims().spin), DimVector::POS,
                            restoreOpcode);
    version =
        store.setLink(version, restoreOpcode, dimensions.at(core.dims().spin),
                      DimVector::POS, cells.at(*originalEntry));
  }
  for (const auto &slot : arena.cells()) {
    const auto source = slot.birthOp;
    for (const auto &edge : arena.dimensionsOf(source)) {
      if (hasBindings && source == core.home() &&
          edge.dim == core.dims().spin &&
          edge.neighbor(DimVector::POS) == originalEntry)
        continue;
      if (const auto target = edge.neighbor(DimVector::POS)) {
        if (!cells.contains(*target))
          throw std::runtime_error("export edge refers to an unmapped cell");
        version =
            store.setLink(version, cells.at(source), dimensions.at(edge.dim),
                          DimVector::POS, cells.at(*target));
      }
    }
  }
  return version;
}
} // namespace zigzag::vortex
