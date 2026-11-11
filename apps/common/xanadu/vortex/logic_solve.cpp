/**
 * @file logic_solve.cpp
 * @brief VortexStdLib's logic entry points (solve, solveOnce, solveQuery).
 *
 * These still refuse: the compiled resolution behind them is the P1 card
 * (logic_compile_core.cpp). They were moved here unchanged so that card, and
 * only that card, edits this file to forward to logic::compileAndRunQuery.
 */
#include <stdexcept>

#include "common/xanadu/vortex/logic_compile.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"

namespace zigzag::vortex {

std::vector<CellRef> VortexStdLib::stdlibPredicates() const {
  const CellRef seeded[] = {predEqual_,          predUnify_,
                            predMember_,         predAppend_,
                            predLength_,         predVortexFunction_,
                            predVortexModule_,   predVortexInstruction_,
                            predVortexContract_, predVortexParam_,
                            predSetting_,        predSettingShape_,
                            predSettingDefault_, predCellValue_,
                            predCellLink_,       predTransclude_,
                            predXanalink_,       predCellSpan_,
                            predBridgeEdge_};
  std::vector<CellRef> cells;
  for (const CellRef cell : seeded) {
    if (core_.arena().contains(cell)) cells.push_back(cell);
  }
  return cells;
}

logic::CompileContext
VortexStdLib::makeLogicContext(std::span<const CellRef> customPredicates) {
  return logic::makeCompileContext(core_, vm_, *this, customPredicates,
                                   logicMaxCycles_);
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
  static_cast<void>(goals);
  static_cast<void>(onSolution);
  static_cast<void>(customPredicates);
  static_cast<void>(maxSolutions);
  throw std::logic_error(
      "Vortex logic execution is unavailable: VM resolution lowering is not "
      "implemented");
}

} // namespace zigzag::vortex
