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
#include <unordered_set>

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
      if (txt.find('.') != std::string::npos) {
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
      sol.bindings.push_back({v, val});
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
    } else if (cutToFrame > 0 && cutToFrame < frameId) {
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

  buildMathModule(modMath);
  buildStringModule(modString);
  buildContractModule(modContract);
  buildPipelineModule(modPipeline);
  buildMemoizeModule(modMemoize);
  buildFunctionalModule(modFunctional);
  buildCollectionsModule(modCollections);
  buildLogicModule(modLogic);
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

    routineBindings_[op] = {{in}, {out}};
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
    routineBindings_[op] = {{in0, in1}, {out}};
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
    routineBindings_[op] = {{in0, in1}, {out}};
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

    routineBindings_[opMax] = {{in, lo, hi}, {out}};
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
    routineBindings_[op] = {{in0, in1}, {out}};
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
    routineBindings_[op] = {{in0, in1}, {out}};
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
    routineBindings_[op] = {{in0, in1}, {out}};
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

    routineBindings_[op] = {{in0, in1}, {out}};
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

    routineBindings_[op] = {{in0, in1}, {out}};
    exportSymbol(mod, "mod", op);
  }

  // neg: #MATH_NEG in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Neg, "#MATH_NEG");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
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
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "to_lower", op);
  }

  // to_upper: #STR_TO_UPPER in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::ToUpper, "#STR_TO_UPPER");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "to_upper", op);
  }

  // trim: #STR_TRIM in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Trim, "#STR_TRIM");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
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

    routineBindings_[opTrim] = {{in}, {out}};
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
    routineBindings_[op] = {{targetOp, cacheKey, capacity}, {}};
    exportSymbol(mod, "memoize", op);
  }

  // flush: cacheKey -> status
  {
    CellRef cacheKey = core_.arena().makeCell();
    CellRef status   = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#MEMO_FLUSH");
    core_.bindInput(op, cacheKey);
    core_.bindOutput(op, status);
    routineBindings_[op] = {{cacheKey}, {status}};
    exportSymbol(mod, "flush", op);
  }

  // retire: cacheKey -> status
  {
    CellRef cacheKey = core_.arena().makeCell();
    CellRef status   = core_.arena().makeCell();
    CellRef op       = vm_.mintOpcode(OpcodeKind::Nop, "#MEMO_RETIRE");
    core_.bindInput(op, cacheKey);
    core_.bindOutput(op, status);
    routineBindings_[op] = {{cacheKey}, {status}};
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
    routineBindings_[op] = {{inHead, inDim, outDim, fnOp}, {out}};
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
    routineBindings_[op] = {{inHead, inDim, outDim, predOp}, {out}};
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
    routineBindings_[op] = {{inHead, inDim, initial, fnOp}, {out}};
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
    routineBindings_[op] = {{headA, headB, dimA, dimB, outDim}, {out}};
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
    routineBindings_[op] = {{inItems, inDim}, {outHead}};
    exportSymbol(mod, "list", op);
  }

  // map: entries -> head
  {
    CellRef inEntries = core_.arena().makeCell();
    CellRef outHead   = core_.arena().makeCell();
    CellRef op        = vm_.mintOpcode(OpcodeKind::Nop, "#COLL_MAP");
    core_.bindInput(op, inEntries);
    core_.bindOutput(op, outHead);
    routineBindings_[op] = {{inEntries}, {outHead}};
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
    routineBindings_[op] = {{inRows, inCols}, {outHead}};
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
    routineBindings_[op] = {{inHead, inVal, inDim}, {}};
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
    routineBindings_[op] = {{inHead, inVal, inDim}, {outHead}};
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
    routineBindings_[op] = {{inHead, inDim}, {outVal}};
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
    routineBindings_[op] = {{in0, in1}, {out}};
    exportSymbol(mod, "unify", op);
  }

  // var: #MAKE_VAR out
  {
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::MakeVar, "#LOGIC_MAKE_VAR");
    core_.bindOutput(op, out);
    routineBindings_[op] = {{}, {out}};
    exportSymbol(mod, "var", op);
  }

  // is_var: #IS_VAR in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::IsVar, "#LOGIC_IS_VAR");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "is_var", op);
  }

  // term: #MAKE_TERM in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::MakeTerm, "#LOGIC_MAKE_TERM");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "term", op);
  }

  // deref: #DEREF in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Deref, "#LOGIC_DEREF");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "deref", op);
  }

  // choice: #CHOICE in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Choice, "#LOGIC_CHOICE");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
    exportSymbol(mod, "choice", op);
  }

  // fail: #FAIL
  {
    CellRef op           = vm_.mintOpcode(OpcodeKind::Fail, "#LOGIC_FAIL");
    routineBindings_[op] = {{}, {}};
    exportSymbol(mod, "fail", op);
  }

  // cut: #CUT in out
  {
    CellRef in  = core_.arena().makeCell();
    CellRef out = core_.arena().makeCell();
    CellRef op  = vm_.mintOpcode(OpcodeKind::Cut, "#LOGIC_CUT");
    core_.bindInput(op, in);
    core_.bindOutput(op, out);
    routineBindings_[op] = {{in}, {out}};
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
  for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
    tail = makeCons(*it, tail);
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

} // namespace zigzag::vortex
