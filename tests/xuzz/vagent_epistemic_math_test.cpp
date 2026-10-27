/**
 * @file vagent_epistemic_math_test.cpp
 * @brief Vortex elementary math opcodes used by vagent graph lowering.
 */
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using namespace zigzag::vortex;

struct MathHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
};

CellRef scalar(ArenaManifold &arena, const CellValue &value) {
  if (const auto real = std::get_if<double>(&value))
    return arena.makeScalarCell(*real);
  if (const auto integer = std::get_if<std::int64_t>(&value))
    return arena.makeScalarCell(*integer);
  if (const auto text = std::get_if<std::string>(&value))
    return arena.makeCell(*text);
  return arena.makeScalarCell(std::get<bool>(value));
}

struct RunResult {
  ExecutionResult execution;
  CellValue output;
};

RunResult runMath(OpcodeKind kind, const std::vector<CellValue> &inputs,
                  CellValue initialOutput = 0.0) {
  MathHarness h;
  const auto opcode = h.vm.mintOpcode(kind);
  for (const auto &input : inputs)
    h.core.bindInput(opcode, scalar(h.arena, input));
  const auto output = h.arena.makeScalarCell(std::get<double>(initialOutput));
  h.core.bindOutput(opcode, output);
  const auto cursor = h.vm.spawnCursor(opcode);
  const auto result = h.vm.step(cursor);
  return {result, h.core.render(output).value()};
}

TEST(VagentEpistemicMathTest, ElementaryFunctionsExecuteAsVortexOpcodes) {
  const auto exp = runMath(OpcodeKind::Exp, {2.0});
  ASSERT_TRUE(exp.execution.success) << exp.execution.errorMessage;
  EXPECT_NEAR(std::get<double>(exp.output), std::exp(2.0), 1e-14);

  const auto ln = runMath(OpcodeKind::Ln, {std::exp(2.0)});
  ASSERT_TRUE(ln.execution.success) << ln.execution.errorMessage;
  EXPECT_NEAR(std::get<double>(ln.output), 2.0, 1e-14);

  const auto sqrt = runMath(OpcodeKind::Sqrt, {9.0});
  ASSERT_TRUE(sqrt.execution.success) << sqrt.execution.errorMessage;
  EXPECT_DOUBLE_EQ(std::get<double>(sqrt.output), 3.0);
}

TEST(VagentEpistemicMathTest, ExpUnderflowToFiniteZeroIsAllowed) {
  const auto result = runMath(OpcodeKind::Exp, {-1000.0});
  ASSERT_TRUE(result.execution.success) << result.execution.errorMessage;
  EXPECT_DOUBLE_EQ(std::get<double>(result.output), 0.0);
}

TEST(VagentEpistemicMathTest, DomainAndOverflowFaultsPreserveOutput) {
  for (const auto &[kind, input] : std::vector<std::pair<OpcodeKind, double>>{
           {OpcodeKind::Exp, std::numeric_limits<double>::max()},
           {OpcodeKind::Ln, 0.0},
           {OpcodeKind::Ln, -1.0},
           {OpcodeKind::Sqrt, -1.0}}) {
    const auto result = runMath(kind, {input}, 47.0);
    EXPECT_FALSE(result.execution.success);
    EXPECT_EQ(result.execution.contractViolation,
              ContractViolationKind::Precondition);
    EXPECT_DOUBLE_EQ(std::get<double>(result.output), 47.0);
  }
}

TEST(VagentEpistemicMathTest, RejectsNonfiniteInputsAndResults) {
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan      = std::numeric_limits<double>::quiet_NaN();
  for (const auto kind : {OpcodeKind::Exp, OpcodeKind::Ln, OpcodeKind::Sqrt}) {
    for (const auto value : {infinity, -infinity, nan}) {
      const auto result = runMath(kind, {value}, 19.0);
      EXPECT_FALSE(result.execution.success);
      EXPECT_EQ(result.execution.contractViolation,
                ContractViolationKind::Precondition);
      EXPECT_DOUBLE_EQ(std::get<double>(result.output), 19.0);
    }
  }
}

TEST(VagentEpistemicMathTest, RequiresExactlyOneDoubleInput) {
  for (const auto kind : {OpcodeKind::Exp, OpcodeKind::Ln, OpcodeKind::Sqrt}) {
    for (const auto &inputs :
         {std::vector<CellValue>{}, std::vector<CellValue>{2.0, 3.0},
          std::vector<CellValue>{std::int64_t{2}}, std::vector<CellValue>{true},
          std::vector<CellValue>{std::string{"2"}}}) {
      const auto result = runMath(kind, inputs, 23.0);
      EXPECT_FALSE(result.execution.success);
      EXPECT_EQ(result.execution.contractViolation,
                ContractViolationKind::Precondition);
      EXPECT_DOUBLE_EQ(std::get<double>(result.output), 23.0);
    }
  }
}

TEST(VagentEpistemicMathTest, PersistedOpcodesDecodeAndExecuteInFreshArena) {
  const std::array cases{
      std::tuple{OpcodeKind::Exp, 0.0, 1.0},
      std::tuple{OpcodeKind::Ln, 1.0, 0.0},
      std::tuple{OpcodeKind::Sqrt, 0.0, 0.0},
  };
  for (const auto &[kind, inputValue, expected] : cases) {
    ArenaManifold original;
    VortexCore originalCore{original};
    VortexVM originalVm{originalCore};
    const auto opcode = originalVm.mintOpcode(kind);

    xanadu::Store store;
    const auto genesis   = store.sliceGenesis({});
    const auto persisted = zigzag::promote(store, genesis, original, opcode);
    ASSERT_TRUE(persisted);
    const auto fold = store.rebuildManifold(persisted->version);
    ArenaManifold resumed{&fold, store};
    VortexCore core{resumed};
    VortexVM vm{core};
    const auto persistedOpcode = persisted->mapping.at(opcode);
    // Bind arena-local operands after restore, as the runtime recompiler does.
    const auto input           = resumed.makeScalarCell(inputValue);
    const auto persistedOutput = resumed.makeScalarCell(-1.0);
    core.bindInput(persistedOpcode, input);
    core.bindOutput(persistedOpcode, persistedOutput);

    ASSERT_EQ(vm.getOpcodeKind(persistedOpcode), kind);
    const auto cursor = vm.spawnCursor(persistedOpcode);
    const auto result = vm.step(cursor);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_DOUBLE_EQ(std::get<double>(core.render(persistedOutput).value()),
                     expected);
  }
}

} // namespace
