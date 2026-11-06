/**
 * @file vql_mutation_guard_profile_test.cpp
 * @brief Opt-in profile of typed mutation guards versus scoped mutation cost.
 *
 * Run explicitly with --gtest_also_run_disabled_tests
 * --gtest_filter='*DISABLED_Scaling'.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
};

std::string sourceFor(const bool typed) {
  return std::string{"def weave put($cell"} + (typed ? ": Int" : "") +
         "): Cell := $cell/value(7); "
         "for $cell in $cells return put($cell)";
}

void runProfileCase(const bool typed, const std::size_t calls) {
  Harness h;
  const CellRef intCell = h.arena.makeScalarCell(std::int64_t{5});
  CompilationOptions options;
  options.initialBindings = {{"cells", std::vector<CellRef>(calls, intCell)}};
  VQLCompiler compiler(h.coordinator, h.vm);
  const std::string source = sourceFor(typed);

  const auto compileStart = Clock::now();
  const auto program      = compiler.compile(source, options);
  const auto compileElapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            compileStart);
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(program.entryOpcode);

  const CellRef cursor =
      h.vm.spawnCursor(*program.entryOpcode, "mutation-guard-profile");
  const std::size_t cellsBefore    = h.arena.cellCount();
  const std::uint64_t cyclesBefore = h.vm.totalCycles();
  const auto runStart              = Clock::now();
  const auto execution             = h.vm.run(cursor, 20'000'000);
  const auto runElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - runStart);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  std::vector<CellRef> results;
  for (const CellRef slot : program.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  ASSERT_EQ(results.size(), calls);
  EXPECT_TRUE(std::ranges::all_of(
      results, [intCell](const CellRef result) { return result == intCell; }));
  EXPECT_EQ(h.arena.asInt64(intCell), 7);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  std::cout << "VQL_MUTATION_GUARD_PROFILE typed=" << typed
            << " calls=" << calls << " compile_us=" << compileElapsed.count()
            << " run_us=" << runElapsed.count()
            << " cycles=" << h.vm.totalCycles() - cyclesBefore
            << " arena_cell_delta=" << h.arena.cellCount() - cellsBefore
            << " emitted_opcodes=" << program.generatedOpcodes.size() << '\n';
}

TEST(VQLMutationGuardProfileTest, DISABLED_Scaling) {
  for (const std::size_t calls : {32U, 128U}) {
    runProfileCase(true, calls);
    runProfileCase(false, calls);
  }
}

} // namespace
} // namespace xanadu::vql
