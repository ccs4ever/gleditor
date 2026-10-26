/**
 * @file vortex_vm.cpp
 * @brief Implementation of Spin-Head Virtual Machine for Vortex.
 */
#include "common/xanadu/vortex/vortex_vm.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/cell_views.hpp"
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

std::optional<CellRef> resolveCell(const std::vector<CellRef> &inCells,
                                   const std::vector<CellValue> &inputs,
                                   std::size_t idx,
                                   const ArenaManifold &arena) {
  if (idx < inCells.size()) {
    CellRef c = inCells[idx];
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

} // namespace

VortexVM::VortexVM(VortexCore &core) : core_(core) {}

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

void VortexVM::setOpcodeKind(CellRef opCell, OpcodeKind op) {
  opcodeMap_[opCell] = op;
}

std::optional<OpcodeKind> VortexVM::getOpcodeKind(CellRef opCell) const {
  auto it = opcodeMap_.find(opCell);
  if (it != opcodeMap_.end()) {
    return it->second;
  }
  auto val = core_.arena().asInt64(opCell);
  if (val && *val >= 0 &&
      static_cast<std::uint64_t>(*val) <=
          static_cast<std::uint64_t>(OpcodeKind::ChoiceDepth)) {
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

void VortexVM::setCursorOpcode(CellRef cursor, std::optional<CellRef> op) {
  if (op && *op == noCell) {
    throw std::invalid_argument(
        "an opcode reference must be a cell or nullopt");
  }
  cursorPC_[cursor] = op;
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

void VortexVM::enableMemoization(CellRef opcode, std::string_view memoKey) {
  memoizedOps_[opcode] = std::string(memoKey);
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
  if (cursor && *cursor == noCell) {
    throw std::invalid_argument(
        "choice-point cursor must be a cell or nullopt");
  }
  if (altOp == noCell || !core_.arena().contains(altOp) ||
      !getOpcodeKind(altOp)) {
    throw std::invalid_argument("choice point requires a valid alternative");
  }
  std::optional<CellRef> stackFrame;
  if (cursor && core_.arena().contains(*cursor)) {
    stackFrame = core_.arena().linked(*cursor, core_.dims().stack, false);
  }
  std::vector<std::uint16_t> altDewey;
  if (!oraclePath_.empty() || cutCallback_) {
    altDewey = currentDeweyPath_;
    altDewey.push_back(1);
    currentDeweyPath_.push_back(0);
  }
  choiceStack_.push_back(ChoicePoint{
      .cursor          = cursor,
      .altOp           = altOp,
      .mark            = core_.arena().mark(),
      .stackFrame      = stackFrame,
      .cutBarrier      = choiceStack_.size(),
      .deweyPath       = std::move(altDewey),
      .nextAltIndex    = 1,
      .cyclesSinceRoot = totalCycles_,
  });
}

bool VortexVM::backtrack(const std::optional<CellRef> cursor) {
  if (cursor && *cursor == noCell) {
    throw std::invalid_argument("backtrack cursor must be a cell or nullopt");
  }
  while (!choiceStack_.empty()) {
    ChoicePoint cp = choiceStack_.back();
    choiceStack_.pop_back();
    if (!cursor || cp.cursor == cursor || !cp.cursor) {
      core_.arena().release(cp.mark);
      if (!oraclePath_.empty() || cutCallback_) {
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
  while (choiceStack_.size() > cutBarrier) {
    ChoicePoint cp = choiceStack_.back();
    choiceStack_.pop_back();
    core_.arena().discard(cp.mark);
    if (cutCallback_ && !cp.deweyPath.empty()) {
      const auto parentSpan =
          std::span<const std::uint16_t>{cp.deweyPath}.subspan(
              0, cp.deweyPath.size() - 1);
      cutCallback_(formatDeweyPath(parentSpan));
    }
  }
  if (cutCallback_ && choiceStack_.empty() && !currentDeweyPath_.empty()) {
    const auto parentSpan =
        std::span<const std::uint16_t>{currentDeweyPath_}.subspan(
            0, currentDeweyPath_.size() - 1);
    cutCallback_(formatDeweyPath(parentSpan));
  }
}

std::size_t VortexVM::choiceDepth() const noexcept {
  return choiceStack_.size();
}

void VortexVM::clearChoicePoints() { cut(0); }

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
  if (choiceStack_.empty()) {
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

void VortexVM::runPipeline(CellRef paramCell) {
  auto pipeOp       = core_.getPipelineHead(paramCell);
  std::size_t limit = 1000;
  while (pipeOp && limit-- > 0) {
    auto kindOpt    = getOpcodeKind(*pipeOp);
    OpcodeKind kind = kindOpt ? *kindOpt : OpcodeKind::Nop;
    if (kind == OpcodeKind::Halt || kind == OpcodeKind::Return) {
      break;
    }

    std::vector<CellRef> inCells = core_.inputsOf(*pipeOp);
    std::vector<CellValue> inVals;
    if (inCells.empty()) {
      inVals.push_back(core_.render(paramCell).value());
    } else {
      for (CellRef c : inCells) {
        inVals.push_back(core_.render(c).value());
      }
    }

    std::vector<CellValue> outVals;
    bool jumped = false;
    executeOpcodeBody(kind, *pipeOp, inVals, outVals, std::nullopt, jumped);

    std::vector<CellRef> outCells = core_.outputsOf(*pipeOp);
    if (outCells.empty()) {
      if (!outVals.empty()) {
        core_.writeValue(paramCell, 0, std::nullopt, outVals[0]);
      }
    } else {
      if (kind != OpcodeKind::New && kind != OpcodeKind::Clone &&
          kind != OpcodeKind::Link && kind != OpcodeKind::Break) {
        for (std::size_t i = 0; i < std::min(outCells.size(), outVals.size());
             ++i) {
          core_.writeValue(outCells[i], 0, std::nullopt, outVals[i]);
        }
      }
    }

    pipeOp = core_.arena().linked(*pipeOp, core_.dims().spin, false);
  }
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

  OpcodeKind kind              = *kindOpt;
  std::vector<CellRef> inCells = core_.inputsOf(actualClause);
  std::vector<CellValue> inVals;
  if (!inCells.empty()) {
    for (CellRef c : inCells) {
      inVals.push_back(core_.render(c).value());
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

ExecutionResult
VortexVM::executeOpcodeBody(OpcodeKind kind, CellRef opcode,
                            const std::vector<CellValue> &inputs,
                            std::vector<CellValue> &outputs,
                            const std::optional<CellRef> cursor, bool &jumped) {
  const auto overflowFailure = [opcode] {
    return ExecutionResult{.success = false,
                           .contractViolation =
                               ContractViolationKind::Precondition,
                           .failingClause = opcode,
                           .errorMessage = "Signed integer overflow in opcode"};
  };
  constexpr auto minInt = std::numeric_limits<std::int64_t>::min();
  switch (kind) {
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      const auto c = resolveCell(inCells, inputs, 0, core_.arena());
      DimRef d     = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg     = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      std::optional<CellRef> tgt = std::nullopt;
      if (inCells.size() >= 4 || inputs.size() >= 4) {
        const auto t = resolveCell(inCells, inputs, 3, core_.arena());
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      const auto c = resolveCell(inCells, inputs, 0, core_.arena());
      DimRef d     = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg     = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      if (c) {
        auto res = core_.breakLink(*c, d, neg);
        outputs.emplace_back(res ? *res : noCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::New: {
    std::vector<CellRef> inCells  = core_.inputsOf(opcode);
    std::vector<CellRef> outCells = core_.outputsOf(opcode);
    if (inCells.size() >= 3 || inputs.size() >= 3) {
      const auto c = resolveCell(inCells, inputs, 0, core_.arena());
      DimRef d     = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      bool neg     = inputs.size() >= 3 && toInt(inputs[2]) < 0;
      if (c) {
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (!inCells.empty() || !inputs.empty()) {
      const auto c     = resolveCell(inCells, inputs, 0, core_.arena());
      std::int64_t off = inputs.size() >= 2 ? toInt(inputs[1]) : 0;
      std::optional<std::int64_t> len =
          inputs.size() >= 3 ? std::optional<std::int64_t>{toInt(inputs[2])}
                             : std::nullopt;
      std::optional<CellValue> repl = inputs.size() >= 4
                                          ? std::optional<CellValue>(inputs[3])
                                          : std::nullopt;
      if (c) {
        auto res = repl ? core_.writeValue(*c, off, len, *repl)
                        : core_.readValue(*c, off, len);
        outputs.emplace_back(res ? *res : noCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Splice: {
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    if (inCells.size() >= 4 || inputs.size() >= 4) {
      const auto c     = resolveCell(inCells, inputs, 0, core_.arena());
      std::int64_t off = toInt(inputs[1]);
      std::int64_t len = toInt(inputs[2]);
      if (c) {
        auto res = core_.splice(*c, off, len, inputs[3]);
        outputs.emplace_back(res ? *res : noCell);
      } else {
        outputs.emplace_back(noCell);
      }
    }
    break;
  }
  case OpcodeKind::Clone: {
    std::vector<CellRef> inCells  = core_.inputsOf(opcode);
    std::vector<CellRef> outCells = core_.outputsOf(opcode);
    if (!inCells.empty() || !inputs.empty()) {
      const auto c = resolveCell(inCells, inputs, 0, core_.arena());
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
    if (inputs.empty() || !VortexCore::evaluateTruthiness(inputs[0])) {
      return ExecutionResult{.success           = false,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = opcode,
                             .errorMessage      = "Assertion failed"};
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    const auto a = resolveCell(inCells, inputs, 0, core_.arena());
    const auto b = resolveCell(inCells, inputs, 1, core_.arena());
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    const auto c = resolveCell(inCells, inputs, 0, core_.arena());
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    std::vector<CellRef> args;
    std::size_t maxArgs = std::max(inCells.size(), inputs.size());
    for (std::size_t i = 1; i < maxArgs; ++i) {
      const auto argCell = resolveCell(inCells, inputs, i, core_.arena());
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
    std::vector<CellRef> inCells = core_.inputsOf(opcode);
    const auto c = resolveCell(inCells, inputs, 0, core_.arena());
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
  const auto op = getCursorOpcode(cursor);
  if (!op) {
    return ExecutionResult{.success           = true,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage      = "Finished"};
  }

  auto kindOpt    = getOpcodeKind(*op);
  OpcodeKind kind = kindOpt ? *kindOpt : OpcodeKind::Nop;
  if (kind == OpcodeKind::Halt) {
    return ExecutionResult{.success           = true,
                           .contractViolation = ContractViolationKind::None,
                           .failingClause     = std::nullopt,
                           .errorMessage      = "Halted"};
  }

  totalCycles_++;

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
  std::vector<CellRef> inputCells = core_.inputsOf(*op);
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
    oldInputs.push_back(core_.render(paramCell).value());
  }

  // 4. Memoization Check (d.cache)
  std::vector<CellRef> outCells = core_.outputsOf(*op);
  bool memoHit                  = false;
  std::vector<CellValue> newOutputs;

  if (isMemoized(*op)) {
    if (const auto key = getMemoKey(*op)) {
      CellRef pin = core_.getOrCreateMemoPin(*key);
      auto cached = core_.lookupMemo(pin, oldInputs);
      if (cached.has_value()) {
        memoHit    = true;
        newOutputs = *cached;
        for (std::size_t i = 0;
             i < std::min(outCells.size(), newOutputs.size()); ++i) {
          core_.writeValue(outCells[i], 0, std::nullopt, newOutputs[i]);
        }
      }
    }
  }

  // 5. Opcode Body Execution & Output Write
  bool jumped = false;
  if (!memoHit) {
    auto execRes =
        executeOpcodeBody(kind, *op, oldInputs, newOutputs, cursor, jumped);
    if (!execRes.success) {
      return execRes;
    }

    // Write outputs for non-structural opcodes
    if (kind != OpcodeKind::New && kind != OpcodeKind::Clone &&
        kind != OpcodeKind::Link && kind != OpcodeKind::Break) {
      for (std::size_t i = 0; i < std::min(outCells.size(), newOutputs.size());
           ++i) {
        core_.writeValue(outCells[i], 0, std::nullopt, newOutputs[i]);
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
    finalOutputs.push_back(core_.render(outCell).value());
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

  // 8. Memoization Record (on Miss)
  if (!memoHit) {
    if (const auto key = getMemoKey(*op)) {
      CellRef pin = core_.getOrCreateMemoPin(*key);
      core_.recordMemo(pin, oldInputs, finalOutputs);
    }
  }

  // 9. Advance Cursor
  if (!jumped) {
    const auto nextOp = core_.arena().linked(*op, core_.dims().spin, false);
    setCursorOpcode(cursor, nextOp);
  }

  return ExecutionResult{.success           = true,
                         .contractViolation = ContractViolationKind::None,
                         .failingClause     = std::nullopt,
                         .errorMessage      = ""};
}

ExecutionResult VortexVM::run(CellRef cursor, std::size_t maxCycles,
                              std::stop_token stopToken) {
  for (std::size_t i = 0; i < maxCycles; ++i) {
    if (stopToken.stop_requested()) {
      return ExecutionResult{.success           = false,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = std::nullopt,
                             .errorMessage      = "Cancelled by cut"};
    }
    const auto op = getCursorOpcode(cursor);
    if (!op) {
      return ExecutionResult{.success           = true,
                             .contractViolation = ContractViolationKind::None,
                             .failingClause     = std::nullopt,
                             .errorMessage      = "Finished"};
    }
    auto kind = getOpcodeKind(*op);
    if (kind && *kind == OpcodeKind::Halt) {
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
