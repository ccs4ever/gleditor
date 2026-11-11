#include "common/xanadu/vagent/inlang_emitter.hpp"

#include <format>
#include <map>
#include <string>
#include <vector>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vagent {

namespace {

using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

struct EmitterState {
  zigzag::ArenaManifold &arena;
  zigzag::vortex::VortexCore &core;
  zigzag::vortex::VortexVM &vm;
  std::vector<CellRef> opChain;
  std::vector<CellRef> bodyCells;
  std::map<std::string, CellRef> env;

  CellRef track(CellRef c) {
    bodyCells.push_back(c);
    return c;
  }

  CellRef mintOp(zigzag::vortex::OpcodeKind kind) {
    const auto op = track(vm.mintOpcode(kind));
    if (!opChain.empty()) {
      zigzag::expectWritten(
          arena.link(opChain.back(), core.dims().spin, Posward, op));
    }
    opChain.push_back(op);
    return op;
  }

  void bindIn(CellRef op, CellRef inCell) {
    core.bindInput(op, inCell);
    track(inCell);
  }

  void bindOut(CellRef op, CellRef outCell) {
    core.bindOutput(op, outCell);
    track(outCell);
  }
};

} // namespace

InlangEmitResult runInlangEmitter(zigzag::ArenaManifold &arena,
                                  zigzag::vortex::VortexCore &core,
                                  zigzag::vortex::VortexVM &vm,
                                  MetaRuntime &meta, std::string_view label,
                                  zigzag::CellRef r1Root,
                                  std::optional<zigzag::CellRef> params) {
  InlangEmitResult result;
  result.label = std::string(label);

  if (!arena.contains(r1Root)) {
    result.error = "invalid R1 AST root cell";
    return result;
  }

  const auto &dims = core.dims();
  EmitterState s{arena, core, vm, {}, {}, {}};

  // 1. Process parameters: emit Resolve opcodes
  struct ParamDesc {
    std::string name;
    std::string typeName;
  };
  std::vector<ParamDesc> paramDescs;

  if (params.has_value() && *params != noCell && arena.contains(*params)) {
    const auto pVal = *params;
    std::vector<CellRef> paramCells;
    if (const auto grabChild = arena.linked(pVal, dims.grab, Posward)) {
      paramCells.push_back(*grabChild);
      for (const auto c : zigzag::rankAfter(arena, *grabChild, dims.step)) {
        paramCells.push_back(c);
      }
    } else {
      paramCells.push_back(pVal);
      for (const auto c : zigzag::rankAfter(arena, pVal, dims.step)) {
        paramCells.push_back(c);
      }
    }

    for (const auto p : paramCells) {
      const auto pName  = arena.textOf(p);
      std::string pType = "Cell";
      if (const auto valCell = arena.linked(p, dims.values, Posward)) {
        pType = arena.textOf(*valCell);
      }
      paramDescs.push_back({pName, pType});

      const auto nameCell = s.track(arena.makeCell(pName));
      const auto slotCell = s.track(arena.makeCell());
      const auto resOp    = s.mintOp(zigzag::vortex::OpcodeKind::Resolve);
      s.bindIn(resOp, nameCell);
      s.bindOut(resOp, slotCell);
      s.env[pName] = slotCell;
    }
  }

  // 2. Emit body DAG
  const auto emitNode = [&](auto &self, CellRef node) -> CellRef {
    if (!arena.contains(node)) return noCell;
    const auto kind = arena.textOf(node);
    const auto tag  = arena.linked(node, dims.name, Posward)
                          .transform([&](CellRef c) { return arena.textOf(c); })
                          .value_or("");

    if (kind == "lit.int") {
      std::int64_t v = 0;
      try {
        v = std::stoll(tag);
      } catch (...) {
        v = 0;
      }
      return s.track(arena.makeScalarCell(v));
    }
    if (kind == "lit.bool") {
      return s.track(arena.makeScalarCell(tag == "true"));
    }
    if (kind == "lit.str") {
      return s.track(arena.makeCell(tag));
    }
    if (kind == "ident") {
      auto it = s.env.find(tag);
      if (it != s.env.end()) return it->second;
      return s.track(arena.makeCell(tag));
    }
    if (kind == "unary") {
      const auto operand = arena.linked(node, dims.grab, Posward);
      if (!operand) return noCell;
      const auto inSlot = self(self, *operand);
      if (inSlot == noCell) return noCell;

      const auto outSlot = s.track(arena.makeCell());
      if (tag == "-") {
        const auto op = s.mintOp(zigzag::vortex::OpcodeKind::Neg);
        s.bindIn(op, inSlot);
        s.bindOut(op, outSlot);
      } else {
        const auto op = s.mintOp(zigzag::vortex::OpcodeKind::Not);
        s.bindIn(op, inSlot);
        s.bindOut(op, outSlot);
      }
      return outSlot;
    }
    if (kind == "binary") {
      const auto left = arena.linked(node, dims.grab, Posward);
      if (!left) return noCell;
      const auto right = arena.linked(*left, dims.step, Posward);
      if (!right) return noCell;

      const auto leftSlot  = self(self, *left);
      const auto rightSlot = self(self, *right);
      if (leftSlot == noCell || rightSlot == noCell) return noCell;

      const auto outSlot                = s.track(arena.makeCell());
      zigzag::vortex::OpcodeKind opKind = zigzag::vortex::OpcodeKind::Add;
      if (tag == "+")
        opKind = zigzag::vortex::OpcodeKind::Add;
      else if (tag == "-")
        opKind = zigzag::vortex::OpcodeKind::Sub;
      else if (tag == "*")
        opKind = zigzag::vortex::OpcodeKind::Mul;
      else if (tag == "/")
        opKind = zigzag::vortex::OpcodeKind::Div;
      else if (tag == "%")
        opKind = zigzag::vortex::OpcodeKind::Mod;
      else if (tag == "==")
        opKind = zigzag::vortex::OpcodeKind::Eq;
      else if (tag == "!=")
        opKind = zigzag::vortex::OpcodeKind::Neq;
      else if (tag == "<")
        opKind = zigzag::vortex::OpcodeKind::Lt;
      else if (tag == "<=")
        opKind = zigzag::vortex::OpcodeKind::Lte;
      else if (tag == ">")
        opKind = zigzag::vortex::OpcodeKind::Gt;
      else if (tag == ">=")
        opKind = zigzag::vortex::OpcodeKind::Gte;
      else if (tag == "&" || tag == "&&")
        opKind = zigzag::vortex::OpcodeKind::And;
      else if (tag == "|" || tag == "||")
        opKind = zigzag::vortex::OpcodeKind::Or;

      const auto op = s.mintOp(opKind);
      s.bindIn(op, leftSlot);
      s.bindIn(op, rightSlot);
      s.bindOut(op, outSlot);
      return outSlot;
    }
    return noCell;
  };

  const auto resultSlot = emitNode(emitNode, r1Root);
  if (resultSlot == noCell) {
    result.error = "failed to emit body DAG";
    return result;
  }

  // 3. Epilogue: set produced flag and return
  const auto prodCell     = s.track(arena.makeScalarCell(false));
  const auto zeroCell     = s.track(arena.makeScalarCell(std::int64_t{0}));
  const auto minusOneCell = s.track(arena.makeScalarCell(std::int64_t{-1}));
  const auto trueVal      = s.track(arena.makeScalarCell(true));
  const auto tmpSuccess   = s.track(arena.makeCell());

  const auto setProd = s.mintOp(zigzag::vortex::OpcodeKind::Value);
  s.bindIn(setProd, prodCell);
  s.bindIn(setProd, zeroCell);
  s.bindIn(setProd, minusOneCell);
  s.bindIn(setProd, trueVal);
  s.bindOut(setProd, tmpSuccess);

  std::ignore = s.mintOp(zigzag::vortex::OpcodeKind::Return);

  // 4. Build spec cell
  const auto spec   = arena.makeCell(label);
  const auto addVar = [&](std::string_view key, CellRef val) {
    const auto rec = varsRecord(arena, core, spec, key, true);
    if (rec) {
      zigzag::expectWritten(arena.link(*rec, dims.values, Posward, val));
    }
  };

  const auto entryOp = s.opChain.front();
  addVar("entry", arena.makeScalarCell(static_cast<std::int64_t>(entryOp)));
  addVar("result_slot",
         arena.makeScalarCell(static_cast<std::int64_t>(resultSlot)));
  addVar("produced_slot",
         arena.makeScalarCell(static_cast<std::int64_t>(prodCell)));

  std::string resTypeStr = "Int";
  if (const auto vt = arena.linked(r1Root, dims.values, Posward)) {
    resTypeStr = arena.textOf(*vt);
  }
  addVar("result_type", arena.makeCell(resTypeStr));
  addVar("pure", arena.makeScalarCell(std::int64_t{1}));
  addVar("closed", arena.makeScalarCell(std::int64_t{1}));

  // Spec parameters
  CellRef pTail = noCell;
  for (const auto &p : paramDescs) {
    const auto pCell = arena.makeCell(p.name);
    zigzag::expectWritten(
        arena.link(pCell, dims.values, Posward, arena.makeCell(p.typeName)));
    if (pTail == noCell) {
      zigzag::expectWritten(arena.link(spec, dims.grab, Posward, pCell));
    } else {
      zigzag::expectWritten(arena.link(pTail, dims.step, Posward, pCell));
    }
    pTail = pCell;
  }

  // Spec refs: covered body cells
  const auto refsDim = core.findOrMintDimension("d.refs");
  CellRef rTail      = noCell;
  for (const auto c : s.bodyCells) {
    const auto r = arena.makeScalarCell(static_cast<std::int64_t>(c));
    if (rTail == noCell) {
      zigzag::expectWritten(arena.link(spec, refsDim, Posward, r));
    } else {
      zigzag::expectWritten(arena.link(rTail, dims.step, Posward, r));
    }
    rTail = r;
  }

  // 5. Build and register fragment
  const auto head = meta.buildFragmentFromSpec(spec);
  if (!head) {
    result.error = "buildFragmentFromSpec failed";
    return result;
  }

  const auto val = meta.validate(*head);
  if (!val.ok) {
    result.error = std::format("validation failed: {}", val.detail);
    return result;
  }

  const auto reg = meta.registerCode(*head);
  if (!reg) {
    result.error = std::format("registerCode failed: {}", reg.error());
    return result;
  }

  result.ok   = true;
  result.head = *head;
  return result;
}

} // namespace xanadu::vagent
