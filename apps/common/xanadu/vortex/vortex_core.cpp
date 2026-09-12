#include "common/xanadu/vortex/vortex_core.hpp"

#include <bit>
#include <cctype>
#include <iostream>

#include "common/xanadu/scalar.hpp"

namespace zigzag::vortex {

VortexCore::VortexCore(ArenaManifold &arena) : arena_(arena) { initGenesis(); }

CellRef VortexCore::mintNamedDimension(std::string_view name,
                                       CellRef &lastDimCell) {
  CellRef dimCell = arena_.makeCell(name);
  if (lastDimCell != noCell && dims_.dims != noCell) {
    arena_.link(lastDimCell, dims_.dims, DimVector::POS, dimCell);
  }
  lastDimCell = dimCell;
  return dimCell;
}

void VortexCore::initGenesis() {
  home_           = arena_.makeCell("home");
  CellRef lastDim = noCell;

  // System Dimension Genesis off home_
  dims_.dims = mintNamedDimension("d.dims", lastDim);
  arena_.link(home_, dims_.dims, DimVector::POS, dims_.dims);

  dims_.grab           = mintNamedDimension("d.grab", lastDim);
  dims_.step           = mintNamedDimension("d.step", lastDim);
  dims_.spin           = mintNamedDimension("d.spin", lastDim);
  dims_.stack          = mintNamedDimension("d.stack", lastDim);
  dims_.contract       = mintNamedDimension("d.contract", lastDim);
  dims_.clone          = mintNamedDimension("d.clone", lastDim);
  dims_.cursors        = mintNamedDimension("d.cursors", lastDim);
  dims_.cache          = mintNamedDimension("d.cache", lastDim);
  dims_.vars           = mintNamedDimension("d.vars", lastDim);
  dims_.values         = mintNamedDimension("d.values", lastDim);
  dims_.pinningCursors = mintNamedDimension("d.pinning-cursors", lastDim);
  dims_.name           = mintNamedDimension("d.name", lastDim);
  dims_.stdlib         = mintNamedDimension("d.stdlib", lastDim);
  dims_.clause         = mintNamedDimension("d.clause", lastDim);
  dims_.stores         = mintNamedDimension("d.stores", lastDim);
  dims_.role           = mintNamedDimension("d.role", lastDim);
}

CellRef VortexCore::mintDimension(std::string_view name) {
  CellRef tail = home_;
  while (true) {
    CellRef next = arena_.linked(tail, dims_.dims, DimVector::POS);
    if (next == noCell || next == tail) {
      break;
    }
    tail = next;
  }
  return mintNamedDimension(name, tail);
}

std::optional<CellRef> VortexCore::link(CellRef cell, DimRef dim, DimVector dir,
                                        std::optional<CellRef> target) {
  if (!arena_.contains(cell)) {
    return std::nullopt;
  }

  // 1. Read form (target omitted)
  if (!target.has_value()) {
    CellRef existing = arena_.linked(cell, dim, dir);
    if (existing == noCell) {
      return std::nullopt;
    }
    return existing;
  }

  const CellRef raw_target = *target;

  // 2. Allocation form (target == -1)
  if (raw_target == static_cast<CellRef>(-1)) {
    CellRef created = arena_.makeCell();
    arena_.link(cell, dim, dir, created);
    return created;
  }

  // 3. Isolation form (target == 0 / noCell)
  if (raw_target == 0 || raw_target == noCell) {
    CellRef old = arena_.linked(cell, dim, dir);
    if (old == noCell) {
      return std::nullopt;
    }
    arena_.link(cell, dim, dir, noCell);
    return old;
  }

  // 4. Literal target
  arena_.link(cell, dim, dir, raw_target);
  return raw_target;
}

std::optional<CellRef> VortexCore::value(CellRef cell, std::int64_t offset,
                                         std::int64_t length,
                                         std::optional<CellValue> replacement) {
  if (!arena_.contains(cell)) {
    return std::nullopt;
  }

  // All reads and writes resolve through the clone master
  const CellRef master = arena_.cloneMaster(cell, dims_.clone);

  // Write Branch
  if (replacement.has_value()) {
    const auto &repl = *replacement;
    if (std::holds_alternative<double>(repl)) {
      arena_.setValueBits(master, xanadu::ValueKind::Double,
                          xanadu::canonicalDoubleBits(std::get<double>(repl)));
      arena_.setContent(master, {});
    } else if (std::holds_alternative<std::int64_t>(repl)) {
      arena_.setValueBits(
          master, xanadu::ValueKind::Int64,
          std::bit_cast<std::uint64_t>(std::get<std::int64_t>(repl)));
      arena_.setContent(master, {});
    } else if (std::holds_alternative<bool>(repl)) {
      arena_.setValueBits(master, xanadu::ValueKind::Bool,
                          std::get<bool>(repl) ? 1 : 0);
      arena_.setContent(master, {});
    } else {
      // String Splice
      const std::string &replStr = std::get<std::string>(repl);
      std::string current        = arena_.textOf(master);
      const auto [from, count] = resolve_range(current.size(), offset, length);
      current.replace(from, count, replStr);
      const auto span = arena_.intern(current);
      arena_.setValueBits(master, xanadu::ValueKind::None, 0);
      arena_.setContent(master,
                        std::span<const xanadu::PrimediaSpan>{&span, 1});
    }
    return cell; // Return cell so path expressions carry through
  }

  // Read Whole Content
  if (offset == 0 && length < 0) {
    return cell;
  }

  // Read Slice (ephemeral transclusion)
  std::string current      = arena_.textOf(master);
  const auto [from, count] = resolve_range(current.size(), offset, length);
  std::string sliceText    = current.substr(from, count);
  CellRef sliceCell        = arena_.makeCell(sliceText);
  return sliceCell;
}

std::optional<CellRef> VortexCore::newCell(CellRef cell, DimRef dim,
                                           DimVector dir) {
  return link(cell, dim, dir, static_cast<CellRef>(-1));
}

std::optional<CellRef> VortexCore::newCell(CellRef cell, DimRef dim,
                                           DimVector dir,
                                           const CellValue &val) {
  auto created = newCell(cell, dim, dir);
  if (created) {
    value(*created, 0, -1, val);
  }
  return created;
}

std::optional<CellRef> VortexCore::breakLink(CellRef cell, DimRef dim,
                                             DimVector dir) {
  return link(cell, dim, dir, noCell);
}

std::optional<CellRef> VortexCore::splice(CellRef cell, std::int64_t offset,
                                          std::int64_t length,
                                          const CellValue &val) {
  return value(cell, offset, length, val);
}

std::optional<CellRef> VortexCore::insert(CellRef cell, std::int64_t offset,
                                          const CellValue &val) {
  return value(cell, offset, 0, val);
}

std::optional<CellRef> VortexCore::append(CellRef cell, const CellValue &val) {
  const auto cur   = render(cell);
  std::int64_t end = 0;
  if (std::holds_alternative<std::string>(cur)) {
    end = static_cast<std::int64_t>(std::get<std::string>(cur).size());
  }
  return value(cell, end, 0, val);
}

std::optional<CellRef> VortexCore::erase(CellRef cell, std::int64_t offset,
                                         std::int64_t length) {
  return value(cell, offset, length, CellValue(std::string{}));
}

CellValue VortexCore::get(CellRef cell, std::int64_t offset,
                          std::int64_t length) {
  if (offset == 0 && length < 0) {
    return render(cell);
  }
  const auto slice = value(cell, offset, length);
  if (!slice) {
    return false;
  }
  return render(*slice);
}

CellValue VortexCore::render(CellRef cell) const {
  if (!arena_.contains(cell)) {
    return false;
  }
  const CellRef master = arena_.cloneMaster(cell, dims_.clone);
  const auto kind      = arena_.valueKindOf(master);
  if (kind == xanadu::ValueKind::Double) {
    auto d = arena_.asDouble(master);
    if (d) return *d;
  } else if (kind == xanadu::ValueKind::Int64) {
    auto i = arena_.asInt64(master);
    if (i) return *i;
  } else if (kind == xanadu::ValueKind::Bool) {
    auto b = arena_.asBool(master);
    if (b) return *b;
  }
  return arena_.textOf(master);
}

bool VortexCore::isTruthy(CellRef cell) const {
  return evaluateTruthiness(render(cell));
}

bool VortexCore::evaluateTruthiness(const CellValue &val) {
  if (std::holds_alternative<bool>(val)) {
    return std::get<bool>(val);
  }
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val) != 0.0;
  }
  if (std::holds_alternative<std::int64_t>(val)) {
    return std::get<std::int64_t>(val) != 0;
  }
  const std::string &s = std::get<std::string>(val);
  std::string lower    = s;
  for (char &c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
    return true;
  }
  if (lower.empty() || lower == "0" || lower == "false" || lower == "no" ||
      lower == "off") {
    return false;
  }
  return true;
}

std::function<CellRef()> VortexCore::cloneGenerator(CellRef source) {
  return [this, source]() -> CellRef {
    CellRef fresh      = arena_.makeCell();
    CellRef cloneTail  = source;
    std::size_t cLimit = arena_.cellCount() + 1;
    while (cLimit-- > 0) {
      CellRef next = arena_.linked(cloneTail, dims_.clone, DimVector::POS);
      if (next == noCell) {
        arena_.link(cloneTail, dims_.clone, DimVector::POS, fresh);
        break;
      }
      cloneTail = next;
    }
    return fresh;
  };
}

std::vector<CellRef> VortexCore::inputsOf(CellRef opcode) const {
  std::vector<CellRef> result;
  CellRef cur       = arena_.linked(opcode, dims_.grab, DimVector::POS);
  std::size_t limit = arena_.cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    auto val = arena_.asInt64(cur);
    if (val && arena_.contains(static_cast<CellRef>(*val))) {
      result.push_back(static_cast<CellRef>(*val));
    } else {
      result.push_back(arena_.cloneMaster(cur, dims_.clone));
    }
    cur = arena_.linked(cur, dims_.step, DimVector::POS);
  }
  return result;
}

std::vector<CellRef> VortexCore::outputsOf(CellRef opcode) const {
  std::vector<CellRef> result;
  CellRef cur       = arena_.linked(opcode, dims_.grab, DimVector::NEG);
  std::size_t limit = arena_.cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    auto val = arena_.asInt64(cur);
    if (val && arena_.contains(static_cast<CellRef>(*val))) {
      result.push_back(static_cast<CellRef>(*val));
    } else {
      result.push_back(arena_.cloneMaster(cur, dims_.clone));
    }
    cur = arena_.linked(cur, dims_.step, DimVector::POS);
  }
  return result;
}

void VortexCore::bindInput(CellRef opcode, CellRef operand) {
  if (!arena_.contains(opcode) || !arena_.contains(operand)) {
    return;
  }
  CellRef slot = arena_.makeScalarCell(static_cast<std::int64_t>(operand));

  // Link slot into opcode's input wing (+d.grab, then chain on +d.step)
  CellRef first = arena_.linked(opcode, dims_.grab, DimVector::POS);
  if (first == noCell) {
    arena_.link(opcode, dims_.grab, DimVector::POS, slot);
    return;
  }
  CellRef cur       = first;
  std::size_t limit = arena_.cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = arena_.linked(cur, dims_.step, DimVector::POS);
    if (next == noCell) {
      arena_.link(cur, dims_.step, DimVector::POS, slot);
      return;
    }
    cur = next;
  }
}

void VortexCore::bindOutput(CellRef opcode, CellRef target) {
  if (!arena_.contains(opcode) || !arena_.contains(target)) {
    return;
  }
  CellRef slot = arena_.makeScalarCell(static_cast<std::int64_t>(target));

  // Link slot into opcode's output wing (-d.grab, then chain on +d.step)
  CellRef first = arena_.linked(opcode, dims_.grab, DimVector::NEG);
  if (first == noCell) {
    arena_.link(opcode, dims_.grab, DimVector::NEG, slot);
    return;
  }
  CellRef cur       = first;
  std::size_t limit = arena_.cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = arena_.linked(cur, dims_.step, DimVector::POS);
    if (next == noCell) {
      arena_.link(cur, dims_.step, DimVector::POS, slot);
      return;
    }
    cur = next;
  }
}

bool VortexCore::hasPipeline(CellRef paramCell) const {
  if (paramCell == home_ || paramCell == noCell) {
    return false;
  }
  // A cell inside an instruction stream has a predecessor along -d.spin;
  // a parameter cell heading a pipeline has only an outgoing +d.spin.
  if (arena_.linked(paramCell, dims_.spin, DimVector::NEG) != noCell) {
    return false;
  }
  return arena_.linked(paramCell, dims_.spin, DimVector::POS) != noCell;
}

CellRef VortexCore::getPipelineHead(CellRef paramCell) const {
  if (paramCell == home_ || paramCell == noCell) {
    return noCell;
  }
  if (arena_.linked(paramCell, dims_.spin, DimVector::NEG) != noCell) {
    return noCell;
  }
  return arena_.linked(paramCell, dims_.spin, DimVector::POS);
}

void VortexCore::attachPipeline(CellRef paramCell, CellRef firstOp) {
  arena_.link(paramCell, dims_.spin, DimVector::POS, firstOp);
}

std::vector<CellRef> VortexCore::preconditionsOf(CellRef opcode) const {
  std::vector<CellRef> result;
  CellRef cur       = arena_.linked(opcode, dims_.contract, DimVector::NEG);
  std::size_t limit = arena_.cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    result.push_back(cur);
    cur = arena_.linked(cur, dims_.step, DimVector::POS);
  }
  return result;
}

std::vector<CellRef> VortexCore::postconditionsOf(CellRef opcode) const {
  std::vector<CellRef> result;
  CellRef cur       = arena_.linked(opcode, dims_.contract, DimVector::POS);
  std::size_t limit = arena_.cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    result.push_back(cur);
    cur = arena_.linked(cur, dims_.step, DimVector::POS);
  }
  return result;
}

void VortexCore::attachPrecondition(CellRef opcode, CellRef conditionOp) {
  CellRef first = arena_.linked(opcode, dims_.contract, DimVector::NEG);
  if (first == noCell) {
    arena_.link(opcode, dims_.contract, DimVector::NEG, conditionOp);
    return;
  }
  CellRef cur       = first;
  std::size_t limit = arena_.cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = arena_.linked(cur, dims_.step, DimVector::POS);
    if (next == noCell) {
      arena_.link(cur, dims_.step, DimVector::POS, conditionOp);
      return;
    }
    cur = next;
  }
}

void VortexCore::attachPostcondition(CellRef opcode, CellRef conditionOp) {
  CellRef first = arena_.linked(opcode, dims_.contract, DimVector::POS);
  if (first == noCell) {
    arena_.link(opcode, dims_.contract, DimVector::POS, conditionOp);
    return;
  }
  CellRef cur       = first;
  std::size_t limit = arena_.cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = arena_.linked(cur, dims_.step, DimVector::POS);
    if (next == noCell) {
      arena_.link(cur, dims_.step, DimVector::POS, conditionOp);
      return;
    }
    cur = next;
  }
}

CellRef VortexCore::pin(std::string_view name, CellRef targetNode) {
  CellRef pinCursor = arena_.makeCell();
  CellRef nameCell  = arena_.makeCell(name);
  arena_.link(pinCursor, dims_.name, DimVector::POS, nameCell);
  if (targetNode != noCell) {
    arena_.link(pinCursor, dims_.cache, DimVector::POS, targetNode);
  }

  // Link into home_ +d.pinning-cursors rank
  CellRef first = arena_.linked(home_, dims_.pinningCursors, DimVector::POS);
  if (first == noCell) {
    arena_.link(home_, dims_.pinningCursors, DimVector::POS, pinCursor);
  } else {
    CellRef cur       = first;
    std::size_t limit = arena_.cellCount() + 1;
    while (limit-- > 0) {
      CellRef next = arena_.linked(cur, dims_.pinningCursors, DimVector::POS);
      if (next == noCell) {
        arena_.link(cur, dims_.pinningCursors, DimVector::POS, pinCursor);
        break;
      }
      cur = next;
    }
  }
  return pinCursor;
}

std::optional<CellRef> VortexCore::findPin(std::string_view name) const {
  CellRef cur = arena_.linked(home_, dims_.pinningCursors, DimVector::POS);
  std::size_t limit = arena_.cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    CellRef nameCell = arena_.linked(cur, dims_.name, DimVector::POS);
    if (nameCell != noCell && arena_.textOf(nameCell) == name) {
      return cur;
    }
    cur = arena_.linked(cur, dims_.pinningCursors, DimVector::POS);
  }
  return std::nullopt;
}

bool VortexCore::flushPin(std::string_view name) {
  auto pinOpt = findPin(name);
  if (!pinOpt) {
    return false;
  }
  arena_.link(*pinOpt, dims_.cache, DimVector::POS, noCell);
  return true;
}

bool VortexCore::retirePin(std::string_view name) {
  CellRef prev = home_;
  CellRef cur  = arena_.linked(home_, dims_.pinningCursors, DimVector::POS);
  std::size_t limit = arena_.cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    CellRef nameCell = arena_.linked(cur, dims_.name, DimVector::POS);
    if (nameCell != noCell && arena_.textOf(nameCell) == name) {
      CellRef next = arena_.linked(cur, dims_.pinningCursors, DimVector::POS);
      if (prev == home_) {
        arena_.link(home_, dims_.pinningCursors, DimVector::POS, next);
      } else {
        arena_.link(prev, dims_.pinningCursors, DimVector::POS, next);
      }
      arena_.link(cur, dims_.pinningCursors, DimVector::POS, noCell);
      arena_.link(cur, dims_.cache, DimVector::POS, noCell);
      return true;
    }
    prev = cur;
    cur  = arena_.linked(cur, dims_.pinningCursors, DimVector::POS);
  }
  return false;
}

std::size_t VortexCore::collectGarbage() {
  std::size_t deadBefore = arena_.deadLinks();
  arena_.compact();
  return deadBefore;
}

CellRef VortexCore::getOrCreateMemoPin(std::string_view opName) {
  auto pinOpt = findPin(opName);
  if (pinOpt) {
    return *pinOpt;
  }
  return pin(opName, noCell);
}

std::optional<std::vector<CellValue>>
VortexCore::lookupMemo(CellRef memoPin, const std::vector<CellValue> &inputs) {
  CellRef entry     = arena_.linked(memoPin, dims_.cache, DimVector::POS);
  std::size_t limit = arena_.cellCount() + 1;
  while (entry != noCell && limit-- > 0) {
    std::vector<CellRef> entryInputs = inputsOf(entry);
    if (entryInputs.size() == inputs.size()) {
      bool match = true;
      for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (render(entryInputs[i]) != inputs[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        std::vector<CellRef> entryOutputs = outputsOf(entry);
        std::vector<CellValue> results;
        results.reserve(entryOutputs.size());
        for (CellRef outRef : entryOutputs) {
          results.push_back(render(outRef));
        }
        return results;
      }
    }
    entry = arena_.linked(entry, dims_.cache, DimVector::POS);
  }
  return std::nullopt;
}

void VortexCore::recordMemo(CellRef memoPin,
                            const std::vector<CellValue> &inputs,
                            const std::vector<CellValue> &outputs) {
  CellRef entry = arena_.makeCell();
  for (const auto &inVal : inputs) {
    CellRef inCell = arena_.makeCell();
    value(inCell, 0, -1, inVal);
    bindInput(entry, inCell);
  }
  for (const auto &outVal : outputs) {
    CellRef outCell = arena_.makeCell();
    value(outCell, 0, -1, outVal);
    bindOutput(entry, outCell);
  }

  // Insert entry at head of memoPin's +d.cache rank
  CellRef oldHead = arena_.linked(memoPin, dims_.cache, DimVector::POS);
  arena_.link(memoPin, dims_.cache, DimVector::POS, entry);
  if (oldHead != noCell) {
    arena_.link(entry, dims_.cache, DimVector::POS, oldHead);
  }
}

bool VortexCore::flushMemo(std::string_view opName) { return flushPin(opName); }

bool VortexCore::retireMemo(std::string_view opName) {
  return retirePin(opName);
}

} // namespace zigzag::vortex
