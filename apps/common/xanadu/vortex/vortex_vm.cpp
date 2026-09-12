/**
 * @file vortex_vm.cpp
 * @brief Implementation of Spin-Head Virtual Machine for Vortex.
 */
#include "common/xanadu/vortex/vortex_vm.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <unordered_set>

#include "common/xanadu/zigzag/vlog.hpp"

namespace zigzag::vortex {

namespace {

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

CellRef resolveCell(const std::vector<CellRef> &inCells,
                    const std::vector<CellValue> &inputs, std::size_t idx,
                    const ArenaManifold &arena) {
  if (idx < inCells.size()) {
    CellRef c = inCells[idx];
    if (arena.contains(c)) {
      return c;
    }
  }
  if (idx < inputs.size()) {
    CellRef c = static_cast<CellRef>(toInt(inputs[idx]));
    if (arena.contains(c)) {
      return c;
    }
  }
  return noCell;
}

} // namespace

VortexVM::VortexVM(VortexCore &core) : core_(core) {}

CellRef VortexVM::mintOpcode(OpcodeKind op, std::string_view label) {
  CellRef opCell = core_.arena().makeScalarCell(static_cast<std::int64_t>(op));
  opcodeMap_[opCell] = op;
  if (!label.empty()) {
    auto span = core_.arena().intern(label);
    core_.arena().setContent(opCell,
                             std::span<const xanadu::PrimediaSpan>{&span, 1});
  }
  return opCell;
}

CellRef VortexVM::assembleSequence(std::span<const OpcodeKind> ops) {
  if (ops.empty()) return noCell;
  CellRef head = mintOpcode(ops[0]);
  CellRef cur  = head;
  for (std::size_t i = 1; i < ops.size(); ++i) {
    CellRef next = mintOpcode(ops[i]);
    core_.arena().link(cur, core_.dims().spin, false, next);
    cur = next;
  }
  return head;
}

void VortexVM::setOpcodeKind(CellRef opCell, OpcodeKind op) {
  opcodeMap_[opCell] = op;
}

std::optional<OpcodeKind> VortexVM::getOpcodeKind(CellRef opCell) const {
  auto it = opcodeMap_.find(opCell);
  if (it != opcodeMap_.end()) {
    return it->second;
  }
  auto val = core_.arena().asInt64(opCell);
  if (val) {
    return static_cast<OpcodeKind>(*val);
  }
  return std::nullopt;
}

CellRef VortexVM::getCursorOpcode(CellRef cursor) const {
  auto it = cursorPC_.find(cursor);
  if (it != cursorPC_.end()) {
    return it->second;
  }
  auto val = core_.arena().asInt64(cursor);
  if (val) {
    return static_cast<CellRef>(*val);
  }
  return noCell;
}

void VortexVM::setCursorOpcode(CellRef cursor, CellRef op) {
  cursorPC_[cursor] = op;
  if (core_.arena().contains(cursor)) {
    core_.arena().setValueBits(cursor, xanadu::ValueKind::Int64,
                               static_cast<std::uint64_t>(op));
  }
}

CellRef VortexVM::spawnCursor(CellRef entryOpcode, std::string_view name) {
  CellRef cursor = core_.arena().makeCell();
  setCursorOpcode(cursor, entryOpcode);
  if (!name.empty()) {
    CellRef nameCell = core_.arena().makeCell(name);
    core_.arena().link(cursor, core_.dims().name, false, nameCell);
  }

  // Link into home_ +d.cursors rank
  CellRef first =
      core_.arena().linked(core_.home(), core_.dims().cursors, false);
  if (first == noCell) {
    core_.arena().link(core_.home(), core_.dims().cursors, false, cursor);
  } else {
    CellRef cur       = first;
    std::size_t limit = core_.arena().cellCount() + 1;
    while (limit-- > 0) {
      CellRef next = core_.arena().linked(cur, core_.dims().cursors, false);
      if (next == noCell) {
        core_.arena().link(cur, core_.dims().cursors, false, cursor);
        break;
      }
      cur = next;
    }
  }
  return cursor;
}

std::vector<CellRef> VortexVM::activeCursors() const {
  std::vector<CellRef> result;
  CellRef cur = core_.arena().linked(core_.home(), core_.dims().cursors, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    result.push_back(cur);
    cur = core_.arena().linked(cur, core_.dims().cursors, false);
  }
  return result;
}

void VortexVM::enableMemoization(CellRef opcode, std::string_view memoKey) {
  memoizedOps_[opcode] = std::string(memoKey);
}

bool VortexVM::isMemoized(CellRef opcode) const {
  return memoizedOps_.find(opcode) != memoizedOps_.end();
}

std::string VortexVM::getMemoKey(CellRef opcode) const {
  auto it = memoizedOps_.find(opcode);
  if (it != memoizedOps_.end()) {
    return it->second;
  }
  return "";
}

void VortexVM::pushChoicePoint(CellRef cursor, CellRef altOp) {
  CellRef stackFrame = noCell;
  if (cursor != noCell && core_.arena().contains(cursor)) {
    stackFrame = core_.arena().linked(cursor, core_.dims().stack, false);
  }
  choiceStack_.push_back(ChoicePoint{
      .cursor     = cursor,
      .altOp      = altOp,
      .mark       = core_.arena().mark(),
      .stackFrame = stackFrame,
      .cutBarrier = choiceStack_.size(),
  });
}

bool VortexVM::backtrack(CellRef cursor) {
  while (!choiceStack_.empty()) {
    ChoicePoint cp = choiceStack_.back();
    choiceStack_.pop_back();
    if (cursor == noCell || cp.cursor == cursor || cp.cursor == noCell) {
      core_.arena().release(cp.mark);
      if (cursor != noCell) {
        setCursorOpcode(cursor, cp.altOp);
        if (cp.stackFrame != noCell && core_.arena().contains(cursor)) {
          core_.arena().link(cursor, core_.dims().stack, false, cp.stackFrame);
        }
      }
      return true;
    }
  }
  return false;
}

void VortexVM::cut(std::size_t cutBarrier) {
  while (choiceStack_.size() > cutBarrier) {
    ChoicePoint cp = choiceStack_.back();
    choiceStack_.pop_back();
    core_.arena().discard(cp.mark);
  }
}

std::size_t VortexVM::choiceDepth() const noexcept {
  return choiceStack_.size();
}

void VortexVM::clearChoicePoints() { choiceStack_.clear(); }

void VortexVM::runPipeline(CellRef paramCell) {
  CellRef pipeOp    = core_.getPipelineHead(paramCell);
  std::size_t limit = 1000;
  while (pipeOp != noCell && limit-- > 0) {
    auto kindOpt    = getOpcodeKind(pipeOp);
    OpcodeKind kind = kindOpt ? *kindOpt : OpcodeKind::Nop;
    if (kind == OpcodeKind::Halt || kind == OpcodeKind::Return) {
      break;
    }

    std::vector<CellRef> inCells = core_.inputsOf(pipeOp);
    std::vector<CellValue> inVals;
    if (inCells.empty()) {
      inVals.push_back(core_.render(paramCell));
    } else {
      for (CellRef c : inCells) {
        inVals.push_back(core_.render(c));
      }
    }

    std::vector<CellValue> outVals;
    bool jumped = false;
    executeOpcodeBody(kind, pipeOp, inVals, outVals, noCell, jumped);

    std::vector<CellRef> outCells = core_.outputsOf(pipeOp);
    if (outCells.empty()) {
      if (!outVals.empty()) {
        core_.value(paramCell, 0, -1, outVals[0]);
      }
    } else {
      if (kind != OpcodeKind::New && kind != OpcodeKind::Clone &&
          kind != OpcodeKind::Link && kind != OpcodeKind::Break) {
        for (std::size_t i = 0; i < std::min(outCells.size(), outVals.size());
             ++i) {
          core_.value(outCells[i], 0, -1, outVals[i]);
        }
      }
    }

    pipeOp = core_.arena().linked(pipeOp, core_.dims().spin, false);
  }
}

bool VortexVM::evaluateCondition(CellRef clause,
                                 const std::vector<CellValue> &oldInputs,
                                 const std::vector<CellValue> &outputs) {
  CellRef actualClause = core_.arena().cloneMaster(clause, core_.dims().clone);
  auto kindOpt         = getOpcodeKind(actualClause);
  if (!kindOpt) {
    return core_.isTruthy(actualClause);
  }

  OpcodeKind kind              = *kindOpt;
  std::vector<CellRef> inCells = core_.inputsOf(actualClause);
  std::vector<CellValue> inVals;
  if (!inCells.empty()) {
    for (CellRef c : inCells) {
      inVals.push_back(core_.render(c));
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
  bool jumped = false;
  executeOpcodeBody(kind, actualClause, inVals, outVals, noCell, jumped);
  if (!outVals.empty()) {
    return VortexCore::evaluateTruthiness(outVals[0]);
  }
  return core_.isTruthy(actualClause);
}

ExecutionResult VortexVM::executeOpcodeBody(
    OpcodeKind kind, CellRef opcode, const std::vector<CellValue> &inputs,
    std::vector<CellValue> &outputs, CellRef cursor, bool &jumped) {
  switch (kind) {
  case OpcodeKind::Add: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.push_back(std::get<std::string>(inputs[0]) +
                          std::get<std::string>(inputs[1]));
      } else if (std::holds_alternative<double>(inputs[0]) ||
                 std::holds_alternative<double>(inputs[1])) {
        outputs.push_back(toDouble(inputs[0]) + toDouble(inputs[1]));
      } else {
        outputs.push_back(toInt(inputs[0]) + toInt(inputs[1]));
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
        outputs.push_back(toDouble(inputs[0]) - toDouble(inputs[1]));
      } else {
        outputs.push_back(toInt(inputs[0]) - toInt(inputs[1]));
      }
    } else if (inputs.size() == 1) {
      outputs.push_back(-toInt(inputs[0]));
    }
    break;
  }
  case OpcodeKind::Mul: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.push_back(toDouble(inputs[0]) * toDouble(inputs[1]));
      } else {
        outputs.push_back(toInt(inputs[0]) * toInt(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Div: {
    if (inputs.size() >= 2) {
      double b = toDouble(inputs[1]);
      if (b == 0.0) {
        outputs.push_back(0.0);
      } else if (std::holds_alternative<double>(inputs[0]) ||
                 std::holds_alternative<double>(inputs[1])) {
        outputs.push_back(toDouble(inputs[0]) / b);
      } else {
        std::int64_t ib = toInt(inputs[1]);
        outputs.push_back(ib != 0 ? toInt(inputs[0]) / ib : 0);
      }
    }
    break;
  }
  case OpcodeKind::DivMod: {
    if (inputs.size() >= 2) {
      std::int64_t a = toInt(inputs[0]);
      std::int64_t b = toInt(inputs[1]);
      if (b != 0) {
        outputs.push_back(a / b);
        outputs.push_back(a % b);
      } else {
        outputs.push_back(static_cast<std::int64_t>(0));
        outputs.push_back(static_cast<std::int64_t>(0));
      }
    }
    break;
  }
  case OpcodeKind::Mod: {
    if (inputs.size() >= 2) {
      std::int64_t b = toInt(inputs[1]);
      outputs.push_back(b != 0 ? toInt(inputs[0]) % b : 0);
    }
    break;
  }
  case OpcodeKind::Neg: {
    if (!inputs.empty()) {
      if (std::holds_alternative<double>(inputs[0])) {
        outputs.push_back(-toDouble(inputs[0]));
      } else {
        outputs.push_back(-toInt(inputs[0]));
      }
    }
    break;
  }
  case OpcodeKind::Eq: {
    if (inputs.size() >= 2) {
      outputs.push_back(inputs[0] == inputs[1]);
    }
    break;
  }
  case OpcodeKind::Neq: {
    if (inputs.size() >= 2) {
      outputs.push_back(inputs[0] != inputs[1]);
    }
    break;
  }
  case OpcodeKind::Lt: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.push_back(std::get<std::string>(inputs[0]) <
                          std::get<std::string>(inputs[1]));
      } else {
        outputs.push_back(toDouble(inputs[0]) < toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Lte: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.push_back(std::get<std::string>(inputs[0]) <=
                          std::get<std::string>(inputs[1]));
      } else {
        outputs.push_back(toDouble(inputs[0]) <= toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Gt: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.push_back(std::get<std::string>(inputs[0]) >
                          std::get<std::string>(inputs[1]));
      } else {
        outputs.push_back(toDouble(inputs[0]) > toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::Gte: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<std::string>(inputs[0]) &&
          std::holds_alternative<std::string>(inputs[1])) {
        outputs.push_back(std::get<std::string>(inputs[0]) >=
                          std::get<std::string>(inputs[1]));
      } else {
        outputs.push_back(toDouble(inputs[0]) >= toDouble(inputs[1]));
      }
    }
    break;
  }
  case OpcodeKind::And: {
    if (inputs.size() >= 2) {
      outputs.push_back(VortexCore::evaluateTruthiness(inputs[0]) &&
                        VortexCore::evaluateTruthiness(inputs[1]));
    }
    break;
  }
  case OpcodeKind::Or: {
    if (inputs.size() >= 2) {
      outputs.push_back(VortexCore::evaluateTruthiness(inputs[0]) ||
                        VortexCore::evaluateTruthiness(inputs[1]));
    }
    break;
  }
  case OpcodeKind::Not: {
    if (!inputs.empty()) {
      outputs.push_back(!VortexCore::evaluateTruthiness(inputs[0]));
    }
    break;
  }
  case OpcodeKind::Link: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      CellRef c = resolveCell(inCells, inputs, 0, core_.arena());
      DimRef d  = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg  = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      std::optional<CellRef> tgt = std::nullopt;
      if (inCells.size() >= 4 || inputs.size() >= 4) {
        CellRef t = resolveCell(inCells, inputs, 3, core_.arena());
        if (t != noCell) {
          tgt = t;
        }
      }
      if (c != noCell) {
        auto res = core_.link(c, d, neg, tgt);
        outputs.push_back(res ? *res : noCell);
      } else {
        outputs.push_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Break: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      CellRef c = resolveCell(inCells, inputs, 0, core_.arena());
      DimRef d  = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg  = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      if (c != noCell) {
        auto res = core_.breakLink(c, d, neg);
        outputs.push_back(res ? *res : noCell);
      } else {
        outputs.push_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::New: {
    std::vector<CellRef> inCells  = core_.inputsOf(opcode);
    std::vector<CellRef> outCells = core_.outputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      CellRef c = resolveCell(inCells, inputs, 0, core_.arena());
      DimRef d  = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg  = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      if (c != noCell) {
        if (!outCells.empty()) {
          CellRef fresh = outCells[0];
          core_.link(c, d, neg, fresh);
          outputs.push_back(fresh);
        } else {
          auto res = core_.newCell(c, d, neg);
          outputs.push_back(res ? *res : noCell);
        }
      } else {
        outputs.push_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Value: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (!inCells.empty() || !inputs.empty()) {
      CellRef c        = resolveCell(inCells, inputs, 0, core_.arena());
      std::int64_t off = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      std::int64_t len = inputs.size() >= 3 ? toInt(inputs[2]) : -1;
      std::optional<CellValue> repl = inputs.size() >= 4
                                          ? std::optional<CellValue>(inputs[3])
                                          : std::nullopt;
      if (c != noCell) {
        auto res = core_.value(c, off, len, repl);
        outputs.push_back(res ? *res : noCell);
      } else {
        outputs.push_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Splice: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (inCells.size() >= 4 || inputs.size() >= 4) {
      CellRef c        = resolveCell(inCells, inputs, 0, core_.arena());
      std::int64_t off = toInt(inputs[1]);
      std::int64_t len = toInt(inputs[2]);
      if (c != noCell) {
        auto res = core_.splice(c, off, len, inputs[3]);
        outputs.push_back(res ? *res : noCell);
      } else {
        outputs.push_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Clone: {
    std::vector<CellRef> inCells  = core_.inputsOf(opcode);
    std::vector<CellRef> outCells = core_.outputsOf(opcode);
    if (!inCells.empty() || !inputs.empty()) {
      CellRef c = resolveCell(inCells, inputs, 0, core_.arena());
      if (c != noCell) {
        CellRef cloneCell =
            !outCells.empty() ? outCells[0] : core_.arena().makeCell();
        CellRef cloneTail  = c;
        std::size_t cLimit = core_.arena().cellCount() + 1;
        while (cLimit-- > 0) {
          CellRef next =
              core_.arena().linked(cloneTail, core_.dims().clone, false);
          if (next == noCell) {
            core_.arena().link(cloneTail, core_.dims().clone, false, cloneCell);
            break;
          }
          if (next == cloneCell) {
            break;
          }
          cloneTail = next;
        }
        outputs.push_back(cloneCell);
      } else {
        outputs.push_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Jump: {
    if (!inputs.empty() && cursor != noCell) {
      CellRef targetOp = toInt(inputs[0]);
      setCursorOpcode(cursor, targetOp);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::Branch: {
    if (inputs.size() >= 2 && cursor != noCell &&
        VortexCore::evaluateTruthiness(inputs[0])) {
      CellRef targetOp = toInt(inputs[1]);
      setCursorOpcode(cursor, targetOp);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::Call: {
    if (!inputs.empty() && cursor != noCell) {
      CellRef targetOp = toInt(inputs[0]);
      CellRef nextOp   = core_.arena().linked(opcode, core_.dims().spin, false);
      if (nextOp != noCell) {
        CellRef frame = core_.arena().makeCell();
        core_.arena().setValueBits(frame, xanadu::ValueKind::Int64,
                                   static_cast<std::uint64_t>(nextOp));
        CellRef oldStack =
            core_.arena().linked(cursor, core_.dims().stack, false);
        core_.arena().link(cursor, core_.dims().stack, false, frame);
        if (oldStack != noCell) {
          core_.arena().link(frame, core_.dims().stack, false, oldStack);
        }
      }
      setCursorOpcode(cursor, targetOp);
      jumped = true;
    }
    break;
  }
  case OpcodeKind::Return: {
    if (cursor != noCell) {
      CellRef frame = core_.arena().linked(cursor, core_.dims().stack, false);
      if (frame != noCell) {
        auto returnOpVal = core_.arena().asInt64(frame);
        CellRef returnOp =
            returnOpVal ? static_cast<CellRef>(*returnOpVal) : noCell;
        CellRef nextStack =
            core_.arena().linked(frame, core_.dims().stack, false);
        core_.arena().link(cursor, core_.dims().stack, false, nextStack);
        setCursorOpcode(cursor, returnOp);
        jumped = true;
      }
    }
    break;
  }
  case OpcodeKind::Bind: {
    // #BIND name val: binds var in cursor's scope (+d.vars and +d.values)
    if (inputs.size() >= 2 && cursor != noCell) {
      std::string varName = std::holds_alternative<std::string>(inputs[0])
                                ? std::get<std::string>(inputs[0])
                                : std::to_string(toInt(inputs[0]));
      CellRef varCell     = core_.arena().makeCell(varName);
      CellRef valCell     = core_.arena().makeCell();
      core_.value(valCell, 0, -1, inputs[1]);
      core_.arena().link(varCell, core_.dims().values, false, valCell);

      CellRef firstVar = core_.arena().linked(cursor, core_.dims().vars, false);
      if (firstVar == noCell) {
        core_.arena().link(cursor, core_.dims().vars, false, varCell);
      } else {
        core_.arena().link(varCell, core_.dims().vars, false, firstVar);
        core_.arena().link(cursor, core_.dims().vars, false, varCell);
      }
      outputs.push_back(valCell);
    }
    break;
  }
  case OpcodeKind::Resolve: {
    // #RESOLVE name: finds var in cursor's scope
    if (!inputs.empty() && cursor != noCell) {
      std::string varName = std::holds_alternative<std::string>(inputs[0])
                                ? std::get<std::string>(inputs[0])
                                : std::to_string(toInt(inputs[0]));
      CellRef curVar = core_.arena().linked(cursor, core_.dims().vars, false);
      std::size_t limit = core_.arena().cellCount() + 1;
      bool found        = false;
      while (curVar != noCell && limit-- > 0) {
        if (core_.arena().textOf(curVar) == varName) {
          CellRef valCell =
              core_.arena().linked(curVar, core_.dims().values, false);
          if (valCell != noCell) {
            outputs.push_back(core_.render(valCell));
            found = true;
            break;
          }
        }
        curVar = core_.arena().linked(curVar, core_.dims().vars, false);
      }
      if (!found) {
        outputs.push_back(false);
      }
    }
    break;
  }
  case OpcodeKind::Assert: {
    if (inputs.empty() || !VortexCore::evaluateTruthiness(inputs[0])) {
      return ExecutionResult{false, ContractViolationKind::None, opcode,
                             "Assertion failed"};
    }
    break;
  }
  case OpcodeKind::Abs: {
    if (!inputs.empty()) {
      if (std::holds_alternative<double>(inputs[0])) {
        outputs.push_back(std::abs(std::get<double>(inputs[0])));
      } else {
        std::int64_t v = toInt(inputs[0]);
        outputs.push_back(v < 0 ? -v : v);
      }
    }
    break;
  }
  case OpcodeKind::Min: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.push_back(std::min(toDouble(inputs[0]), toDouble(inputs[1])));
      } else {
        outputs.push_back(std::min(toInt(inputs[0]), toInt(inputs[1])));
      }
    }
    break;
  }
  case OpcodeKind::Max: {
    if (inputs.size() >= 2) {
      if (std::holds_alternative<double>(inputs[0]) ||
          std::holds_alternative<double>(inputs[1])) {
        outputs.push_back(std::max(toDouble(inputs[0]), toDouble(inputs[1])));
      } else {
        outputs.push_back(std::max(toInt(inputs[0]), toInt(inputs[1])));
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
        outputs.push_back(std::clamp(v, lo, hi));
      } else {
        std::int64_t v  = toInt(inputs[0]);
        std::int64_t lo = toInt(inputs[1]);
        std::int64_t hi = toInt(inputs[2]);
        outputs.push_back(std::clamp(v, lo, hi));
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
        outputs.push_back(std::string{});
      } else {
        std::size_t end = s.find_last_not_of(" \t\n\r");
        outputs.push_back(s.substr(start, end - start + 1));
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
      outputs.push_back(s);
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
      outputs.push_back(s);
    }
    break;
  }
  case OpcodeKind::Unify: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    CellRef a = resolveCell(inCells, inputs, 0, core_.arena());
    CellRef b = resolveCell(inCells, inputs, 1, core_.arena());
    if (a != noCell && b != noCell) {
      zigzag::Vlog vlog{.m     = core_.arena(),
                        .clone = core_.dims().clone,
                        .grab  = core_.dims().grab,
                        .step  = core_.dims().step,
                        .vars  = core_.dims().vars};
      outputs.push_back(vlog.unify(a, b));
    } else {
      outputs.push_back(false);
    }
    break;
  }
  case OpcodeKind::IsVar: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    CellRef c = resolveCell(inCells, inputs, 0, core_.arena());
    if (c != noCell) {
      zigzag::Vlog vlog{.m     = core_.arena(),
                        .clone = core_.dims().clone,
                        .grab  = core_.dims().grab,
                        .step  = core_.dims().step,
                        .vars  = core_.dims().vars};
      outputs.push_back(vlog.isUnbound(c));
    } else {
      outputs.push_back(false);
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
    outputs.push_back(static_cast<std::int64_t>(var));
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    std::vector<CellRef> args;
    std::size_t maxArgs = std::max(inCells.size(), inputs.size());
    for (std::size_t i = 1; i < maxArgs; ++i) {
      CellRef argCell = resolveCell(inCells, inputs, i, core_.arena());
      if (argCell != noCell) {
        args.push_back(argCell);
      }
    }
    CellRef term = core_.arena().makeCell(functor);
    CellRef prev = noCell;
    for (CellRef arg : args) {
      if (prev == noCell) {
        core_.arena().link(term, core_.dims().grab, false, arg);
      } else {
        core_.arena().link(prev, core_.dims().step, false, arg);
      }
      prev = arg;
    }
    outputs.push_back(static_cast<std::int64_t>(term));
    break;
  }
  case OpcodeKind::Deref: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    CellRef c = resolveCell(inCells, inputs, 0, core_.arena());
    if (c != noCell) {
      zigzag::Vlog vlog{.m     = core_.arena(),
                        .clone = core_.dims().clone,
                        .grab  = core_.dims().grab,
                        .step  = core_.dims().step,
                        .vars  = core_.dims().vars};
      outputs.push_back(static_cast<std::int64_t>(vlog.deref(c)));
    } else {
      outputs.push_back(static_cast<std::int64_t>(noCell));
    }
    break;
  }
  case OpcodeKind::Choice: {
    CellRef altOp = noCell;
    if (!inputs.empty()) {
      altOp = static_cast<CellRef>(toInt(inputs[0]));
    }
    pushChoicePoint(cursor, altOp);
    outputs.push_back(true);
    break;
  }
  case OpcodeKind::Fail: {
    if (backtrack(cursor)) {
      jumped = true;
    } else {
      return ExecutionResult{false, ContractViolationKind::None, opcode,
                             "Logic failure: no remaining choice points"};
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
    outputs.push_back(true);
    break;
  }
  case OpcodeKind::Halt:
  case OpcodeKind::Nop:
  default:
    break;
  }
  return ExecutionResult{true, ContractViolationKind::None, noCell, ""};
}

ExecutionResult VortexVM::step(CellRef cursor) {
  CellRef op = getCursorOpcode(cursor);
  if (op == noCell) {
    return ExecutionResult{true, ContractViolationKind::None, noCell,
                           "Finished"};
  }

  auto kindOpt    = getOpcodeKind(op);
  OpcodeKind kind = kindOpt ? *kindOpt : OpcodeKind::Nop;
  if (kind == OpcodeKind::Halt) {
    return ExecutionResult{true, ContractViolationKind::None, noCell, "Halted"};
  }

  // 1. Precondition Evaluation (-d.contract)
  std::vector<CellRef> preconds = core_.preconditionsOf(op);
  for (CellRef clause : preconds) {
    if (!evaluateCondition(clause, {}, {})) {
      return ExecutionResult{false, ContractViolationKind::Precondition, clause,
                             "Precondition violation: assertion failed"};
    }
  }

  // 2. Input Parameter Preprocessing (+d.grab -> +d.spin)
  std::vector<CellRef> inputCells = core_.inputsOf(op);
  std::unordered_set<CellRef> processedIn;
  for (CellRef paramCell : inputCells) {
    if (processedIn.insert(paramCell).second && core_.hasPipeline(paramCell)) {
      runPipeline(paramCell);
    }
  }

  // 3. Snapshot-Before-Write
  std::vector<CellValue> oldInputs;
  oldInputs.reserve(inputCells.size());
  for (CellRef paramCell : inputCells) {
    oldInputs.push_back(core_.render(paramCell));
  }

  // 4. Memoization Check (d.cache)
  std::vector<CellRef> outCells = core_.outputsOf(op);
  bool memoHit                  = false;
  std::vector<CellValue> newOutputs;

  if (isMemoized(op)) {
    std::string key = getMemoKey(op);
    CellRef pin     = core_.getOrCreateMemoPin(key);
    auto cached     = core_.lookupMemo(pin, oldInputs);
    if (cached.has_value()) {
      memoHit    = true;
      newOutputs = *cached;
      for (std::size_t i = 0; i < std::min(outCells.size(), newOutputs.size());
           ++i) {
        core_.value(outCells[i], 0, -1, newOutputs[i]);
      }
    }
  }

  // 5. Opcode Body Execution & Output Write
  bool jumped = false;
  if (!memoHit) {
    auto execRes =
        executeOpcodeBody(kind, op, oldInputs, newOutputs, cursor, jumped);
    if (!execRes.success) {
      return execRes;
    }

    // Write outputs for non-structural opcodes
    if (kind != OpcodeKind::New && kind != OpcodeKind::Clone &&
        kind != OpcodeKind::Link && kind != OpcodeKind::Break) {
      for (std::size_t i = 0; i < std::min(outCells.size(), newOutputs.size());
           ++i) {
        core_.value(outCells[i], 0, -1, newOutputs[i]);
      }
    }

    // 6. Output Parameter Postprocessing (-d.grab -> +d.spin)
    std::unordered_set<CellRef> processedOut;
    for (CellRef outCell : outCells) {
      if (processedOut.insert(outCell).second && core_.hasPipeline(outCell)) {
        runPipeline(outCell);
      }
    }
  }

  // 7. Postcondition Evaluation (+d.contract)
  std::vector<CellValue> finalOutputs;
  finalOutputs.reserve(outCells.size());
  for (CellRef outCell : outCells) {
    finalOutputs.push_back(core_.render(outCell));
  }

  std::vector<CellRef> postconds = core_.postconditionsOf(op);
  for (CellRef clause : postconds) {
    if (!evaluateCondition(clause, oldInputs, finalOutputs)) {
      return ExecutionResult{false, ContractViolationKind::Postcondition,
                             clause,
                             "Postcondition violation: assertion failed"};
    }
  }

  // 8. Memoization Record (on Miss)
  if (isMemoized(op) && !memoHit) {
    std::string key = getMemoKey(op);
    CellRef pin     = core_.getOrCreateMemoPin(key);
    core_.recordMemo(pin, oldInputs, finalOutputs);
  }

  // 9. Advance Cursor
  if (!jumped) {
    CellRef nextOp = core_.arena().linked(op, core_.dims().spin, false);
    setCursorOpcode(cursor, nextOp);
  }

  return ExecutionResult{true, ContractViolationKind::None, noCell, ""};
}

ExecutionResult VortexVM::run(CellRef cursor, std::size_t maxCycles) {
  for (std::size_t i = 0; i < maxCycles; ++i) {
    CellRef op = getCursorOpcode(cursor);
    if (op == noCell) {
      return ExecutionResult{true, ContractViolationKind::None, noCell,
                             "Finished"};
    }
    auto kind = getOpcodeKind(op);
    if (kind && *kind == OpcodeKind::Halt) {
      return ExecutionResult{true, ContractViolationKind::None, noCell,
                             "Halted"};
    }
    auto res = step(cursor);
    if (!res.success) {
      return res;
    }
  }
  return ExecutionResult{false, ContractViolationKind::None, noCell,
                         "Exceeded max cycles"};
}

std::size_t VortexVM::stepScheduler() {
  std::size_t executed         = 0;
  std::vector<CellRef> cursors = activeCursors();
  for (CellRef cursor : cursors) {
    CellRef op = getCursorOpcode(cursor);
    if (op != noCell) {
      auto res = step(cursor);
      if (res.success) {
        executed++;
      }
    }
  }
  return executed;
}

} // namespace zigzag::vortex
