/**
 * @file vortex_stdlib.cpp
 * @brief Implementation of Vortex Standard Library in Vortex.
 */
#include "common/xanadu/vortex/vortex_stdlib.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <ranges>
#include <sstream>
#include <unordered_set>

#include <gleditor/logging.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_worker_pool.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

namespace zigzag::vortex {

namespace {

std::optional<CellRef> cellRefArgument(ArenaManifold &arena,
                                       const CellRef argument) {
  if (const auto value = arena.asInt64(argument)) {
    if (*value < 0 || static_cast<std::uint64_t>(*value) >
                          std::numeric_limits<CellRef>::max()) {
      return std::nullopt;
    }
    const auto cell = static_cast<CellRef>(*value);
    return arena.contains(cell) ? std::optional<CellRef>{cell} : std::nullopt;
  }
  return arena.contains(argument) ? std::optional<CellRef>{argument}
                                  : std::nullopt;
}

/// Link @p cells into one rank along @p dim and answer its optional head.
/// An empty vector is a valid empty collection, not an allocation failure.
std::optional<CellRef> chainCells(ArenaManifold &arena,
                                  const std::vector<CellRef> &cells,
                                  const DimRef dim) {
  for (const auto [from, to] : cells | std::views::adjacent<2>) {
    zigzag::expectWritten(arena.link(from, dim, DimVector::POS, to));
  }
  return cells.empty() ? std::nullopt : std::optional<CellRef>{cells.front()};
}

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

void collectVariablesHelper(const VortexStdLib &stdlib, CellRef term,
                            std::vector<CellRef> &vars,
                            std::unordered_set<CellRef> &seenVars,
                            std::unordered_set<CellRef> &visitedTerms) {
  const auto actual = stdlib.deref(term);
  if (!actual) return;
  if (stdlib.isVar(*actual)) {
    if (seenVars.insert(*actual).second) {
      vars.push_back(term);
    }
    return;
  }
  if (!visitedTerms.insert(*actual).second) {
    return;
  }
  auto args = stdlib.argumentsOf(*actual);
  for (CellRef arg : args) {
    collectVariablesHelper(stdlib, arg, vars, seenVars, visitedTerms);
  }
}

void collectVariables(const VortexStdLib &stdlib, CellRef term,
                      std::vector<CellRef> &vars,
                      std::unordered_set<CellRef> &seen) {
  std::unordered_set<CellRef> visitedTerms;
  collectVariablesHelper(stdlib, term, vars, seen, visitedTerms);
}

std::optional<CellRef>
freshenTerm(VortexStdLib &stdlib, VortexCore &core, CellRef term,
            std::unordered_map<CellRef, CellRef> &varMap,
            std::unordered_map<CellRef, CellRef> &visiting) {
  const auto actual = stdlib.deref(term);
  if (!actual) {
    return std::nullopt;
  }
  if (stdlib.isVar(*actual)) {
    auto it = varMap.find(*actual);
    if (it != varMap.end()) {
      CellRef master    = it->second;
      CellRef cloneCell = core.arena().makeCell();
      zigzag::Vlog vlog{.m     = core.arena(),
                        .clone = core.dims().clone,
                        .grab  = core.dims().grab,
                        .step  = core.dims().step,
                        .vars  = core.dims().vars};
      zigzag::expectWritten(
          core.arena().link(vlog.endOfRank(master, core.dims().clone, false),
                            core.dims().clone, false, cloneCell));
      return cloneCell;
    }
    CellRef fresh       = stdlib.makeVar();
    const auto nameCell = core.arena().linked(*actual, core.dims().name, false);
    if (nameCell && core.arena().contains(*nameCell)) {
      zigzag::expectWritten(
          core.arena().link(fresh, core.dims().name, false, *nameCell));
    }
    varMap[*actual] = fresh;
    return fresh;
  }

  auto args = stdlib.argumentsOf(*actual);
  if (args.empty()) {
    CellRef cloneCell = core.arena().makeCell();
    zigzag::Vlog vlog{.m     = core.arena(),
                      .clone = core.dims().clone,
                      .grab  = core.dims().grab,
                      .step  = core.dims().step,
                      .vars  = core.dims().vars};
    zigzag::expectWritten(
        core.arena().link(vlog.endOfRank(*actual, core.dims().clone, false),
                          core.dims().clone, false, cloneCell));
    return cloneCell;
  }

  auto vit = visiting.find(*actual);
  if (vit != visiting.end()) {
    return vit->second;
  }

  std::string fn    = core.arena().textOf(*actual);
  CellRef freshTerm = core.arena().makeCell(fn);
  visiting[*actual] = freshTerm;

  CellRef prev = noCell;
  for (CellRef arg : args) {
    auto freshArg = freshenTerm(stdlib, core, arg, varMap, visiting);
    if (!freshArg) {
      visiting.erase(*actual);
      return std::nullopt;
    }
    if (prev == noCell) {
      zigzag::expectWritten(
          core.arena().link(freshTerm, core.dims().grab, false, *freshArg));
    } else {
      zigzag::expectWritten(
          core.arena().link(prev, core.dims().step, false, *freshArg));
    }
    prev = *freshArg;
  }

  visiting.erase(*actual);
  return freshTerm;
}

bool evalArithmetic(const VortexStdLib &stdlib, const VortexCore &core,
                    CellRef term, double &outVal, bool &outIsInt) {
  const auto cur = stdlib.deref(term);
  if (!cur || stdlib.isVar(*cur)) {
    return false;
  }
  const auto kind = core.arena().valueKindOf(*cur);
  if (kind == xanadu::ValueKind::Int64) {
    if (auto v = core.arena().asInt64(*cur)) {
      outVal   = static_cast<double>(*v);
      outIsInt = true;
      return true;
    }
  } else if (kind == xanadu::ValueKind::Double) {
    if (auto v = core.arena().asDouble(*cur)) {
      outVal   = *v;
      outIsInt = false;
      return true;
    }
  }

  const auto fn = stdlib.functorOf(*cur);
  if (!fn) return false;
  auto args = stdlib.argumentsOf(*cur);

  if (*fn == "-" && args.size() == 1) {
    double v = 0.0;
    bool isI = false;
    if (evalArithmetic(stdlib, core, args[0], v, isI)) {
      outVal   = -v;
      outIsInt = isI;
      return true;
    }
    return false;
  }

  if (*fn == "+" && args.size() == 1) {
    return evalArithmetic(stdlib, core, args[0], outVal, outIsInt);
  }

  if (args.size() == 2) {
    double v1 = 0.0;
    double v2 = 0.0;
    bool isI1 = false;
    bool isI2 = false;
    if (!evalArithmetic(stdlib, core, args[0], v1, isI1) ||
        !evalArithmetic(stdlib, core, args[1], v2, isI2)) {
      return false;
    }
    if (*fn == "+") {
      outVal   = v1 + v2;
      outIsInt = isI1 && isI2;
      return true;
    }
    if (*fn == "-") {
      outVal   = v1 - v2;
      outIsInt = isI1 && isI2;
      return true;
    }
    if (*fn == "*") {
      outVal   = v1 * v2;
      outIsInt = isI1 && isI2;
      return true;
    }
    if (*fn == "/") {
      if (v2 == 0.0) return false;
      outVal   = v1 / v2;
      outIsInt = false;
      return true;
    }
    if (*fn == "//") {
      if (static_cast<std::int64_t>(v2) == 0) return false;
      // Deliberate floor division -- every numeric result here is stored as
      // a double regardless of semantic type, with outIsInt tracking whether
      // it should be read back as an integer.
      // NOLINTNEXTLINE(bugprone-integer-division)
      outVal   = static_cast<double>(static_cast<std::int64_t>(v1) /
                                     static_cast<std::int64_t>(v2));
      outIsInt = true;
      return true;
    }
    if (*fn == "mod") {
      if (static_cast<std::int64_t>(v2) == 0) return false;
      outVal   = static_cast<double>(static_cast<std::int64_t>(v1) %
                                     static_cast<std::int64_t>(v2));
      outIsInt = true;
      return true;
    }
  }

  std::string txt = core.arena().textOf(*cur);
  if (!txt.empty() &&
      (std::isdigit(txt[0]) ||
       (txt.size() > 1 && txt[0] == '-' && std::isdigit(txt[1])))) {
    try {
      if (txt.contains('.')) {
        outVal   = std::stod(txt);
        outIsInt = false;
      } else {
        outVal   = static_cast<double>(std::stoll(txt));
        outIsInt = true;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  return false;
}

struct ActiveGoal {
  CellRef term{noCell};
  std::size_t cutFrame{0};
};

bool solveQueryHelper(VortexStdLib &stdlib, VortexCore &core,
                      std::vector<ActiveGoal> goals,
                      std::span<const CellRef> candidatePreds,
                      const std::vector<CellRef> &queryVars,
                      std::function<bool(const LogicSolution &)> onSolution,
                      std::size_t &solutionsCount, std::size_t maxSolutions,
                      std::size_t depth, std::size_t &nextFrameId,
                      std::size_t &cutToFrame) {
  core.arena().consumeWork();
  if (core.arena().executionLimited() && depth >= 512)
    throw std::runtime_error("Prolog resolution nesting budget exhausted");
  if (solutionsCount >= maxSolutions || depth > 1000) {
    return false;
  }
  if (goals.empty()) {
    LogicSolution sol;
    for (CellRef v : queryVars) {
      core.arena().consumeWork();
      const auto val = stdlib.deref(v);
      if (!val) return false;
      sol.bindings.emplace_back(v, *val);
      sol.varMap[v] = *val;
      std::string varName;
      const auto nameCell =
          core.arena().linked(v, core.dims().name, DimVector::POS);
      if (nameCell && core.arena().contains(*nameCell)) {
        varName = core.arena().textOf(*nameCell);
      } else {
        varName = "_G" + std::to_string(v);
      }
      const auto rendered = stdlib.renderTerm(v);
      if (!rendered) return false;
      sol.formatted[varName] = *rendered;
    }
    solutionsCount++;
    if (onSolution) {
      return onSolution(sol);
    }
    return true;
  }

  ActiveGoal curGoal = goals[0];
  std::vector<ActiveGoal> restGoals(goals.begin() + 1, goals.end());

  const auto resolvedFunctor = stdlib.functorOf(curGoal.term);
  if (!resolvedFunctor) return true;
  const std::string &goalFunctor = *resolvedFunctor;
  std::vector<CellRef> goalArgs  = stdlib.argumentsOf(curGoal.term);

  // Optimization for length/2 when list is known
  if (goalFunctor == "length" && goalArgs.size() == 2) {
    const auto listArg = stdlib.deref(goalArgs[0]);
    const auto lenArg  = stdlib.deref(goalArgs[1]);
    if (listArg && lenArg && !stdlib.isVar(*listArg)) {
      std::int64_t count = 0;
      auto cur           = listArg;
      std::size_t limit  = core.arena().cellCount() + 1;
      while (cur && !stdlib.isVar(*cur) && limit-- > 0) {
        core.arena().consumeWork();
        cur = stdlib.deref(*cur);
        if (!cur) break;
        auto args             = stdlib.argumentsOf(*cur);
        const auto curFunctor = stdlib.functorOf(*cur);
        if (curFunctor && *curFunctor == "." && args.size() >= 2) {
          count++;
          cur = stdlib.deref(args[1]);
        } else {
          break;
        }
      }
      const auto tailFunctor = cur ? stdlib.functorOf(*cur) : std::nullopt;
      if (tailFunctor && *tailFunctor == "[]") {
        auto mark       = core.arena().mark();
        CellRef numCell = core.arena().makeScalarCell(count);
        if (stdlib.unify(*lenArg, numCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          return keepGoing;
        }
        core.arena().release(mark);
        return false;
      }
    }
  }

  // Built-in predicates: true/0, fail/0, !/0
  if (goalFunctor == "true" && goalArgs.empty()) {
    return solveQueryHelper(stdlib, core, restGoals, candidatePreds, queryVars,
                            onSolution, solutionsCount, maxSolutions, depth + 1,
                            nextFrameId, cutToFrame);
  }
  if (goalFunctor == "fail" && goalArgs.empty()) {
    return true;
  }
  if (goalFunctor == "!" && goalArgs.empty()) {
    bool keepGoing = solveQueryHelper(
        stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
        solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
    if (curGoal.cutFrame > 0) {
      cutToFrame = curGoal.cutFrame;
    }
    return keepGoing;
  }

  // Manifold stdlib introspection: vortex_module/1 or stdlib_module/1
  if ((goalFunctor == "vortex_module" || goalFunctor == "stdlib_module") &&
      goalArgs.size() == 1) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
      auto mark     = core.arena().mark();
      CellRef mCell = core.arena().makeCell(modName);
      if (stdlib.unify(goalArgs[0], mCell)) {
        bool keepGoing = solveQueryHelper(
            stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
            solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
        core.arena().release(mark);
        if (cutToFrame > 0) return false;
        if (!keepGoing && onSolution != nullptr) {
          return false;
        }
      } else {
        core.arena().release(mark);
      }
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_function/2 or stdlib_function/2
  if ((goalFunctor == "vortex_function" || goalFunctor == "stdlib_function") &&
      goalArgs.size() == 2) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        auto mark     = core.arena().mark();
        CellRef mCell = core.arena().makeCell(modName);
        CellRef fCell = core.arena().makeCell(symName);
        if (stdlib.unify(goalArgs[0], mCell) &&
            stdlib.unify(goalArgs[1], fCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) {
            return false;
          }
        } else {
          core.arena().release(mark);
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_function/3 or stdlib_function/3 (Mod,
  // Fn, Path)
  if ((goalFunctor == "vortex_function" || goalFunctor == "stdlib_function") &&
      goalArgs.size() == 3) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        auto mark            = core.arena().mark();
        CellRef mCell        = core.arena().makeCell(modName);
        CellRef fCell        = core.arena().makeCell(symName);
        std::string fullPath = modName;
        fullPath += "/";
        fullPath += symName;
        CellRef pCell = core.arena().makeCell(fullPath);
        if (stdlib.unify(goalArgs[0], mCell) &&
            stdlib.unify(goalArgs[1], fCell) &&
            stdlib.unify(goalArgs[2], pCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) {
            return false;
          }
        } else {
          core.arena().release(mark);
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_function/1 or stdlib_function/1
  if ((goalFunctor == "vortex_function" || goalFunctor == "stdlib_function") &&
      goalArgs.size() == 1) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        auto mark     = core.arena().mark();
        CellRef fCell = core.arena().makeCell(symName);
        if (stdlib.unify(goalArgs[0], fCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) {
            return false;
          }
        } else {
          core.arena().release(mark);
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_instruction(Path, StepIndex,
  // OpcodeLabel)
  if (goalFunctor == "vortex_instruction" && goalArgs.size() == 3) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        std::string fullPath = modName;
        fullPath += "/";
        fullPath += symName;
        const auto resolved = stdlib.resolve(fullPath);
        if (!resolved) continue;
        const CellRef entryOp = *resolved;

        std::optional<CellRef> curOp = entryOp;
        std::int64_t stepIdx         = 0;
        std::size_t limit            = core.arena().cellCount() + 1;
        while (curOp && limit-- > 0) {
          core.arena().consumeWork();
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark         = core.arena().mark();
          CellRef pCell     = core.arena().makeCell(fullPath);
          CellRef idxCell   = core.arena().makeScalarCell(stepIdx);
          std::string label = core.arena().textOf(*curOp);
          CellRef lblCell   = core.arena().makeCell(label);

          if (stdlib.unify(goalArgs[0], pCell) &&
              stdlib.unify(goalArgs[1], idxCell) &&
              stdlib.unify(goalArgs[2], lblCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) {
              return false;
            }
          } else {
            core.arena().release(mark);
          }
          curOp = core.arena().linked(*curOp, core.dims().spin, DimVector::POS);
          stepIdx++;
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_contract(Path, Type, ContractLabel)
  if (goalFunctor == "vortex_contract" && goalArgs.size() == 3) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        std::string fullPath = modName;
        fullPath += "/";
        fullPath += symName;
        const auto resolved = stdlib.resolve(fullPath);
        if (!resolved) continue;
        const CellRef entryOp = *resolved;

        std::optional<CellRef> curOp = entryOp;
        std::size_t limit            = core.arena().cellCount() + 1;
        while (curOp && limit-- > 0) {
          core.arena().consumeWork();
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          // Preconditions along -d.contract
          std::vector<CellRef> preconds = core.preconditionsOf(*curOp);
          for (CellRef p : preconds) {
            core.arena().consumeWork();
            if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
            auto mark         = core.arena().mark();
            CellRef pCell     = core.arena().makeCell(fullPath);
            CellRef typeCell  = core.arena().makeCell("precondition");
            std::string label = core.arena().textOf(p);
            CellRef lblCell   = core.arena().makeCell(label);
            if (stdlib.unify(goalArgs[0], pCell) &&
                stdlib.unify(goalArgs[1], typeCell) &&
                stdlib.unify(goalArgs[2], lblCell)) {
              bool keepGoing = solveQueryHelper(
                  stdlib, core, restGoals, candidatePreds, queryVars,
                  onSolution, solutionsCount, maxSolutions, depth + 1,
                  nextFrameId, cutToFrame);
              core.arena().release(mark);
              if (cutToFrame > 0) return false;
              if (!keepGoing && onSolution != nullptr) return false;
            } else {
              core.arena().release(mark);
            }
          }

          // Postconditions along +d.contract
          std::vector<CellRef> postconds = core.postconditionsOf(*curOp);
          for (CellRef p : postconds) {
            core.arena().consumeWork();
            if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
            auto mark         = core.arena().mark();
            CellRef pCell     = core.arena().makeCell(fullPath);
            CellRef typeCell  = core.arena().makeCell("postcondition");
            std::string label = core.arena().textOf(p);
            CellRef lblCell   = core.arena().makeCell(label);
            if (stdlib.unify(goalArgs[0], pCell) &&
                stdlib.unify(goalArgs[1], typeCell) &&
                stdlib.unify(goalArgs[2], lblCell)) {
              bool keepGoing = solveQueryHelper(
                  stdlib, core, restGoals, candidatePreds, queryVars,
                  onSolution, solutionsCount, maxSolutions, depth + 1,
                  nextFrameId, cutToFrame);
              core.arena().release(mark);
              if (cutToFrame > 0) return false;
              if (!keepGoing && onSolution != nullptr) return false;
            } else {
              core.arena().release(mark);
            }
          }

          curOp = core.arena().linked(*curOp, core.dims().spin, DimVector::POS);
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_param(Path, Wing, SlotIndex, Target)
  if (goalFunctor == "vortex_param" && goalArgs.size() == 4) {
    for (const auto &modName : stdlib.modules()) {
      core.arena().consumeWork();
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        std::string fullPath = modName;
        fullPath += "/";
        fullPath += symName;
        const auto resolved = stdlib.resolve(fullPath);
        if (!resolved) continue;
        const CellRef entryOp = *resolved;

        // Input wing
        std::vector<CellRef> inCells = core.inputsOf(entryOp);
        for (std::size_t i = 0; i < inCells.size(); ++i) {
          core.arena().consumeWork();
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark     = core.arena().mark();
          CellRef pCell = core.arena().makeCell(fullPath);
          CellRef wCell = core.arena().makeCell("input");
          CellRef idxCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(i));
          CellRef slotCell = core.arena().makeScalarCell(
              static_cast<std::int64_t>(inCells[i]));
          if (stdlib.unify(goalArgs[0], pCell) &&
              stdlib.unify(goalArgs[1], wCell) &&
              stdlib.unify(goalArgs[2], idxCell) &&
              stdlib.unify(goalArgs[3], slotCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }

        // Output wing
        std::vector<CellRef> outCells = core.outputsOf(entryOp);
        for (std::size_t i = 0; i < outCells.size(); ++i) {
          core.arena().consumeWork();
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark     = core.arena().mark();
          CellRef pCell = core.arena().makeCell(fullPath);
          CellRef wCell = core.arena().makeCell("output");
          CellRef idxCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(i));
          CellRef slotCell = core.arena().makeScalarCell(
              static_cast<std::int64_t>(outCells[i]));
          if (stdlib.unify(goalArgs[0], pCell) &&
              stdlib.unify(goalArgs[1], wCell) &&
              stdlib.unify(goalArgs[2], idxCell) &&
              stdlib.unify(goalArgs[3], slotCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Arithmetic evaluation: is/2
  if (goalFunctor == "is" && goalArgs.size() == 2) {
    double evalResult = 0.0;
    bool isInt        = false;
    if (evalArithmetic(stdlib, core, goalArgs[1], evalResult, isInt)) {
      auto mark       = core.arena().mark();
      CellRef valCell = isInt ? core.arena().makeScalarCell(
                                    static_cast<std::int64_t>(evalResult))
                              : core.arena().makeScalarCell(evalResult);
      if (stdlib.unify(goalArgs[0], valCell)) {
        bool keepGoing = solveQueryHelper(
            stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
            solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
        core.arena().release(mark);
        return keepGoing;
      }
      core.arena().release(mark);
    }
    return true;
  }

  // Arithmetic comparisons: <, >, =<, >=, =:=, =\=
  if ((goalFunctor == "<" || goalFunctor == ">" || goalFunctor == "=<" ||
       goalFunctor == ">=" || goalFunctor == "=:=" || goalFunctor == "=\\=") &&
      goalArgs.size() == 2) {
    double v1 = 0.0;
    double v2 = 0.0;
    bool isI1 = false;
    bool isI2 = false;
    if (evalArithmetic(stdlib, core, goalArgs[0], v1, isI1) &&
        evalArithmetic(stdlib, core, goalArgs[1], v2, isI2)) {
      bool cond = false;
      if (goalFunctor == "<")
        cond = (v1 < v2);
      else if (goalFunctor == ">")
        cond = (v1 > v2);
      else if (goalFunctor == "=<")
        cond = (v1 <= v2);
      else if (goalFunctor == ">=")
        cond = (v1 >= v2);
      else if (goalFunctor == "=:=")
        cond = (v1 == v2);
      else if (goalFunctor == "=\\=")
        cond = (v1 != v2);

      if (cond) {
        return solveQueryHelper(
            stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
            solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
      }
    }
    return true;
  }

  // Negation as failure: \+ Goal
  if (goalFunctor == "\\+" && goalArgs.size() == 1) {
    auto mark                  = core.arena().mark();
    std::size_t subCount       = 0;
    bool hasSubSolution        = false;
    std::size_t subNextFrameId = nextFrameId;
    std::size_t subCutToFrame  = 0;
    ActiveGoal subGoal{.term = goalArgs[0], .cutFrame = subNextFrameId};
    solveQueryHelper(
        stdlib, core, {subGoal}, candidatePreds, {},
        [&](const LogicSolution &) {
          hasSubSolution = true;
          return false;
        },
        subCount, 1, depth + 1, subNextFrameId, subCutToFrame);
    core.arena().release(mark);
    if (!hasSubSolution) {
      return solveQueryHelper(stdlib, core, restGoals, candidatePreds,
                              queryVars, onSolution, solutionsCount,
                              maxSolutions, depth + 1, nextFrameId, cutToFrame);
    }
    return true;
  }

  // Non-unifiable: \=(A, B)
  if (goalFunctor == "\\=" && goalArgs.size() == 2) {
    auto mark          = core.arena().mark();
    const bool unifies = stdlib.unify(goalArgs[0], goalArgs[1]).has_value();
    core.arena().release(mark);
    if (!unifies) {
      return solveQueryHelper(stdlib, core, restGoals, candidatePreds,
                              queryVars, onSolution, solutionsCount,
                              maxSolutions, depth + 1, nextFrameId, cutToFrame);
    }
    return true;
  }

  // Setting introspection: setting(Name, Value)
  if (goalFunctor == "setting" && goalArgs.size() == 2) {
    auto evalSetting = [&](std::string_view sName,
                           const auto &makeValCell) -> bool {
      if (solutionsCount >= maxSolutions || cutToFrame > 0) return false;
      auto mark        = core.arena().mark();
      CellRef nameCell = core.arena().makeCell(sName);
      CellRef valCell  = makeValCell();
      if (stdlib.unify(goalArgs[0], nameCell) &&
          stdlib.unify(goalArgs[1], valCell)) {
        bool keepGoing = solveQueryHelper(
            stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
            solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
        core.arena().release(mark);
        if (cutToFrame > 0) return false;
        if (!keepGoing && onSolution != nullptr) return false;
      } else {
        core.arena().release(mark);
      }
      return true;
    };

    if (stdlib.boundStore() != nullptr) {
      const auto model =
          xanadu::SystemStoreModel::fromStore(*stdlib.boundStore());
      for (const auto &s : model.settings()) {
        core.arena().consumeWork();
        bool cont = evalSetting(s.name, [&]() -> CellRef {
          if (s.value.elements.empty()) {
            return core.arena().makeCell("");
          }
          if (s.value.elements.size() == 1) {
            const auto &el = s.value.elements[0];
            if (std::holds_alternative<double>(el)) {
              return core.arena().makeScalarCell(std::get<double>(el));
            }
            if (std::holds_alternative<std::int64_t>(el)) {
              return core.arena().makeScalarCell(std::get<std::int64_t>(el));
            }
            if (std::holds_alternative<bool>(el)) {
              return core.arena().makeScalarCell(std::get<bool>(el));
            }
            return core.arena().makeCell(std::get<std::string>(el));
          }
          std::vector<CellRef> elCells;
          for (const auto &el : s.value.elements) {
            core.arena().consumeWork();
            if (std::holds_alternative<double>(el)) {
              elCells.push_back(
                  core.arena().makeScalarCell(std::get<double>(el)));
            } else if (std::holds_alternative<std::int64_t>(el)) {
              elCells.push_back(
                  core.arena().makeScalarCell(std::get<std::int64_t>(el)));
            } else if (std::holds_alternative<bool>(el)) {
              elCells.push_back(
                  core.arena().makeScalarCell(std::get<bool>(el)));
            } else {
              elCells.push_back(
                  core.arena().makeCell(std::get<std::string>(el)));
            }
          }
          return stdlib.makeList(elCells);
        });
        if (!cont) break;
      }
    } else {
      DimRef varsDim = core.dims().vars;
      DimRef valsDim = core.dims().values;
      std::optional<CellRef> curVar =
          core.arena().linked(core.home(), varsDim, DimVector::POS);
      std::size_t limit = core.arena().cellCount() + 1;
      while (curVar && limit-- > 0) {
        core.arena().consumeWork();
        std::string sName = core.arena().textOf(*curVar);
        const auto curVal =
            core.arena().linked(*curVar, valsDim, DimVector::POS);
        bool cont = evalSetting(sName, [&]() -> CellRef {
          return curVal.value_or(core.arena().makeCell(""));
        });
        if (!cont) break;
        curVar = core.arena().linked(*curVar, varsDim, DimVector::POS);
      }
    }
    return true;
  }

  // Setting schema shape: setting_shape(Name, Shape)
  if (goalFunctor == "setting_shape" && goalArgs.size() == 2) {
    if (stdlib.boundStore() != nullptr) {
      const auto model =
          xanadu::SystemStoreModel::fromStore(*stdlib.boundStore());
      for (const auto &s : model.settings()) {
        core.arena().consumeWork();
        for (const auto &alt : s.schema.alternatives) {
          core.arena().consumeWork();
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark        = core.arena().mark();
          CellRef nameCell = core.arena().makeCell(s.name);
          std::vector<CellRef> typeCells;
          typeCells.reserve(alt.expectedTypes.size());
          for (const auto &t : alt.expectedTypes) {
            core.arena().consumeWork();
            typeCells.push_back(core.arena().makeCell(t));
          }
          CellRef shapeCell = stdlib.makeList(typeCells);
          if (stdlib.unify(goalArgs[0], nameCell) &&
              stdlib.unify(goalArgs[1], shapeCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
      }
    }
    return true;
  }

  // Setting default values: setting_default(Name, Default)
  if (goalFunctor == "setting_default" && goalArgs.size() == 2) {
    if (stdlib.boundStore() != nullptr) {
      const auto model =
          xanadu::SystemStoreModel::fromStore(*stdlib.boundStore());
      for (const auto &s : model.settings()) {
        core.arena().consumeWork();
        for (const auto &alt : s.schema.alternatives) {
          core.arena().consumeWork();
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark        = core.arena().mark();
          CellRef nameCell = core.arena().makeCell(s.name);
          CellRef defCell  = noCell;
          if (alt.defaultValues.size() == 1) {
            const auto &el = alt.defaultValues[0];
            if (std::holds_alternative<double>(el)) {
              defCell = core.arena().makeScalarCell(std::get<double>(el));
            } else if (std::holds_alternative<std::int64_t>(el)) {
              defCell = core.arena().makeScalarCell(std::get<std::int64_t>(el));
            } else if (std::holds_alternative<bool>(el)) {
              defCell = core.arena().makeScalarCell(std::get<bool>(el));
            } else {
              defCell = core.arena().makeCell(std::get<std::string>(el));
            }
          } else {
            std::vector<CellRef> defCells;
            for (const auto &el : alt.defaultValues) {
              core.arena().consumeWork();
              if (std::holds_alternative<double>(el)) {
                defCells.push_back(
                    core.arena().makeScalarCell(std::get<double>(el)));
              } else if (std::holds_alternative<std::int64_t>(el)) {
                defCells.push_back(
                    core.arena().makeScalarCell(std::get<std::int64_t>(el)));
              } else if (std::holds_alternative<bool>(el)) {
                defCells.push_back(
                    core.arena().makeScalarCell(std::get<bool>(el)));
              } else {
                defCells.push_back(
                    core.arena().makeCell(std::get<std::string>(el)));
              }
            }
            defCell = stdlib.makeList(defCells);
          }
          if (stdlib.unify(goalArgs[0], nameCell) &&
              stdlib.unify(goalArgs[1], defCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
      }
    }
    return true;
  }

  // Cell value: cell_value(CellId, Value)
  if (goalFunctor == "cell_value" && goalArgs.size() == 2) {
    const auto dId = stdlib.deref(goalArgs[0]);
    if (dId && !stdlib.isVar(*dId)) {
      if (const auto target = cellRefArgument(core.arena(), *dId)) {
        auto mark       = core.arena().mark();
        CellRef valCell = noCell;
        if (auto d = core.arena().asDouble(*target); d.has_value()) {
          valCell = core.arena().makeScalarCell(*d);
        } else if (auto i = core.arena().asInt64(*target); i.has_value()) {
          valCell = core.arena().makeScalarCell(*i);
        } else if (auto b = core.arena().asBool(*target); b.has_value()) {
          valCell = core.arena().makeScalarCell(*b);
        } else {
          valCell = core.arena().makeCell(core.arena().textOf(*target));
        }
        if (stdlib.unify(goalArgs[1], valCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) return false;
        } else {
          core.arena().release(mark);
        }
      }
    } else if (dId && stdlib.isVar(*dId)) {
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        core.arena().consumeWork();
        CellRef target =
            zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
        if (!core.arena().contains(target)) continue;
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        auto mark = core.arena().mark();
        CellRef idCell =
            core.arena().makeScalarCell(static_cast<std::int64_t>(target));
        CellRef valCell = noCell;
        if (auto d = core.arena().asDouble(target); d.has_value()) {
          valCell = core.arena().makeScalarCell(*d);
        } else if (auto i = core.arena().asInt64(target); i.has_value()) {
          valCell = core.arena().makeScalarCell(*i);
        } else if (auto b = core.arena().asBool(target); b.has_value()) {
          valCell = core.arena().makeScalarCell(*b);
        } else {
          valCell = core.arena().makeCell(core.arena().textOf(target));
        }
        if (stdlib.unify(goalArgs[0], idCell) &&
            stdlib.unify(goalArgs[1], valCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) return false;
        } else {
          core.arena().release(mark);
        }
      }
    }
    return true;
  }

  // Cell link: cell_link(FromId, Dim, Dir, ToId)
  if (goalFunctor == "cell_link" && goalArgs.size() == 4) {
    auto tryUnifyLink = [&](CellRef fromCell, std::string_view dimName,
                            std::string_view dirStr, CellRef toCell) -> bool {
      if (solutionsCount >= maxSolutions || cutToFrame > 0) return false;
      auto mark = core.arena().mark();
      CellRef fromRefCell =
          core.arena().makeScalarCell(static_cast<std::int64_t>(fromCell));
      CellRef dimCell = core.arena().makeCell(dimName);
      CellRef dirCell = core.arena().makeCell(dirStr);
      CellRef toRefCell =
          core.arena().makeScalarCell(static_cast<std::int64_t>(toCell));

      if (stdlib.unify(goalArgs[0], fromRefCell) &&
          stdlib.unify(goalArgs[1], dimCell) &&
          stdlib.unify(goalArgs[2], dirCell) &&
          stdlib.unify(goalArgs[3], toRefCell)) {
        bool keepGoing = solveQueryHelper(
            stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
            solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
        core.arena().release(mark);
        if (cutToFrame > 0) return false;
        if (!keepGoing && onSolution != nullptr) return false;
      } else {
        core.arena().release(mark);
      }
      return true;
    };

    const auto dFrom = stdlib.deref(goalArgs[0]);
    if (dFrom && !stdlib.isVar(*dFrom)) {
      if (const auto fromCell = cellRefArgument(core.arena(), *dFrom)) {
        for (const auto &dl : core.arena().dimensionsOf(*fromCell)) {
          core.arena().consumeWork();
          std::string dimName = core.arena().textOf(dl.dim);
          if (dimName.empty()) {
            dimName = "d." + std::to_string(dl.dim);
          }
          if (dl.pos != noCell) {
            if (!tryUnifyLink(*fromCell, dimName, "pos", dl.pos)) break;
          }
          if (dl.neg != noCell) {
            if (!tryUnifyLink(*fromCell, dimName, "neg", dl.neg)) break;
          }
        }
      }
    } else if (dFrom && stdlib.isVar(*dFrom)) {
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        core.arena().consumeWork();
        CellRef fromCell =
            zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
        if (!core.arena().contains(fromCell)) continue;
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        for (const auto &dl : core.arena().dimensionsOf(fromCell)) {
          core.arena().consumeWork();
          std::string dimName = core.arena().textOf(dl.dim);
          if (dimName.empty()) {
            dimName = "d." + std::to_string(dl.dim);
          }
          if (dl.pos != noCell) {
            if (!tryUnifyLink(fromCell, dimName, "pos", dl.pos)) break;
          }
          if (dl.neg != noCell) {
            if (!tryUnifyLink(fromCell, dimName, "neg", dl.neg)) break;
          }
        }
      }
    }
    return true;
  }

  // Transclusion: transclude(OriginDoc, OriginSpan, TargetDoc)
  if (goalFunctor == "transclude" && goalArgs.size() == 3) {
    if (stdlib.boundStore() != nullptr) {
      const auto &store       = *stdlib.boundStore();
      const std::size_t count = store.opCount();
      for (std::size_t i = 0; i < count; ++i) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        const auto *cop = store.getCompactOp(static_cast<std::uint32_t>(i));
        if (cop != nullptr && cop->kind == xanadu::OpKind::Transclude) {
          auto mark           = core.arena().mark();
          CellRef oDocCell    = core.arena().makeCell("store");
          std::string spanStr = "[" + std::to_string(cop->spanStart) + "," +
                                std::to_string(cop->spanLength) + "]";
          CellRef spanCell = core.arena().makeCell(spanStr);
          CellRef tDocCell = core.arena().makeCell("active");
          if (stdlib.unify(goalArgs[0], oDocCell) &&
              stdlib.unify(goalArgs[1], spanCell) &&
              stdlib.unify(goalArgs[2], tDocCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }
      }
    }
    return true;
  }

  // Xanalink: xanalink(FromSpan, LinkType, ToSpan)
  if (goalFunctor == "xanalink" && goalArgs.size() == 3) {
    if (stdlib.boundStore() != nullptr) {
      const auto &store = *stdlib.boundStore();
      for (const auto &link : store.linkView()) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        auto mark = core.arena().mark();
        std::string leftStr =
            link.left.empty()
                ? "[]"
                : ("[" + std::to_string(link.left[0].start) + "," +
                   std::to_string(link.left[0].length) + "]");
        std::string rightStr =
            link.right.empty()
                ? "[]"
                : ("[" + std::to_string(link.right[0].start) + "," +
                   std::to_string(link.right[0].length) + "]");
        std::string typeStr = link.owner.empty()
                                  ? std::string(xanadu::linkTypeName(link.type))
                                  : link.owner;
        CellRef lCell       = core.arena().makeCell(leftStr);
        CellRef tCell       = core.arena().makeCell(typeStr);
        CellRef rCell       = core.arena().makeCell(rightStr);
        if (stdlib.unify(goalArgs[0], lCell) &&
            stdlib.unify(goalArgs[1], tCell) &&
            stdlib.unify(goalArgs[2], rCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) return false;
        } else {
          core.arena().release(mark);
        }
      }
    }
    return true;
  }

  // Cell span: cell_span(CellId, Span)
  if (goalFunctor == "cell_span" && goalArgs.size() == 2) {
    const auto dId      = stdlib.deref(goalArgs[0]);
    auto matchCellSpans = [&](CellRef c) -> bool {
      auto spans = core.arena().contentOf(c);
      for (const auto &sp : spans) {
        core.arena().consumeWork();
        if (solutionsCount >= maxSolutions || cutToFrame > 0) return false;
        auto mark = core.arena().mark();
        CellRef cCell =
            core.arena().makeScalarCell(static_cast<std::int64_t>(c));
        std::string spanStr = "[" + std::to_string(sp.start) + "," +
                              std::to_string(sp.length) + "]";
        CellRef spCell = core.arena().makeCell(spanStr);
        if (stdlib.unify(goalArgs[0], cCell) &&
            stdlib.unify(goalArgs[1], spCell)) {
          bool keepGoing = solveQueryHelper(
              stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
              solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
          core.arena().release(mark);
          if (cutToFrame > 0) return false;
          if (!keepGoing && onSolution != nullptr) return false;
        } else {
          core.arena().release(mark);
        }
      }
      return true;
    };

    if (dId && !stdlib.isVar(*dId)) {
      if (const auto cell = cellRefArgument(core.arena(), *dId)) {
        matchCellSpans(*cell);
      }
    } else if (dId && stdlib.isVar(*dId)) {
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        core.arena().consumeWork();
        CellRef target =
            zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
        if (!core.arena().contains(target)) continue;
        if (!matchCellSpans(target)) break;
      }
    }
    return true;
  }

  // Bridge edge: bridge_edge(PresentationCell, DocumentCell) or bridge_edge(U,
  // Dim, V)
  if (goalFunctor == "bridge_edge" &&
      (goalArgs.size() == 2 || goalArgs.size() == 3)) {
    if (goalArgs.size() == 2) {
      DimRef cloneDim   = core.dims().clone;
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        core.arena().consumeWork();
        CellRef presCell =
            zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
        if (!core.arena().contains(presCell)) continue;
        const auto docCell =
            core.arena().linked(presCell, cloneDim, DimVector::POS);
        if (docCell) {
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark = core.arena().mark();
          CellRef pCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(presCell));
          CellRef dCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(*docCell));
          if (stdlib.unify(goalArgs[0], pCell) &&
              stdlib.unify(goalArgs[1], dCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }
      }
    } else {
      const auto dDim = stdlib.deref(goalArgs[1]);
      std::string reqDimName;
      if (dDim && !stdlib.isVar(*dDim)) {
        reqDimName = core.arena().textOf(*dDim);
      }
      auto matchEdgesFrom = [&](CellRef fromCell) -> bool {
        if (!core.arena().contains(fromCell)) return true;
        for (const auto &dl : core.arena().dimensionsOf(fromCell)) {
          core.arena().consumeWork();
          if (dl.pos == noCell) continue;
          std::string dimName = core.arena().textOf(dl.dim);
          if (dimName.empty()) {
            dimName = "d." + std::to_string(dl.dim);
          }
          if (!reqDimName.empty() && dimName != reqDimName) continue;
          auto mark = core.arena().mark();
          CellRef uCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(fromCell));
          CellRef dCell = core.arena().makeCell(dimName);
          CellRef vCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(dl.pos));
          if (stdlib.unify(goalArgs[0], uCell) &&
              stdlib.unify(goalArgs[1], dCell) &&
              stdlib.unify(goalArgs[2], vCell)) {
            bool keepGoing = solveQueryHelper(
                stdlib, core, restGoals, candidatePreds, queryVars, onSolution,
                solutionsCount, maxSolutions, depth + 1, nextFrameId,
                cutToFrame);
            core.arena().release(mark);
            if (cutToFrame > 0) return false;
            if (!keepGoing && onSolution != nullptr) return false;
          } else {
            core.arena().release(mark);
          }
        }
        return true;
      };

      const auto dU = stdlib.deref(goalArgs[0]);
      if (dU && !stdlib.isVar(*dU)) {
        if (const auto from = cellRefArgument(core.arena(), *dU)) {
          matchEdgesFrom(*from);
        }
      } else if (dU && stdlib.isVar(*dU)) {
        std::size_t total = core.arena().cellCount();
        for (std::size_t c = 1; c < total; ++c) {
          core.arena().consumeWork();
          CellRef fromCell =
              zigzag::ArenaManifold::refOf(static_cast<std::uint32_t>(c));
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          if (!matchEdgesFrom(fromCell)) break;
        }
      }
    }
    return true;
  }

  // Iterate over matching predicates
  for (CellRef pred : candidatePreds) {
    core.arena().consumeWork();
    if (pred == noCell) continue;
    if (core.arena().textOf(pred) != goalFunctor) continue;

    const std::size_t frameId = nextFrameId++;
    std::optional<CellRef> clause =
        core.arena().linked(pred, core.dims().clause, DimVector::POS);
    std::size_t limit = core.arena().cellCount() + 1;
    while (clause && solutionsCount < maxSolutions && limit-- > 0) {
      core.arena().consumeWork();
      if (cutToFrame > 0 && cutToFrame <= frameId) break;
      auto mark = core.arena().mark();

      std::unordered_map<CellRef, CellRef> varMap;
      std::unordered_map<CellRef, CellRef> visited;
      const auto rawHead =
          core.arena().linked(*clause, core.dims().grab, DimVector::POS);
      if (!rawHead) {
        core.arena().release(mark);
        clause =
            core.arena().linked(*clause, core.dims().clause, DimVector::POS);
        continue;
      }
      CellRef head =
          freshenTerm(stdlib, core, *rawHead, varMap, visited).value();

      if (stdlib.unify(curGoal.term, head)) {
        std::vector<ActiveGoal> nextGoals;
        std::optional<CellRef> curBody =
            core.arena().linked(*clause, core.dims().spin, DimVector::POS);
        std::size_t bodyLimit = core.arena().cellCount() + 1;
        while (curBody && bodyLimit-- > 0) {
          core.arena().consumeWork();
          std::unordered_map<CellRef, CellRef> bodyVisited;
          CellRef bTerm =
              freshenTerm(stdlib, core, *curBody, varMap, bodyVisited).value();
          nextGoals.push_back(ActiveGoal{.term = bTerm, .cutFrame = frameId});
          curBody =
              core.arena().linked(*curBody, core.dims().spin, DimVector::POS);
        }
        nextGoals.insert(nextGoals.end(), restGoals.begin(), restGoals.end());

        bool keepGoing = solveQueryHelper(
            stdlib, core, nextGoals, candidatePreds, queryVars, onSolution,
            solutionsCount, maxSolutions, depth + 1, nextFrameId, cutToFrame);
        core.arena().release(mark);
        if (cutToFrame > 0 && cutToFrame <= frameId) break;
        if (!keepGoing && onSolution != nullptr) {
          return false;
        }
      } else {
        core.arena().release(mark);
      }

      clause = core.arena().linked(*clause, core.dims().clause, DimVector::POS);
    }
    if (cutToFrame == frameId) {
      cutToFrame = 0;
      break;
    }
    if (cutToFrame > 0 && cutToFrame < frameId) {
      break;
    }
  }
  return true;
}

} // namespace

VortexStdLib::VortexStdLib(VortexCore &core, VortexVM &vm)
    : core_(core), vm_(vm) {
  bootstrap();
}

CellRef VortexStdLib::getOrCreateModule(std::string_view modulePath) {
  return core_.getOrCreateModule(modulePath);
}

std::vector<std::string> VortexStdLib::namesAfter(const CellRef from,
                                                  const DimRef dim) const {
  return rankAfter(core_.arena(), from, dim) |
         std::views::transform([this](const CellRef cell) {
           return core_.arena().textOf(cell);
         }) |
         std::ranges::to<std::vector>();
}

void VortexStdLib::exportSymbol(CellRef moduleCell, std::string_view symbolName,
                                CellRef entryOp) {
  core_.exportSymbol(moduleCell, symbolName, entryOp);
}

void VortexStdLib::bootstrap() {
  CellRef modMemoize     = getOrCreateModule("std:memoize");
  CellRef modContract    = getOrCreateModule("std:contract");
  CellRef modPipeline    = getOrCreateModule("std:pipeline");
  CellRef modFunctional  = getOrCreateModule("std:functional");
  CellRef modCollections = getOrCreateModule("std:collections");
  CellRef modMath        = getOrCreateModule("std:math");
  CellRef modString      = getOrCreateModule("std:string");
  CellRef modLogic       = getOrCreateModule("std:logic");
  // std:goal weaves in after std:logic because its generators call
  // std:logic's #Choice/#Fail plumbing (vicon spec section 7.1).
  CellRef modGoal   = getOrCreateModule("std:goal");
  CellRef modArray  = getOrCreateModule("sys:array");
  CellRef modZigzag = getOrCreateModule("std:zigzag");
  CellRef modGC     = getOrCreateModule("std:gc");
  CellRef modUI     = getOrCreateModule("std:ui");
  CellRef modNav    = getOrCreateModule("std:nav");
  CellRef modBridge = getOrCreateModule("std:bridge");
  CellRef modXuzz   = getOrCreateModule("std:xuzz");
  CellRef modXudu   = getOrCreateModule("std:xudu");

  buildMathModule(modMath);
  buildStringModule(modString);
  buildContractModule(modContract);
  buildPipelineModule(modPipeline);
  buildMemoizeModule(modMemoize);
  buildFunctionalModule(modFunctional);
  buildCollectionsModule(modCollections);
  buildLogicModule(modLogic);
  buildGoalModule(modGoal);
  buildArrayModule(modArray);
  buildZigzagModule(modZigzag);
  buildGCModule(modGC);
  buildUiModule(modUI);
  buildNavModule(modNav);
  buildBridgeModule(modBridge);
  buildXuzzModule(modXuzz);
  buildXuduModule(modXudu);
}

void VortexStdLib::buildMathModule(CellRef mod) {
  // abs: #MATH_ABS in out (postcondition: out >= 0)
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Abs, "#MATH_ABS");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);

    CellRef zero = core_.arena().makeScalarCell(static_cast<std::int64_t>(0));
    CellRef reqNonNeg =
        vm_.mintOpcode(OpcodeKind::Gte, "#REQUIRE_NON_NEGATIVE");
    core_.bindInput(reqNonNeg, out);
    core_.bindInput(reqNonNeg, zero);
    core_.attachPostcondition(op, reqNonNeg);

    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "abs", op);
  }

  // min: #MATH_MIN in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Min, "#MATH_MIN");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "min", op);
  }

  // max: #MATH_MAX in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Max, "#MATH_MAX");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "max", op);
  }

  // clamp: #CLAMP_MAX in lo -> temp; #CLAMP_MIN temp hi -> out (contract: lo <=
  // hi)
  {
    CellRef in   = core_.arena().makeCell();
    CellRef lo   = core_.arena().makeCell();
    CellRef hi   = core_.arena().makeCell();
    CellRef temp = core_.arena().makeCell();
    CellRef out  = core_.arena().makeCell();

    CellRef opMax = vm_.mintOpcode(OpcodeKind::Max, "#CLAMP_MAX");
    core_.bindInput(opMax, in);
    core_.bindInput(opMax, lo);
    core_.bindOutput(opMax, temp);

    CellRef reqRange = vm_.mintOpcode(OpcodeKind::Lte, "#REQUIRE_RANGE_VALID");
    core_.bindInput(reqRange, lo);
    core_.bindInput(reqRange, hi);
    core_.attachPrecondition(opMax, reqRange);

    CellRef opMin = vm_.mintOpcode(OpcodeKind::Min, "#CLAMP_MIN");
    core_.bindInput(opMin, temp);
    core_.bindInput(opMin, hi);
    core_.bindOutput(opMin, out);

    zigzag::expectWritten(
        core_.arena().link(opMax, core_.dims().spin, false, opMin));

    routineBindings_[opMax] = {.inputParams  = {in, lo, hi},
                               .outputParams = {out}};
    exportSymbol(mod, "clamp", opMax);
  }

  // add: #MATH_ADD in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Add, "#MATH_ADD");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "add", op);
  }

  // sub: #MATH_SUB in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Sub, "#MATH_SUB");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "sub", op);
  }

  // mul: #MATH_MUL in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Mul, "#MATH_MUL");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "mul", op);
  }

  // div: #MATH_DIV in0 in1 out (precondition: in1 != 0)
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Div, "#MATH_DIV");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);

    CellRef zero = core_.arena().makeScalarCell(static_cast<std::int64_t>(0));
    CellRef reqDiff = vm_.mintOpcode(OpcodeKind::Neq, "#REQUIRE_NON_ZERO");
    core_.bindInput(reqDiff, in1);
    core_.bindInput(reqDiff, zero);
    core_.attachPrecondition(op, reqDiff);

    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "div", op);
  }

  // mod: #MATH_MOD in0 in1 out (precondition: in1 != 0)
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Mod, "#MATH_MOD");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);

    CellRef zero = core_.arena().makeScalarCell(static_cast<std::int64_t>(0));
    CellRef reqDiff = vm_.mintOpcode(OpcodeKind::Neq, "#REQUIRE_NON_ZERO");
    core_.bindInput(reqDiff, in1);
    core_.bindInput(reqDiff, zero);
    core_.attachPrecondition(op, reqDiff);

    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "mod", op);
  }

  // neg: #MATH_NEG in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Neg, "#MATH_NEG");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "neg", op);
  }
}

void VortexStdLib::buildStringModule(CellRef mod) {
  // to_lower: #STR_TO_LOWER in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::ToLower, "#STR_TO_LOWER");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "to_lower", op);
  }

  // to_upper: #STR_TO_UPPER in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::ToUpper, "#STR_TO_UPPER");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "to_upper", op);
  }

  // trim: #STR_TRIM in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Trim, "#STR_TRIM");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "trim", op);
  }

  // clean: #STR_TRIM in -> temp; #STR_TO_LOWER temp -> out
  {
    CellRef in   = core_.arena().makeCell();
    CellRef temp = core_.arena().makeCell();
    CellRef out  = core_.arena().makeCell();

    CellRef opTrim = vm_.mintOpcode(OpcodeKind::Trim, "#STR_TRIM");
    core_.bindInput(opTrim, in);
    core_.bindOutput(opTrim, temp);

    CellRef opLower = vm_.mintOpcode(OpcodeKind::ToLower, "#STR_TO_LOWER");
    core_.bindInput(opLower, temp);
    core_.bindOutput(opLower, out);

    zigzag::expectWritten(
        core_.arena().link(opTrim, core_.dims().spin, false, opLower));

    routineBindings_[opTrim] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "clean", opTrim);
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
  // memoize: targetOp, cacheKey, capacity
  {
    CellRef targetOp = core_.arena().makeCell();
    CellRef cacheKey = core_.arena().makeCell();
    CellRef capacity = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#MEMO_WRAPPER");
    core_.bindInput(op, targetOp);
    core_.bindInput(op, cacheKey);
    core_.bindInput(op, capacity);
    routineBindings_[op] = {.inputParams  = {targetOp, cacheKey, capacity},
                            .outputParams = {}};
    exportSymbol(mod, "memoize", op);
  }

  // flush: cacheKey -> status
  {
    CellRef cacheKey = core_.arena().makeCell();
    CellRef status   = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#MEMO_FLUSH");
    core_.bindInput(op, cacheKey);
    core_.bindOutput(op, status);
    routineBindings_[op] = {.inputParams  = {cacheKey},
                            .outputParams = {status}};
    exportSymbol(mod, "flush", op);
  }

  // retire: cacheKey -> status
  {
    CellRef cacheKey = core_.arena().makeCell();
    CellRef status   = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#MEMO_RETIRE");
    core_.bindInput(op, cacheKey);
    core_.bindOutput(op, status);
    routineBindings_[op] = {.inputParams  = {cacheKey},
                            .outputParams = {status}};
    exportSymbol(mod, "retire", op);
  }
}

void VortexStdLib::buildFunctionalModule(CellRef mod) {
  // map: inHead, inDim, outDim, fnOp -> outHead
  {
    CellRef inHead = core_.arena().makeCell();
    CellRef inDim  = core_.arena().makeCell();
    CellRef outDim = core_.arena().makeCell();
    CellRef fnOp   = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_MAP");
    core_.bindInput(op, inHead);
    core_.bindInput(op, inDim);
    core_.bindInput(op, outDim);
    core_.bindInput(op, fnOp);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {inHead, inDim, outDim, fnOp},
                            .outputParams = {out}};
    exportSymbol(mod, "map", op);
  }

  // filter: inHead, inDim, outDim, predOp -> outHead
  {
    CellRef inHead = core_.arena().makeCell();
    CellRef inDim  = core_.arena().makeCell();
    CellRef outDim = core_.arena().makeCell();
    CellRef predOp = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_FILTER");
    core_.bindInput(op, inHead);
    core_.bindInput(op, inDim);
    core_.bindInput(op, outDim);
    core_.bindInput(op, predOp);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {inHead, inDim, outDim, predOp},
                            .outputParams = {out}};
    exportSymbol(mod, "filter", op);
  }

  // fold: inHead, inDim, initial, fnOp -> acc
  {
    CellRef inHead  = core_.arena().makeCell();
    CellRef inDim   = core_.arena().makeCell();
    CellRef initial = core_.arena().makeCell();
    CellRef fnOp    = core_.arena().makeCell();
    CellRef out     = core_.arena().makeCell();
    CellRef op      = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_FOLD");
    core_.bindInput(op, inHead);
    core_.bindInput(op, inDim);
    core_.bindInput(op, initial);
    core_.bindInput(op, fnOp);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {inHead, inDim, initial, fnOp},
                            .outputParams = {out}};
    exportSymbol(mod, "fold", op);
  }

  // zip: headA, headB, dimA, dimB, outDim -> outHead
  {
    CellRef headA  = core_.arena().makeCell();
    CellRef headB  = core_.arena().makeCell();
    CellRef dimA   = core_.arena().makeCell();
    CellRef dimB   = core_.arena().makeCell();
    CellRef outDim = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#FUNC_ZIP");
    core_.bindInput(op, headA);
    core_.bindInput(op, headB);
    core_.bindInput(op, dimA);
    core_.bindInput(op, dimB);
    core_.bindInput(op, outDim);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {headA, headB, dimA, dimB, outDim},
                            .outputParams = {out}};
    exportSymbol(mod, "zip", op);
  }
}

void VortexStdLib::buildCollectionsModule(CellRef mod) {
  // list: items, dim -> head
  {
    CellRef inItems = core_.arena().makeCell();
    CellRef inDim   = core_.arena().makeCell();
    CellRef outHead = core_.arena().makeCell();
    CellRef op      = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_LIST");
    core_.bindInput(op, inItems);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inItems, inDim},
                            .outputParams = {outHead}};
    exportSymbol(mod, "list", op);
  }

  // map: entries -> head
  {
    CellRef inEntries = core_.arena().makeCell();
    CellRef outHead   = core_.arena().makeCell();
    CellRef op        = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_MAP");
    core_.bindInput(op, inEntries);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inEntries},
                            .outputParams = {outHead}};
    exportSymbol(mod, "map", op);
  }

  // grid: rows, cols -> head
  {
    CellRef inRows  = core_.arena().makeCell();
    CellRef inCols  = core_.arena().makeCell();
    CellRef outHead = core_.arena().makeCell();
    CellRef op      = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_GRID");
    core_.bindInput(op, inRows);
    core_.bindInput(op, inCols);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inRows, inCols},
                            .outputParams = {outHead}};
    exportSymbol(mod, "grid", op);
  }

  // push_back: head, val, dim
  {
    CellRef inHead = core_.arena().makeCell();
    CellRef inVal  = core_.arena().makeCell();
    CellRef inDim  = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_PUSH_BACK");
    core_.bindInput(op, inHead);
    core_.bindInput(op, inVal);
    core_.bindInput(op, inDim);
    routineBindings_[op] = {.inputParams  = {inHead, inVal, inDim},
                            .outputParams = {}};
    exportSymbol(mod, "push_back", op);
  }

  // push_front: head, val, dim -> outHead
  {
    CellRef inHead  = core_.arena().makeCell();
    CellRef inVal   = core_.arena().makeCell();
    CellRef inDim   = core_.arena().makeCell();
    CellRef outHead = core_.arena().makeCell();
    CellRef op      = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_PUSH_FRONT");
    core_.bindInput(op, inHead);
    core_.bindInput(op, inVal);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inHead, inVal, inDim},
                            .outputParams = {outHead}};
    exportSymbol(mod, "push_front", op);
  }

  // pop_back: head, dim -> outVal
  {
    CellRef inHead = core_.arena().makeCell();
    CellRef inDim  = core_.arena().makeCell();
    CellRef outVal = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_POP_BACK");
    core_.bindInput(op, inHead);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outVal);
    routineBindings_[op] = {.inputParams  = {inHead, inDim},
                            .outputParams = {outVal}};
    exportSymbol(mod, "pop_back", op);
  }
}

std::optional<CellRef> VortexStdLib::resolve(std::string_view path) const {
  const auto slashPos = path.find('/');
  const auto module   = core_.findModule(path.substr(0, slashPos));
  if (slashPos == std::string_view::npos) {
    return module;
  }
  // A symbol is a cell on its module's d.vars rank; its entry op hangs off it
  // on d.values.
  const auto symName = path.substr(slashPos + 1);
  return module
      .and_then([&](const CellRef mod) {
        return firstOf(rankAfter(core_.arena(), mod, core_.dims().vars) |
                       std::views::filter([&](const CellRef sym) {
                         return core_.arena().textOf(sym) == symName;
                       }));
      })
      .and_then(hop(core_.arena(), core_.dims().values));
}

bool VortexStdLib::has(std::string_view path) const {
  return resolve(path).has_value();
}

std::vector<std::string> VortexStdLib::modules() const {
  return namesAfter(core_.home(), core_.dims().stdlib);
}

std::vector<std::string>
VortexStdLib::symbolsInModule(std::string_view modulePath) const {
  return resolve(modulePath)
      .transform([this](const CellRef mod) {
        return namesAfter(mod, core_.dims().vars);
      })
      .value_or(std::vector<std::string>{});
}

std::vector<CellValue> VortexStdLib::call(std::string_view path,
                                          const std::vector<CellValue> &args) {
  const auto op = resolve(path);
  if (!op) {
    return {};
  }
  return call(*op, args);
}

std::vector<CellValue> VortexStdLib::call(CellRef fnOp,
                                          const std::vector<CellValue> &args) {
  if (fnOp == noCell) {
    return {};
  }
  std::string opName = core_.arena().textOf(fnOp);
  if (opName.starts_with("#ARRAY_")) {
    if (opName == "#ARRAY_IOTA") {
      std::size_t n =
          args.empty() ? 0 : static_cast<std::size_t>(toInt64(args[0]));
      std::optional<DimRef> dim;
      if (args.size() > 1) dim = static_cast<DimRef>(toInt64(args[1]));
      const auto res = arrayIota(n, dim);
      return {static_cast<std::int64_t>(res.value_or(noCell))};
    }
    if (opName == "#ARRAY_TALLY") {
      if (args.size() < 2) {
        throw std::invalid_argument("array tally expects origin and dimension");
      }
      const CellRef origin = static_cast<CellRef>(toInt64(args[0]));
      const DimRef dim     = static_cast<DimRef>(toInt64(args[1]));
      // The scalar language uses noCell for an empty array, whose tally is 0.
      // Other invalid references remain errors instead of becoming zero.
      if (origin == noCell) return {static_cast<std::int64_t>(0)};
      const auto tally = arrayTally(origin, dim);
      if (!tally) {
        throw std::invalid_argument("array tally origin is not in the arena");
      }
      return {static_cast<std::int64_t>(*tally)};
    }
    if (opName == "#ARRAY_TAKE") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                   : core_.dims().step;
      std::size_t count =
          args.size() > 2 ? static_cast<std::size_t>(toInt64(args[2])) : 0;
      const auto res = arrayTake(origin, dim, count);
      return {static_cast<std::int64_t>(res.value_or(noCell))};
    }
    if (opName == "#ARRAY_DROP") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                   : core_.dims().step;
      std::size_t count =
          args.size() > 2 ? static_cast<std::size_t>(toInt64(args[2])) : 0;
      const auto res = arrayDrop(origin, dim, count);
      return {static_cast<std::int64_t>(res.value_or(noCell))};
    }
    if (opName == "#ARRAY_REVERSE") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim     = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                       : core_.dims().step;
      const auto res = arrayReverse(origin, dim);
      return {static_cast<std::int64_t>(res.value_or(noCell))};
    }
  }
  if (opName.starts_with("#ZZ_")) {
    if (opName == "#ZZ_STEP") {
      std::optional<CellRef> cursor;
      if (!args.empty()) {
        const auto value = static_cast<CellRef>(toInt64(args[0]));
        if (value != noCell) cursor = value;
      }
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      DimVector dir = DimVector::POS;
      if (args.size() > 2 && toInt64(args[2]) < 0) {
        dir = DimVector::NEG;
      }
      const auto next = zzStep(cursor, dim, dir);
      return {static_cast<std::int64_t>(next.value_or(noCell))};
    }
    if (opName == "#ZZ_INSERT") {
      CellRef cursor =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      DimVector dir = DimVector::POS;
      if (args.size() > 2 && toInt64(args[2]) < 0) {
        dir = DimVector::NEG;
      }
      std::string text =
          args.size() > 3 && std::holds_alternative<std::string>(args[3])
              ? std::get<std::string>(args[3])
              : "";
      return {static_cast<std::int64_t>(
          zzInsert(cursor, dim, dir, text).value_or(noCell))};
    }
    if (opName == "#ZZ_UNLINK") {
      CellRef cursor =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      DimVector dir = DimVector::POS;
      if (args.size() > 2 && toInt64(args[2]) < 0) {
        dir = DimVector::NEG;
      }
      return {static_cast<std::int64_t>(
          zzUnlink(cursor, dim, dir).value_or(noCell))};
    }
    if (opName == "#ZZ_LINK") {
      CellRef cellA =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      CellRef cellB =
          args.size() > 1 ? static_cast<CellRef>(toInt64(args[1])) : noCell;
      DimRef dim =
          args.size() > 2 ? static_cast<DimRef>(toInt64(args[2])) : noCell;
      DimVector dir = DimVector::POS;
      if (args.size() > 3 && toInt64(args[3]) < 0) {
        dir = DimVector::NEG;
      }
      return {static_cast<std::int64_t>(
          zzLink(cellA, cellB, dim, dir).value_or(noCell))};
    }
    if (opName == "#ZZ_DELETE") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      return {static_cast<std::int64_t>(zzDelete(cell).value_or(noCell))};
    }
    if (opName == "#ZZ_CLONE_CHAIN") {
      CellRef sym =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      CellRef tgt =
          args.size() > 1 ? static_cast<CellRef>(toInt64(args[1])) : noCell;
      return {
          static_cast<std::int64_t>(zzCloneToChain(sym, tgt).value_or(noCell))};
    }
    if (opName == "#ZZ_DUPLICATE") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      return {static_cast<std::int64_t>(zzDuplicate(cell).value_or(noCell))};
    }
  }
  if (opName.starts_with("#NAV_")) {
    if (opName == "#NAV_HOP_HEAD") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      return {static_cast<std::int64_t>(hopHead(cell, dim).value_or(noCell))};
    }
    if (opName == "#NAV_HOP_TAIL") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      return {static_cast<std::int64_t>(hopTail(cell, dim).value_or(noCell))};
    }
    if (opName == "#NAV_JUMP_HOME") {
      return {static_cast<std::int64_t>(jumpHome())};
    }
  }
  if (opName.starts_with("#UI_")) {
    return {static_cast<std::int64_t>(1)};
  }
  if (opName.starts_with("#BRIDGE_")) {
    return {static_cast<std::int64_t>(1)};
  }
  if (opName == "#GC_SWEEP") {
    return {static_cast<std::int64_t>(gcSweep())};
  }
  if (opName.starts_with("#GOAL_")) {
    // std:goal's host-driven routines: the synchronous forms of control
    // structures the vicon compiler emits inline. Args carry CellRefs as
    // integers, as the #ZZ_ routines take them.
    const auto refArg = [&args](const std::size_t i) -> CellRef {
      return i < args.size() ? static_cast<CellRef>(toInt64(args[i])) : noCell;
    };
    const auto optionalRefArg =
        [&args](const std::size_t i) -> std::optional<CellRef> {
      if (i >= args.size()) return std::nullopt;
      const auto ref = static_cast<CellRef>(toInt64(args[i]));
      return ref == noCell ? std::nullopt : std::optional<CellRef>{ref};
    };
    if (opName == "#GOAL_BANG") {
      // !x over a rank: every element's CellRef as a std:collections list,
      // eagerly. The lazy one-#Choice-per-step form is the inline emission.
      const CellRef head = refArg(0);
      const DimRef dim   = refArg(1) != noCell ? static_cast<DimRef>(refArg(1))
                                               : core_.dims().step;
      std::vector<CellValue> items;
      if (head != noCell && core_.arena().contains(head)) {
        for (const CellRef cell : zigzag::rank(core_.arena(), head, dim)) {
          items.push_back(static_cast<std::int64_t>(cell));
        }
      }
      return {static_cast<std::int64_t>(createList(items).value_or(noCell))};
    }
    if (opName == "#GOAL_EVERY") {
      const CellRef haltOp = refArg(2) != noCell ? refArg(2) : goalHalt_;
      const auto values    = goalResults(refArg(0), optionalRefArg(1), haltOp);
      return {static_cast<std::int64_t>(createList(values).value_or(noCell))};
    }
    if (opName == "#GOAL_IF") {
      const CellRef haltOp = refArg(6) != noCell ? refArg(6) : goalHalt_;
      if (goalBoundedFirst(refArg(0), optionalRefArg(1), haltOp).has_value()) {
        // The condition produced; the if's result is the then-arm's.
        if (auto v = goalBoundedFirst(refArg(2), optionalRefArg(3), haltOp)) {
          return {*v};
        }
        return {};
      }
      const CellRef elseEntry = refArg(4);
      if (elseEntry == noCell) {
        return {}; // no else arm: a failing condition fails the if
      }
      if (auto v = goalBoundedFirst(elseEntry, optionalRefArg(5), haltOp)) {
        return {*v};
      }
      return {};
    }
    if (opName == "#GOAL_WHILE" || opName == "#GOAL_UNTIL") {
      const CellRef condEntry = refArg(0);
      const auto condOut      = optionalRefArg(1);
      const CellRef bodyEntry = refArg(2);
      const auto bodyOut      = optionalRefArg(3);
      const CellRef haltOp    = refArg(4) != noCell ? refArg(4) : goalHalt_;
      const bool isWhile      = opName == "#GOAL_WHILE";
      std::int64_t iterations = 0;
      // The condition is re-entered each iteration under bounded evaluation
      // (section 5.1); the same run-cycle bound guards a `repeat` condition.
      while (iterations < 100000) {
        if (goalBoundedFirst(condEntry, condOut, haltOp).has_value() !=
            isWhile) {
          break;
        }
        std::ignore = goalBoundedFirst(bodyEntry, bodyOut, haltOp);
        ++iterations;
      }
      return {iterations};
    }
  }
  auto it = routineBindings_.find(fnOp);
  if (it != routineBindings_.end()) {
    const auto &bindings = it->second;
    for (std::size_t i = 0;
         i < std::min(args.size(), bindings.inputParams.size()); ++i) {
      core_.writeValue(bindings.inputParams[i], 0, std::nullopt, args[i]);
    }
    CellRef cursor = vm_.spawnCursor(fnOp, "call");
    auto execRes   = vm_.run(cursor, 100);
    if (!execRes.success) {
      return {};
    }
    std::vector<CellValue> results;
    results.reserve(bindings.outputParams.size());
    for (CellRef outCell : bindings.outputParams) {
      results.push_back(core_.render(outCell).value());
    }
    return results;
  }

  // Fallback: Bind directly to opcode input wings
  std::vector<CellRef> inCells = core_.inputsOf(fnOp);
  for (std::size_t i = 0; i < std::min(args.size(), inCells.size()); ++i) {
    core_.writeValue(inCells[i], 0, std::nullopt, args[i]);
  }
  CellRef cursor = vm_.spawnCursor(fnOp, "call");
  auto execRes   = vm_.run(cursor, 100);
  if (!execRes.success) {
    return {};
  }
  std::vector<CellRef> outCells = core_.outputsOf(fnOp);
  std::vector<CellValue> results;
  results.reserve(outCells.size());
  for (CellRef outCell : outCells) {
    results.push_back(core_.render(outCell).value());
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
  return static_cast<std::size_t>(std::ranges::distance(
      rankAfter(core_.arena(), *pinOpt, core_.dims().cache)));
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
std::optional<CellRef> VortexStdLib::map(
    CellRef head, DimRef inDim, DimRef outDim,
    gleditor::cpp26::function_ref<CellValue(const CellValue &)> fn) {
  // Pre-render inputs serially (one arena read pass, single-threaded), then
  // apply fn across workers over the pure value vector (no arena access during
  // the parallel phase), then mint outputs serially in index order. The result
  // is bit-identical to the serial path for any worker count, because fn is
  // applied to the same value at the same index regardless of which worker
  // runs it (vortex-multithreading-study.md §6.2). Arena mutation stays on the
  // calling thread, so the single-arena model is never touched concurrently.
  const auto inputs = rank(core_.arena(), head, inDim) |
                      std::views::transform([this](const CellRef cell) {
                        return core_.render(cell).value();
                      }) |
                      std::ranges::to<std::vector>();

  std::vector<CellValue> results(inputs.size());
  if (workerPool_ != nullptr && inputs.size() >= parallelThreshold_ &&
      workerPool_->parallelism() > 1) {
    const std::uint32_t chunks = workerPool_->parallelism();
    const std::size_t chunkSize =
        (inputs.size() + chunks - 1) / std::max<std::uint32_t>(chunks, 1);
    workerPool_->run(chunks, [&](std::uint32_t c) {
      const std::size_t begin = static_cast<std::size_t>(c) * chunkSize;
      const std::size_t end   = std::min(begin + chunkSize, inputs.size());
      for (std::size_t i = begin; i < end; ++i) {
        results[i] = fn(inputs[i]);
      }
    });
  } else {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      results[i] = fn(inputs[i]);
    }
  }

  std::vector<CellRef> outCells;
  outCells.reserve(results.size());
  for (const auto &v : results) {
    outCells.push_back(valueCell(v));
  }
  return chainCells(core_.arena(), outCells, outDim);
}

std::optional<CellRef> VortexStdLib::map(CellRef head, DimRef inDim,
                                         DimRef outDim, CellRef fnOp) {
  return map(head, inDim, outDim, [this, fnOp](const CellValue &val) {
    auto res = call(fnOp, {val});
    return res.empty() ? val : res[0];
  });
}

std::optional<CellRef> VortexStdLib::filter(
    CellRef head, DimRef inDim, DimRef outDim,
    gleditor::cpp26::function_ref<bool(const CellValue &)> pred) {
  // Rendered once into values first: pred may be a Vortex call, and a filter
  // over a transform would render each kept cell twice. The predicate is then
  // applied across workers over the pure value vector; the kept cells are
  // minted serially in input order, so the result is bit-identical to serial
  // for any worker count (vortex-multithreading-study.md §6.2).
  const auto values = rank(core_.arena(), head, inDim) |
                      std::views::transform([this](const CellRef cell) {
                        return core_.render(cell).value();
                      }) |
                      std::ranges::to<std::vector>();

  std::vector<char> keep(values.size());
  if (workerPool_ != nullptr && values.size() >= parallelThreshold_ &&
      workerPool_->parallelism() > 1) {
    const std::uint32_t chunks = workerPool_->parallelism();
    const std::size_t chunkSize =
        (values.size() + chunks - 1) / std::max<std::uint32_t>(chunks, 1);
    workerPool_->run(chunks, [&](std::uint32_t c) {
      const std::size_t begin = static_cast<std::size_t>(c) * chunkSize;
      const std::size_t end   = std::min(begin + chunkSize, values.size());
      for (std::size_t i = begin; i < end; ++i) {
        keep[i] = pred(values[i]) ? char{1} : char{0};
      }
    });
  } else {
    for (std::size_t i = 0; i < values.size(); ++i) {
      keep[i] = pred(values[i]) ? char{1} : char{0};
    }
  }

  std::vector<CellRef> kept;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (keep[i] != 0) {
      kept.push_back(valueCell(values[i]));
    }
  }
  return chainCells(core_.arena(), kept, outDim);
}

std::optional<CellRef> VortexStdLib::filter(CellRef head, DimRef inDim,
                                            DimRef outDim, CellRef predOp) {
  return filter(head, inDim, outDim, [this, predOp](const CellValue &val) {
    auto res = call(predOp, {val});
    if (res.empty()) return false;
    return VortexCore::evaluateTruthiness(res[0]);
  });
}

CellValue
VortexStdLib::fold(CellRef head, DimRef inDim, CellValue initial,
                   gleditor::cpp26::function_ref<CellValue(const CellValue &,
                                                           const CellValue &)>
                       fn) {
  // A general left fold is inherently sequential: its accumulator may have a
  // different type than the elements (e.g. fold a list of ints into a string),
  // and fn is applied strictly left-to-right, so there is no safe way to split
  // it into independent per-chunk partials without assuming the combiner is a
  // homogeneous monoid. The data-parallel win in VPL is in map/filter (one
  // independent function per element); fold stays a serial left fold here, so
  // it is bit-identical for every combiner, commutative or not
  // (vortex-multithreading-study.md §6.2: the determinism guarantee is that a
  // non-commutative combiner matches the serial fold -- which a serial fold
  // satisfies by construction). The arena reads are the only part worth
  // parallelising, and they are unsafe to run concurrently on one arena, so
  // the render pass is serial too.
  return std::ranges::fold_left(
      rank(core_.arena(), head, inDim) |
          std::views::transform([this](const CellRef cell) {
            return core_.render(cell).value();
          }),
      std::move(initial), fn);
}

CellValue VortexStdLib::fold(CellRef head, DimRef inDim, CellValue initial,
                             CellRef fnOp) {
  return fold(head, inDim, std::move(initial),
              [this, fnOp](const CellValue &acc, const CellValue &item) {
                auto res = call(fnOp, {acc, item});
                return res.empty() ? acc : res[0];
              });
}

std::optional<CellRef> VortexStdLib::zip(CellRef headA, CellRef headB,
                                         DimRef dimA, DimRef dimB,
                                         DimRef outDim) {
  auto &arena = core_.arena();
  // In zzstructures, a pair cell can link its A cell negward on dimA, and its
  // B cell posward on dimB. Materialised before any pair is linked: linking a
  // pair onto dimA would otherwise put it on the rank still being walked.
  const auto pairs =
      std::views::zip(rank(arena, headA, dimA), rank(arena, headB, dimB)) |
      std::ranges::to<std::vector>();
  return chainCells(
      arena,
      pairs | std::views::transform([&](const auto &pair) {
        const auto [a, b]      = pair;
        const CellRef pairCell = arena.makeCell();
        zigzag::expectWritten(arena.link(pairCell, dimA, DimVector::NEG, a));
        zigzag::expectWritten(arena.link(pairCell, dimB, DimVector::POS, b));
        return pairCell;
      }) | std::ranges::to<std::vector>(),
      outDim);
}

// -- Module 5: std:collections ----------------------------------------------
std::optional<CellRef>
VortexStdLib::createList(const std::vector<CellValue> &items,
                         std::optional<DimRef> dim) {
  return chainCells(core_.arena(),
                    items | std::views::transform([this](const CellValue &v) {
                      return valueCell(v);
                    }) | std::ranges::to<std::vector>(),
                    listDim(dim));
}

std::vector<CellValue>
VortexStdLib::listToVector(CellRef head, std::optional<DimRef> dim) const {
  return rank(core_.arena(), head, listDim(dim)) |
         std::views::transform([this](const CellRef cell) {
           return core_.render(cell).value();
         }) |
         std::ranges::to<std::vector>();
}

void VortexStdLib::pushBack(CellRef head, const CellValue &val,
                            std::optional<DimRef> dim) {
  if (head == noCell) return;
  const DimRef linkDim = listDim(dim);
  zigzag::expectWritten(
      core_.arena().link(rankTail(core_.arena(), head, linkDim), linkDim,
                         DimVector::POS, valueCell(val)));
}

CellRef VortexStdLib::pushFront(CellRef head, const CellValue &val,
                                std::optional<DimRef> dim) {
  const CellRef c = valueCell(val);
  if (head != noCell) {
    zigzag::expectWritten(
        core_.arena().link(c, listDim(dim), DimVector::POS, head));
  }
  return c;
}

std::optional<CellValue> VortexStdLib::popBack(CellRef head,
                                               std::optional<DimRef> dim) {
  if (head == noCell) return std::nullopt;
  const DimRef linkDim = listDim(dim);
  // The last pair of the list is (the new tail, the cell popped). A list of
  // one has no pair, and popping it leaves the head where it is.
  const auto lastPair =
      lastOf(rank(core_.arena(), head, linkDim) | std::views::adjacent<2>);
  if (!lastPair) {
    return core_.render(head).value();
  }
  const auto [newTail, popped] = *lastPair;
  CellValue val                = core_.render(popped).value();
  core_.breakLink(newTail, linkDim, DimVector::POS);
  return val;
}

std::optional<CellRef> VortexStdLib::popFront(std::optional<CellRef> head,
                                              std::optional<DimRef> dim) {
  if (!head || *head == noCell || !core_.arena().contains(*head)) {
    return std::nullopt;
  }
  const DimRef linkDim = listDim(dim);
  const auto next      = core_.arena().linked(*head, linkDim, DimVector::POS);
  core_.breakLink(*head, linkDim, DimVector::POS);
  return next;
}

std::size_t VortexStdLib::listLength(CellRef head,
                                     std::optional<DimRef> dim) const {
  return static_cast<std::size_t>(
      std::ranges::distance(rank(core_.arena(), head, listDim(dim))));
}

CellRef VortexStdLib::createMap() { return core_.arena().makeCell("map"); }

void VortexStdLib::mapSet(CellRef mapRoot, std::string_view key,
                          const CellValue &val) {
  if (mapRoot == noCell) return;
  auto &arena = core_.arena();
  if (const auto keyCell = mapKeyCell(mapRoot, key)) {
    // An existing key: restate its value cell, minting one if it had none.
    const auto valCell = step(arena, *keyCell, core_.dims().values);
    if (valCell) {
      core_.writeValue(*valCell, 0, std::nullopt, val);
    } else {
      zigzag::expectWritten(
          arena.link(*keyCell, core_.dims().values, false, valueCell(val)));
    }
    return;
  }

  // A fresh key-value pair onto the tail of the key rank.
  const CellRef keyCell = arena.makeCell(key);
  zigzag::expectWritten(
      arena.link(keyCell, core_.dims().values, false, valueCell(val)));
  zigzag::expectWritten(arena.link(rankTail(arena, mapRoot, core_.dims().vars),
                                   core_.dims().vars, false, keyCell));
}

std::optional<CellValue> VortexStdLib::mapGet(CellRef mapRoot,
                                              std::string_view key) const {
  if (mapRoot == noCell) return std::nullopt;
  return mapKeyCell(mapRoot, key)
      .and_then(hop(core_.arena(), core_.dims().values))
      .transform(
          [this](const CellRef cell) { return core_.render(cell).value(); });
}

bool VortexStdLib::mapHas(CellRef mapRoot, std::string_view key) const {
  return mapGet(mapRoot, key).has_value();
}

std::vector<std::string> VortexStdLib::mapKeys(CellRef mapRoot) const {
  return rankAfter(core_.arena(), mapRoot, core_.dims().vars) |
         std::views::transform([this](const CellRef cell) {
           return core_.arena().textOf(cell);
         }) |
         std::ranges::to<std::vector>();
}

GridResult VortexStdLib::createGrid(std::size_t rows, std::size_t cols,
                                    DimRef dRow, DimRef dCol,
                                    const CellValue &initVal) {
  if (rows == 0 || cols == 0) {
    return GridResult{.head = std::nullopt, .rows = rows, .cols = cols};
  }
  std::vector<std::vector<CellRef>> grid(rows,
                                         std::vector<CellRef>(cols, noCell));

  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c < cols; ++c) {
      grid[r][c] = core_.arena().makeCell();
      core_.writeValue(grid[r][c], 0, std::nullopt, initVal);
    }
  }

  // Link horizontally on dCol
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c + 1 < cols; ++c) {
      zigzag::expectWritten(
          core_.arena().link(grid[r][c], dCol, DimVector::POS, grid[r][c + 1]));
    }
  }

  // Link vertically on dRow
  for (std::size_t r = 0; r + 1 < rows; ++r) {
    for (std::size_t c = 0; c < cols; ++c) {
      zigzag::expectWritten(
          core_.arena().link(grid[r][c], dRow, DimVector::POS, grid[r + 1][c]));
    }
  }

  return GridResult{.head = grid[0][0], .rows = rows, .cols = cols};
}

std::optional<CellValue> VortexStdLib::getGrid(CellRef gridRoot, std::size_t r,
                                               std::size_t c, DimRef dRow,
                                               DimRef dCol) const {
  return gridCell(gridRoot, r, c, dRow, dCol)
      .transform(
          [this](const CellRef cell) { return core_.render(cell).value(); });
}

void VortexStdLib::setGrid(CellRef gridRoot, std::size_t r, std::size_t c,
                           DimRef dRow, DimRef dCol, const CellValue &val) {
  if (const auto cell = gridCell(gridRoot, r, c, dRow, dCol)) {
    core_.writeValue(*cell, 0, std::nullopt, val);
  }
}

std::optional<CellRef> VortexStdLib::gridCell(const CellRef gridRoot,
                                              const std::size_t r,
                                              const std::size_t c,
                                              const DimRef dRow,
                                              const DimRef dCol) const {
  return hops(core_.arena(), gridRoot, dRow, r)
      .and_then(
          [&](const CellRef row) { return hops(core_.arena(), row, dCol, c); });
}

CellRef VortexStdLib::valueCell(const CellValue &val) {
  const CellRef cell = core_.arena().makeCell();
  core_.writeValue(cell, 0, std::nullopt, val);
  return cell;
}

CellRef VortexStdLib::copyValueCell(const CellRef source) {
  // Typed bits copy as bits (R6); anything else copies as its rendering.
  auto &arena = core_.arena();
  const auto scalar =
      arena.asDouble(source)
          .transform([&](const double d) { return arena.makeScalarCell(d); })
          .or_else([&] {
            return arena.asInt64(source).transform(
                [&](const std::int64_t n) { return arena.makeScalarCell(n); });
          })
          .or_else([&] {
            return arena.asBool(source).transform(
                [&](const bool b) { return arena.makeScalarCell(b); });
          });
  return scalar ? *scalar : valueCell(core_.render(source).value());
}

DimRef VortexStdLib::listDim(const std::optional<DimRef> dim) const noexcept {
  return dim.value_or(core_.dims().step);
}

std::optional<CellRef>
VortexStdLib::mapKeyCell(const CellRef mapRoot,
                         const std::string_view key) const {
  return firstOf(rankAfter(core_.arena(), mapRoot, core_.dims().vars) |
                 std::views::filter([&](const CellRef cell) {
                   return core_.arena().textOf(cell) == key;
                 }));
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

void VortexStdLib::buildLogicModule(CellRef mod) {
  // unify: #UNIFY in0 in1 out
  {
    CellRef in0 = core_.arena().makeCell();
    CellRef in1 = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Unify, "#LOGIC_UNIFY");
    core_.bindInput(op, in0);
    core_.bindInput(op, in1);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in0, in1}, .outputParams = {out}};
    exportSymbol(mod, "unify", op);
  }

  // var: #MAKE_VAR out
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::MakeVar, "#LOGIC_MAKE_VAR");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "var", op);
  }

  // is_var: #IS_VAR in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::IsVar, "#LOGIC_IS_VAR");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "is_var", op);
  }

  // term: #MAKE_TERM in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::MakeTerm, "#LOGIC_MAKE_TERM");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "term", op);
  }

  // deref: #DEREF in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Deref, "#LOGIC_DEREF");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "deref", op);
  }

  // choice: #CHOICE in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Choice, "#LOGIC_CHOICE");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "choice", op);
  }

  // fail: #FAIL
  {
    CellRef op           = vm_.mintOpcode(OpcodeKind::Fail, "#LOGIC_FAIL");
    routineBindings_[op] = {.inputParams = {}, .outputParams = {}};
    exportSymbol(mod, "fail", op);
  }

  // cut: #CUT in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Cut, "#LOGIC_CUT");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {in}, .outputParams = {out}};
    exportSymbol(mod, "cut", op);
  }

  // Predicate: equal/2
  // equal(X, X).
  {
    predEqual_   = createPredicate("equal");
    CellRef x    = makeVar();
    CellRef head = makeTerm("equal", {x, x});
    addClause(predEqual_, head);
    exportSymbol(mod, "equal", predEqual_);
  }

  // Predicate: =/2
  // =(X, X).
  {
    predUnify_   = createPredicate("=");
    CellRef x    = makeVar();
    CellRef head = makeTerm("=", {x, x});
    addClause(predUnify_, head);
    exportSymbol(mod, "=", predUnify_);
  }

  // Predicate: member/2
  // member(X, [X | _]).
  // member(X, [_ | T]) :- member(X, T).
  {
    predMember_   = createPredicate("member");
    CellRef x1    = makeVar();
    CellRef wild1 = makeVar();
    CellRef head1 = makeTerm("member", {x1, makeCons(x1, wild1)});
    addClause(predMember_, head1);

    CellRef x2    = makeVar();
    CellRef wild2 = makeVar();
    CellRef t2    = makeVar();
    CellRef head2 = makeTerm("member", {x2, makeCons(wild2, t2)});
    CellRef body2 = makeTerm("member", {x2, t2});
    addClause(predMember_, head2, std::span<const CellRef>{&body2, 1});

    exportSymbol(mod, "member", predMember_);
  }

  // Predicate: append/3
  // append([], L, L).
  // append([H | T], L, [H | R]) :- append(T, L, R).
  {
    predAppend_       = createPredicate("append");
    CellRef emptyList = core_.arena().makeCell("[]");
    CellRef l1        = makeVar();
    CellRef head1     = makeTerm("append", {emptyList, l1, l1});
    addClause(predAppend_, head1);

    CellRef h     = makeVar();
    CellRef t     = makeVar();
    CellRef l2    = makeVar();
    CellRef r     = makeVar();
    CellRef head2 = makeTerm("append", {makeCons(h, t), l2, makeCons(h, r)});
    CellRef body2 = makeTerm("append", {t, l2, r});
    addClause(predAppend_, head2, std::span<const CellRef>{&body2, 1});

    exportSymbol(mod, "append", predAppend_);
  }

  // Predicate: length/2
  // length([], 0).
  {
    predLength_       = createPredicate("length");
    CellRef emptyList = core_.arena().makeCell("[]");
    CellRef zero  = core_.arena().makeScalarCell(static_cast<std::int64_t>(0));
    CellRef head1 = makeTerm("length", {emptyList, zero});
    addClause(predLength_, head1);

    exportSymbol(mod, "length", predLength_);
  }

  // Predicate: vortex_function/2 and vortex_module/1 introspection
  {
    predVortexFunction_ = createPredicate("vortex_function");
    exportSymbol(mod, "vortex_function", predVortexFunction_);

    predVortexModule_ = createPredicate("vortex_module");
    exportSymbol(mod, "vortex_module", predVortexModule_);

    predVortexInstruction_ = createPredicate("vortex_instruction");
    exportSymbol(mod, "vortex_instruction", predVortexInstruction_);

    predVortexContract_ = createPredicate("vortex_contract");
    exportSymbol(mod, "vortex_contract", predVortexContract_);

    predVortexParam_ = createPredicate("vortex_param");
    exportSymbol(mod, "vortex_param", predVortexParam_);

    predSetting_ = createPredicate("setting");
    exportSymbol(mod, "setting", predSetting_);

    predSettingShape_ = createPredicate("setting_shape");
    exportSymbol(mod, "setting_shape", predSettingShape_);

    predSettingDefault_ = createPredicate("setting_default");
    exportSymbol(mod, "setting_default", predSettingDefault_);

    predCellValue_ = createPredicate("cell_value");
    exportSymbol(mod, "cell_value", predCellValue_);

    predCellLink_ = createPredicate("cell_link");
    exportSymbol(mod, "cell_link", predCellLink_);

    predTransclude_ = createPredicate("transclude");
    exportSymbol(mod, "transclude", predTransclude_);

    predXanalink_ = createPredicate("xanalink");
    exportSymbol(mod, "xanalink", predXanalink_);

    predCellSpan_ = createPredicate("cell_span");
    exportSymbol(mod, "cell_span", predCellSpan_);

    predBridgeEdge_ = createPredicate("bridge_edge");
    exportSymbol(mod, "bridge_edge", predBridgeEdge_);
  }
}

// -- Module 15: std:goal ------------------------------------------------------
//
// Icon's goal-directed evaluation over Vlog's choice-point frame
// (design/vicon-goal-directed-language.md section 7), assembled per the
// emission idioms pinned in design/vicon-implementation.md. Every export is
// an entry opcode whose operand cells take the cell-arg convention: the
// caller (call(), a test, or a compiled program's prologue) writes argument
// values -- and refs where an operand names an op -- spawns a cursor at the
// entry, and the graph obeys the shared suspension protocol:
//
//   produce: write the result into the `out` operand, Jump to the op the
//            `cont` operand names (prefilled with goalHalt_)
//   resume:  land the cursor on a #Fail op (goalFail_) -- backtrack pops the
//            generator's top ChoicePoint, releases its Mark, and continues
//            at the recorded altOp
//   exhaust: a #Fail finding no choice point -- the driver reads nullopt
//
// The vicon compiler emits the same graphs inline; these routines are the
// canonical forms the symbols resolve to for import()/reflection.
void VortexStdLib::buildGoalModule(CellRef mod) {
  goalHalt_     = vm_.mintOpcode(OpcodeKind::Halt, "#GOAL_HALT");
  goalFail_     = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_FAIL");
  goalSentinel_ = vm_.mintOpcode(OpcodeKind::Halt, "#GOAL_SENTINEL");

  const auto lit = [this](const std::int64_t v) {
    return core_.arena().makeScalarCell(v);
  };
  // A Jump/Branch/Choice target travels as an operand cell's *value*: the VM
  // renders the cell and takes the integer as the op to run -- the same
  // idiom vortex_logic_test.cpp drives #Choice with.
  const auto opref = [this](const CellRef target) {
    return core_.arena().makeScalarCell(static_cast<std::int64_t>(target));
  };
  const auto spin = [this](const CellRef a, const CellRef b) {
    zigzag::expectWritten(
        core_.arena().link(a, core_.dims().spin, DimVector::POS, b));
  };
  const auto operand = [this] { return core_.arena().makeCell(); };
  // A `cont` operand prefilled with the module's shared halt op keeps every
  // graph self-contained: produce suspends on it until a caller overwrites
  // the operand with its own continuation's ref.
  const auto contCell = [this] {
    return core_.arena().makeScalarCell(static_cast<std::int64_t>(goalHalt_));
  };

  const CellRef zero   = lit(0);
  const CellRef one    = lit(1);
  const CellRef negOne = lit(-1);

  // to: `i to j by k` -- the bounded retry-idiom generator.
  // in: lo, hi, step, out, cont.
  {
    CellRef lo     = operand();
    CellRef hi     = operand();
    CellRef stepIn = operand();
    CellRef out    = operand();
    CellRef cont   = contCell();

    CellRef cur = operand(); // the live scalar, stepped per resume
    CellRef dir = operand(); // step >= 0
    CellRef t   = operand(); // exhausted flag
    CellRef ok  = operand(); // in-range flag

    CellRef entry   = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_TO");
    CellRef init    = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_TO_INIT");
    CellRef prod    = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_TO_PRODUCE");
    CellRef susp    = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_TO_SUSPEND");
    CellRef next    = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_TO_STEP");
    CellRef dirTest = vm_.mintOpcode(OpcodeKind::Gte, "#GOAL_TO_DIR");
    CellRef dirBr   = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_TO_DIRBR");
    CellRef negTest = vm_.mintOpcode(OpcodeKind::Lt, "#GOAL_TO_NEG");
    CellRef joinJ   = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_TO_JOIN");
    CellRef posTest = vm_.mintOpcode(OpcodeKind::Lt, "#GOAL_TO_POS");
    CellRef test    = vm_.mintOpcode(OpcodeKind::Not, "#GOAL_TO_TEST");
    CellRef againBr = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_TO_AGAIN");
    CellRef fail    = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_TO_FAIL");
    CellRef again   = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_TO_RECHOICE");
    CellRef prod2   = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_TO_PRODUCE2");
    CellRef susp2   = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_TO_SUSPEND2");

    spin(entry, init);
    spin(init, prod);
    spin(prod, susp);
    spin(susp, next);
    spin(next, dirTest);
    spin(dirTest, dirBr);
    spin(dirBr, negTest);
    spin(negTest, joinJ);
    spin(joinJ, posTest);
    spin(posTest, test);
    spin(test, againBr);
    spin(againBr, fail);
    spin(fail, again);
    spin(again, prod2);
    spin(prod2, susp2);

    // entry: Choice NEXT -- the resume path steps and tests before produce.
    core_.bindInput(entry, opref(next));
    // cur := lo
    core_.bindInput(init, lo);
    core_.bindInput(init, zero);
    core_.bindOutput(init, cur);
    // produce: out := cur, then suspend on `cont`.
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, cur);
    core_.bindInput(susp, cont);
    // next: cur += step.
    core_.bindInput(next, cur);
    core_.bindInput(next, stepIn);
    core_.bindOutput(next, cur);
    // dir := (step >= 0): the bound check differs per walking direction.
    core_.bindInput(dirTest, stepIn);
    core_.bindInput(dirTest, zero);
    core_.bindOutput(dirTest, dir);
    core_.bindInput(dirBr, dir);
    core_.bindInput(dirBr, opref(posTest));
    // Descending: exhausted once cur < hi.
    core_.bindInput(negTest, cur);
    core_.bindInput(negTest, hi);
    core_.bindOutput(negTest, t);
    core_.bindInput(joinJ, opref(test));
    // Ascending: exhausted once hi < cur.
    core_.bindInput(posTest, hi);
    core_.bindInput(posTest, cur);
    core_.bindOutput(posTest, t);
    core_.bindInput(test, t);
    core_.bindOutput(test, ok);
    core_.bindInput(againBr, ok);
    core_.bindInput(againBr, opref(again));
    // Re-push the identical frame, produce, suspend (the contract's retry
    // idiom: a resume consumes the frame and immediately replaces it).
    core_.bindInput(again, opref(next));
    core_.bindInput(prod2, out);
    core_.bindInput(prod2, zero);
    core_.bindInput(prod2, negOne);
    core_.bindInput(prod2, cur);
    core_.bindInput(susp2, cont);

    routineBindings_[entry] = {.inputParams  = {lo, hi, stepIn, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "to", entry);
  }

  // from: `i by k` forever -- the unbounded integer sequence; `seq` is the
  // same generator under the contract's other name for it.
  // in: lo, step, out, cont.
  {
    CellRef lo     = operand();
    CellRef stepIn = operand();
    CellRef out    = operand();
    CellRef cont   = contCell();
    CellRef cur    = operand();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_FROM");
    CellRef init  = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_FROM_INIT");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_FROM_PRODUCE");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_FROM_SUSPEND");
    CellRef next  = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_FROM_STEP");
    CellRef again = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_FROM_RECHOICE");
    CellRef prod2 = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_FROM_PRODUCE2");
    CellRef susp2 = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_FROM_SUSPEND2");

    spin(entry, init);
    spin(init, prod);
    spin(prod, susp);
    spin(susp, next);
    spin(next, again);
    spin(again, prod2);
    spin(prod2, susp2);

    core_.bindInput(entry, opref(next));
    core_.bindInput(init, lo);
    core_.bindInput(init, zero);
    core_.bindOutput(init, cur);
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, cur);
    core_.bindInput(susp, cont);
    core_.bindInput(next, cur);
    core_.bindInput(next, stepIn);
    core_.bindOutput(next, cur);
    core_.bindInput(again, opref(next));
    core_.bindInput(prod2, out);
    core_.bindInput(prod2, zero);
    core_.bindInput(prod2, negOne);
    core_.bindInput(prod2, cur);
    core_.bindInput(susp2, cont);

    routineBindings_[entry] = {.inputParams  = {lo, stepIn, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "from", entry);
    exportSymbol(mod, "seq", entry);
  }

  // repeat: produce `in` forever -- the frame re-pushes itself as its own
  // altOp, so every resume republishes the same value.
  // in: in, out, cont.
  {
    CellRef in   = operand();
    CellRef out  = operand();
    CellRef cont = contCell();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_REPEAT");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_REPEAT_PRODUCE");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_REPEAT_SUSPEND");
    CellRef again = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_REPEAT_RECHOICE");
    CellRef prod2 = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_REPEAT_PRODUCE2");
    CellRef susp2 = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_REPEAT_SUSPEND2");

    spin(entry, prod);
    spin(prod, susp);
    spin(susp, again);
    spin(again, prod2);
    spin(prod2, susp2);

    core_.bindInput(entry, opref(again));
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, in);
    core_.bindInput(susp, cont);
    core_.bindInput(again, opref(again));
    core_.bindInput(prod2, out);
    core_.bindInput(prod2, zero);
    core_.bindInput(prod2, negOne);
    core_.bindInput(prod2, in);
    core_.bindInput(susp2, cont);

    routineBindings_[entry] = {.inputParams  = {in, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "repeat", entry);
  }

  // write: produce `in` once, then the sequence is dry -- Icon's write
  // returns its argument; printing happens at the driver's render boundary.
  // in: in, out, cont.
  {
    CellRef in   = operand();
    CellRef out  = operand();
    CellRef cont = contCell();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_WRITE");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_WRITE_PRODUCE");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_WRITE_SUSPEND");
    CellRef dead  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_WRITE_DEAD");

    spin(entry, prod);
    spin(prod, susp);
    spin(susp, dead);

    core_.bindInput(entry, opref(dead));
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, in);
    core_.bindInput(susp, cont);

    routineBindings_[entry] = {.inputParams  = {in, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "write", entry);
  }

  // writeSeq: `!s` for a string subject -- every one-character quoting cell
  // value(subject, i, 1) for i in [1, len], each produce an ephemeral cell
  // ref (spec section 5.2). in: subject, len, out, cont.
  {
    CellRef subj = operand();
    CellRef len  = operand();
    CellRef out  = operand();
    CellRef cont = contCell();

    CellRef idx = operand();
    CellRef t   = operand();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_WRITESEQ");
    CellRef init  = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_WRITESEQ_INIT");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_WRITESEQ_PRODUCE");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_WRITESEQ_SUSPEND");
    CellRef next  = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_WRITESEQ_STEP");
    CellRef test  = vm_.mintOpcode(OpcodeKind::Lte, "#GOAL_WRITESEQ_TEST");
    CellRef br    = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_WRITESEQ_BR");
    CellRef fail  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_WRITESEQ_FAIL");
    CellRef again =
        vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_WRITESEQ_RECHOICE");
    CellRef prod2 =
        vm_.mintOpcode(OpcodeKind::Value, "#GOAL_WRITESEQ_PRODUCE2");
    CellRef susp2 = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_WRITESEQ_SUSPEND2");

    spin(entry, init);
    spin(init, prod);
    spin(prod, susp);
    spin(susp, next);
    spin(next, test);
    spin(test, br);
    spin(br, fail);
    spin(fail, again);
    spin(again, prod2);
    spin(prod2, susp2);

    core_.bindInput(entry, opref(next));
    // &pos convention: positions are 1-based.
    core_.bindInput(init, one);
    core_.bindInput(init, zero);
    core_.bindOutput(init, idx);
    // #Value read form: out := the quoting cell for subject[idx:1].
    core_.bindInput(prod, subj);
    core_.bindInput(prod, idx);
    core_.bindInput(prod, one);
    core_.bindOutput(prod, out);
    core_.bindInput(susp, cont);
    core_.bindInput(next, idx);
    core_.bindInput(next, one);
    core_.bindOutput(next, idx);
    core_.bindInput(test, idx);
    core_.bindInput(test, len);
    core_.bindOutput(test, t);
    core_.bindInput(br, t);
    core_.bindInput(br, opref(again));
    core_.bindInput(again, opref(next));
    core_.bindInput(prod2, subj);
    core_.bindInput(prod2, idx);
    core_.bindInput(prod2, one);
    core_.bindOutput(prod2, out);
    core_.bindInput(susp2, cont);

    routineBindings_[entry] = {.inputParams  = {subj, len, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "writeSeq", entry);
  }

  // limit: `e \ n` -- the kill-flag counter. A resume cannot be intercepted
  // once a choice point sits on the stack, so the inner generator's stale
  // frames are poisoned instead (vicon-implementation.md): `limit_gate` is
  // the produce continuation a compiled `e` jumps to after writing its
  // result into `limit_result`, and every resume path inside `e` branches
  // on `limit_killflag` toward `limit_killop`. When the counter hits zero
  // the flag stands and each live `e` frame pops exactly once, sees it, and
  // fails through -- O(live frames) cheap pops, so `\` on an infinite
  // generator still terminates. Those wiring cells ride along as extra
  // symbols because an opcode graph can only write cells bound at assembly
  // time: callers discover them with resolve(), not guesses.
  // in: eEntry, n, out, cont.
  {
    CellRef eEntry = operand();
    CellRef n      = operand();
    CellRef out    = operand();
    CellRef cont   = contCell();

    CellRef cnt  = operand();
    CellRef kill = operand();
    CellRef eRes = operand(); // the shared result cell `e` produces into
    CellRef t    = operand();
    CellRef t0   = operand();

    CellRef entry   = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_LIMIT");
    CellRef preTest = vm_.mintOpcode(OpcodeKind::Lte, "#GOAL_LIMIT_PRE");
    CellRef preBr   = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_LIMIT_PREBR");
    CellRef init    = vm_.mintOpcode(OpcodeKind::Add, "#GOAL_LIMIT_INIT");
    CellRef initK   = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_LIMIT_KILLCLR");
    CellRef goE     = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_LIMIT_GO");
    CellRef gate    = vm_.mintOpcode(OpcodeKind::Sub, "#GOAL_LIMIT_GATE");
    CellRef cntTest = vm_.mintOpcode(OpcodeKind::Lte, "#GOAL_LIMIT_CNT");
    CellRef killBr  = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_LIMIT_KILLBR");
    CellRef delivJ  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_LIMIT_DELIVJ");
    CellRef setKill = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_LIMIT_KILLSET");
    CellRef deliv   = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_LIMIT_DELIVER");
    CellRef susp    = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_LIMIT_SUSPEND");
    CellRef killOp  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_LIMIT_KILL");
    CellRef exit    = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_LIMIT_EXIT");

    spin(entry, preTest);
    spin(preTest, preBr);
    spin(preBr, init);
    spin(init, initK);
    spin(initK, goE);
    spin(goE, gate);
    spin(gate, cntTest);
    spin(cntTest, killBr);
    spin(killBr, delivJ);
    spin(delivJ, setKill);
    spin(setKill, deliv);
    spin(deliv, susp);
    spin(susp, killOp);
    spin(killOp, exit);

    // The entry frame's altOp is the failure propagator: when `e` exhausts
    // naturally its #Fail lands here and the failure walks outward.
    core_.bindInput(entry, opref(exit));
    // e \ 0 fails before e is entered.
    core_.bindInput(preTest, n);
    core_.bindInput(preTest, zero);
    core_.bindOutput(preTest, t0);
    core_.bindInput(preBr, t0);
    core_.bindInput(preBr, opref(exit));
    // cnt := n; kill := 0.
    core_.bindInput(init, n);
    core_.bindInput(init, zero);
    core_.bindOutput(init, cnt);
    core_.bindInput(initK, kill);
    core_.bindInput(initK, zero);
    core_.bindInput(initK, negOne);
    core_.bindInput(initK, zero);
    core_.bindInput(goE, eEntry);
    // gate: one produce survived; cnt -= 1 and arm the poison when it is
    // spent.
    core_.bindInput(gate, cnt);
    core_.bindInput(gate, one);
    core_.bindOutput(gate, cnt);
    core_.bindInput(cntTest, cnt);
    core_.bindInput(cntTest, zero);
    core_.bindOutput(cntTest, t);
    core_.bindInput(killBr, t);
    core_.bindInput(killBr, opref(setKill));
    core_.bindInput(delivJ, opref(deliv));
    core_.bindInput(setKill, kill);
    core_.bindInput(setKill, zero);
    core_.bindInput(setKill, negOne);
    core_.bindInput(setKill, one);
    // deliver: out := eResult, then the produce suspends on `cont`.
    core_.bindInput(deliv, out);
    core_.bindInput(deliv, zero);
    core_.bindInput(deliv, negOne);
    core_.bindInput(deliv, eRes);
    core_.bindInput(susp, cont);

    routineBindings_[entry] = {.inputParams  = {eEntry, n, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "limit", entry);
    exportSymbol(mod, "limit_gate", gate);
    exportSymbol(mod, "limit_killflag", kill);
    exportSymbol(mod, "limit_killop", killOp);
    exportSymbol(mod, "limit_result", eRes);
  }

  // cmp: the goal-directed comparison -- produces `right` when `left op
  // right` holds, else fails (spec section 2: a comparison is a generator
  // of its right operand). in: op, left, right, out, cont; `op` is the
  // selector string "<", "<=", "=", "~=", ">", ">=".
  {
    CellRef opSel = operand();
    CellRef left  = operand();
    CellRef right = operand();
    CellRef out   = operand();
    CellRef cont  = contCell();

    CellRef sel = operand(); // selector match flag
    CellRef t   = operand(); // comparison result

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_CMP");
    CellRef unkJ  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_CMP_UNKNOWN");
    CellRef test  = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_CMP_TEST");
    CellRef fail  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_CMP_FAIL");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_CMP_PRODUCE");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_CMP_SUSPEND");
    CellRef dead  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_CMP_DEAD");
    core_.bindInput(entry, opref(dead));

    // One Eq/Branch pair per selector; a non-match falls through to the next
    // pair, and the cascade's tail Jump fails on an unknown selector. Each
    // comparison arm lives behind the cascade on the spin chain and lands on
    // `test` through its own Jump, so no arm is reachable by fall-through.
    constexpr std::string_view kSelectors[] = {"<", "<=", "=", "~=", ">", ">="};
    constexpr OpcodeKind kOps[]             = {OpcodeKind::Lt, OpcodeKind::Lte,
                                               OpcodeKind::Eq, OpcodeKind::Neq,
                                               OpcodeKind::Gt, OpcodeKind::Gte};
    std::vector<CellRef> arms;
    CellRef prev = entry;
    for (std::size_t i = 0; i < std::size(kSelectors); ++i) {
      CellRef selCell = core_.arena().makeCell(kSelectors[i]);
      CellRef eqOp    = vm_.mintOpcode(OpcodeKind::Eq, "#GOAL_CMP_SEL");
      CellRef brOp    = vm_.mintOpcode(OpcodeKind::Branch, "#GOAL_CMP_SELBR");
      CellRef doOp    = vm_.mintOpcode(kOps[i], "#GOAL_CMP_DO");
      CellRef join    = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_CMP_JOIN");
      spin(prev, eqOp);
      spin(eqOp, brOp);
      prev = brOp;
      core_.bindInput(eqOp, opSel);
      core_.bindInput(eqOp, selCell);
      core_.bindOutput(eqOp, sel);
      core_.bindInput(brOp, sel);
      core_.bindInput(brOp, opref(doOp));
      core_.bindInput(doOp, left);
      core_.bindInput(doOp, right);
      core_.bindOutput(doOp, t);
      core_.bindInput(join, opref(test));
      // Arm region: doOp -> join -> test. Chained only for introspection;
      // execution reaches doOp solely through its selector's Branch.
      arms.push_back(doOp);
      arms.push_back(join);
    }
    spin(prev, unkJ);
    for (const CellRef arm : arms) {
      spin(prev, arm);
      prev = arm;
    }
    spin(prev, test);
    spin(test, fail);
    spin(fail, prod);
    spin(prod, susp);
    spin(susp, dead);

    core_.bindInput(unkJ, opref(dead));
    core_.bindInput(test, t);
    core_.bindInput(test, opref(prod));
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, right);
    core_.bindInput(susp, cont);

    routineBindings_[entry] = {.inputParams  = {opSel, left, right, out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "cmp", entry);
  }

  // alt: `e1 | e2` -- the two-arm choice. Fall-through runs e1; when e1's
  // own frames are exhausted the #Fail pops this frame and lands on e2.
  // in: e1Entry, e2Entry.
  {
    CellRef e1Entry = operand();
    CellRef e2Entry = operand();

    CellRef entry  = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_ALT");
    CellRef goE1   = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_ALT_FIRST");
    CellRef altArm = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_ALT_SECOND");

    spin(entry, goE1);
    spin(goE1, altArm);

    core_.bindInput(entry, opref(altArm));
    core_.bindInput(goE1, e1Entry);
    core_.bindInput(altArm, e2Entry);

    routineBindings_[entry] = {.inputParams  = {e1Entry, e2Entry},
                               .outputParams = {}};
    exportSymbol(mod, "alt", entry);
  }

  // fail: #Fail alone -- the exhausted generator as a symbol.
  routineBindings_[goalFail_] = {.inputParams = {}, .outputParams = {}};
  exportSymbol(mod, "fail", goalFail_);

  // succeed: produce &null once -- the bare cell, an empty result.
  // in: out, cont.
  {
    CellRef out  = operand();
    CellRef cont = contCell();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_SUCCEED");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_SUCCEED_PROD");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_SUCCEED_SUSP");
    CellRef dead  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_SUCCEED_DEAD");
    CellRef nullV = core_.arena().makeCell("");

    spin(entry, prod);
    spin(prod, susp);
    spin(susp, dead);

    core_.bindInput(entry, opref(dead));
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, nullV);
    core_.bindInput(susp, cont);

    routineBindings_[entry] = {.inputParams  = {out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "succeed", entry);
  }

  // -- d.scan: the scanning environment ---------------------------------------
  // A scanning cursor is a cell on home's +d.scan rank whose content is &pos
  // (1-based) and whose +d.grab is the subject cell; the posward-most cursor
  // is current. The routine below keeps one fixed cursor cell because an
  // opcode stream cannot write through a runtime ref -- every produce target
  // must be a cell bound at assembly time. That cell predates any mark a
  // generator inside `e` takes, so its &pos writes are trailed and an inner
  // failure restores them for free (spec section 6.2); when `e` drains, the
  // #Fail that pops the scan frame releases its Mark and the whole push --
  // rank link, &pos, subject grab -- unwinds with it.
  CellRef scanCursorCell = operand();
  CellRef scanSubjCell   = operand();
  {
    CellRef subj   = operand();
    CellRef eEntry = operand();

    const CellRef scanDimCell =
        lit(static_cast<std::int64_t>(core_.dims().scan));
    const CellRef grabDimCell =
        lit(static_cast<std::int64_t>(core_.dims().grab));

    CellRef entry   = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_SCAN");
    CellRef push    = vm_.mintOpcode(OpcodeKind::New, "#GOAL_SCAN_PUSH");
    CellRef posSet  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_SCAN_POS");
    CellRef recSub  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_SCAN_SUBJ");
    CellRef linkSub = vm_.mintOpcode(OpcodeKind::Link, "#GOAL_SCAN_GRAB");
    CellRef goE     = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_SCAN_GO");
    CellRef exit    = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_SCAN_EXIT");

    spin(entry, push);
    spin(push, posSet);
    spin(posSet, recSub);
    spin(recSub, linkSub);
    spin(linkSub, goE);
    spin(goE, exit);

    core_.bindInput(entry, opref(exit));
    // Push the cursor onto home's +d.scan rank (New links the bound output
    // cell posward of its anchor).
    core_.bindInput(push, core_.home());
    core_.bindInput(push, scanDimCell);
    core_.bindInput(push, one);
    core_.bindOutput(push, scanCursorCell);
    // &pos := 1 (Icon positions are 1-based).
    core_.bindInput(posSet, scanCursorCell);
    core_.bindInput(posSet, zero);
    core_.bindInput(posSet, negOne);
    core_.bindInput(posSet, one);
    // &subject bookkeeping for the subject routine: the cursor's +d.grab is
    // the subject operand cell; a content copy tracks it for producers.
    core_.bindInput(recSub, scanSubjCell);
    core_.bindInput(recSub, zero);
    core_.bindInput(recSub, negOne);
    core_.bindInput(recSub, subj);
    core_.bindInput(linkSub, scanCursorCell);
    core_.bindInput(linkSub, grabDimCell);
    core_.bindInput(linkSub, one);
    core_.bindInput(linkSub, subj);
    core_.bindInput(goE, eEntry);

    routineBindings_[entry] = {.inputParams  = {subj, eEntry},
                               .outputParams = {}};
    exportSymbol(mod, "scan", entry);
  }

  // subject: produce &subject once -- the subject content the open `scan`
  // recorded. in: out, cont.
  {
    CellRef out  = operand();
    CellRef cont = contCell();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_SUBJECT");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_SUBJECT_PROD");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_SUBJECT_SUSP");
    CellRef dead  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_SUBJECT_DEAD");

    spin(entry, prod);
    spin(prod, susp);
    spin(susp, dead);

    core_.bindInput(entry, opref(dead));
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, scanSubjCell);
    core_.bindInput(susp, cont);

    routineBindings_[entry] = {.inputParams  = {out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "subject", entry);
  }

  // pos: produce &pos once -- the open scan cursor's content.
  // in: out, cont.
  {
    CellRef out  = operand();
    CellRef cont = contCell();

    CellRef entry = vm_.mintOpcode(OpcodeKind::Choice, "#GOAL_POS");
    CellRef prod  = vm_.mintOpcode(OpcodeKind::Value, "#GOAL_POS_PROD");
    CellRef susp  = vm_.mintOpcode(OpcodeKind::Jump, "#GOAL_POS_SUSP");
    CellRef dead  = vm_.mintOpcode(OpcodeKind::Fail, "#GOAL_POS_DEAD");

    spin(entry, prod);
    spin(prod, susp);
    spin(susp, dead);

    core_.bindInput(entry, opref(dead));
    core_.bindInput(prod, out);
    core_.bindInput(prod, zero);
    core_.bindInput(prod, negOne);
    core_.bindInput(prod, scanCursorCell);
    core_.bindInput(susp, cont);

    routineBindings_[entry] = {.inputParams  = {out, cont},
                               .outputParams = {out}};
    exportSymbol(mod, "pos", entry);
  }

  // Host-driven routines: the synchronous forms of the control structures
  // the compiler emits inline, dispatched by label inside call() -- the
  // same style as #ZZ_/#ARRAY_. Their operand cells carry entry-op and
  // result-cell refs as integer values.
  const auto hostRoutine =
      [this, mod](std::string_view symbol, std::string_view label,
                  std::initializer_list<CellRef> ins, CellRef outCell) {
        CellRef op = vm_.mintOpcode(OpcodeKind::Nop, label);
        for (const CellRef in : ins) {
          core_.bindInput(op, in);
        }
        core_.bindOutput(op, outCell);
        routineBindings_[op] = {.inputParams  = std::vector<CellRef>(ins),
                                .outputParams = {outCell}};
        exportSymbol(mod, symbol, op);
      };

  // every: genEntry, genOut, halt -> list of every produce.
  hostRoutine("every", "#GOAL_EVERY", {operand(), operand(), operand()},
              operand());
  // bang: !x -- head, dim -> list of element refs (eager rank walk).
  hostRoutine("bang", "#GOAL_BANG", {operand(), operand()}, operand());
  // if: condEntry, condOut, thenEntry, thenOut, elseEntry, elseOut, halt.
  hostRoutine("if", "#GOAL_IF",
              {operand(), operand(), operand(), operand(), operand(), operand(),
               operand()},
              operand());
  // while/until: condEntry, condOut, bodyEntry, bodyOut, halt -> iteration
  // count.
  hostRoutine("while", "#GOAL_WHILE",
              {operand(), operand(), operand(), operand(), operand()},
              operand());
  hostRoutine("until", "#GOAL_UNTIL",
              {operand(), operand(), operand(), operand(), operand()},
              operand());

  GLEDITOR_LOG_DEBUG("vortex.stdlib",
                     "std:goal woven: generators, limit, cmp, scan plumbing");
}

std::optional<CellRef> VortexStdLib::goalProduce(const CellRef entryOp,
                                                 const CellRef haltOp,
                                                 const std::string_view name) {
  if (entryOp == noCell || haltOp == noCell ||
      !core_.arena().contains(entryOp)) {
    return std::nullopt;
  }
  const std::size_t base = vm_.choiceDepth();
  const CellRef cursor   = vm_.spawnCursor(entryOp, name);
  // The sentinel frame sits beneath the generator's own: when the sequence
  // runs dry the drain lands on it -- a Halt -- instead of consuming choice
  // points that are not this activation's.
  vm_.pushChoicePoint(cursor, goalSentinel_);
  const auto res = vm_.run(cursor);
  if (res.success && vm_.getCursorOpcode(cursor) == haltOp) {
    return cursor;
  }
  // Failed activation: unwind what it left so the write trail replays and
  // the stack is back where the caller found it.
  while (vm_.choiceDepth() > base && vm_.backtrack(cursor)) {
  }
  return std::nullopt;
}

std::optional<CellRef> VortexStdLib::goalResume(const CellRef cursor,
                                                const CellRef failOp,
                                                const CellRef haltOp) {
  if (cursor == noCell || failOp == noCell) {
    return std::nullopt;
  }
  // Landing on a #Fail is Icon's resumption: backtrack pops the top choice
  // point, releases its Mark, and continues at the recorded altOp.
  vm_.setCursorOpcode(cursor, failOp);
  const auto res = vm_.run(cursor);
  if (res.success && vm_.getCursorOpcode(cursor) == haltOp) {
    return cursor;
  }
  return std::nullopt;
}

std::optional<CellValue>
VortexStdLib::goalBoundedFirst(const CellRef entryOp,
                               const std::optional<CellRef> outCell,
                               const CellRef haltOp) {
  if (entryOp == noCell || !core_.arena().contains(entryOp)) {
    return std::nullopt;
  }
  const std::size_t base = vm_.choiceDepth();
  const CellRef cursor   = vm_.spawnCursor(entryOp, "goal.bounded");
  vm_.pushChoicePoint(cursor, goalSentinel_);
  const auto res = vm_.run(cursor);
  if (res.success && vm_.getCursorOpcode(cursor) == haltOp) {
    const CellValue v =
        outCell ? core_.render(*outCell).value() : CellValue{true};
    // Bounded evaluation commits its first result: the choice points are
    // discarded and the produce write stands (spec section 4.4).
    vm_.cut(base);
    return v;
  }
  while (vm_.choiceDepth() > base && vm_.backtrack(cursor)) {
  }
  return std::nullopt;
}

std::vector<CellValue>
VortexStdLib::goalResults(const CellRef entryOp,
                          const std::optional<CellRef> outCell,
                          const CellRef haltOp) {
  std::vector<CellValue> results;
  if (entryOp == noCell || !core_.arena().contains(entryOp)) {
    return results;
  }
  const std::size_t base = vm_.choiceDepth();
  const CellRef cursor   = vm_.spawnCursor(entryOp, "goal.every");
  vm_.pushChoicePoint(cursor, goalSentinel_);
  for (;;) {
    const auto res = vm_.run(cursor);
    if (!res.success) {
      break;
    }
    const auto pc = vm_.getCursorOpcode(cursor);
    if (!pc || *pc == goalSentinel_ || *pc != haltOp) {
      break; // drained to the sentinel, or finished without producing
    }
    results.push_back(outCell ? core_.render(*outCell).value()
                              : CellValue{true});
    // Resume: land on the shared #Fail, which backtracks into the next
    // alternative -- or onto the sentinel when the generator is dry.
    vm_.setCursorOpcode(cursor, goalFail_);
  }
  while (vm_.choiceDepth() > base && vm_.backtrack(cursor)) {
  }
  return results;
}

std::span<const CellRef>
VortexStdLib::routineInputs(const CellRef entryOp) const {
  const auto it = routineBindings_.find(entryOp);
  return it == routineBindings_.end()
             ? std::span<const CellRef>{}
             : std::span<const CellRef>{it->second.inputParams};
}

std::span<const CellRef>
VortexStdLib::routineOutputs(const CellRef entryOp) const {
  const auto it = routineBindings_.find(entryOp);
  return it == routineBindings_.end()
             ? std::span<const CellRef>{}
             : std::span<const CellRef>{it->second.outputParams};
}

std::optional<CellRef> VortexStdLib::scanPush(const CellRef subject) {
  if (!core_.arena().contains(subject)) {
    return std::nullopt;
  }
  // &pos is 1-based; appended at the tail, so the posward-most cursor is
  // the current one (spec section 6.1).
  const CellRef cursor =
      core_.arena().makeScalarCell(static_cast<std::int64_t>(1));
  zigzag::expectWritten(core_.arena().link(
      zigzag::rankTail(core_.arena(), core_.home(), core_.dims().scan),
      core_.dims().scan, DimVector::POS, cursor));
  zigzag::expectWritten(
      core_.arena().link(cursor, core_.dims().grab, DimVector::POS, subject));
  return cursor;
}

void VortexStdLib::scanPop(const CellRef cursor) {
  if (cursor == noCell || !core_.arena().contains(cursor)) {
    return;
  }
  const auto prev =
      zigzag::step(core_.arena(), cursor, core_.dims().scan, DimVector::NEG);
  const auto next =
      zigzag::step(core_.arena(), cursor, core_.dims().scan, DimVector::POS);
  zigzag::expectWritten(
      core_.arena().unlink(cursor, core_.dims().scan, DimVector::NEG));
  zigzag::expectWritten(
      core_.arena().unlink(cursor, core_.dims().scan, DimVector::POS));
  // Heal the gap so the enclosing scan's cursor is reachable again.
  if (prev.has_value() && next.has_value()) {
    zigzag::expectWritten(
        core_.arena().link(*prev, core_.dims().scan, DimVector::POS, *next));
  }
}

std::optional<CellRef> VortexStdLib::scanCursor() const {
  const CellRef tail =
      zigzag::rankTail(core_.arena(), core_.home(), core_.dims().scan);
  return tail != core_.home() ? std::optional<CellRef>{tail} : std::nullopt;
}

std::optional<CellRef> VortexStdLib::scanSubject() const {
  return scanCursor().and_then(zigzag::hop(core_.arena(), core_.dims().grab));
}

std::optional<std::int64_t> VortexStdLib::scanPos() const {
  return scanCursor().and_then(
      [this](const CellRef cursor) { return core_.arena().asInt64(cursor); });
}

bool VortexStdLib::scanSetPos(const std::int64_t pos) {
  return scanCursor()
      .and_then([this, pos](const CellRef cursor) {
        // Trailed under any live mark taken inside the scan, so an
        // inner #Fail restores &pos for free (spec section 6.2).
        return core_.writeValue(cursor, 0, std::nullopt, pos);
      })
      .has_value();
}

void VortexStdLib::buildArrayModule(CellRef mod) {
  // iota: n, dim -> head
  {
    CellRef inN     = core_.arena().makeCell();
    CellRef inDim   = core_.arena().makeCell();
    CellRef outHead = core_.arena().makeCell();
    CellRef op      = vm_.mintOpcode(OpcodeKind::Nop, "#ARRAY_IOTA");
    core_.bindInput(op, inN);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inN, inDim},
                            .outputParams = {outHead}};
    exportSymbol(mod, "iota", op);
  }

  // shape: origin, dim -> shape
  {
    CellRef inOrigin = core_.arena().makeCell();
    CellRef inDim    = core_.arena().makeCell();
    CellRef outShape = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#ARRAY_SHAPE");
    core_.bindInput(op, inOrigin);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outShape);
    routineBindings_[op] = {.inputParams  = {inOrigin, inDim},
                            .outputParams = {outShape}};
    exportSymbol(mod, "shape", op);
  }

  // take: origin, dim, count -> head
  {
    CellRef inOrigin = core_.arena().makeCell();
    CellRef inDim    = core_.arena().makeCell();
    CellRef inCount  = core_.arena().makeCell();
    CellRef outHead  = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#ARRAY_TAKE");
    core_.bindInput(op, inOrigin);
    core_.bindInput(op, inDim);
    core_.bindInput(op, inCount);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inOrigin, inDim, inCount},
                            .outputParams = {outHead}};
    exportSymbol(mod, "take", op);
  }

  // drop: origin, dim, count -> head
  {
    CellRef inOrigin = core_.arena().makeCell();
    CellRef inDim    = core_.arena().makeCell();
    CellRef inCount  = core_.arena().makeCell();
    CellRef outHead  = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#ARRAY_DROP");
    core_.bindInput(op, inOrigin);
    core_.bindInput(op, inDim);
    core_.bindInput(op, inCount);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inOrigin, inDim, inCount},
                            .outputParams = {outHead}};
    exportSymbol(mod, "drop", op);
  }

  // reverse: origin, dim -> head
  {
    CellRef inOrigin = core_.arena().makeCell();
    CellRef inDim    = core_.arena().makeCell();
    CellRef outHead  = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#ARRAY_REVERSE");
    core_.bindInput(op, inOrigin);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {.inputParams  = {inOrigin, inDim},
                            .outputParams = {outHead}};
    exportSymbol(mod, "reverse", op);
  }

  // tally: origin, dim -> count
  {
    CellRef inOrigin = core_.arena().makeCell();
    CellRef inDim    = core_.arena().makeCell();
    CellRef outCount = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#ARRAY_TALLY");
    core_.bindInput(op, inOrigin);
    core_.bindInput(op, inDim);
    core_.bindOutput(op, outCount);
    routineBindings_[op] = {.inputParams  = {inOrigin, inDim},
                            .outputParams = {outCount}};
    exportSymbol(mod, "tally", op);
  }
}

CellRef VortexStdLib::makeVar(std::string_view name) {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  CellRef var = v.makeVar();
  if (!name.empty()) {
    CellRef nameCell = core_.arena().makeCell(name);
    zigzag::expectWritten(
        core_.arena().link(var, core_.dims().name, false, nameCell));
  }
  return var;
}

CellRef VortexStdLib::makeTerm(std::string_view functor,
                               std::initializer_list<CellRef> args) {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  return v.makeTerm(functor, args);
}

CellRef VortexStdLib::makeTerm(std::string_view functor,
                               std::span<const CellRef> args) {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  return v.makeTerm(functor, args);
}

CellRef VortexStdLib::makeCons(CellRef head, CellRef tail) {
  return makeTerm(".", {head, tail});
}

CellRef VortexStdLib::makeList(std::initializer_list<CellRef> elements) {
  return makeList(std::span<const CellRef>{elements.begin(), elements.end()});
}

CellRef VortexStdLib::makeList(std::span<const CellRef> elements) {
  CellRef tail = core_.arena().makeCell("[]");
  for (unsigned int element : std::views::reverse(elements)) {
    tail = makeCons(element, tail);
  }
  return tail;
}

bool VortexStdLib::isVar(CellRef cell) const {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  return v.isUnbound(cell);
}

std::optional<CellRef> VortexStdLib::deref(CellRef cell) const {
  if (!core_.arena().contains(cell)) return std::nullopt;
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  const CellRef resolved = v.deref(cell);
  if (!core_.arena().contains(resolved)) return std::nullopt;
  return resolved;
}

zigzag::UnifyResult VortexStdLib::unify(CellRef a, CellRef b) {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  return v.unify(a, b);
}

std::vector<CellRef> VortexStdLib::argumentsOf(CellRef term) const {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  return v.argumentsOf(term);
}

std::optional<std::string> VortexStdLib::functorOf(CellRef term) const {
  const auto actual = deref(term);
  if (!actual) return std::nullopt;
  return core_.arena().textOf(*actual);
}

std::optional<std::string> VortexStdLib::renderTerm(CellRef term) const {
  const auto actual = deref(term);
  if (!actual) return std::nullopt;
  if (isVar(*actual)) {
    const auto nameCell =
        core_.arena().linked(*actual, core_.dims().name, false);
    if (nameCell && core_.arena().contains(*nameCell)) {
      std::string n = core_.arena().textOf(*nameCell);
      if (!n.empty()) return n;
    }
    return "_G" + std::to_string(*actual);
  }

  const auto kind = core_.arena().valueKindOf(*actual);
  if (kind == xanadu::ValueKind::Int64) {
    auto val = core_.arena().asInt64(*actual);
    if (val) return std::to_string(*val);
  } else if (kind == xanadu::ValueKind::Double) {
    auto val = core_.arena().asDouble(*actual);
    if (val) return std::to_string(*val);
  } else if (kind == xanadu::ValueKind::Bool) {
    auto val = core_.arena().asBool(*actual);
    if (val) return *val ? "true" : "false";
  }

  std::string fn = core_.arena().textOf(*actual);
  auto args      = argumentsOf(*actual);
  if (args.empty()) {
    return fn;
  }

  if (fn == "." && args.size() >= 2) {
    std::string s              = "[";
    std::optional<CellRef> cur = actual;
    bool first                 = true;
    std::size_t limit          = core_.arena().cellCount() + 1;
    while (cur && limit-- > 0) {
      cur = deref(*cur);
      if (!cur) return std::nullopt;
      if (isVar(*cur)) {
        const auto rendered = renderTerm(*cur);
        if (!rendered) return std::nullopt;
        s += "|" + *rendered;
        break;
      }
      const auto cfn = functorOf(*cur);
      if (!cfn) return std::nullopt;
      if (*cfn == "[]") {
        break;
      }
      if (*cfn == ".") {
        auto cArgs = argumentsOf(*cur);
        if (!first) s += ", ";
        first = false;
        if (!cArgs.empty()) {
          const auto rendered = renderTerm(cArgs[0]);
          if (!rendered) return std::nullopt;
          s += *rendered;
        }
        if (cArgs.size() < 2) return std::nullopt;
        cur = cArgs[1];
      } else {
        const auto rendered = renderTerm(*cur);
        if (!rendered) return std::nullopt;
        s += "|" + *rendered;
        break;
      }
    }
    s += "]";
    return s;
  }

  std::string s = fn + "(";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) s += ", ";
    const auto rendered = renderTerm(args[i]);
    if (!rendered) return std::nullopt;
    s += *rendered;
  }
  s += ")";
  return s;
}

CellRef VortexStdLib::createPredicate(std::string_view name) {
  return core_.arena().makeCell(name);
}

CellRef VortexStdLib::addClause(CellRef predCell, CellRef headTerm,
                                std::span<const CellRef> bodyGoals) {
  auto &arena              = core_.arena();
  const CellRef clauseCell = arena.makeCell();
  zigzag::expectWritten(
      arena.link(clauseCell, core_.dims().grab, false, headTerm));

  // The body hangs off the clause cell as one d.spin rank.
  std::vector<CellRef> spine{clauseCell};
  spine.insert(spine.end(), bodyGoals.begin(), bodyGoals.end());
  chainCells(arena, spine, core_.dims().spin);

  zigzag::expectWritten(
      arena.link(rankTail(arena, predCell, core_.dims().clause),
                 core_.dims().clause, false, clauseCell));
  return clauseCell;
}

bool VortexStdLib::solveOnce(CellRef goal,
                             std::span<const CellRef> customPredicates) {
  return solveOnce(std::span<const CellRef>{&goal, 1}, customPredicates);
}

bool VortexStdLib::solveOnce(std::span<const CellRef> goals,
                             std::span<const CellRef> customPredicates) {
  bool found = false;
  solve(
      goals,
      [&](const LogicSolution &) {
        found = true;
        return false;
      },
      customPredicates, 1);
  return found;
}

std::vector<LogicSolution>
VortexStdLib::solveQuery(CellRef goal,
                         std::span<const CellRef> customPredicates,
                         std::size_t maxSolutions) {
  return solveQuery(std::span<const CellRef>{&goal, 1}, customPredicates,
                    maxSolutions);
}

std::vector<LogicSolution>
VortexStdLib::solveQuery(std::span<const CellRef> goals,
                         std::span<const CellRef> customPredicates,
                         std::size_t maxSolutions) {
  std::vector<LogicSolution> solutions;
  solve(
      goals,
      [&](const LogicSolution &sol) {
        solutions.push_back(sol);
        return true;
      },
      customPredicates, maxSolutions);
  return solutions;
}

bool VortexStdLib::solve(CellRef goal,
                         std::function<bool(const LogicSolution &)> onSolution,
                         std::span<const CellRef> customPredicates,
                         std::size_t maxSolutions) {
  return solve(std::span<const CellRef>{&goal, 1}, std::move(onSolution),
               customPredicates, maxSolutions);
}

bool VortexStdLib::solve(std::span<const CellRef> goals,
                         std::function<bool(const LogicSolution &)> onSolution,
                         std::span<const CellRef> customPredicates,
                         std::size_t maxSolutions) {
  if (goals.empty()) return true;

  std::vector<CellRef> candidatePreds;
  candidatePreds.push_back(predEqual_);
  candidatePreds.push_back(predUnify_);
  candidatePreds.push_back(predMember_);
  candidatePreds.push_back(predAppend_);
  candidatePreds.push_back(predLength_);
  candidatePreds.push_back(predVortexFunction_);
  candidatePreds.push_back(predVortexModule_);
  candidatePreds.push_back(predVortexInstruction_);
  candidatePreds.push_back(predVortexContract_);
  candidatePreds.push_back(predVortexParam_);
  candidatePreds.push_back(predSetting_);
  candidatePreds.push_back(predSettingShape_);
  candidatePreds.push_back(predSettingDefault_);
  candidatePreds.push_back(predCellValue_);
  candidatePreds.push_back(predCellLink_);
  candidatePreds.push_back(predTransclude_);
  candidatePreds.push_back(predXanalink_);
  candidatePreds.push_back(predCellSpan_);
  candidatePreds.push_back(predBridgeEdge_);
  for (CellRef cp : customPredicates) {
    candidatePreds.push_back(cp);
  }

  std::vector<CellRef> queryVars;
  std::unordered_set<CellRef> seen;
  for (CellRef g : goals) {
    collectVariables(*this, g, queryVars, seen);
  }

  std::size_t solCount            = 0;
  const std::size_t topLevelFrame = 1;
  std::size_t nextFrameId         = 2;
  std::size_t cutToFrame          = 0;
  std::vector<ActiveGoal> activeGoals;
  activeGoals.reserve(goals.size());
  for (CellRef g : goals) {
    activeGoals.push_back(ActiveGoal{.term = g, .cutFrame = topLevelFrame});
  }
  return solveQueryHelper(*this, core_, std::move(activeGoals), candidatePreds,
                          queryVars, std::move(onSolution), solCount,
                          maxSolutions, 0, nextFrameId, cutToFrame);
}

// -- Module 9: sys:array ----------------------------------------------------
std::optional<CellRef> VortexStdLib::arrayIota(std::size_t n,
                                               std::optional<DimRef> dim,
                                               std::optional<CellRef> origin) {
  if (origin && !core_.arena().contains(*origin)) {
    throw std::invalid_argument("arrayIota origin is not in the arena");
  }
  if (n == 0) {
    return std::nullopt;
  }
  DimRef linkDim = dim.value_or(core_.dims().step);
  CellRef head{};
  if (origin) {
    head = *origin;
  } else {
    head = core_.arena().makeScalarCell(static_cast<std::int64_t>(1));
  }
  CellRef prev = head;
  for (std::size_t i = 1; i < n; ++i) {
    CellRef next =
        core_.arena().makeScalarCell(static_cast<std::int64_t>(i + 1));
    zigzag::expectWritten(
        core_.arena().link(prev, linkDim, DimVector::POS, next));
    prev = next;
  }
  return head;
}

std::optional<std::vector<std::size_t>>
VortexStdLib::arrayShape(CellRef origin, std::span<const DimRef> dims) const {
  if (origin == noCell || !core_.arena().contains(origin)) {
    return std::nullopt;
  }
  if (dims.empty()) {
    const auto tally = arrayTally(origin, core_.dims().step);
    if (!tally) return std::nullopt;
    return std::vector<std::size_t>{*tally};
  }
  const auto held = [this](const CellRef cell) {
    return core_.arena().contains(cell);
  };
  return dims | std::views::transform([&](const DimRef d) {
           return static_cast<std::size_t>(std::ranges::distance(
               rank(core_.arena(), origin, d) | std::views::take_while(held)));
         }) |
         std::ranges::to<std::vector>();
}

std::optional<CellRef> VortexStdLib::arrayTake(CellRef origin, DimRef dim,
                                               std::size_t count) {
  if (!core_.arena().contains(origin)) {
    throw std::invalid_argument("arrayTake origin is not in the arena");
  }
  if (count == 0) {
    return std::nullopt;
  }
  const DimRef linkDim = listDim(dim);
  return chainCells(core_.arena(),
                    rank(core_.arena(), origin, linkDim) |
                        std::views::take(count) |
                        std::views::transform([this](const CellRef cell) {
                          return copyValueCell(cell);
                        }) |
                        std::ranges::to<std::vector>(),
                    linkDim);
}

std::optional<CellRef> VortexStdLib::arrayDrop(CellRef origin, DimRef dim,
                                               std::size_t count) {
  if (!core_.arena().contains(origin)) {
    throw std::invalid_argument("arrayDrop origin is not in the arena");
  }
  return hops(core_.arena(), origin, listDim(dim), count);
}

std::optional<CellRef> VortexStdLib::arrayReverse(CellRef origin, DimRef dim) {
  if (!core_.arena().contains(origin)) {
    throw std::invalid_argument("arrayReverse origin is not in the arena");
  }
  const DimRef linkDim = listDim(dim);
  // Collected first: a rank is a forward range, and reverse needs both ends.
  const auto cells =
      rank(core_.arena(), origin, linkDim) | std::ranges::to<std::vector>();
  return chainCells(core_.arena(),
                    cells | std::views::reverse |
                        std::views::transform([this](const CellRef cell) {
                          return copyValueCell(cell);
                        }) |
                        std::ranges::to<std::vector>(),
                    linkDim);
}

std::optional<std::size_t> VortexStdLib::arrayTally(CellRef origin,
                                                    DimRef dim) const {
  if (origin == noCell || !core_.arena().contains(origin)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(
      std::ranges::distance(rank(core_.arena(), origin, listDim(dim))));
}

void VortexStdLib::buildZigzagModule(CellRef mod) {
  // step: cursor, dim, dir -> next
  {
    CellRef cursor = core_.arena().makeCell();
    CellRef dim    = core_.arena().makeCell();
    CellRef dir    = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_STEP");
    core_.bindInput(op, cursor);
    core_.bindInput(op, dim);
    core_.bindInput(op, dir);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {cursor, dim, dir},
                            .outputParams = {out}};
    exportSymbol(mod, "step", op);
  }

  // insert: cursor, dim, dir, text -> fresh
  {
    CellRef cursor = core_.arena().makeCell();
    CellRef dim    = core_.arena().makeCell();
    CellRef dir    = core_.arena().makeCell();
    CellRef text   = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_INSERT");
    core_.bindInput(op, cursor);
    core_.bindInput(op, dim);
    core_.bindInput(op, dir);
    core_.bindInput(op, text);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {cursor, dim, dir, text},
                            .outputParams = {out}};
    exportSymbol(mod, "insert", op);
  }

  // unlink: cursor, dim, dir -> old
  {
    CellRef cursor = core_.arena().makeCell();
    CellRef dim    = core_.arena().makeCell();
    CellRef dir    = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_UNLINK");
    core_.bindInput(op, cursor);
    core_.bindInput(op, dim);
    core_.bindInput(op, dir);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {cursor, dim, dir},
                            .outputParams = {out}};
    exportSymbol(mod, "unlink", op);
  }

  // link: cellA, cellB, dim, dir -> cellB
  {
    CellRef cellA = core_.arena().makeCell();
    CellRef cellB = core_.arena().makeCell();
    CellRef dim   = core_.arena().makeCell();
    CellRef dir   = core_.arena().makeCell();
    CellRef out   = core_.arena().makeCell();
    CellRef op    = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_LINK");
    core_.bindInput(op, cellA);
    core_.bindInput(op, cellB);
    core_.bindInput(op, dim);
    core_.bindInput(op, dir);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {cellA, cellB, dim, dir},
                            .outputParams = {out}};
    exportSymbol(mod, "link", op);
  }

  // delete: cell -> cell
  {
    CellRef cell = core_.arena().makeCell();
    CellRef out  = core_.arena().makeCell();
    CellRef op   = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_DELETE");
    core_.bindInput(op, cell);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {cell}, .outputParams = {out}};
    exportSymbol(mod, "delete", op);
  }

  // clone_to_chain: symbolOp, targetCell -> clone
  {
    CellRef sym = core_.arena().makeCell();
    CellRef tgt = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_CLONE_CHAIN");
    core_.bindInput(op, sym);
    core_.bindInput(op, tgt);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {sym, tgt}, .outputParams = {out}};
    exportSymbol(mod, "clone_to_chain", op);
  }

  // duplicate: cell -> clone (#ZZ_DUPLICATE)
  {
    CellRef cell = core_.arena().makeCell();
    CellRef out  = core_.arena().makeCell();
    CellRef op   = vm_.mintOpcode(OpcodeKind::Nop, "#ZZ_DUPLICATE");
    core_.bindInput(op, cell);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {cell}, .outputParams = {out}};
    exportSymbol(mod, "duplicate", op);
  }

  constexpr std::pair<std::string_view, std::string_view> kExtraZzActions[] = {
      {"rasterize_print", "#ZZ_RASTERIZE_PRINT"},
      {"export_link_package", "#ZZ_EXPORT_LINK_PACKAGE"},
      {"insert_cell_x_pos", "#ZZ_INSERT_CELL_X_POS"},
      {"insert_cell_x_neg", "#ZZ_INSERT_CELL_X_NEG"},
      {"insert_cell_y_pos", "#ZZ_INSERT_CELL_Y_POS"},
      {"insert_cell_y_neg", "#ZZ_INSERT_CELL_Y_NEG"},
      {"unlink_x_pos", "#ZZ_UNLINK_X_POS"},
      {"unlink_x_neg", "#ZZ_UNLINK_X_NEG"},
      {"delete_focus_cell", "#ZZ_DELETE_FOCUS_CELL"},
      {"delete_focus_cell_bksp", "#ZZ_DELETE_FOCUS_CELL_BKSP"},
      {"save_store", "#ZZ_SAVE_STORE"},
      {"zigzag_duplicate_cell", "#ZZ_ZZ_DUPLICATE_CELL"},
      {"zigzag_save_store", "#ZZ_ZZ_SAVE_STORE"},
  };
  for (const auto &[name, opcode] : kExtraZzActions) {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, opcode);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, name, op);
  }
}

void VortexStdLib::buildGCModule(CellRef mod) {
  // sweep: -> reclaimedCount
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#GC_SWEEP");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "sweep", op);
  }
}

void VortexStdLib::buildUiModule(CellRef mod) {
  // view: dimX, dimY, dimZ -> res (#UI_VIEW)
  {
    CellRef dimX = core_.arena().makeCell();
    CellRef dimY = core_.arena().makeCell();
    CellRef dimZ = core_.arena().makeCell();
    CellRef out  = core_.arena().makeCell();
    CellRef op   = vm_.mintOpcode(OpcodeKind::Nop, "#UI_VIEW");
    core_.bindInput(op, dimX);
    core_.bindInput(op, dimY);
    core_.bindInput(op, dimZ);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {dimX, dimY, dimZ},
                            .outputParams = {out}};
    exportSymbol(mod, "view", op);
  }

  // swap_axes: #UI_SWAP_AXES
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_SWAP_AXES");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "swap_axes", op);
  }

  // cycle_dims_forward: #UI_CYCLE_DIMS_FORWARD
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_CYCLE_DIMS_FORWARD");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "cycle_dims_forward", op);
  }

  // cycle_dims_backward: #UI_CYCLE_DIMS_BACKWARD
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_CYCLE_DIMS_BACKWARD");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "cycle_dims_backward", op);
  }

  // bundle_execution: #UI_BUNDLE_EXECUTION
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_BUNDLE_EXECUTION");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "bundle_execution", op);
  }

  // bundle_scope: #UI_BUNDLE_SCOPE
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_BUNDLE_SCOPE");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "bundle_scope", op);
  }

  // bundle_contract: #UI_BUNDLE_CONTRACT
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_BUNDLE_CONTRACT");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "bundle_contract", op);
  }

  // bundle_logic: #UI_BUNDLE_LOGIC
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_BUNDLE_LOGIC");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "bundle_logic", op);
  }

  // bundle_stdlib: #UI_BUNDLE_STDLIB
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#UI_BUNDLE_STDLIB");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "bundle_stdlib", op);
  }

  constexpr std::pair<std::string_view, std::string_view> kExtraUiActions[] = {
      {"bundle_cycle", "#UI_BUNDLE_CYCLE"},
      {"swap_xy", "#UI_SWAP_XY"},
      {"toggle_palette", "#UI_TOGGLE_PALETTE"},
      {"vql_translate_attach", "#UI_VQL_TRANSLATE_ATTACH"},
      {"toggle_command_bar", "#UI_TOGGLE_COMMAND_BAR"},
      {"open_command_bar_slash", "#UI_OPEN_COMMAND_BAR_SLASH"},
      {"open_command_bar_colon", "#UI_OPEN_COMMAND_BAR_COLON"},
      {"confirm_action", "#UI_CONFIRM_ACTION"},
      {"dismiss_overlay", "#UI_DISMISS_OVERLAY"},
      {"view_mode_content_1", "#UI_VIEW_MODE_CONTENT_1"},
      {"view_mode_content_v", "#UI_VIEW_MODE_CONTENT_V"},
      {"view_mode_topology", "#UI_VIEW_MODE_TOPOLOGY"},
      {"view_mode_topology_t", "#UI_VIEW_MODE_TOPOLOGY_T"},
      {"zigzag_toggle_palette", "#UI_ZZ_TOGGLE_PALETTE"},
      {"zigzag_vql_translate_attach", "#UI_ZZ_VQL_TRANSLATE_ATTACH"},
      {"zigzag_toggle_command_bar", "#UI_ZZ_TOGGLE_COMMAND_BAR"},
      {"zigzag_open_command_bar_slash", "#UI_ZZ_OPEN_COMMAND_BAR_SLASH"},
      {"zigzag_open_command_bar_colon", "#UI_ZZ_OPEN_COMMAND_BAR_COLON"},
      {"zigzag_view_mode_content", "#UI_ZZ_VIEW_MODE_CONTENT"},
      {"zigzag_view_mode_topology", "#UI_ZZ_VIEW_MODE_TOPOLOGY"},
      {"zigzag_bundle_execution", "#UI_ZZ_BUNDLE_EXECUTION"},
      {"zigzag_bundle_scope", "#UI_ZZ_BUNDLE_SCOPE"},
      {"zigzag_bundle_contract", "#UI_ZZ_BUNDLE_CONTRACT"},
      {"zigzag_bundle_logic", "#UI_ZZ_BUNDLE_LOGIC"},
      {"zigzag_bundle_stdlib", "#UI_ZZ_BUNDLE_STDLIB"},
      {"zigzag_bundle_cycle", "#UI_ZZ_BUNDLE_CYCLE"},
      {"zigzag_swap_xy", "#UI_ZZ_SWAP_XY"},
      {"zigzag_cycle_dims_forward", "#UI_ZZ_CYCLE_DIMS_FORWARD"},
      {"zigzag_cycle_dims_backward", "#UI_ZZ_CYCLE_DIMS_BACKWARD"},
  };
  for (const auto &[name, opcode] : kExtraUiActions) {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, opcode);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, name, op);
  }
}

void VortexStdLib::buildNavModule(CellRef mod) {
  // hop_head: cursor, dim -> head (#NAV_HOP_HEAD)
  {
    CellRef cursor = core_.arena().makeCell();
    CellRef dim    = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#NAV_HOP_HEAD");
    core_.bindInput(op, cursor);
    core_.bindInput(op, dim);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {cursor, dim},
                            .outputParams = {out}};
    exportSymbol(mod, "hop_head", op);
  }

  // hop_tail: cursor, dim -> tail (#NAV_HOP_TAIL)
  {
    CellRef cursor = core_.arena().makeCell();
    CellRef dim    = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#NAV_HOP_TAIL");
    core_.bindInput(op, cursor);
    core_.bindInput(op, dim);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {cursor, dim},
                            .outputParams = {out}};
    exportSymbol(mod, "hop_tail", op);
  }

  // jump_home: -> home (#NAV_JUMP_HOME)
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#NAV_JUMP_HOME");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "jump_home", op);
  }

  constexpr std::pair<std::string_view, std::string_view> kExtraNavActions[] = {
      {"step_x_pos", "#NAV_STEP_X_POS"},
      {"step_x_neg", "#NAV_STEP_X_NEG"},
      {"step_y_pos", "#NAV_STEP_Y_POS"},
      {"step_y_neg", "#NAV_STEP_Y_NEG"},
      {"step_z_pos", "#NAV_STEP_Z_POS"},
      {"step_z_neg", "#NAV_STEP_Z_NEG"},
      {"zigzag_jump_home", "#NAV_ZZ_JUMP_HOME"},
      {"zigzag_hop_head", "#NAV_ZZ_HOP_HEAD"},
      {"zigzag_hop_tail", "#NAV_ZZ_HOP_TAIL"},
      {"zigzag_step_x_pos", "#NAV_ZZ_STEP_X_POS"},
      {"zigzag_step_x_neg", "#NAV_ZZ_STEP_X_NEG"},
      {"zigzag_step_y_pos", "#NAV_ZZ_STEP_Y_POS"},
      {"zigzag_step_y_neg", "#NAV_ZZ_STEP_Y_NEG"},
      {"zigzag_step_z_pos", "#NAV_ZZ_STEP_Z_POS"},
      {"zigzag_step_z_neg", "#NAV_ZZ_STEP_Z_NEG"},
  };
  for (const auto &[name, opcode] : kExtraNavActions) {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, opcode);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, name, op);
  }
}

void VortexStdLib::buildBridgeModule(CellRef mod) {
  // doc_text: -> text (#BRIDGE_DOC_TEXT)
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#BRIDGE_DOC_TEXT");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "doc_text", op);
  }

  // store_version: -> ver (#BRIDGE_STORE_VERSION)
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, "#BRIDGE_STORE_VERSION");
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, "store_version", op);
  }

  // cell_to_doc: cell, offset -> status (#BRIDGE_CELL_TO_DOC)
  {
    CellRef inCell = core_.arena().makeCell();
    CellRef inOff  = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#BRIDGE_CELL_TO_DOC");
    core_.bindInput(op, inCell);
    core_.bindInput(op, inOff);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {inCell, inOff},
                            .outputParams = {out}};
    exportSymbol(mod, "cell_to_doc", op);
  }

  // doc_to_cell: offset, length -> cell (#BRIDGE_DOC_TO_CELL)
  {
    CellRef inOff = core_.arena().makeCell();
    CellRef inLen = core_.arena().makeCell();
    CellRef out   = core_.arena().makeCell();
    CellRef op    = vm_.mintOpcode(OpcodeKind::Nop, "#BRIDGE_DOC_TO_CELL");
    core_.bindInput(op, inOff);
    core_.bindInput(op, inLen);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams  = {inOff, inLen},
                            .outputParams = {out}};
    exportSymbol(mod, "doc_to_cell", op);
  }

  // cell_royalty: cell -> price (#BRIDGE_CELL_ROYALTY)
  {
    CellRef inCell = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#BRIDGE_CELL_ROYALTY");
    core_.bindInput(op, inCell);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {inCell}, .outputParams = {out}};
    exportSymbol(mod, "cell_royalty", op);
  }

  // cell_unlock: cell -> status (#BRIDGE_CELL_UNLOCK)
  {
    CellRef inCell = core_.arena().makeCell();
    CellRef out    = core_.arena().makeCell();
    CellRef op     = vm_.mintOpcode(OpcodeKind::Nop, "#BRIDGE_CELL_UNLOCK");
    core_.bindInput(op, inCell);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {inCell}, .outputParams = {out}};
    exportSymbol(mod, "cell_unlock", op);
  }
}

std::optional<CellRef> VortexStdLib::zzStep(std::optional<CellRef> cursor,
                                            DimRef dim, DimVector dir) {
  if (!cursor || *cursor == noCell || !core_.arena().contains(*cursor)) {
    return std::nullopt;
  }
  auto res = core_.readLink(*cursor, dim, dir);
  return res;
}

std::optional<CellRef> VortexStdLib::zzInsert(CellRef cursor, DimRef dim,
                                              DimVector dir,
                                              std::string_view text) {
  if (cursor == noCell || !core_.arena().contains(cursor)) {
    return std::nullopt;
  }
  return core_.newCell(cursor, dim, dir, std::string(text));
}

std::optional<CellRef> VortexStdLib::zzUnlink(CellRef cursor, DimRef dim,
                                              DimVector dir) {
  if (cursor == noCell || !core_.arena().contains(cursor)) {
    return std::nullopt;
  }
  return core_.breakLink(cursor, dim, dir);
}

std::optional<CellRef> VortexStdLib::zzLink(CellRef cellA, CellRef cellB,
                                            DimRef dim, DimVector dir) {
  if (cellA == noCell || !core_.arena().contains(cellA) || cellB == noCell ||
      !core_.arena().contains(cellB)) {
    return std::nullopt;
  }
  return core_.setLink(cellA, dim, dir, cellB);
}

std::optional<CellRef> VortexStdLib::zzDelete(CellRef cell) {
  if (cell == noCell || !core_.arena().contains(cell)) {
    return std::nullopt;
  }
  auto dims = core_.arena().dimensionsOf(cell);
  for (const auto &dLink : dims) {
    if (dLink.pos != noCell) {
      core_.breakLink(cell, dLink.dim, DimVector::POS);
    }
    if (dLink.neg != noCell) {
      core_.breakLink(cell, dLink.dim, DimVector::NEG);
    }
  }
  return cell;
}

std::optional<CellRef> VortexStdLib::zzCloneToChain(CellRef symbolOp,
                                                    CellRef targetCell) {
  if (symbolOp == noCell || !core_.arena().contains(symbolOp) ||
      targetCell == noCell || !core_.arena().contains(targetCell)) {
    return std::nullopt;
  }
  CellRef clone = core_.arena().makeCell();
  zigzag::expectWritten(
      core_.arena().link(symbolOp, core_.dims().clone, DimVector::POS, clone));
  zigzag::expectWritten(
      core_.arena().link(targetCell, core_.dims().spin, DimVector::POS, clone));
  return clone;
}

std::size_t VortexStdLib::gcSweep() { return core_.collectGarbage(); }

std::optional<CellRef> VortexStdLib::zzDuplicate(CellRef cell) {
  if (cell == noCell || !core_.arena().contains(cell)) {
    return std::nullopt;
  }
  std::string text = core_.arena().textOf(cell);
  CellRef dup      = core_.arena().makeCell(text);
  auto vk          = core_.arena().valueKindOf(cell);
  if (vk != xanadu::ValueKind::None) {
    if (auto d = core_.arena().asDouble(cell)) {
      zigzag::expectWritten(core_.arena().setValueBits(
          dup, vk, std::bit_cast<std::uint64_t>(*d)));
    } else if (auto i = core_.arena().asInt64(cell)) {
      zigzag::expectWritten(
          core_.arena().setValueBits(dup, vk, static_cast<std::uint64_t>(*i)));
    } else if (auto b = core_.arena().asBool(cell)) {
      zigzag::expectWritten(
          core_.arena().setValueBits(dup, vk, *b ? 1ULL : 0ULL));
    }
  }
  core_.setLink(cell, core_.dims().clone, DimVector::POS, dup);
  return dup;
}

void VortexStdLib::swapAxes(ViewAxisBinding &axes) {
  std::swap(axes.x_dimension, axes.y_dimension);
}

void VortexStdLib::cycleDims(ViewAxisBinding &axes, const bool forward) {
  if (forward) {
    const auto tmp   = axes.x_dimension;
    axes.x_dimension = axes.y_dimension;
    axes.y_dimension = axes.z_dimension;
    axes.z_dimension = tmp;
  } else {
    const auto tmp   = axes.z_dimension;
    axes.z_dimension = axes.y_dimension;
    axes.y_dimension = axes.x_dimension;
    axes.x_dimension = tmp;
  }
}

void VortexStdLib::applyBundle(ViewAxisBinding &axes, DimensionBundle bundle) {
  if (bundle != DimensionBundle::Custom) {
    axes = dimensionBundleAxes(bundle);
  }
}

void VortexStdLib::setView(ViewAxisBinding &axes, std::string_view dimX,
                           std::string_view dimY, std::string_view dimZ) {
  if (!dimX.empty()) {
    axes.x_dimension = std::string(dimX);
  }
  if (!dimY.empty()) {
    axes.y_dimension = std::string(dimY);
  }
  if (!dimZ.empty()) {
    axes.z_dimension = std::string(dimZ);
  }
}

std::optional<CellRef> VortexStdLib::hopHead(CellRef cursor, DimRef dim) {
  if (cursor == noCell || !core_.arena().contains(cursor) || dim == noCell) {
    return std::nullopt;
  }
  return rankTail(core_.arena(), cursor, dim, DimVector::NEG);
}

std::optional<CellRef> VortexStdLib::hopTail(CellRef cursor, DimRef dim) {
  if (cursor == noCell || !core_.arena().contains(cursor) || dim == noCell) {
    return std::nullopt;
  }
  return rankTail(core_.arena(), cursor, dim, DimVector::POS);
}

CellRef VortexStdLib::jumpHome() const noexcept { return core_.home(); }

bool VortexStdLib::exportModuleToStore(std::string_view modulePath,
                                       xanadu::Store &destStore) const {
  const auto resolved = resolve(modulePath);
  if (!resolved) {
    return false;
  }
  const CellRef mod = *resolved;
  auto parent       = destStore.allVersions().empty()
                          ? xanadu::MicroversionId{}
                          : destStore.primaryCurrentVersion();
  zigzag::PromotionBudget budget{.maxOps = 100000};
  auto promoted =
      zigzag::promote(destStore, parent, core_.arena(), mod, budget);
  if (!promoted) {
    return false;
  }
  destStore.repointCurrentVersion(promoted->version);
  return true;
}

bool VortexStdLib::exportStandardLibraryToStore(
    xanadu::Store &destStore) const {
  const CellRef stdlibRoot =
      core_.arena()
          .linked(core_.home(), core_.dims().stdlib, DimVector::POS)
          .value_or(core_.home());
  auto parent = destStore.allVersions().empty()
                    ? xanadu::MicroversionId{}
                    : destStore.primaryCurrentVersion();
  zigzag::PromotionBudget budget{.maxOps = 200000};
  auto promoted =
      zigzag::promote(destStore, parent, core_.arena(), stdlibRoot, budget);
  if (!promoted) {
    return false;
  }
  destStore.repointCurrentVersion(promoted->version);
  return true;
}

std::optional<CellRef>
VortexStdLib::importModuleFromStore(const xanadu::Store &srcStore) {
  auto ver         = srcStore.allVersions().empty() ? xanadu::MicroversionId{}
                                                    : srcStore.primaryCurrentVersion();
  auto srcManifold = srcStore.rebuildManifold(ver);
  if (srcManifold.cellCount() == 0) {
    return std::nullopt;
  }

  std::unordered_map<CellRef, CellRef> cellMapping;
  for (const auto &slot : srcManifold.cells()) {
    CellRef srcRef = slot.birthOp;
    if (srcRef == noCell) {
      continue;
    }
    std::string text = srcManifold.textOf(srcRef, srcStore);
    CellRef dstRef   = core_.arena().makeCell(text);
    if (slot.valueKind != 0) {
      zigzag::expectWritten(core_.arena().setValueBits(
          dstRef, static_cast<xanadu::ValueKind>(slot.valueKind),
          slot.valueBits));
    }
    cellMapping[srcRef] = dstRef;
  }

  for (const auto &slot : srcManifold.cells()) {
    CellRef srcRef = slot.birthOp;
    if (!cellMapping.contains(srcRef)) {
      continue;
    }
    CellRef dstRef = cellMapping[srcRef];
    for (const auto &link : srcManifold.dimensionsOf(srcRef)) {
      if (!cellMapping.contains(link.dim)) {
        continue;
      }
      DimRef dstDim = cellMapping[link.dim];
      if (link.pos != noCell && cellMapping.contains(link.pos)) {
        zigzag::expectWritten(core_.arena().link(dstRef, dstDim, DimVector::POS,
                                                 cellMapping[link.pos]));
      }
      if (link.neg != noCell && cellMapping.contains(link.neg)) {
        zigzag::expectWritten(core_.arena().link(dstRef, dstDim, DimVector::NEG,
                                                 cellMapping[link.neg]));
      }
    }
  }

  std::optional<CellRef> firstModuleCell;
  for (const auto &[srcRef, dstRef] : cellMapping) {
    std::string text = core_.arena().textOf(dstRef);
    if (text.starts_with("std:") && !text.contains('/')) {
      if (!firstModuleCell) {
        firstModuleCell = dstRef;
      }
      CellRef tail = core_.home();
      auto next =
          core_.arena().linked(tail, core_.dims().stdlib, DimVector::POS);
      while (next) {
        tail = *next;
        if (tail == dstRef) {
          break;
        }
        next = core_.arena().linked(tail, core_.dims().stdlib, DimVector::POS);
      }
      if (tail != dstRef) {
        zigzag::expectWritten(core_.arena().link(tail, core_.dims().stdlib,
                                                 DimVector::POS, dstRef));
      }
    }
  }
  if (firstModuleCell) {
    return firstModuleCell;
  }
  if (cellMapping.empty()) {
    return std::nullopt;
  }
  return cellMapping.begin()->second;
}

CellRef VortexStdLib::bridgeDocText(const xanadu::Store &store) {
  auto ver        = store.allVersions().empty() ? xanadu::MicroversionId{}
                                                : store.primaryCurrentVersion();
  std::string txt = store.textOf(ver);
  return core_.arena().makeCell(txt);
}

CellRef VortexStdLib::bridgeStoreVersion(const xanadu::Store &store) {
  auto ver = store.allVersions().empty() ? xanadu::MicroversionId{}
                                         : store.primaryCurrentVersion();
  return core_.arena().makeCell(ver.str());
}

bool VortexStdLib::bridgeCellToDoc(xanadu::Store &store, CellRef cell,
                                   std::uint32_t docOffset) {
  if (cell == noCell || !core_.arena().contains(cell)) {
    return false;
  }
  std::string text = core_.arena().textOf(cell);
  auto parent      = store.allVersions().empty() ? xanadu::MicroversionId{}
                                                 : store.primaryCurrentVersion();
  auto newVer      = store.insert(parent, docOffset, text);
  store.repointCurrentVersion(newVer);
  return true;
}

CellRef VortexStdLib::bridgeDocToCell(xanadu::Store &store,
                                      std::uint32_t docOffset,
                                      std::uint32_t length) {
  auto ver            = store.allVersions().empty() ? xanadu::MicroversionId{}
                                                    : store.primaryCurrentVersion();
  std::string docText = store.textOf(ver);
  if (docOffset >= docText.size()) {
    return core_.arena().makeCell("");
  }
  std::string sub = docText.substr(docOffset, length);
  return core_.arena().makeCell(sub);
}

std::optional<xanadu::TranscopyrightDescriptor> VortexStdLib::bridgeCellRoyalty(
    const xanadu::ZigzagPresentationSurface &surface, const CellRef cell) {
  return surface.cellRoyalty(cell);
}

bool VortexStdLib::bridgeCellUnlock(xanadu::ZigzagPresentationSurface &surface,
                                    const CellRef cell) {
  return surface.unlockCell(cell);
}

void VortexStdLib::buildXuzzModule(CellRef mod) {
  constexpr std::pair<std::string_view, std::string_view> kXuzzActions[] = {
      {"quit", "#XUZZ_QUIT"},
      {"save", "#XUZZ_SAVE"},
      {"close", "#XUZZ_CLOSE"},
      {"next_doc", "#XUZZ_NEXT_DOC"},
      {"prev_doc", "#XUZZ_PREV_DOC"},
      {"doc_1", "#XUZZ_DOC_1"},
      {"doc_2", "#XUZZ_DOC_2"},
      {"doc_3", "#XUZZ_DOC_3"},
      {"doc_4", "#XUZZ_DOC_4"},
      {"doc_5", "#XUZZ_DOC_5"},
      {"doc_6", "#XUZZ_DOC_6"},
      {"doc_7", "#XUZZ_DOC_7"},
      {"doc_8", "#XUZZ_DOC_8"},
      {"doc_9", "#XUZZ_DOC_9"},
      {"back", "#XUZZ_BACK"},
      {"new_doc", "#XUZZ_NEW_DOC"},
      {"forward", "#XUZZ_FORWARD"},
      {"open_doc", "#XUZZ_OPEN_DOC"},
      {"close_doc", "#XUZZ_CLOSE_DOC"},
      {"onion_skin", "#XUZZ_ONION_SKIN"},
      {"pouch_toggle", "#XUZZ_POUCH_TOGGLE"},
      {"pouch_toggle_f2", "#XUZZ_POUCH_TOGGLE_F2"},
      {"telescope_toggle", "#XUZZ_TELESCOPE_TOGGLE"},
      {"telescope_toggle_f3", "#XUZZ_TELESCOPE_TOGGLE_F3"},
      {"tension_physics_toggle", "#XUZZ_TENSION_PHYSICS_TOGGLE"},
      {"unlock_transcopyright", "#XUZZ_UNLOCK_TRANSCOPYRIGHT"},
      {"unlock_transcopyright_f5", "#XUZZ_UNLOCK_TRANSCOPYRIGHT_F5"},
      {"unlock_transcopyright_ctrl_u", "#XUZZ_UNLOCK_TRANSCOPYRIGHT_CTRL_U"},
      {"scrub_forward", "#XUZZ_SCRUB_FORWARD"},
      {"scrub_backward", "#XUZZ_SCRUB_BACKWARD"},
      {"transclude", "#XUZZ_TRANSCLUDE"},
      {"xanalink", "#XUZZ_XANALINK"},
      {"cancel_link", "#XUZZ_CANCEL_LINK"},
      {"beams", "#XUZZ_BEAMS"},
      {"sworph", "#XUZZ_SWORPH"},
      {"publish", "#XUZZ_PUBLISH"},
      {"history", "#XUZZ_HISTORY"},
      {"delete", "#XUZZ_DELETE"},
      {"page_break", "#XUZZ_PAGE_BREAK"},
      {"hypertime_map", "#XUZZ_HYPERTIME_MAP"},
      {"map", "#XUZZ_MAP"},
      {"scrub_back", "#XUZZ_SCRUB_BACK"},
      {"radial_menu", "#XUZZ_RADIAL_MENU"},
  };

  for (const auto &[name, opcode] : kXuzzActions) {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, opcode);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, name, op);
  }
}

void VortexStdLib::buildXuduModule(CellRef mod) {
  constexpr std::pair<std::string_view, std::string_view> kXuduActions[] = {
      {"quit", "#XUDU_QUIT"},
      {"save", "#XUDU_SAVE"},
      {"close", "#XUDU_CLOSE"},
      {"next_doc", "#XUDU_NEXT_DOC"},
      {"prev_doc", "#XUDU_PREV_DOC"},
      {"doc_1", "#XUDU_DOC_1"},
      {"doc_2", "#XUDU_DOC_2"},
      {"doc_3", "#XUDU_DOC_3"},
      {"doc_4", "#XUDU_DOC_4"},
      {"doc_5", "#XUDU_DOC_5"},
      {"doc_6", "#XUDU_DOC_6"},
      {"doc_7", "#XUDU_DOC_7"},
      {"doc_8", "#XUDU_DOC_8"},
      {"doc_9", "#XUDU_DOC_9"},
      {"back", "#XUDU_BACK"},
      {"new_doc", "#XUDU_NEW_DOC"},
      {"forward", "#XUDU_FORWARD"},
      {"open_doc", "#XUDU_OPEN_DOC"},
      {"close_doc", "#XUDU_CLOSE_DOC"},
      {"onion_skin", "#XUDU_ONION_SKIN"},
      {"pouch_toggle", "#XUDU_POUCH_TOGGLE"},
      {"pouch_toggle_f2", "#XUDU_POUCH_TOGGLE_F2"},
      {"telescope_toggle", "#XUDU_TELESCOPE_TOGGLE"},
      {"telescope_toggle_f3", "#XUDU_TELESCOPE_TOGGLE_F3"},
      {"tension_physics_toggle", "#XUDU_TENSION_PHYSICS_TOGGLE"},
      {"unlock_transcopyright", "#XUDU_UNLOCK_TRANSCOPYRIGHT"},
      {"unlock_transcopyright_f5", "#XUDU_UNLOCK_TRANSCOPYRIGHT_F5"},
      {"unlock_transcopyright_ctrl_u", "#XUDU_UNLOCK_TRANSCOPYRIGHT_CTRL_U"},
      {"scrub_forward", "#XUDU_SCRUB_FORWARD"},
      {"scrub_backward", "#XUDU_SCRUB_BACKWARD"},
      {"transclude", "#XUDU_TRANSCLUDE"},
      {"xanalink", "#XUDU_XANALINK"},
      {"cancel_link", "#XUDU_CANCEL_LINK"},
      {"beams", "#XUDU_BEAMS"},
      {"sworph", "#XUDU_SWORPH"},
      {"publish", "#XUDU_PUBLISH"},
      {"history", "#XUDU_HISTORY"},
      {"delete", "#XUDU_DELETE"},
      {"page_break", "#XUDU_PAGE_BREAK"},
      {"hypertime_map", "#XUDU_HYPERTIME_MAP"},
      {"map", "#XUDU_MAP"},
      {"scrub_back", "#XUDU_SCRUB_BACK"},
      {"radial_menu", "#XUDU_RADIAL_MENU"},
  };

  for (const auto &[name, opcode] : kXuduActions) {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Nop, opcode);
    core_.bindOutput(op, out);
    routineBindings_[op] = {.inputParams = {}, .outputParams = {out}};
    exportSymbol(mod, name, op);
  }
}

} // namespace zigzag::vortex
