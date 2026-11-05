/**
 * @file vql_contract_validator_profile_test.cpp
 * @brief Opt-in raw profile for computed-return validator capture layouts.
 *
 * Run explicitly with --gtest_also_run_disabled_tests
 * --gtest_filter='*DISABLED_Scaling'.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

constexpr std::string_view source =
    "def weave verify($item: Cell): Cell { +d.absent: Int := 2 + 3 } := "
    "$item; for $item in $items return verify($item)";

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
};

std::optional<CellRef> validatorInvoke(const Harness &h,
                                       const CompilationResult &program) {
  for (const CellRef opcode : program.generatedOpcodes) {
    if (h.arena.textOf(opcode).starts_with("#INVOKE_CONTRACT_VALIDATOR "))
      return opcode;
  }
  return std::nullopt;
}

void runProfileCase(const std::string_view variant, const std::size_t calls,
                    const std::size_t unusedCount) {
  Harness h;
  const CellRef item   = h.arena.makeCell("profile item");
  const CellRef unused = h.arena.makeCell("unused lexical binding");
  std::vector<CellRef> items(calls, item);
  std::vector<CellRef> unusedStream(unusedCount, unused);
  CompilationOptions options;
  options.initialBindings = {{"items", items}, {"unused", unusedStream}};
  if (variant == "control") {
    const CellRef trigger = h.arena.makeCell("unused pipeline binding");
    const CellRef halt    = h.vm.mintOpcode(OpcodeKind::Halt, "unused halt");
    h.core.attachPipeline(trigger, halt);
    EXPECT_TRUE(h.core.hasPipeline(trigger));
    options.initialBindings.emplace("control_trigger",
                                    std::vector<CellRef>{trigger});
  }

  VQLCompiler compiler(h.coordinator, h.vm);
  const CompilationResult compiled = compiler.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_TRUE(compiled.entryOpcode);
  const auto invoke = validatorInvoke(h, compiled);
  if (variant == "validator") {
    ASSERT_TRUE(invoke);
    ASSERT_EQ(h.vm.getOpcodeKind(*invoke), OpcodeKind::InvokeFrame);
  } else {
    ASSERT_FALSE(invoke);
  }

  std::size_t validatorLocals    = 0;
  std::size_t validatorArguments = 0;
  if (invoke) {
    const auto inputs = h.core.inputsOf(*invoke);
    ASSERT_FALSE(inputs.empty());
    const auto descriptor = h.vm.frameDescriptor(inputs.front());
    ASSERT_TRUE(descriptor) << descriptor.error().message;
    validatorLocals    = descriptor->locals.size();
    validatorArguments = descriptor->argumentPorts.size();
  }
  std::size_t maxStreamAppendInputs = 0;
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (h.vm.getOpcodeKind(opcode) == OpcodeKind::StreamAppend)
      maxStreamAppendInputs =
          std::max(maxStreamAppendInputs, h.core.inputsOf(opcode).size());
  }

  const CellRef cursor =
      h.vm.spawnCursor(*compiled.entryOpcode, "contract-validator-profile");
  const std::size_t cellsBefore = h.arena.cellCount();
  const auto start              = Clock::now();
  const auto execution          = h.vm.run(cursor, 20'000'000);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - start);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const std::size_t arenaCellDelta = h.arena.cellCount() - cellsBefore;

  std::vector<CellRef> results;
  for (const CellRef slot : compiled.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  ASSERT_EQ(results.size(), calls);
  EXPECT_TRUE(std::ranges::all_of(
      results, [item](CellRef result) { return result == item; }));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  std::cout << "VQL_CONTRACT_VALIDATOR_PROFILE variant=" << variant
            << " calls=" << calls << " unused_refs=" << unusedCount
            << " elapsed_us=" << elapsed.count()
            << " arena_cell_delta=" << arenaCellDelta
            << " validator_locals=" << validatorLocals
            << " validator_arguments=" << validatorArguments
            << " stream_append_max_inputs=" << maxStreamAppendInputs << '\n';
}

TEST(VQLContractValidatorProfileTest, DISABLED_Scaling) {
  for (const std::size_t calls : {32U, 128U}) {
    for (const std::size_t unusedCount : {0U, 1000U}) {
      runProfileCase("validator", calls, unusedCount);
      runProfileCase("control", calls, unusedCount);
    }
  }
}

} // namespace
} // namespace xanadu::vql
