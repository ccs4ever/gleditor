#include "common/xanadu/vortex/program_export.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace zigzag::vortex {

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
  for (const auto opcode : opcodes) {
    const auto kind = vm.getOpcodeKind(opcode);
    if (!kind) throw std::runtime_error("cannot export an unknown opcode");
    if (*kind == OpcodeKind::ScopedChoice) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.size() != 1 || !core.outputsOf(opcode).empty() ||
          !vm.getOpcodeKind(inputs.front()))
        throw std::runtime_error("invalid scoped choice graph target");
    }
    if (*kind == OpcodeKind::Jump || *kind == OpcodeKind::Branch ||
        *kind == OpcodeKind::Call)
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
        slot = arena.linked(*slot, core.dims().step, DimVector::POS);
      }
    }
    if (*kind == OpcodeKind::InvokeFrame || *kind == OpcodeKind::ReturnFrame ||
        *kind == OpcodeKind::PushGuard) {
      const auto inputs = core.inputsOf(opcode);
      if (inputs.empty())
        throw std::runtime_error(
            "frame or guard opcode has no descriptor operand");
      const auto descriptor = vm.frameDescriptor(inputs.front());
      if (!descriptor) throw std::runtime_error(descriptor.error().message);
      for (const auto &[record, target] : descriptor->referenceCells)
        referenceOperands.emplace(record, target);
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
