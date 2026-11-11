#include "common/xanadu/vagent/bootstrap.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vagent {

namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::Posward;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

struct FragmentApparatusBuilder {
  zigzag::ArenaManifold &arena;
  VortexCore &core;
  DimRef nameDim;
  DimRef valuesDim;
  DimRef grabDim;
  DimRef stepDim;
  DimRef varsDim;
  DimRef spinDim;
  DimRef entryDim;
  DimRef recipeDim;
  DimRef refsDim;
  DimRef refDim;
  DimRef meshRelocDim;

  std::vector<CellRef> allCells;
  struct LinkRecord {
    CellRef from;
    DimRef dim;
    bool neg;
    CellRef to;
    std::string dimName;
  };
  std::vector<LinkRecord> allLinks;

  CellRef track(CellRef c) {
    allCells.push_back(c);
    return c;
  }

  void link(CellRef from, DimRef dim, bool neg, CellRef to,
            std::string_view dimName) {
    const auto dir = neg ? DimVector::NEG : DimVector::POS;
    zigzag::expectWritten(arena.link(from, dim, dir, to));
    allLinks.push_back({from, dim, neg, to, std::string(dimName)});
  }

  void spin(CellRef from, CellRef to) {
    link(from, spinDim, false, to, "d.spin");
  }

  CellRef bindInputTracked(CellRef op, CellRef operand) {
    const auto slot =
        track(arena.makeScalarCell(static_cast<std::int64_t>(operand)));
    const auto first = arena.linked(op, grabDim, DimVector::POS);
    if (!first) {
      link(op, grabDim, false, slot, "d.grab");
    } else {
      const auto tail = zigzag::rankTail(arena, *first, stepDim);
      link(tail, stepDim, false, slot, "d.step");
    }
    return slot;
  }

  CellRef bindOutputTracked(CellRef op, CellRef target) {
    const auto slot =
        track(arena.makeScalarCell(static_cast<std::int64_t>(target)));
    const auto first = arena.linked(op, grabDim, DimVector::NEG);
    if (!first) {
      link(op, grabDim, true, slot, "d.grab");
    } else {
      const auto tail = zigzag::rankTail(arena, *first, stepDim);
      link(tail, stepDim, false, slot, "d.step");
    }
    return slot;
  }

  void finishApparatus(
      CellRef head, CellRef entryOp, CellRef resultCell, CellRef producedCell,
      std::string_view label,
      std::span<const std::pair<std::string_view, std::string_view>> params) {
    link(head, nameDim, false, track(arena.makeCell(label)), "d.name");
    link(head, entryDim, false, entryOp, "d.entry");

    const auto descriptor = track(arena.makeCell());
    link(head, valuesDim, false, descriptor, "d.values");

    const auto addField = [&](std::string_view key, CellRef val) {
      const auto rec = track(arena.makeCell(key));
      link(rec, valuesDim, false, val, "d.values");
      const auto tail = zigzag::rankTail(arena, descriptor, varsDim);
      link(tail, varsDim, false, rec, "d.vars");
      return rec;
    };

    const auto resRec = addField("result", resultCell);
    link(resRec, nameDim, false, track(arena.makeCell("Cell")), "d.name");
    addField("produced", producedCell);
    addField("pure", track(arena.makeScalarCell(std::int64_t{0})));
    addField("closed", track(arena.makeScalarCell(std::int64_t{1})));
    addField("arity", track(arena.makeScalarCell(
                          static_cast<std::int64_t>(params.size()))));

    CellRef paramTail = noCell;
    for (const auto &[pName, pType] : params) {
      addField(pName, track(arena.makeCell(pType)));

      const auto pChainCell = track(arena.makeCell());
      link(pChainCell, nameDim, false, track(arena.makeCell(pName)), "d.name");
      link(pChainCell, valuesDim, false, track(arena.makeCell(pType)),
           "d.values");

      if (paramTail == noCell) {
        link(descriptor, grabDim, false, pChainCell, "d.grab");
      } else {
        link(paramTail, stepDim, false, pChainCell, "d.step");
      }
      paramTail = pChainCell;
    }

    // Build +d.recipe
    std::vector<CellRef> recipeMembers;
    const auto pushMember = [&](std::int64_t pos) {
      const auto m = arena.makeScalarCell(pos);
      recipeMembers.push_back(m);
      return m;
    };

    for (const auto c : allCells) {
      pushMember(static_cast<std::int64_t>(c));
    }

    const auto addTuple = [&](CellRef from, DimRef dim, bool neg, CellRef to,
                              std::string_view dName) {
      pushMember(static_cast<std::int64_t>(from));
      const auto anchor = pushMember(static_cast<std::int64_t>(dim));
      zigzag::expectWritten(
          arena.link(anchor, nameDim, Posward, arena.makeCell(dName)));
      pushMember(neg ? -1 : 1);
      pushMember(static_cast<std::int64_t>(to));
    };

    for (const auto &al : allLinks) {
      addTuple(al.from, al.dim, al.neg, al.to, al.dimName);
    }

    if (!recipeMembers.empty()) {
      zigzag::expectWritten(
          arena.link(head, recipeDim, Posward, recipeMembers.front()));
      for (std::size_t i = 1; i < recipeMembers.size(); ++i) {
        zigzag::expectWritten(arena.link(recipeMembers[i - 1], stepDim, Posward,
                                         recipeMembers[i]));
      }
    }

    // Build +d.refs
    std::vector<CellRef> refRecords;
    const auto addRefRecord = [&](CellRef target) {
      const auto rec = arena.makeCell();
      zigzag::expectWritten(arena.link(rec, refDim, Posward, target));
      zigzag::expectWritten(
          arena.link(target, meshRelocDim, Posward, arena.makeCell("cell")));
      refRecords.push_back(rec);
    };

    for (const auto c : allCells) {
      addRefRecord(c);
    }
    for (const auto m : recipeMembers) {
      addRefRecord(m);
    }

    if (!refRecords.empty()) {
      zigzag::expectWritten(
          arena.link(head, refsDim, Posward, refRecords.front()));
      for (std::size_t i = 1; i < refRecords.size(); ++i) {
        zigzag::expectWritten(
            arena.link(refRecords[i - 1], stepDim, Posward, refRecords[i]));
      }
    }
  }
};

CellRef mintBootstrapWriter(VortexCore &core, VortexVM &vm) {
  auto &arena = core.arena();
  FragmentApparatusBuilder b{
      .arena        = arena,
      .core         = core,
      .nameDim      = core.dims().name,
      .valuesDim    = core.dims().values,
      .grabDim      = core.dims().grab,
      .stepDim      = core.dims().step,
      .varsDim      = core.dims().vars,
      .spinDim      = core.dims().spin,
      .entryDim     = core.findOrMintDimension("d.entry"),
      .recipeDim    = core.findOrMintDimension("d.recipe"),
      .refsDim      = core.findOrMintDimension("d.refs"),
      .refDim       = core.findOrMintDimension("d.ref"),
      .meshRelocDim = core.findOrMintDimension("d.mesh.relocation"),
      .allCells     = {},
      .allLinks     = {},
  };

  const auto head = b.track(arena.makeCell());

  // Constants & temporary cells
  const auto zeroCell     = b.track(arena.makeScalarCell(std::int64_t{0}));
  const auto minusOneCell = b.track(arena.makeScalarCell(std::int64_t{-1}));
  const auto falseVal     = b.track(arena.makeScalarCell(false));
  const auto trueVal      = b.track(arena.makeScalarCell(true));
  const auto depthCell    = b.track(arena.makeScalarCell(std::int64_t{0}));
  const auto resultCell   = b.track(arena.makeCell());
  const auto producedCell = b.track(arena.makeScalarCell(false));
  const auto tmpEntry     = b.track(arena.makeCell());
  const auto tmpSuccess   = b.track(arena.makeCell());
  const auto tmpFail      = b.track(arena.makeCell());

  // Failure arm: produced := false -> retFailOp
  const auto retFailOp = b.track(vm.mintOpcode(OpcodeKind::Return));
  const auto onFail    = b.track(vm.mintOpcode(OpcodeKind::Value));
  b.bindInputTracked(onFail, producedCell);
  b.bindInputTracked(onFail, zeroCell);
  b.bindInputTracked(onFail, minusOneCell);
  b.bindInputTracked(onFail, falseVal);
  b.bindOutputTracked(onFail, tmpFail);
  b.spin(onFail, retFailOp);

  // Success arm: produced := true -> retSuccessOp
  const auto retSuccessOp = b.track(vm.mintOpcode(OpcodeKind::Return));
  const auto onSuccess    = b.track(vm.mintOpcode(OpcodeKind::Value));
  b.bindInputTracked(onSuccess, producedCell);
  b.bindInputTracked(onSuccess, zeroCell);
  b.bindInputTracked(onSuccess, minusOneCell);
  b.bindInputTracked(onSuccess, trueVal);
  b.bindOutputTracked(onSuccess, tmpSuccess);
  b.spin(onSuccess, retSuccessOp);

  // Cut opcode: cut(depth) -> onSuccess
  const auto cutOp = b.track(vm.mintOpcode(OpcodeKind::Cut));
  b.bindInputTracked(cutOp, depthCell);
  b.spin(cutOp, onSuccess);

  // Body opcode: Value(targetSlot, zero, minusOne, sourceSlot) -> [resultCell]
  // -> cutOp
  const auto valOp      = b.track(vm.mintOpcode(OpcodeKind::Value));
  const auto targetSlot = b.track(arena.makeScalarCell(std::int64_t{0}));
  const auto zeroSlot =
      b.track(arena.makeScalarCell(static_cast<std::int64_t>(zeroCell)));
  const auto minusOneSlot =
      b.track(arena.makeScalarCell(static_cast<std::int64_t>(minusOneCell)));
  const auto sourceSlot = b.track(arena.makeScalarCell(std::int64_t{0}));

  b.link(valOp, b.grabDim, false, targetSlot, "d.grab");
  b.link(targetSlot, b.stepDim, false, zeroSlot, "d.step");
  b.link(zeroSlot, b.stepDim, false, minusOneSlot, "d.step");
  b.link(minusOneSlot, b.stepDim, false, sourceSlot, "d.step");

  b.bindOutputTracked(valOp, resultCell);
  b.spin(valOp, cutOp);

  // Choice opcode: Choice(onFailRef) -> valOp
  const auto choiceOp = b.track(vm.mintOpcode(OpcodeKind::Choice));
  const auto onFailRef =
      b.track(arena.makeScalarCell(static_cast<std::int64_t>(onFail)));
  b.bindInputTracked(choiceOp, onFailRef);
  b.spin(choiceOp, valOp);

  // ChoiceDepth opcode: [] -> [depthCell] -> choiceOp
  const auto readDepth = b.track(vm.mintOpcode(OpcodeKind::ChoiceDepth));
  b.bindOutputTracked(readDepth, depthCell);
  b.spin(readDepth, choiceOp);

  // Resolve "source" -> sourceSlot -> readDepth
  const auto resolveSource  = b.track(vm.mintOpcode(OpcodeKind::Resolve));
  const auto sourceNameCell = b.track(arena.makeCell("source"));
  b.bindInputTracked(resolveSource, sourceNameCell);
  b.bindOutputTracked(resolveSource, sourceSlot);
  b.spin(resolveSource, readDepth);

  // Resolve "target" -> targetSlot -> resolveSource
  const auto resolveTarget  = b.track(vm.mintOpcode(OpcodeKind::Resolve));
  const auto targetNameCell = b.track(arena.makeCell("target"));
  b.bindInputTracked(resolveTarget, targetNameCell);
  b.bindOutputTracked(resolveTarget, targetSlot);
  b.spin(resolveTarget, resolveSource);

  // Entry opcode: produced := false -> resolveTarget
  const auto entryOp = b.track(vm.mintOpcode(OpcodeKind::Value));
  b.bindInputTracked(entryOp, producedCell);
  b.bindInputTracked(entryOp, zeroCell);
  b.bindInputTracked(entryOp, minusOneCell);
  b.bindInputTracked(entryOp, falseVal);
  b.bindOutputTracked(entryOp, tmpEntry);
  b.spin(entryOp, resolveTarget);

  static constexpr std::pair<std::string_view, std::string_view> kParams[] = {
      {"target", "Cell"},
      {"source", "Cell"},
  };

  b.finishApparatus(head, entryOp, resultCell, producedCell, "boot:writer",
                    kParams);
  return head;
}

CellRef mintBootstrapLinker(VortexCore &core, VortexVM &vm) {
  auto &arena = core.arena();
  FragmentApparatusBuilder b{
      .arena        = arena,
      .core         = core,
      .nameDim      = core.dims().name,
      .valuesDim    = core.dims().values,
      .grabDim      = core.dims().grab,
      .stepDim      = core.dims().step,
      .varsDim      = core.dims().vars,
      .spinDim      = core.dims().spin,
      .entryDim     = core.findOrMintDimension("d.entry"),
      .recipeDim    = core.findOrMintDimension("d.recipe"),
      .refsDim      = core.findOrMintDimension("d.refs"),
      .refDim       = core.findOrMintDimension("d.ref"),
      .meshRelocDim = core.findOrMintDimension("d.mesh.relocation"),
      .allCells     = {},
      .allLinks     = {},
  };

  const auto head = b.track(arena.makeCell());

  // Constants & temporary cells
  const auto zeroCell     = b.track(arena.makeScalarCell(std::int64_t{0}));
  const auto minusOneCell = b.track(arena.makeScalarCell(std::int64_t{-1}));
  const auto falseVal     = b.track(arena.makeScalarCell(false));
  const auto trueVal      = b.track(arena.makeScalarCell(true));
  const auto depthCell    = b.track(arena.makeScalarCell(std::int64_t{0}));
  const auto resultCell   = b.track(arena.makeCell());
  const auto producedCell = b.track(arena.makeScalarCell(false));
  const auto tmpEntry     = b.track(arena.makeCell());
  const auto tmpSuccess   = b.track(arena.makeCell());
  const auto tmpFail      = b.track(arena.makeCell());
  const auto dimLocal     = b.track(arena.makeCell());
  const auto dirLocal     = b.track(arena.makeCell());

  // Failure arm: produced := false -> retFailOp
  const auto retFailOp = b.track(vm.mintOpcode(OpcodeKind::Return));
  const auto onFail    = b.track(vm.mintOpcode(OpcodeKind::Value));
  b.bindInputTracked(onFail, producedCell);
  b.bindInputTracked(onFail, zeroCell);
  b.bindInputTracked(onFail, minusOneCell);
  b.bindInputTracked(onFail, falseVal);
  b.bindOutputTracked(onFail, tmpFail);
  b.spin(onFail, retFailOp);

  // Success arm: produced := true -> retSuccessOp
  const auto retSuccessOp = b.track(vm.mintOpcode(OpcodeKind::Return));
  const auto onSuccess    = b.track(vm.mintOpcode(OpcodeKind::Value));
  b.bindInputTracked(onSuccess, producedCell);
  b.bindInputTracked(onSuccess, zeroCell);
  b.bindInputTracked(onSuccess, minusOneCell);
  b.bindInputTracked(onSuccess, trueVal);
  b.bindOutputTracked(onSuccess, tmpSuccess);
  b.spin(onSuccess, retSuccessOp);

  // Cut opcode: cut(depth) -> onSuccess
  const auto cutOp = b.track(vm.mintOpcode(OpcodeKind::Cut));
  b.bindInputTracked(cutOp, depthCell);
  b.spin(cutOp, onSuccess);

  // Body opcode: Link(slotFrom, slotDim, slotDir, slotTo) -> [resultCell] ->
  // cutOp
  const auto linkOp   = b.track(vm.mintOpcode(OpcodeKind::Link));
  const auto slotFrom = b.track(arena.makeScalarCell(std::int64_t{0}));
  const auto slotDim =
      b.track(arena.makeScalarCell(static_cast<std::int64_t>(dimLocal)));
  const auto slotDir =
      b.track(arena.makeScalarCell(static_cast<std::int64_t>(dirLocal)));
  const auto slotTo = b.track(arena.makeScalarCell(std::int64_t{0}));

  b.link(linkOp, b.grabDim, false, slotFrom, "d.grab");
  b.link(slotFrom, b.stepDim, false, slotDim, "d.step");
  b.link(slotDim, b.stepDim, false, slotDir, "d.step");
  b.link(slotDir, b.stepDim, false, slotTo, "d.step");

  b.bindOutputTracked(linkOp, resultCell);
  b.spin(linkOp, cutOp);

  // Choice opcode: Choice(onFailRef) -> linkOp
  const auto choiceOp = b.track(vm.mintOpcode(OpcodeKind::Choice));
  const auto onFailRef =
      b.track(arena.makeScalarCell(static_cast<std::int64_t>(onFail)));
  b.bindInputTracked(choiceOp, onFailRef);
  b.spin(choiceOp, linkOp);

  // ChoiceDepth opcode: [] -> [depthCell] -> choiceOp
  const auto readDepth = b.track(vm.mintOpcode(OpcodeKind::ChoiceDepth));
  b.bindOutputTracked(readDepth, depthCell);
  b.spin(readDepth, choiceOp);

  // Resolve "to" -> slotTo -> readDepth
  const auto resolveTo  = b.track(vm.mintOpcode(OpcodeKind::Resolve));
  const auto toNameCell = b.track(arena.makeCell("to"));
  b.bindInputTracked(resolveTo, toNameCell);
  b.bindOutputTracked(resolveTo, slotTo);
  b.spin(resolveTo, readDepth);

  // Resolve "dir" -> dirLocal -> resolveTo
  const auto resolveDir  = b.track(vm.mintOpcode(OpcodeKind::Resolve));
  const auto dirNameCell = b.track(arena.makeCell("dir"));
  b.bindInputTracked(resolveDir, dirNameCell);
  b.bindOutputTracked(resolveDir, dirLocal);
  b.spin(resolveDir, resolveTo);

  // Resolve "dim" -> dimLocal -> resolveDir
  const auto resolveDim  = b.track(vm.mintOpcode(OpcodeKind::Resolve));
  const auto dimNameCell = b.track(arena.makeCell("dim"));
  b.bindInputTracked(resolveDim, dimNameCell);
  b.bindOutputTracked(resolveDim, dimLocal);
  b.spin(resolveDim, resolveDir);

  // Resolve "from" -> slotFrom -> resolveDim
  const auto resolveFrom  = b.track(vm.mintOpcode(OpcodeKind::Resolve));
  const auto fromNameCell = b.track(arena.makeCell("from"));
  b.bindInputTracked(resolveFrom, fromNameCell);
  b.bindOutputTracked(resolveFrom, slotFrom);
  b.spin(resolveFrom, resolveDim);

  // Entry opcode: produced := false -> resolveFrom
  const auto entryOp = b.track(vm.mintOpcode(OpcodeKind::Value));
  b.bindInputTracked(entryOp, producedCell);
  b.bindInputTracked(entryOp, zeroCell);
  b.bindInputTracked(entryOp, minusOneCell);
  b.bindInputTracked(entryOp, falseVal);
  b.bindOutputTracked(entryOp, tmpEntry);
  b.spin(entryOp, resolveFrom);

  static constexpr std::pair<std::string_view, std::string_view> kParams[] = {
      {"from", "Cell"},
      {"dim", "Cell"},
      {"dir", "Int"},
      {"to", "Cell"},
  };

  b.finishApparatus(head, entryOp, resultCell, producedCell, "boot:linker",
                    kParams);
  return head;
}

} // namespace

std::expected<BootstrapPair, std::string>
ensureBootstrapPair(VortexCore &core, VortexVM &vm, CellRef programRoot) {
  auto &arena         = core.arena();
  const auto entryDim = core.findOrMintDimension("d.entry");
  const auto valDim   = core.dims().values;

  const auto checkExisting =
      [&](std::string_view key) -> std::optional<CellRef> {
    const auto rec = varsRecord(arena, core, programRoot, key, false);
    if (!rec) return std::nullopt;
    const auto valCell = arena.linked(*rec, valDim, Posward);
    if (!valCell) return std::nullopt;
    const auto headPos = arena.asInt64(*valCell);
    if (!headPos) return std::nullopt;
    const CellRef head = static_cast<CellRef>(*headPos);
    if (!arena.contains(head)) return std::nullopt;
    if (!arena.linked(head, entryDim, Posward).has_value()) return std::nullopt;
    return head;
  };

  const auto existingWriter = checkExisting("$boot:writer");
  const auto existingLinker = checkExisting("$boot:linker");
  if (existingWriter && existingLinker) {
    return BootstrapPair{.writer = *existingWriter, .linker = *existingLinker};
  }

  const CellRef writerHead =
      existingWriter ? *existingWriter : mintBootstrapWriter(core, vm);
  const CellRef linkerHead =
      existingLinker ? *existingLinker : mintBootstrapLinker(core, vm);

  if (!existingWriter) {
    const auto writerRec =
        varsRecord(arena, core, programRoot, "$boot:writer", true);
    if (!writerRec) {
      return std::unexpected(
          "ensureBootstrapPair: could not file $boot:writer varsRecord");
    }
    zigzag::expectWritten(arena.link(*writerRec, valDim, Posward,
                                     refCell(arena, core, writerHead)));
  }

  if (!existingLinker) {
    const auto linkerRec =
        varsRecord(arena, core, programRoot, "$boot:linker", true);
    if (!linkerRec) {
      return std::unexpected(
          "ensureBootstrapPair: could not file $boot:linker varsRecord");
    }
    zigzag::expectWritten(arena.link(*linkerRec, valDim, Posward,
                                     refCell(arena, core, linkerHead)));
  }

  return BootstrapPair{.writer = writerHead, .linker = linkerHead};
}

} // namespace xanadu::vagent
