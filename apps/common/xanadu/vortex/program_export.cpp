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
    if (*kind == OpcodeKind::ApplyClosure) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.empty())
        throw std::runtime_error("ApplyClosure has no closure handle input");
      if (const auto bound = vm.cellReferenceOf(inputs.front());
          bound && *bound != inputs.front()) {
        const auto closure = vm.closureValue(*bound);
        if (!closure)
          throw std::runtime_error(
              "ApplyClosure handle binding is not a valid closure value");
        throw std::runtime_error(
            "export of a runtime-bound ApplyClosure handle requires a direct "
            "closure graph value or persisted runtime binding");
      }
    }
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
    if (const auto target = vm.cellReferenceOf(cell.birthOp))
      referenceOperands.emplace(cell.birthOp, *target);
    for (const auto &link : arena.dimensionsOf(cell.birthOp))
      axes.insert(link.dim);
  }
  for (const auto &[operand, axis] : dimensionOperands) {
    (void)operand;
    axes.insert(axis);
  }
  for (const auto axis : axes)
    if (!arena.contains(axis) || arena.textOf(axis).empty())
      throw std::runtime_error(
          "export dimension has no local identity or name");

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
    const auto source = slot.birthOp;
    if (referenceOperands.contains(source)) continue;
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
  for (const auto &slot : arena.cells()) {
    const auto source = slot.birthOp;
    for (const auto &edge : arena.dimensionsOf(source)) {
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
