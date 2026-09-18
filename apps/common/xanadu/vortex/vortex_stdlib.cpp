/**
 * @file vortex_stdlib.cpp
 * @brief Implementation of Vortex Standard Library in Vortex.
 */
#include "common/xanadu/vortex/vortex_stdlib.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <numeric>
#include <ranges>
#include <sstream>
#include <unordered_set>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

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

void collectVariablesHelper(const VortexStdLib &stdlib, CellRef term,
                            std::vector<CellRef> &vars,
                            std::unordered_set<CellRef> &seenVars,
                            std::unordered_set<CellRef> &visitedTerms) {
  CellRef actual = stdlib.deref(term);
  if (actual == noCell) {
    return;
  }
  if (stdlib.isVar(actual)) {
    if (seenVars.insert(actual).second) {
      vars.push_back(term);
    }
    return;
  }
  if (!visitedTerms.insert(actual).second) {
    return;
  }
  auto args = stdlib.argumentsOf(actual);
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

CellRef freshenTerm(VortexStdLib &stdlib, VortexCore &core, CellRef term,
                    std::unordered_map<CellRef, CellRef> &varMap,
                    std::unordered_map<CellRef, CellRef> &visiting) {
  CellRef actual = stdlib.deref(term);
  if (actual == noCell) {
    return noCell;
  }
  if (stdlib.isVar(actual)) {
    auto it = varMap.find(actual);
    if (it != varMap.end()) {
      CellRef master    = it->second;
      CellRef cloneCell = core.arena().makeCell();
      zigzag::Vlog vlog{.m     = core.arena(),
                        .clone = core.dims().clone,
                        .grab  = core.dims().grab,
                        .step  = core.dims().step,
                        .vars  = core.dims().vars};
      core.arena().link(vlog.endOfRank(master, core.dims().clone, false),
                        core.dims().clone, false, cloneCell);
      return cloneCell;
    }
    CellRef fresh    = stdlib.makeVar();
    CellRef nameCell = core.arena().linked(actual, core.dims().name, false);
    if (nameCell != noCell && core.arena().contains(nameCell)) {
      core.arena().link(fresh, core.dims().name, false, nameCell);
    }
    varMap[actual] = fresh;
    return fresh;
  }

  auto args = stdlib.argumentsOf(actual);
  if (args.empty()) {
    CellRef cloneCell = core.arena().makeCell();
    zigzag::Vlog vlog{.m     = core.arena(),
                      .clone = core.dims().clone,
                      .grab  = core.dims().grab,
                      .step  = core.dims().step,
                      .vars  = core.dims().vars};
    core.arena().link(vlog.endOfRank(actual, core.dims().clone, false),
                      core.dims().clone, false, cloneCell);
    return cloneCell;
  }

  auto vit = visiting.find(actual);
  if (vit != visiting.end()) {
    return vit->second;
  }

  std::string fn    = core.arena().textOf(actual);
  CellRef freshTerm = core.arena().makeCell(fn);
  visiting[actual]  = freshTerm;

  CellRef prev = noCell;
  for (CellRef arg : args) {
    CellRef freshArg = freshenTerm(stdlib, core, arg, varMap, visiting);
    if (prev == noCell) {
      core.arena().link(freshTerm, core.dims().grab, false, freshArg);
    } else {
      core.arena().link(prev, core.dims().step, false, freshArg);
    }
    prev = freshArg;
  }

  visiting.erase(actual);
  return freshTerm;
}

bool evalArithmetic(const VortexStdLib &stdlib, const VortexCore &core,
                    CellRef term, double &outVal, bool &outIsInt) {
  CellRef cur = stdlib.deref(term);
  if (cur == noCell || stdlib.isVar(cur)) {
    return false;
  }
  const auto kind = core.arena().valueKindOf(cur);
  if (kind == xanadu::ValueKind::Int64) {
    if (auto v = core.arena().asInt64(cur)) {
      outVal   = static_cast<double>(*v);
      outIsInt = true;
      return true;
    }
  } else if (kind == xanadu::ValueKind::Double) {
    if (auto v = core.arena().asDouble(cur)) {
      outVal   = *v;
      outIsInt = false;
      return true;
    }
  }

  std::string fn = stdlib.functorOf(cur);
  auto args      = stdlib.argumentsOf(cur);

  if (fn == "-" && args.size() == 1) {
    double v = 0.0;
    bool isI = false;
    if (evalArithmetic(stdlib, core, args[0], v, isI)) {
      outVal   = -v;
      outIsInt = isI;
      return true;
    }
    return false;
  }

  if (fn == "+" && args.size() == 1) {
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
    if (fn == "+") {
      outVal   = v1 + v2;
      outIsInt = isI1 && isI2;
      return true;
    }
    if (fn == "-") {
      outVal   = v1 - v2;
      outIsInt = isI1 && isI2;
      return true;
    }
    if (fn == "*") {
      outVal   = v1 * v2;
      outIsInt = isI1 && isI2;
      return true;
    }
    if (fn == "/") {
      if (v2 == 0.0) return false;
      outVal   = v1 / v2;
      outIsInt = false;
      return true;
    }
    if (fn == "//") {
      if (static_cast<std::int64_t>(v2) == 0) return false;
      outVal   = static_cast<double>(static_cast<std::int64_t>(v1) /
                                     static_cast<std::int64_t>(v2));
      outIsInt = true;
      return true;
    }
    if (fn == "mod") {
      if (static_cast<std::int64_t>(v2) == 0) return false;
      outVal   = static_cast<double>(static_cast<std::int64_t>(v1) %
                                     static_cast<std::int64_t>(v2));
      outIsInt = true;
      return true;
    }
  }

  std::string txt = core.arena().textOf(cur);
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
  if (solutionsCount >= maxSolutions || depth > 1000) {
    return false;
  }
  if (goals.empty()) {
    LogicSolution sol;
    for (CellRef v : queryVars) {
      CellRef val = stdlib.deref(v);
      sol.bindings.emplace_back(v, val);
      sol.varMap[v] = val;
      std::string varName;
      CellRef nameCell =
          core.arena().linked(v, core.dims().name, DimVector::POS);
      if (nameCell != noCell && core.arena().contains(nameCell)) {
        varName = core.arena().textOf(nameCell);
      } else {
        varName = "_G" + std::to_string(v);
      }
      sol.formatted[varName] = stdlib.renderTerm(v);
    }
    solutionsCount++;
    if (onSolution) {
      return onSolution(sol);
    }
    return true;
  }

  ActiveGoal curGoal = goals[0];
  std::vector<ActiveGoal> restGoals(goals.begin() + 1, goals.end());

  std::string goalFunctor       = stdlib.functorOf(curGoal.term);
  std::vector<CellRef> goalArgs = stdlib.argumentsOf(curGoal.term);

  // Optimization for length/2 when list is known
  if (goalFunctor == "length" && goalArgs.size() == 2) {
    CellRef listArg = stdlib.deref(goalArgs[0]);
    CellRef lenArg  = stdlib.deref(goalArgs[1]);
    if (!stdlib.isVar(listArg)) {
      std::int64_t count = 0;
      CellRef cur        = listArg;
      std::size_t limit  = core.arena().cellCount() + 1;
      while (cur != noCell && !stdlib.isVar(cur) && limit-- > 0) {
        cur       = stdlib.deref(cur);
        auto args = stdlib.argumentsOf(cur);
        if (stdlib.functorOf(cur) == "." && args.size() >= 2) {
          count++;
          cur = stdlib.deref(args[1]);
        } else {
          break;
        }
      }
      if (cur != noCell && stdlib.functorOf(cur) == "[]") {
        auto mark       = core.arena().mark();
        CellRef numCell = core.arena().makeScalarCell(count);
        if (stdlib.unify(lenArg, numCell)) {
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
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
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
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        auto mark            = core.arena().mark();
        CellRef mCell        = core.arena().makeCell(modName);
        CellRef fCell        = core.arena().makeCell(symName);
        std::string fullPath = modName + "/" + symName;
        CellRef pCell        = core.arena().makeCell(fullPath);
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
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
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
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        std::string fullPath = modName + "/" + symName;
        CellRef entryOp      = stdlib.resolve(fullPath);
        if (entryOp == noCell) continue;

        CellRef curOp        = entryOp;
        std::int64_t stepIdx = 0;
        std::size_t limit    = core.arena().cellCount() + 1;
        while (curOp != noCell && limit-- > 0) {
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark         = core.arena().mark();
          CellRef pCell     = core.arena().makeCell(fullPath);
          CellRef idxCell   = core.arena().makeScalarCell(stepIdx);
          std::string label = core.arena().textOf(curOp);
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
          curOp = core.arena().linked(curOp, core.dims().spin, DimVector::POS);
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
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        std::string fullPath = modName + "/" + symName;
        CellRef entryOp      = stdlib.resolve(fullPath);
        if (entryOp == noCell) continue;

        CellRef curOp     = entryOp;
        std::size_t limit = core.arena().cellCount() + 1;
        while (curOp != noCell && limit-- > 0) {
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          // Preconditions along -d.contract
          std::vector<CellRef> preconds = core.preconditionsOf(curOp);
          for (CellRef p : preconds) {
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
          std::vector<CellRef> postconds = core.postconditionsOf(curOp);
          for (CellRef p : postconds) {
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

          curOp = core.arena().linked(curOp, core.dims().spin, DimVector::POS);
        }
      }
      if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
    }
    return true;
  }

  // Manifold stdlib introspection: vortex_param(Path, Wing, SlotIndex, Target)
  if (goalFunctor == "vortex_param" && goalArgs.size() == 4) {
    for (const auto &modName : stdlib.modules()) {
      for (const auto &symName : stdlib.symbolsInModule(modName)) {
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        std::string fullPath = modName + "/" + symName;
        CellRef entryOp      = stdlib.resolve(fullPath);
        if (entryOp == noCell) continue;

        // Input wing
        std::vector<CellRef> inCells = core.inputsOf(entryOp);
        for (std::size_t i = 0; i < inCells.size(); ++i) {
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
    auto mark    = core.arena().mark();
    bool unifies = stdlib.unify(goalArgs[0], goalArgs[1]);
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
      CellRef curVar =
          core.arena().linked(core.home(), varsDim, DimVector::POS);
      std::size_t limit = core.arena().cellCount() + 1;
      while (curVar != noCell && limit-- > 0) {
        std::string sName = core.arena().textOf(curVar);
        CellRef curVal = core.arena().linked(curVar, valsDim, DimVector::POS);
        bool cont      = evalSetting(sName, [&]() -> CellRef {
          return curVal == noCell ? core.arena().makeCell("") : curVal;
        });
        if (!cont) break;
        curVar = core.arena().linked(curVar, varsDim, DimVector::POS);
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
        for (const auto &alt : s.schema.alternatives) {
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark        = core.arena().mark();
          CellRef nameCell = core.arena().makeCell(s.name);
          std::vector<CellRef> typeCells;
          for (const auto &t : alt.expectedTypes) {
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
        for (const auto &alt : s.schema.alternatives) {
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
    CellRef dId = stdlib.deref(goalArgs[0]);
    if (!stdlib.isVar(dId)) {
      std::int64_t idNum = -1;
      if (auto n = core.arena().asInt64(dId); n.has_value()) {
        idNum = *n;
      } else {
        idNum = static_cast<std::int64_t>(dId);
      }
      if (idNum >= 0 && core.arena().contains(static_cast<CellRef>(idNum))) {
        auto target     = static_cast<CellRef>(idNum);
        auto mark       = core.arena().mark();
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
    } else {
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        CellRef target = core.arena().refOf(static_cast<std::uint32_t>(c));
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

    CellRef dFrom = stdlib.deref(goalArgs[0]);
    if (!stdlib.isVar(dFrom)) {
      std::int64_t fromNum = -1;
      if (auto n = core.arena().asInt64(dFrom); n.has_value()) {
        fromNum = *n;
      } else {
        fromNum = static_cast<std::int64_t>(dFrom);
      }
      if (fromNum >= 0 &&
          core.arena().contains(static_cast<CellRef>(fromNum))) {
        auto fromCell = static_cast<CellRef>(fromNum);
        for (const auto &dl : core.arena().dimensionsOf(fromCell)) {
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
    } else {
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        CellRef fromCell = core.arena().refOf(static_cast<std::uint32_t>(c));
        if (!core.arena().contains(fromCell)) continue;
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        for (const auto &dl : core.arena().dimensionsOf(fromCell)) {
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
        if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
        const auto *cop = store.getCompactOp(static_cast<std::uint32_t>(i));
        if (cop != nullptr && cop->kind == xanadu::OpKind::Transclude) {
          auto mark           = core.arena().mark();
          CellRef oDocCell    = core.arena().makeCell("store");
          std::string spanStr = "[" + std::to_string(cop->spanStart) + "," +
                                std::to_string(cop->spanLength) + "]";
          CellRef spanCell    = core.arena().makeCell(spanStr);
          CellRef tDocCell    = core.arena().makeCell("active");
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
      for (const auto &[id, link] : store.links()) {
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
    CellRef dId         = stdlib.deref(goalArgs[0]);
    auto matchCellSpans = [&](CellRef c) -> bool {
      auto spans = core.arena().contentOf(c);
      for (const auto &sp : spans) {
        if (solutionsCount >= maxSolutions || cutToFrame > 0) return false;
        auto mark = core.arena().mark();
        CellRef cCell =
            core.arena().makeScalarCell(static_cast<std::int64_t>(c));
        std::string spanStr = "[" + std::to_string(sp.start) + "," +
                              std::to_string(sp.length) + "]";
        CellRef spCell      = core.arena().makeCell(spanStr);
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

    if (!stdlib.isVar(dId)) {
      std::int64_t idNum = -1;
      if (auto n = core.arena().asInt64(dId); n.has_value()) {
        idNum = *n;
      } else {
        idNum = static_cast<std::int64_t>(dId);
      }
      if (idNum >= 0 && core.arena().contains(static_cast<CellRef>(idNum))) {
        matchCellSpans(static_cast<CellRef>(idNum));
      }
    } else {
      std::size_t total = core.arena().cellCount();
      for (std::size_t c = 1; c < total; ++c) {
        CellRef target = core.arena().refOf(static_cast<std::uint32_t>(c));
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
        CellRef presCell = core.arena().refOf(static_cast<std::uint32_t>(c));
        if (!core.arena().contains(presCell)) continue;
        CellRef docCell =
            core.arena().linked(presCell, cloneDim, DimVector::POS);
        if (docCell != noCell) {
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          auto mark = core.arena().mark();
          CellRef pCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(presCell));
          CellRef dCell =
              core.arena().makeScalarCell(static_cast<std::int64_t>(docCell));
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
      CellRef dDim = stdlib.deref(goalArgs[1]);
      std::string reqDimName;
      if (!stdlib.isVar(dDim)) {
        reqDimName = core.arena().textOf(dDim);
      }
      auto matchEdgesFrom = [&](CellRef fromCell) -> bool {
        if (!core.arena().contains(fromCell)) return true;
        for (const auto &dl : core.arena().dimensionsOf(fromCell)) {
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

      CellRef dU = stdlib.deref(goalArgs[0]);
      if (!stdlib.isVar(dU)) {
        std::int64_t uNum = -1;
        if (auto n = core.arena().asInt64(dU); n.has_value()) {
          uNum = *n;
        } else {
          uNum = static_cast<std::int64_t>(dU);
        }
        if (uNum >= 0 && core.arena().contains(static_cast<CellRef>(uNum))) {
          matchEdgesFrom(static_cast<CellRef>(uNum));
        }
      } else {
        std::size_t total = core.arena().cellCount();
        for (std::size_t c = 1; c < total; ++c) {
          CellRef fromCell = core.arena().refOf(static_cast<std::uint32_t>(c));
          if (solutionsCount >= maxSolutions || cutToFrame > 0) break;
          if (!matchEdgesFrom(fromCell)) break;
        }
      }
    }
    return true;
  }

  // Iterate over matching predicates
  for (CellRef pred : candidatePreds) {
    if (pred == noCell) continue;
    if (core.arena().textOf(pred) != goalFunctor) continue;

    const std::size_t frameId = nextFrameId++;
    CellRef clause =
        core.arena().linked(pred, core.dims().clause, DimVector::POS);
    std::size_t limit = core.arena().cellCount() + 1;
    while (clause != noCell && solutionsCount < maxSolutions && limit-- > 0) {
      if (cutToFrame > 0 && cutToFrame <= frameId) break;
      auto mark = core.arena().mark();

      std::unordered_map<CellRef, CellRef> varMap;
      std::unordered_map<CellRef, CellRef> visited;
      CellRef rawHead =
          core.arena().linked(clause, core.dims().grab, DimVector::POS);
      CellRef head = freshenTerm(stdlib, core, rawHead, varMap, visited);

      if (stdlib.unify(curGoal.term, head)) {
        std::vector<ActiveGoal> nextGoals;
        CellRef curBody =
            core.arena().linked(clause, core.dims().spin, DimVector::POS);
        std::size_t bodyLimit = core.arena().cellCount() + 1;
        while (curBody != noCell && bodyLimit-- > 0) {
          std::unordered_map<CellRef, CellRef> bodyVisited;
          CellRef bTerm =
              freshenTerm(stdlib, core, curBody, varMap, bodyVisited);
          nextGoals.push_back(ActiveGoal{.term = bTerm, .cutFrame = frameId});
          curBody =
              core.arena().linked(curBody, core.dims().spin, DimVector::POS);
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

      clause = core.arena().linked(clause, core.dims().clause, DimVector::POS);
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
  CellRef modLogic       = getOrCreateModule("std:logic");
  CellRef modArray       = getOrCreateModule("sys:array");
  CellRef modZigzag      = getOrCreateModule("std:zigzag");
  CellRef modGC          = getOrCreateModule("std:gc");
  CellRef modUI          = getOrCreateModule("std:ui");
  CellRef modNav         = getOrCreateModule("std:nav");
  CellRef modBridge      = getOrCreateModule("std:bridge");
  CellRef modXudu        = getOrCreateModule("std:xudu");

  buildMathModule(modMath);
  buildStringModule(modString);
  buildContractModule(modContract);
  buildPipelineModule(modPipeline);
  buildMemoizeModule(modMemoize);
  buildFunctionalModule(modFunctional);
  buildCollectionsModule(modCollections);
  buildLogicModule(modLogic);
  buildArrayModule(modArray);
  buildZigzagModule(modZigzag);
  buildGCModule(modGC);
  buildUiModule(modUI);
  buildNavModule(modNav);
  buildBridgeModule(modBridge);
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

    core_.arena().link(opMax, core_.dims().spin, false, opMin);

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

    core_.arena().link(opTrim, core_.dims().spin, false, opLower);

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
  std::string opName = core_.arena().textOf(fnOp);
  if (opName.starts_with("#ARRAY_")) {
    if (opName == "#ARRAY_IOTA") {
      std::size_t n =
          args.empty() ? 0 : static_cast<std::size_t>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      CellRef res = arrayIota(n, dim);
      return {static_cast<std::int64_t>(res)};
    }
    if (opName == "#ARRAY_TALLY") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                   : core_.dims().step;
      return {static_cast<std::int64_t>(arrayTally(origin, dim))};
    }
    if (opName == "#ARRAY_TAKE") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                   : core_.dims().step;
      std::size_t count =
          args.size() > 2 ? static_cast<std::size_t>(toInt64(args[2])) : 0;
      CellRef res = arrayTake(origin, dim, count);
      return {static_cast<std::int64_t>(res)};
    }
    if (opName == "#ARRAY_DROP") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                   : core_.dims().step;
      std::size_t count =
          args.size() > 2 ? static_cast<std::size_t>(toInt64(args[2])) : 0;
      CellRef res = arrayDrop(origin, dim, count);
      return {static_cast<std::int64_t>(res)};
    }
    if (opName == "#ARRAY_REVERSE") {
      CellRef origin =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim  = args.size() > 1 ? static_cast<DimRef>(toInt64(args[1]))
                                    : core_.dims().step;
      CellRef res = arrayReverse(origin, dim);
      return {static_cast<std::int64_t>(res)};
    }
  }
  if (opName.starts_with("#ZZ_")) {
    if (opName == "#ZZ_STEP") {
      CellRef cursor =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      DimVector dir = args.size() > 2 ? (toInt64(args[2]) < 0 ? DimVector::NEG
                                                              : DimVector::POS)
                                      : DimVector::POS;
      return {static_cast<std::int64_t>(zzStep(cursor, dim, dir))};
    }
    if (opName == "#ZZ_INSERT") {
      CellRef cursor =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      DimVector dir = args.size() > 2 ? (toInt64(args[2]) < 0 ? DimVector::NEG
                                                              : DimVector::POS)
                                      : DimVector::POS;
      std::string text =
          args.size() > 3 && std::holds_alternative<std::string>(args[3])
              ? std::get<std::string>(args[3])
              : "";
      return {static_cast<std::int64_t>(zzInsert(cursor, dim, dir, text))};
    }
    if (opName == "#ZZ_UNLINK") {
      CellRef cursor =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      DimVector dir = args.size() > 2 ? (toInt64(args[2]) < 0 ? DimVector::NEG
                                                              : DimVector::POS)
                                      : DimVector::POS;
      return {static_cast<std::int64_t>(zzUnlink(cursor, dim, dir))};
    }
    if (opName == "#ZZ_LINK") {
      CellRef cellA =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      CellRef cellB =
          args.size() > 1 ? static_cast<CellRef>(toInt64(args[1])) : noCell;
      DimRef dim =
          args.size() > 2 ? static_cast<DimRef>(toInt64(args[2])) : noCell;
      DimVector dir = args.size() > 3 ? (toInt64(args[3]) < 0 ? DimVector::NEG
                                                              : DimVector::POS)
                                      : DimVector::POS;
      return {static_cast<std::int64_t>(zzLink(cellA, cellB, dim, dir))};
    }
    if (opName == "#ZZ_DELETE") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      return {static_cast<std::int64_t>(zzDelete(cell))};
    }
    if (opName == "#ZZ_CLONE_CHAIN") {
      CellRef sym =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      CellRef tgt =
          args.size() > 1 ? static_cast<CellRef>(toInt64(args[1])) : noCell;
      return {static_cast<std::int64_t>(zzCloneToChain(sym, tgt))};
    }
    if (opName == "#ZZ_DUPLICATE") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      return {static_cast<std::int64_t>(zzDuplicate(cell))};
    }
  }
  if (opName.starts_with("#NAV_")) {
    if (opName == "#NAV_HOP_HEAD") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      return {static_cast<std::int64_t>(hopHead(cell, dim))};
    }
    if (opName == "#NAV_HOP_TAIL") {
      CellRef cell =
          args.empty() ? noCell : static_cast<CellRef>(toInt64(args[0]));
      DimRef dim =
          args.size() > 1 ? static_cast<DimRef>(toInt64(args[1])) : noCell;
      return {static_cast<std::int64_t>(hopTail(cell, dim))};
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
  auto it = routineBindings_.find(fnOp);
  if (it != routineBindings_.end()) {
    const auto &bindings = it->second;
    for (std::size_t i = 0;
         i < std::min(args.size(), bindings.inputParams.size()); ++i) {
      core_.value(bindings.inputParams[i], 0, -1, args[i]);
    }
    CellRef cursor = vm_.spawnCursor(fnOp, "call");
    auto execRes   = vm_.run(cursor, 100);
    if (!execRes.success) {
      return {};
    }
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
  auto execRes   = vm_.run(cursor, 100);
  if (!execRes.success) {
    return {};
  }
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
      core_.arena().link(resTail, outDim, DimVector::POS, newC);
      resTail = newC;
    }
    cur = core_.arena().linked(cur, inDim, DimVector::POS);
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
        core_.arena().link(resTail, outDim, DimVector::POS, newC);
        resTail = newC;
      }
    }
    cur = core_.arena().linked(cur, inDim, DimVector::POS);
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
    cur = core_.arena().linked(cur, inDim, DimVector::POS);
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
    core_.arena().link(pairCell, dimA, DimVector::NEG, curA);
    core_.arena().link(pairCell, dimB, DimVector::POS, curB);

    if (resHead == noCell) {
      resHead = pairCell;
      resTail = pairCell;
    } else {
      core_.arena().link(resTail, outDim, DimVector::POS, pairCell);
      resTail = pairCell;
    }
    curA = core_.arena().linked(curA, dimA, DimVector::POS);
    curB = core_.arena().linked(curB, dimB, DimVector::POS);
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
      core_.arena().link(tail, linkDim, DimVector::POS, c);
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
    cur = core_.arena().linked(cur, linkDim, DimVector::POS);
  }
  return result;
}

void VortexStdLib::pushBack(CellRef head, const CellValue &val, DimRef dim) {
  if (head == noCell) return;
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (limit-- > 0) {
    CellRef next = core_.arena().linked(cur, linkDim, DimVector::POS);
    if (next == noCell) {
      CellRef c = core_.arena().makeCell();
      core_.value(c, 0, -1, val);
      core_.arena().link(cur, linkDim, DimVector::POS, c);
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
    core_.arena().link(c, linkDim, DimVector::POS, head);
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
    CellRef next = core_.arena().linked(cur, linkDim, DimVector::POS);
    if (next == noCell) {
      CellValue val = core_.render(cur);
      if (prev != noCell) {
        core_.breakLink(prev, linkDim, DimVector::POS);
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
  CellRef next   = core_.arena().linked(head, linkDim, DimVector::POS);
  core_.breakLink(head, linkDim, DimVector::POS);
  return next;
}

std::size_t VortexStdLib::listLength(CellRef head, DimRef dim) const {
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  std::size_t len   = 0;
  CellRef cur       = head;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    len++;
    cur = core_.arena().linked(cur, linkDim, DimVector::POS);
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
      core_.arena().link(grid[r][c], dCol, DimVector::POS, grid[r][c + 1]);
    }
  }

  // Link vertically on dRow
  for (std::size_t r = 0; r + 1 < rows; ++r) {
    for (std::size_t c = 0; c < cols; ++c) {
      core_.arena().link(grid[r][c], dRow, DimVector::POS, grid[r + 1][c]);
    }
  }

  return grid[0][0];
}

CellValue VortexStdLib::getGrid(CellRef gridRoot, std::size_t r, std::size_t c,
                                DimRef dRow, DimRef dCol) const {
  CellRef cur = gridRoot;
  for (std::size_t i = 0; i < r && cur != noCell; ++i) {
    cur = core_.arena().linked(cur, dRow, DimVector::POS);
  }
  for (std::size_t j = 0; j < c && cur != noCell; ++j) {
    cur = core_.arena().linked(cur, dCol, DimVector::POS);
  }
  if (cur == noCell) return false;
  return core_.render(cur);
}

void VortexStdLib::setGrid(CellRef gridRoot, std::size_t r, std::size_t c,
                           DimRef dRow, DimRef dCol, const CellValue &val) {
  CellRef cur = gridRoot;
  for (std::size_t i = 0; i < r && cur != noCell; ++i) {
    cur = core_.arena().linked(cur, dRow, DimVector::POS);
  }
  for (std::size_t j = 0; j < c && cur != noCell; ++j) {
    cur = core_.arena().linked(cur, dCol, DimVector::POS);
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
    core_.arena().link(var, core_.dims().name, false, nameCell);
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

CellRef VortexStdLib::deref(CellRef cell) const {
  zigzag::Vlog v{.m     = core_.arena(),
                 .clone = core_.dims().clone,
                 .grab  = core_.dims().grab,
                 .step  = core_.dims().step,
                 .vars  = core_.dims().vars};
  return v.deref(cell);
}

bool VortexStdLib::unify(CellRef a, CellRef b) {
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

std::string VortexStdLib::functorOf(CellRef term) const {
  CellRef actual = deref(term);
  return core_.arena().textOf(actual);
}

std::string VortexStdLib::renderTerm(CellRef term) const {
  CellRef actual = deref(term);
  if (isVar(actual)) {
    CellRef nameCell = core_.arena().linked(actual, core_.dims().name, false);
    if (nameCell != noCell && core_.arena().contains(nameCell)) {
      std::string n = core_.arena().textOf(nameCell);
      if (!n.empty()) return n;
    }
    return "_G" + std::to_string(actual);
  }

  const auto kind = core_.arena().valueKindOf(actual);
  if (kind == xanadu::ValueKind::Int64) {
    auto val = core_.arena().asInt64(actual);
    if (val) return std::to_string(*val);
  } else if (kind == xanadu::ValueKind::Double) {
    auto val = core_.arena().asDouble(actual);
    if (val) return std::to_string(*val);
  } else if (kind == xanadu::ValueKind::Bool) {
    auto val = core_.arena().asBool(actual);
    if (val) return *val ? "true" : "false";
  }

  std::string fn = core_.arena().textOf(actual);
  auto args      = argumentsOf(actual);
  if (args.empty()) {
    return fn;
  }

  if (fn == "." && args.size() >= 2) {
    std::string s     = "[";
    CellRef cur       = actual;
    bool first        = true;
    std::size_t limit = core_.arena().cellCount() + 1;
    while (cur != noCell && limit-- > 0) {
      cur = deref(cur);
      if (isVar(cur)) {
        s += "|" + renderTerm(cur);
        break;
      }
      std::string cfn = functorOf(cur);
      if (cfn == "[]") {
        break;
      }
      if (cfn == ".") {
        auto cArgs = argumentsOf(cur);
        if (!first) s += ", ";
        first = false;
        if (!cArgs.empty()) {
          s += renderTerm(cArgs[0]);
        }
        cur = (cArgs.size() >= 2) ? cArgs[1] : noCell;
      } else {
        s += "|" + renderTerm(cur);
        break;
      }
    }
    s += "]";
    return s;
  }

  std::string s = fn + "(";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) s += ", ";
    s += renderTerm(args[i]);
  }
  s += ")";
  return s;
}

CellRef VortexStdLib::createPredicate(std::string_view name) {
  return core_.arena().makeCell(name);
}

CellRef VortexStdLib::addClause(CellRef predCell, CellRef headTerm,
                                std::span<const CellRef> bodyGoals) {
  CellRef clauseCell = core_.arena().makeCell();
  core_.arena().link(clauseCell, core_.dims().grab, false, headTerm);

  CellRef prevGoal = noCell;
  for (CellRef goal : bodyGoals) {
    if (prevGoal == noCell) {
      core_.arena().link(clauseCell, core_.dims().spin, false, goal);
    } else {
      core_.arena().link(prevGoal, core_.dims().spin, false, goal);
    }
    prevGoal = goal;
  }

  CellRef cur = core_.arena().linked(predCell, core_.dims().clause, false);
  if (cur == noCell) {
    core_.arena().link(predCell, core_.dims().clause, false, clauseCell);
  } else {
    std::size_t limit = core_.arena().cellCount() + 1;
    while (limit-- > 0) {
      CellRef next = core_.arena().linked(cur, core_.dims().clause, false);
      if (next == noCell) {
        core_.arena().link(cur, core_.dims().clause, false, clauseCell);
        break;
      }
      cur = next;
    }
  }
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
  return solve(std::span<const CellRef>{&goal, 1}, onSolution, customPredicates,
               maxSolutions);
}

bool VortexStdLib::solve(std::span<const CellRef> goals,
                         std::function<bool(const LogicSolution &)> onSolution,
                         std::span<const CellRef> customPredicates,
                         std::size_t maxSolutions) {
  if (goals.empty()) return true;

  std::vector<CellRef> candidatePreds;
  if (predEqual_ != noCell) candidatePreds.push_back(predEqual_);
  if (predUnify_ != noCell) candidatePreds.push_back(predUnify_);
  if (predMember_ != noCell) candidatePreds.push_back(predMember_);
  if (predAppend_ != noCell) candidatePreds.push_back(predAppend_);
  if (predLength_ != noCell) candidatePreds.push_back(predLength_);
  if (predVortexFunction_ != noCell)
    candidatePreds.push_back(predVortexFunction_);
  if (predVortexModule_ != noCell) candidatePreds.push_back(predVortexModule_);
  if (predVortexInstruction_ != noCell)
    candidatePreds.push_back(predVortexInstruction_);
  if (predVortexContract_ != noCell)
    candidatePreds.push_back(predVortexContract_);
  if (predVortexParam_ != noCell) candidatePreds.push_back(predVortexParam_);
  if (predSetting_ != noCell) candidatePreds.push_back(predSetting_);
  if (predSettingShape_ != noCell) candidatePreds.push_back(predSettingShape_);
  if (predSettingDefault_ != noCell)
    candidatePreds.push_back(predSettingDefault_);
  if (predCellValue_ != noCell) candidatePreds.push_back(predCellValue_);
  if (predCellLink_ != noCell) candidatePreds.push_back(predCellLink_);
  if (predTransclude_ != noCell) candidatePreds.push_back(predTransclude_);
  if (predXanalink_ != noCell) candidatePreds.push_back(predXanalink_);
  if (predCellSpan_ != noCell) candidatePreds.push_back(predCellSpan_);
  if (predBridgeEdge_ != noCell) candidatePreds.push_back(predBridgeEdge_);
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
                          queryVars, onSolution, solCount, maxSolutions, 0,
                          nextFrameId, cutToFrame);
}

// -- Module 9: sys:array ----------------------------------------------------
CellRef VortexStdLib::arrayIota(std::size_t n, DimRef dim, CellRef origin) {
  if (n == 0) {
    return noCell;
  }
  DimRef linkDim = dim == noCell ? core_.dims().step : dim;
  CellRef head   = origin;
  if (head == noCell) {
    head = core_.arena().makeScalarCell(static_cast<std::int64_t>(1));
  }
  CellRef prev = head;
  for (std::size_t i = 1; i < n; ++i) {
    CellRef next =
        core_.arena().makeScalarCell(static_cast<std::int64_t>(i + 1));
    core_.arena().link(prev, linkDim, DimVector::POS, next);
    prev = next;
  }
  return head;
}

std::vector<std::size_t>
VortexStdLib::arrayShape(CellRef origin, std::span<const DimRef> dims) const {
  if (origin == noCell || !core_.arena().contains(origin)) {
    return {};
  }
  if (dims.empty()) {
    return {arrayTally(origin, core_.dims().step)};
  }
  std::vector<std::size_t> shape;
  shape.reserve(dims.size());
  for (DimRef d : dims) {
    std::size_t len   = 0;
    CellRef cur       = origin;
    std::size_t limit = core_.arena().cellCount() + 1;
    while (cur != noCell && core_.arena().contains(cur) && limit-- > 0) {
      ++len;
      cur = core_.arena().linked(cur, d, DimVector::POS);
    }
    shape.push_back(len);
  }
  return shape;
}

CellRef VortexStdLib::arrayTake(CellRef origin, DimRef dim, std::size_t count) {
  if (origin == noCell || count == 0 || !core_.arena().contains(origin)) {
    return noCell;
  }
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  CellRef cur       = origin;
  CellRef head      = noCell;
  CellRef prev      = noCell;
  std::size_t limit = core_.arena().cellCount() + 1;
  for (std::size_t i = 0; i < count && cur != noCell && limit-- > 0; ++i) {
    CellRef copy = core_.arena().makeCell();
    if (auto d = core_.arena().asDouble(cur); d.has_value()) {
      core_.arena().makeScalarCell(*d);
      // copy scalar value
      copy = core_.arena().makeScalarCell(*d);
    } else if (auto n = core_.arena().asInt64(cur); n.has_value()) {
      copy = core_.arena().makeScalarCell(*n);
    } else if (auto b = core_.arena().asBool(cur); b.has_value()) {
      copy = core_.arena().makeScalarCell(*b);
    } else {
      core_.value(copy, 0, -1, core_.render(cur));
    }
    if (head == noCell) {
      head = copy;
    } else {
      core_.arena().link(prev, linkDim, DimVector::POS, copy);
    }
    prev = copy;
    cur  = core_.arena().linked(cur, linkDim, DimVector::POS);
  }
  return head;
}

CellRef VortexStdLib::arrayDrop(CellRef origin, DimRef dim, std::size_t count) {
  if (origin == noCell || !core_.arena().contains(origin)) {
    return noCell;
  }
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  CellRef cur       = origin;
  std::size_t limit = core_.arena().cellCount() + 1;
  for (std::size_t i = 0; i < count && cur != noCell && limit-- > 0; ++i) {
    cur = core_.arena().linked(cur, linkDim, DimVector::POS);
  }
  return cur;
}

CellRef VortexStdLib::arrayReverse(CellRef origin, DimRef dim) {
  if (origin == noCell || !core_.arena().contains(origin)) {
    return noCell;
  }
  DimRef linkDim = dim == noCell ? core_.dims().step : dim;
  std::vector<CellRef> cells;
  CellRef cur       = origin;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    cells.push_back(cur);
    cur = core_.arena().linked(cur, linkDim, DimVector::POS);
  }
  if (cells.empty()) {
    return noCell;
  }
  std::reverse(cells.begin(), cells.end());
  CellRef head = noCell;
  CellRef prev = noCell;
  for (CellRef c : cells) {
    CellRef copy = core_.arena().makeCell();
    if (auto d = core_.arena().asDouble(c); d.has_value()) {
      copy = core_.arena().makeScalarCell(*d);
    } else if (auto n = core_.arena().asInt64(c); n.has_value()) {
      copy = core_.arena().makeScalarCell(*n);
    } else if (auto b = core_.arena().asBool(c); b.has_value()) {
      copy = core_.arena().makeScalarCell(*b);
    } else {
      core_.value(copy, 0, -1, core_.render(c));
    }
    if (head == noCell) {
      head = copy;
    } else {
      core_.arena().link(prev, linkDim, DimVector::POS, copy);
    }
    prev = copy;
  }
  return head;
}

std::size_t VortexStdLib::arrayTally(CellRef origin, DimRef dim) const {
  if (origin == noCell || !core_.arena().contains(origin)) {
    return 0;
  }
  DimRef linkDim    = dim == noCell ? core_.dims().step : dim;
  std::size_t count = 0;
  CellRef cur       = origin;
  std::size_t limit = core_.arena().cellCount() + 1;
  while (cur != noCell && limit-- > 0) {
    ++count;
    cur = core_.arena().linked(cur, linkDim, DimVector::POS);
  }
  return count;
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

CellRef VortexStdLib::zzStep(CellRef cursor, DimRef dim, DimVector dir) {
  if (cursor == noCell || !core_.arena().contains(cursor)) {
    return noCell;
  }
  auto res = core_.link(cursor, dim, dir);
  return res ? *res : noCell;
}

CellRef VortexStdLib::zzInsert(CellRef cursor, DimRef dim, DimVector dir,
                               std::string_view text) {
  if (cursor == noCell || !core_.arena().contains(cursor)) {
    return noCell;
  }
  auto res = core_.newCell(cursor, dim, dir, std::string(text));
  return res ? *res : noCell;
}

CellRef VortexStdLib::zzUnlink(CellRef cursor, DimRef dim, DimVector dir) {
  if (cursor == noCell || !core_.arena().contains(cursor)) {
    return noCell;
  }
  auto res = core_.breakLink(cursor, dim, dir);
  return res ? *res : noCell;
}

CellRef VortexStdLib::zzLink(CellRef cellA, CellRef cellB, DimRef dim,
                             DimVector dir) {
  if (cellA == noCell || !core_.arena().contains(cellA) || cellB == noCell ||
      !core_.arena().contains(cellB)) {
    return noCell;
  }
  auto res = core_.link(cellA, dim, dir, cellB);
  return res ? *res : noCell;
}

CellRef VortexStdLib::zzDelete(CellRef cell) {
  if (cell == noCell || !core_.arena().contains(cell)) {
    return noCell;
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

CellRef VortexStdLib::zzCloneToChain(CellRef symbolOp, CellRef targetCell) {
  if (symbolOp == noCell || !core_.arena().contains(symbolOp) ||
      targetCell == noCell || !core_.arena().contains(targetCell)) {
    return noCell;
  }
  CellRef clone = core_.arena().makeCell();
  core_.arena().link(symbolOp, core_.dims().clone, DimVector::POS, clone);
  core_.arena().link(targetCell, core_.dims().spin, DimVector::POS, clone);
  return clone;
}

std::size_t VortexStdLib::gcSweep() { return core_.collectGarbage(); }

CellRef VortexStdLib::zzDuplicate(CellRef cell) {
  if (cell == noCell || !core_.arena().contains(cell)) {
    return noCell;
  }
  std::string text = core_.arena().textOf(cell);
  CellRef dup      = core_.arena().makeCell(text);
  auto vk          = core_.arena().valueKindOf(cell);
  if (vk != xanadu::ValueKind::None) {
    if (auto d = core_.arena().asDouble(cell)) {
      core_.arena().setValueBits(dup, vk, std::bit_cast<std::uint64_t>(*d));
    } else if (auto i = core_.arena().asInt64(cell)) {
      core_.arena().setValueBits(dup, vk, static_cast<std::uint64_t>(*i));
    } else if (auto b = core_.arena().asBool(cell)) {
      core_.arena().setValueBits(dup, vk, *b ? 1ULL : 0ULL);
    }
  }
  core_.link(cell, core_.dims().clone, DimVector::POS, dup);
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

CellRef VortexStdLib::hopHead(CellRef cursor, DimRef dim) {
  if (cursor == noCell || !core_.arena().contains(cursor) || dim == noCell) {
    return cursor;
  }
  CellRef cur       = cursor;
  std::size_t limit = core_.arena().cellCount() + 10;
  while (limit-- > 0) {
    CellRef prev = core_.arena().linked(cur, dim, DimVector::NEG);
    if (prev == noCell || prev == cur || prev == cursor) {
      break;
    }
    cur = prev;
  }
  return cur;
}

CellRef VortexStdLib::hopTail(CellRef cursor, DimRef dim) {
  if (cursor == noCell || !core_.arena().contains(cursor) || dim == noCell) {
    return cursor;
  }
  CellRef cur       = cursor;
  std::size_t limit = core_.arena().cellCount() + 10;
  while (limit-- > 0) {
    CellRef next = core_.arena().linked(cur, dim, DimVector::POS);
    if (next == noCell || next == cur || next == cursor) {
      break;
    }
    cur = next;
  }
  return cur;
}

CellRef VortexStdLib::jumpHome() const noexcept { return core_.home(); }

bool VortexStdLib::exportModuleToStore(std::string_view modulePath,
                                       xanadu::Store &destStore) const {
  CellRef mod = resolve(modulePath);
  if (mod == noCell) {
    return false;
  }
  auto parent = destStore.allVersions().empty()
                    ? xanadu::MicroversionId::parse("1")
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
  CellRef stdlibRoot =
      core_.arena().linked(core_.home(), core_.dims().stdlib, DimVector::POS);
  if (stdlibRoot == noCell) {
    stdlibRoot = core_.home();
  }
  auto parent = destStore.allVersions().empty()
                    ? xanadu::MicroversionId::parse("1")
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

CellRef VortexStdLib::importModuleFromStore(const xanadu::Store &srcStore) {
  auto ver = srcStore.allVersions().empty() ? xanadu::MicroversionId::parse("1")
                                            : srcStore.primaryCurrentVersion();
  auto srcManifold = srcStore.rebuildManifold(ver);
  if (srcManifold.cellCount() == 0) {
    return noCell;
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
      core_.arena().setValueBits(dstRef,
                                 static_cast<xanadu::ValueKind>(slot.valueKind),
                                 slot.valueBits);
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
        core_.arena().link(dstRef, dstDim, DimVector::POS,
                           cellMapping[link.pos]);
      }
      if (link.neg != noCell && cellMapping.contains(link.neg)) {
        core_.arena().link(dstRef, dstDim, DimVector::NEG,
                           cellMapping[link.neg]);
      }
    }
  }

  CellRef firstModuleCell = noCell;
  for (const auto &[srcRef, dstRef] : cellMapping) {
    std::string text = core_.arena().textOf(dstRef);
    if (text.starts_with("std:") && !text.contains('/')) {
      if (firstModuleCell == noCell) {
        firstModuleCell = dstRef;
      }
      CellRef tail = core_.home();
      while (core_.arena().linked(tail, core_.dims().stdlib, DimVector::POS) !=
             noCell) {
        tail = core_.arena().linked(tail, core_.dims().stdlib, DimVector::POS);
        if (tail == dstRef) {
          break;
        }
      }
      if (tail != dstRef) {
        core_.arena().link(tail, core_.dims().stdlib, DimVector::POS, dstRef);
      }
    }
  }
  return firstModuleCell != noCell
             ? firstModuleCell
             : (cellMapping.empty() ? noCell : cellMapping.begin()->second);
}

CellRef VortexStdLib::bridgeDocText(const xanadu::Store &store) {
  auto ver = store.allVersions().empty() ? xanadu::MicroversionId::parse("1")
                                         : store.primaryCurrentVersion();
  std::string txt = store.textOf(ver);
  return core_.arena().makeCell(txt);
}

CellRef VortexStdLib::bridgeStoreVersion(const xanadu::Store &store) {
  auto ver = store.allVersions().empty() ? xanadu::MicroversionId::parse("1")
                                         : store.primaryCurrentVersion();
  return core_.arena().makeCell(ver.str());
}

bool VortexStdLib::bridgeCellToDoc(xanadu::Store &store, CellRef cell,
                                   std::uint32_t docOffset) {
  if (cell == noCell || !core_.arena().contains(cell)) {
    return false;
  }
  std::string text = core_.arena().textOf(cell);
  auto parent = store.allVersions().empty() ? xanadu::MicroversionId::parse("1")
                                            : store.primaryCurrentVersion();
  auto newVer = store.insert(parent, docOffset, text);
  store.repointCurrentVersion(newVer);
  return true;
}

CellRef VortexStdLib::bridgeDocToCell(xanadu::Store &store,
                                      std::uint32_t docOffset,
                                      std::uint32_t length) {
  auto ver = store.allVersions().empty() ? xanadu::MicroversionId::parse("1")
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
