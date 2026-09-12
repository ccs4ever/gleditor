/**
 * @file vortex_stdlib.cpp
 * @brief Implementation of Vortex Standard Library in Vortex.
 */
#include "common/xanadu/vortex/vortex_stdlib.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <sstream>

namespace zigzag::vortex {

namespace {

std::int64_t toInt64(const CellValue &v) {
  if (std::holds_alternative<std::int64_t>(v)) {
    return std::get<std::int64_t>(v);
  }
  if (std::holds_alternative<double>(v)) {
    return static_cast<std::int64_t>(std::get<double>(v));
  }
  if (std::holds_alternative<bool>(v)) {
    return std::get<bool>(v) ? 1 : 0;
  }
  try {
    return std::stoll(std::get<std::string>(v));
  } catch (...) {
    return 0;
  }
}

double toDoubleVal(const CellValue &v) {
  if (std::holds_alternative<double>(v)) {
    return std::get<double>(v);
  }
  if (std::holds_alternative<std::int64_t>(v)) {
    return static_cast<double>(std::get<std::int64_t>(v));
  }
  if (std::holds_alternative<bool>(v)) {
    return std::get<bool>(v) ? 1.0 : 0.0;
  }
  try {
    return std::stod(std::get<std::string>(v));
  } catch (...) {
    return 0.0;
  }
}

} // namespace

VortexStdLib::VortexStdLib(VortexCore &core, VortexVM &vm)
    : core_(core), vm_(vm) {
  bootstrap();
}

CellRef VortexStdLib::getOrCreateModule(std::string_view modulePath) {
  CellRef cur  = core_.arena().linked(core_.home(), core_.dims().stdlib, false);
  CellRef prev = core_.home();
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    if (core_.arena().textOf(cur) == modulePath) {
      return cur;
    }
    prev = cur;
    cur  = core_.arena().linked(cur, core_.dims().stdlib, false);
  }

  // Mint new module cell
  CellRef modCell = core_.arena().makeCell(modulePath);
  if (prev == core_.home()) {
    core_.arena().link(core_.home(), core_.dims().stdlib, false, modCell);
  } else {
    core_.arena().link(prev, core_.dims().stdlib, false, modCell);
  }
  return modCell;
}

void VortexStdLib::exportSymbol(CellRef moduleCell, std::string_view symbolName,
                                CellRef entryOp) {
  CellRef symCell = core_.arena().makeCell(symbolName);
  core_.arena().link(symCell, core_.dims().values, false, entryOp);

  CellRef first = core_.arena().linked(moduleCell, core_.dims().vars, false);
  if (first == noCell) {
    core_.arena().link(moduleCell, core_.dims().vars, false, symCell);
  } else {
    CellRef cur       = first;
    std::size_t limit = core_.arena().cellCount() + 1;
    while (limit-- > 0) {
      CellRef next = core_.arena().linked(cur, core_.dims().vars, false);
      if (next == noCell) {
        core_.arena().link(cur, core_.dims().vars, false, symCell);
        break;
      }
      cur = next;
    }
  }
}

void VortexStdLib::bootstrap() {
  CellRef modMemoize     = getOrCreateModule("std:memoize");
  CellRef modContract    = getOrCreateModule("std:contract");
  CellRef modPipeline    = getOrCreateModule("std:pipeline");
  CellRef modFunctional  = getOrCreateModule("std:functional");
  CellRef modCollections = getOrCreateModule("std:collections");
  CellRef modMath        = getOrCreateModule("std:math");
  CellRef modString      = getOrCreateModule("std:string");

  buildMathModule(modMath);
  buildStringModule(modString);
  buildContractModule(modContract);
  buildPipelineModule(modPipeline);
  buildMemoizeModule(modMemoize);
  buildFunctionalModule(modFunctional);
  buildCollectionsModule(modCollections);
}

void VortexStdLib::buildMathModule(CellRef mod) {
  // abs: #ABS in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Abs, "#MATH_ABS");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "abs", op);
  }

  // min: #MIN in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Min, "#MATH_MIN");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in0, in1}, {out}};
    exportSymbol(mod, "min", op);
  }

  // max: #MAX in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Max, "#MATH_MAX");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in0, in1}, {out}};
    exportSymbol(mod, "max", op);
  }

  // clamp: #CLAMP in lo hi out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef lo  = core_.arena().makeCell();
    CellRef hi  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Clamp, "#MATH_CLAMP");
    core_.bindInput(op, in);
    core_.bindInput(op, lo);
    core_.bindInput(op, hi);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in, lo, hi}, {out}};
    exportSymbol(mod, "clamp", op);
  }

  // add: #ADD in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Add, "#MATH_ADD");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in0, in1}, {out}};
    exportSymbol(mod, "add", op);
  }

  // mul: #MUL in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Mul, "#MATH_MUL");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in0, in1}, {out}};
    exportSymbol(mod, "mul", op);
  }
}

void VortexStdLib::buildStringModule(CellRef mod) {
  // to_lower: #TO_LOWER in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::ToLower, "#STR_TO_LOWER");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "to_lower", op);
  }

  // to_upper: #TO_UPPER in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::ToUpper, "#STR_TO_UPPER");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "to_upper", op);
  }

  // trim: #TRIM in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Trim, "#STR_TRIM");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "trim", op);
  }
}

void VortexStdLib::buildContractModule(CellRef mod) {
  // Export templates as named symbols for lookup
  CellRef dummyIn = core_.arena().makeCell();
  exportSymbol(mod, "require_positive", createRequirePositive(dummyIn));
  exportSymbol(mod, "require_non_negative", createRequireNonNegative(dummyIn));
  exportSymbol(mod, "require_non_empty", createRequireNonEmpty(dummyIn));
}

void VortexStdLib::buildPipelineModule(CellRef mod) {
  exportSymbol(mod, "trim", createTrimPipeline());
  exportSymbol(mod, "to_lower", createToLowerPipeline());
  exportSymbol(mod, "to_upper", createToUpperPipeline());
}

void VortexStdLib::buildMemoizeModule(CellRef mod) {
  CellRef stubOp = vm_.mintOpcode(OpcodeKind::Nop, "#MEMO_DISPATCH");
  exportSymbol(mod, "memoize", stubOp);
}

void VortexStdLib::buildFunctionalModule(CellRef mod) {
  CellRef stubMap = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_MAP");
  exportSymbol(mod, "map", stubMap);
  CellRef stubFilter = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_FILTER");
  exportSymbol(mod, "filter", stubFilter);
  CellRef stubFold = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_FOLD");
  exportSymbol(mod, "fold", stubFold);
  CellRef stubZip = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_ZIP");
  exportSymbol(mod, "zip", stubZip);
}

void VortexStdLib::buildCollectionsModule(CellRef mod) {
  CellRef stubList = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_LIST");
  exportSymbol(mod, "list", stubList);
  CellRef stubMap = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_MAP");
  exportSymbol(mod, "map", stubMap);
  CellRef stubGrid = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_GRID");
  exportSymbol(mod, "grid", stubGrid);
}

CellRef VortexStdLib::resolve(std::string_view path) const {
  auto slashPos = path.find('/');
  std::string_view modPath =
      slashPos == std::string_view::npos ? path : path.substr(0, slashPos);
  std::string_view symName = slashPos == std::string_view::npos
                                 ? std::string_view{}
                                 : path.substr(slashPos + 1);

  CellRef curMod =
      core_.arena().linked(core_.home(), core_.dims().stdlib, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (curMod != noCell && limit-- > 0) {
    if (core_.arena().textOf(curMod) == modPath) {
      if (symName.empty()) {
        return curMod;
      }
      // Walk +d.vars for symbol
      CellRef curSym = core_.arena().linked(curMod, core_.dims().vars, false);
      std::size_t symLimit = core_.arena().cellCount() + 1;
      while (curSym != noCell && symLimit-- > 0) {
        if (core_.arena().textOf(curSym) == symName) {
          return core_.arena().linked(curSym, core_.dims().values, false);
        }
        curSym = core_.arena().linked(curSym, core_.dims().vars, false);
      }
      return noCell;
    }
    curMod = core_.arena().linked(curMod, core_.dims().stdlib, false);
  }
  return noCell;
}

bool VortexStdLib::has(std::string_view path) const {
  return resolve(path) != noCell;
}

std::vector<std::string> VortexStdLib::modules() const {
  std::vector<std::string> result;
  CellRef curMod =
      core_.arena().linked(core_.home(), core_.dims().stdlib, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (curMod != noCell && limit-- > 0) {
    result.push_back(core_.arena().textOf(curMod));
    curMod = core_.arena().linked(curMod, core_.dims().stdlib, false);
  }
  return result;
}

std::vector<std::string>
VortexStdLib::symbolsInModule(std::string_view modulePath) const {
  std::vector<std::string> result;
  CellRef mod = resolve(modulePath);
  if (mod == noCell) {
    return result;
  }
  CellRef curSym    = core_.arena().linked(mod, core_.dims().vars, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (curSym != noCell && limit-- > 0) {
    result.push_back(core_.arena().textOf(curSym));
    curSym = core_.arena().linked(curSym, core_.dims().vars, false);
  }
  return result;
}

std::vector<CellValue> VortexStdLib::call(std::string_view path,
                                          const std::vector<CellValue> &args) {
  CellRef op = resolve(path);
  if (op == noCell) {
    return {};
  }
  return call(op, args);
}

std::vector<CellValue> VortexStdLib::call(CellRef fnOp,
                                          const std::vector<CellValue> &args) {
  if (fnOp == noCell) {
    return {};
  }
  auto it = routineBindings_.find(fnOp);
  if (it != routineBindings_.end()) {
    const auto &bindings = it->second;
    for (std::size_t i = 0;
         i < std::min(args.size(), bindings.inputParams.size()); ++i) {
      core_.value(bindings.inputParams[i], 0, -1, args[i]);
    }
    CellRef cursor = vm_.spawnCursor(fnOp, "call");
    vm_.run(cursor, 100);
    std::vector<CellValue> results;
    results.reserve(bindings.outputParams.size());
    for (CellRef outCell : bindings.outputParams) {
      results.push_back(core_.render(outCell));
    }
    return results;
  }

  // Fallback: Bind directly to opcode input wings
  std::vector<CellRef> inCells = core_.inputsOf(fnOp);
  for (std::size_t i = 0; i < std::min(args.size(), inCells.size()); ++i) {
    core_.value(inCells[i], 0, -1, args[i]);
  }
  CellRef cursor = vm_.spawnCursor(fnOp, "call");
  vm_.run(cursor, 100);
  std::vector<CellRef> outCells = core_.outputsOf(fnOp);
  std::vector<CellValue> results;
  results.reserve(outCells.size());
  for (CellRef outCell : outCells) {
    results.push_back(core_.render(outCell));
  }
  return results;
}

// -- Module 1: std:memoize ----------------------------------------------------
CellRef VortexStdLib::memoize(CellRef op, std::string_view cacheKey,
                              std::size_t /*capacity*/) {
  vm_.enableMemoization(op, cacheKey);
  return core_.getOrCreateMemoPin(cacheKey);
}

bool VortexStdLib::flushMemo(std::string_view cacheKey) {
  return core_.flushMemo(cacheKey);
}

bool VortexStdLib::retireMemo(std::string_view cacheKey) {
  return core_.retireMemo(cacheKey);
}

std::size_t VortexStdLib::memoEntryCount(std::string_view cacheKey) const {
  auto pinOpt = core_.findPin(cacheKey);
  if (!pinOpt) {
    return 0;
  }
  std::size_t count = 0;
  CellRef cur       = core_.arena().linked(*pinOpt, core_.dims().cache, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    count++;
    cur = core_.arena().linked(cur, core_.dims().cache, false);
  }
  return count;
}

// -- Module 2: std:contract ---------------------------------------------------
CellRef VortexStdLib::createRequirePositive(CellRef inParam) {
  CellRef zero = core_.arena().makeScalarCell(static_cast<std::int64_t>(0));
  CellRef req  = vm_.mintOpcode(OpcodeKind::Gt, "#REQUIRE_POSITIVE");
  core_.bindInput(req, inParam);
  core_.bindInput(req, zero);
  return req;
}

CellRef VortexStdLib::createRequireNonNegative(CellRef inParam) {
  CellRef zero = core_.arena().makeScalarCell(static_cast<std::int64_t>(0));
  CellRef req  = vm_.mintOpcode(OpcodeKind::Gte, "#REQUIRE_NON_NEGATIVE");
  core_.bindInput(req, inParam);
  core_.bindInput(req, zero);
  return req;
}

CellRef VortexStdLib::createRequireRange(CellRef inParam, double lo,
                                         double hi) {
  CellRef loCell = core_.arena().makeScalarCell(lo);
  CellRef hiCell = core_.arena().makeScalarCell(hi);
  CellRef gte    = vm_.mintOpcode(OpcodeKind::Gte, "#RANGE_LO");
  core_.bindInput(gte, inParam);
  core_.bindInput(gte, loCell);

  CellRef lte = vm_.mintOpcode(OpcodeKind::Lte, "#RANGE_HI");
  core_.bindInput(lte, inParam);
  core_.bindInput(lte, hiCell);

  CellRef andOp = vm_.mintOpcode(OpcodeKind::And, "#RANGE_AND");
  core_.bindInput(andOp, gte);
  core_.bindInput(andOp, lte);
  return andOp;
}

CellRef VortexStdLib::createRequireNonEmpty(CellRef inParam) {
  CellRef emptyStr = core_.arena().makeCell("");
  CellRef req      = vm_.mintOpcode(OpcodeKind::Neq, "#REQUIRE_NON_EMPTY");
  core_.bindInput(req, inParam);
  core_.bindInput(req, emptyStr);
  return req;
}

CellRef VortexStdLib::createEnsureGrowth(CellRef outParam, CellRef inParam) {
  CellRef req = vm_.mintOpcode(OpcodeKind::Gte, "#ENSURE_GROWTH");
  core_.bindInput(req, outParam);
  core_.bindInput(req, inParam);
  return req;
}

// -- Module 3: std:pipeline ---------------------------------------------------
CellRef VortexStdLib::createTrimPipeline() {
  return vm_.mintOpcode(OpcodeKind::Trim, "#PIPE_TRIM");
}

CellRef VortexStdLib::createToLowerPipeline() {
  return vm_.mintOpcode(OpcodeKind::ToLower, "#PIPE_TO_LOWER");
}

CellRef VortexStdLib::createToUpperPipeline() {
  return vm_.mintOpcode(OpcodeKind::ToUpper, "#PIPE_TO_UPPER");
}

CellRef VortexStdLib::createClampPipeline(double lo, double hi) {
  CellRef op     = vm_.mintOpcode(OpcodeKind::Clamp, "#PIPE_CLAMP");
  CellRef loCell = core_.arena().makeScalarCell(lo);
  CellRef hiCell = core_.arena().makeScalarCell(hi);
  core_.bindInput(op, loCell);
  core_.bindInput(op, hiCell);
  return op;
}

CellRef VortexStdLib::createParseNumPipeline() {
  return vm_.mintOpcode(OpcodeKind::Add, "#PIPE_PARSE_NUM");
}

CellRef VortexStdLib::createFormatCurrencyPipeline() {
  return vm_.mintOpcode(OpcodeKind::Add, "#PIPE_FORMAT_CURRENCY");
}

// -- Module 4: std:functional -------------------------------------------------
CellRef VortexStdLib::map(CellRef head, DimRef inDim, DimRef outDim,
                          std::function<CellValue(const CellValue &)> fn) {
  if (head == noCell) return noCell;
  CellRef resHead = noCell;
  CellRef resTail = noCell;

  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    CellValue inVal  = core_.render(cur);
    CellValue outVal = fn(inVal);
    CellRef newC     = core_.arena().makeCell();
    core_.value(newC, 0, -1, outVal);
    if (resHead == noCell) {
      resHead = newC;
      resTail = newC;
    } else {
      core_.arena().link(resTail, outDim, false, newC);
      resTail = newC;
    }
    cur = core_.arena().linked(cur, inDim, false);
  }
  return resHead;
}

CellRef VortexStdLib::map(CellRef head, DimRef inDim, DimRef outDim,
                          CellRef fnOp) {
  return map(head, inDim, outDim, [this, fnOp](const CellValue &val) {
    auto res = call(fnOp, {val});
    return res.empty() ? val : res[0];
  });
}

CellRef VortexStdLib::filter(CellRef head, DimRef inDim, DimRef outDim,
                             std::function<bool(const CellValue &)> pred) {
  if (head == noCell) return noCell;
  CellRef resHead = noCell;
  CellRef resTail = noCell;

  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    CellValue inVal = core_.render(cur);
    if (pred(inVal)) {
      CellRef newC = core_.arena().makeCell();
      core_.value(newC, 0, -1, inVal);
      if (resHead == noCell) {
        resHead = newC;
        resTail = newC;
      } else {
        core_.arena().link(resTail, outDim, false, newC);
        resTail = newC;
      }
    }
    cur = core_.arena().linked(cur, inDim, false);
  }
  return resHead;
}

CellRef VortexStdLib::filter(CellRef head, DimRef inDim, DimRef outDim,
                             CellRef predOp) {
  return filter(head, inDim, outDim, [this, predOp](const CellValue &val) {
    auto res = call(predOp, {val});
    if (res.empty()) return false;
    return VortexCore::evaluateTruthiness(res[0]);
  });
}

CellValue VortexStdLib::fold(
    CellRef head, DimRef inDim, CellValue initial,
    std::function<CellValue(const CellValue &, const CellValue &)> fn) {
  CellValue acc     = initial;
  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    acc = fn(acc, core_.render(cur));
    cur = core_.arena().linked(cur, inDim, false);
  }
  return acc;
}

CellValue VortexStdLib::fold(CellRef head, DimRef inDim, CellValue initial,
                             CellRef fnOp) {
  return fold(head, inDim, initial,
              [this, fnOp](const CellValue &acc, const CellValue &item) {
                auto res = call(fnOp, {acc, item});
                return res.empty() ? acc : res[0];
              });
}

CellRef VortexStdLib::zip(CellRef headA, CellRef headB, DimRef dimA,
                          DimRef dimB, DimRef outDim) {
  CellRef curA      = headA;
  CellRef curB      = headB;
  CellRef resHead   = noCell;
  CellRef resTail   = noCell;
  std::size_t limit = core_.arena().cellCount() + 1;

  while (curA != noCell && curB != noCell && limit-- > 0) {
    CellRef pairCell = core_.arena().makeCell();
    // In zzstructures, a pair cell can link curA negward on dimA, and curB
    // posward on dimB
    core_.arena().link(pairCell, dimA, true, curA);
    core_.arena().link(pairCell, dimB, false, curB);

    if (resHead == noCell) {
      resHead = pairCell;
      resTail = pairCell;
    } else {
      core_.arena().link(resTail, outDim, false, pairCell);
      resTail = pairCell;
    }
    curA = core_.arena().linked(curA, dimA, false);
    curB = core_.arena().linked(curB, dimB, false);
  }
  return resHead;
}

// -- Module 5: std:collections ----------------------------------------------
CellRef VortexStdLib::createList(const std::vector<CellValue> &items,
                                 DimRef dim) {
  DimRef linkDim = dim == noCell ? core_.dims().step : dim;
  CellRef head   = noCell;
  CellRef tail   = noCell;
  for (const auto &item : items) {
    CellRef c = core_.arena().makeCell();
    core_.value(c, 0, -1, item);
    if (head == noCell) {
      head = c;
      tail = c;
    } else {
      core_.arena().link(tail, linkDim, false, c);
      tail = c;
    }
  }
  return head;
}

std::vector<CellValue> VortexStdLib::listToVector(CellRef head,
                                                  DimRef dim) const {
  DimRef linkDim = dim == noCell ? core_.dims().step : dim;
  std::vector<CellValue> result;
  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    result.push_back(core_.render(cur));
    cur = core_.arena().linked(cur, linkDim, false);
  }
  return result;
}

void VortexStdLib::pushBack(CellRef head, const CellValue &val, DimRef dim) {
  if (head == noCell) return;
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = core_.arena().linked(cur, linkDim, false);
    if (next == noCell) {
      CellRef c = core_.arena().makeCell();
      core_.value(c, 0, -1, val);
      core_.arena().link(cur, linkDim, false, c);
      return;
    }
    cur = next;
  }
}

CellRef VortexStdLib::pushFront(CellRef head, const CellValue &val,
                                DimRef dim) {
  DimRef linkDim = dim == noCell ? core_.dims().step : dim;
  CellRef c      = core_.arena().makeCell();
  core_.value(c, 0, -1, val);
  if (head != noCell) {
    core_.arena().link(c, linkDim, false, head);
  }
  return c;
}

std::optional<CellValue> VortexStdLib::popBack(CellRef head, DimRef dim) {
  if (head == noCell) return std::nullopt;
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  CellRef cur       = head;
  CellRef prev      = noCell;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = core_.arena().linked(cur, linkDim, false);
    if (next == noCell) {
      CellValue val = core_.render(cur);
      if (prev != noCell) {
        core_.breakLink(prev, linkDim, false);
      }
      return val;
    }
    prev = cur;
    cur  = next;
  }
  return std::nullopt;
}

CellRef VortexStdLib::popFront(CellRef head, DimRef dim) {
  if (head == noCell) return noCell;
  DimRef linkDim = dim == noCell ? core_.dims().step : dim;
  CellRef next   = core_.arena().linked(head, linkDim, false);
  core_.breakLink(head, linkDim, false);
  return next;
}

std::size_t VortexStdLib::listLength(CellRef head, DimRef dim) const {
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  std::size_t len   = 0;
  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    len++;
    cur = core_.arena().linked(cur, linkDim, false);
  }
  return len;
}

CellRef VortexStdLib::createMap() { return core_.arena().makeCell("map"); }

void VortexStdLib::mapSet(CellRef mapRoot, std::string_view key,
                          const CellValue &val) {
  if (mapRoot == noCell) return;
  // Check existing key
  CellRef cur       = core_.arena().linked(mapRoot, core_.dims().vars, false);
  CellRef prev      = mapRoot;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    if (core_.arena().textOf(cur) == key) {
      CellRef valCell = core_.arena().linked(cur, core_.dims().values, false);
      if (valCell == noCell) {
        valCell = core_.arena().makeCell();
        core_.arena().link(cur, core_.dims().values, false, valCell);
      }
      core_.value(valCell, 0, -1, val);
      return;
    }
    prev = cur;
    cur  = core_.arena().linked(cur, core_.dims().vars, false);
  }

  // Mint fresh key-value pair
  CellRef keyCell = core_.arena().makeCell(key);
  CellRef valCell = core_.arena().makeCell();
  core_.value(valCell, 0, -1, val);
  core_.arena().link(keyCell, core_.dims().values, false, valCell);

  if (prev == mapRoot) {
    core_.arena().link(mapRoot, core_.dims().vars, false, keyCell);
  } else {
    core_.arena().link(prev, core_.dims().vars, false, keyCell);
  }
}

std::optional<CellValue> VortexStdLib::mapGet(CellRef mapRoot,
                                              std::string_view key) const {
  if (mapRoot == noCell) return std::nullopt;
  CellRef cur       = core_.arena().linked(mapRoot, core_.dims().vars, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    if (core_.arena().textOf(cur) == key) {
      CellRef valCell = core_.arena().linked(cur, core_.dims().values, false);
      if (valCell != noCell) {
        return core_.render(valCell);
      }
      return std::nullopt;
    }
    cur = core_.arena().linked(cur, core_.dims().vars, false);
  }
  return std::nullopt;
}

bool VortexStdLib::mapHas(CellRef mapRoot, std::string_view key) const {
  return mapGet(mapRoot, key).has_value();
}

std::vector<std::string> VortexStdLib::mapKeys(CellRef mapRoot) const {
  std::vector<std::string> keys;
  if (mapRoot == noCell) return keys;
  CellRef cur       = core_.arena().linked(mapRoot, core_.dims().vars, false);
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    keys.push_back(core_.arena().textOf(cur));
    cur = core_.arena().linked(cur, core_.dims().vars, false);
  }
  return keys;
}

CellRef VortexStdLib::createGrid(std::size_t rows, std::size_t cols,
                                 DimRef dRow, DimRef dCol,
                                 const CellValue &initVal) {
  if (rows == 0 || cols == 0) return noCell;
  std::vector<std::vector<CellRef>> grid(rows,
                                         std::vector<CellRef>(cols, noCell));

  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c < cols; ++c) {
      grid[r][c] = core_.arena().makeCell();
      core_.value(grid[r][c], 0, -1, initVal);
    }
  }

  // Link horizontally on dCol
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c + 1 < cols; ++c) {
      core_.arena().link(grid[r][c], dCol, false, grid[r][c + 1]);
    }
  }

  // Link vertically on dRow
  for (std::size_t r = 0; r + 1 < rows; ++r) {
    for (std::size_t c = 0; c < cols; ++c) {
      core_.arena().link(grid[r][c], dRow, false, grid[r + 1][c]);
    }
  }

  return grid[0][0];
}

CellValue VortexStdLib::getGrid(CellRef gridRoot, std::size_t r, std::size_t c,
                                DimRef dRow, DimRef dCol) const {
  CellRef cur = gridRoot;
  for (std::size_t i = 0; i < r && cur != noCell; ++i) {
    cur = core_.arena().linked(cur, dRow, false);
  }
  for (std::size_t j = 0; j < c && cur != noCell; ++j) {
    cur = core_.arena().linked(cur, dCol, false);
  }
  if (cur == noCell) return false;
  return core_.render(cur);
}

void VortexStdLib::setGrid(CellRef gridRoot, std::size_t r, std::size_t c,
                           DimRef dRow, DimRef dCol, const CellValue &val) {
  CellRef cur = gridRoot;
  for (std::size_t i = 0; i < r && cur != noCell; ++i) {
    cur = core_.arena().linked(cur, dRow, false);
  }
  for (std::size_t j = 0; j < c && cur != noCell; ++j) {
    cur = core_.arena().linked(cur, dCol, false);
  }
  if (cur != noCell) {
    core_.value(cur, 0, -1, val);
  }
}

// -- Module 6: std:math -------------------------------------------------------
CellValue VortexStdLib::mathAbs(const CellValue &x) {
  if (std::holds_alternative<double>(x)) {
    return std::abs(std::get<double>(x));
  }
  std::int64_t v = toInt64(x);
  return v < 0 ? -v : v;
}

CellValue VortexStdLib::mathMin(const CellValue &a, const CellValue &b) {
  if (std::holds_alternative<double>(a) || std::holds_alternative<double>(b)) {
    return std::min(toDoubleVal(a), toDoubleVal(b));
  }
  return std::min(toInt64(a), toInt64(b));
}

CellValue VortexStdLib::mathMax(const CellValue &a, const CellValue &b) {
  if (std::holds_alternative<double>(a) || std::holds_alternative<double>(b)) {
    return std::max(toDoubleVal(a), toDoubleVal(b));
  }
  return std::max(toInt64(a), toInt64(b));
}

CellValue VortexStdLib::mathClamp(const CellValue &x, const CellValue &lo,
                                  const CellValue &hi) {
  if (std::holds_alternative<double>(x) || std::holds_alternative<double>(lo) ||
      std::holds_alternative<double>(hi)) {
    return std::clamp(toDoubleVal(x), toDoubleVal(lo), toDoubleVal(hi));
  }
  return std::clamp(toInt64(x), toInt64(lo), toInt64(hi));
}

std::int64_t VortexStdLib::mathGcd(std::int64_t a, std::int64_t b) {
  return std::gcd(a, b);
}

std::int64_t VortexStdLib::mathLcm(std::int64_t a, std::int64_t b) {
  return std::lcm(a, b);
}

double VortexStdLib::mathPow(double base, double exp) {
  return std::pow(base, exp);
}

double VortexStdLib::mathSqrt(double x) { return std::sqrt(x); }

// -- Module 7: std:string -----------------------------------------------------
std::string VortexStdLib::strToUpper(std::string_view s) {
  std::string res(s);
  for (char &c : res) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return res;
}

std::string VortexStdLib::strToLower(std::string_view s) {
  std::string res(s);
  for (char &c : res) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return res;
}

bool VortexStdLib::strStartsWith(std::string_view s, std::string_view prefix) {
  return s.starts_with(prefix);
}

bool VortexStdLib::strEndsWith(std::string_view s, std::string_view suffix) {
  return s.ends_with(suffix);
}

std::vector<std::string> VortexStdLib::strSplit(std::string_view s,
                                                std::string_view delim) {
  std::vector<std::string> parts;
  if (delim.empty()) {
    parts.emplace_back(s);
    return parts;
  }
  std::size_t start = 0;
  while (start < s.size()) {
    auto pos = s.find(delim, start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(s.substr(start));
      break;
    }
    parts.emplace_back(s.substr(start, pos - start));
    start = pos + delim.size();
  }
  return parts;
}

std::string VortexStdLib::strJoin(const std::vector<std::string> &parts,
                                  std::string_view delim) {
  std::string res;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) res += delim;
    res += parts[i];
  }
  return res;
}

std::string VortexStdLib::strTrim(std::string_view s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string_view::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return std::string(s.substr(start, end - start + 1));
}

} // namespace zigzag::vortex
