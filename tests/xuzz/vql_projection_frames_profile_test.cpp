/**
 * @file vql_projection_frames_profile_test.cpp
 * @brief Opt-in raw profile for native-frame VQL shape projections.
 *
 * Run explicitly with --gtest_also_run_disabled_tests
 * --gtest_filter='*DISABLED_NativeFrameProjection'. Measurements are raw
 * costs and make no speedup claim.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using Clock = std::chrono::steady_clock;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

constexpr std::uint64_t maxCycles = 20'000'000;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  void link(const CellRef owner, const DimRef dimension,
            const DimVector direction, const CellRef target) {
    zigzag::expectWritten(arena.link(owner, dimension, direction, target));
  }
};

std::optional<CellRef> stampInvoke(const Harness &h,
                                   const CompilationResult &program) {
  for (const CellRef opcode : program.generatedOpcodes) {
    if (h.arena.textOf(opcode) == "#INVOKE_FRAME stamp") return opcode;
  }
  return std::nullopt;
}

void runProjectionFrameProfile(const std::size_t ownerCount,
                               const bool typedParameter) {
  Harness h;
  const DimRef tagDimension  = h.coordinator.resolveDimension("d.tag");
  const DimRef keepDimension = h.coordinator.resolveDimension("d.keep");
  std::vector<CellRef> owners;
  owners.reserve(ownerCount);
  for (std::size_t index = 0; index < ownerCount; ++index) {
    const CellRef owner = h.arena.makeCell("owner " + std::to_string(index));
    const CellRef keep  = h.arena.makeCell("keep " + std::to_string(index));
    h.link(owner, keepDimension, DimVector::POS, keep);
    owners.push_back(owner);
  }

  CompilationOptions options;
  options.initialBindings.emplace("nodes", owners);
  const std::string_view signature =
      typedParameter ? "($node: Cell): Cell" : "($node): Cell";
  const std::string query = "def pure stamp" + std::string(signature) +
                            " := $node{ *, +d.tag: 7 }; "
                            "for $node in $nodes return stamp($node)";

  VQLCompiler compiler(h.coordinator, h.vm);
  const std::size_t cellsBeforeCompile = h.arena.cellCount();
  const auto compileStart              = Clock::now();
  const CompilationResult program      = compiler.compile(query, options);
  const auto compileElapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            compileStart);
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(program.cursor);
  const std::size_t cellsAfterCompile = h.arena.cellCount();

  const auto invoke = stampInvoke(h, program);
  ASSERT_TRUE(invoke) << "stamp must lower to its own native frame invocation";
  ASSERT_EQ(h.vm.getOpcodeKind(*invoke), OpcodeKind::InvokeFrame);
  const auto invokeInputs = h.core.inputsOf(*invoke);
  ASSERT_FALSE(invokeInputs.empty());
  const auto descriptor = h.vm.frameDescriptor(invokeInputs.front());
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  EXPECT_FALSE(descriptor->locals.empty());
  const std::size_t frameLocalCount = descriptor->locals.size();

  const std::uint64_t cyclesBefore = h.vm.totalCycles();
  const auto runStart              = Clock::now();
  const auto execution             = h.vm.run(*program.cursor, maxCycles);
  const auto runElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - runStart);
  const std::size_t cellsAfterRun = h.arena.cellCount();
  const std::uint64_t cyclesUsed  = h.vm.totalCycles() - cyclesBefore;
  h.vm.setCursorOpcode(*program.cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_LT(cyclesUsed, maxCycles);

  std::vector<CellRef> results;
  for (const CellRef slot : program.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  EXPECT_EQ(results, owners);

  std::unordered_set<CellRef> tagValues;
  tagValues.reserve(ownerCount);
  for (std::size_t index = 0; index < ownerCount; ++index) {
    const CellRef owner = owners[index];
    const auto tag      = h.arena.linked(owner, tagDimension, DimVector::POS);
    ASSERT_TRUE(tag) << "owner " << index << " has no projected tag";
    EXPECT_EQ(h.arena.asInt64(*tag), std::optional<std::int64_t>{7});
    EXPECT_TRUE(tagValues.insert(*tag).second)
        << "owner " << index << " reused another owner's scalar cell";
    EXPECT_TRUE(h.arena.linked(owner, keepDimension, DimVector::POS));
    EXPECT_EQ(h.arena.linked(*tag, tagDimension, DimVector::NEG), owner);
  }
  EXPECT_EQ(tagValues.size(), ownerCount);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  std::cout << "VQL_PROJECTION_FRAME_PROFILE parameter="
            << (typedParameter ? "Cell" : "untyped") << " owners=" << ownerCount
            << " compile_us=" << compileElapsed.count()
            << " run_us=" << runElapsed.count() << " vm_cycles=" << cyclesUsed
            << " compile_cell_delta=" << cellsAfterCompile - cellsBeforeCompile
            << " run_cell_delta=" << cellsAfterRun - cellsAfterCompile
            << " generated_opcodes=" << program.generatedOpcodes.size()
            << " stamp_frame_locals=" << frameLocalCount << '\n';
}

TEST(VQLProjectionFramesProfileTest, DISABLED_NativeFrameProjection) {
  for (const std::size_t ownerCount : {32U, 128U}) {
    runProjectionFrameProfile(ownerCount, false);
    runProjectionFrameProfile(ownerCount, true);
  }
}

} // namespace
} // namespace xanadu::vql
