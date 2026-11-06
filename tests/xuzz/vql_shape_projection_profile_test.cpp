/**
 * @file vql_shape_projection_profile_test.cpp
 * @brief Opt-in scaling profile for graph-native VQL shape masks.
 *
 * Run explicitly with --gtest_also_run_disabled_tests
 * --gtest_filter='*DISABLED_Scaling'.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
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

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
};

struct OwnerEdges {
  CellRef owner;
  CellRef keep;
  std::vector<std::pair<DimRef, CellRef>> dropped;
};

void runMaskProfile(const std::size_t ownerCount,
                    const std::size_t incidentDimensionCount) {
  Harness h;
  const DimRef keepDimension = h.coordinator.resolveDimension("d.keep");
  std::vector<DimRef> dropDimensions;
  dropDimensions.reserve(incidentDimensionCount - 1);
  for (std::size_t index = 0; index + 1 < incidentDimensionCount; ++index) {
    dropDimensions.push_back(
        h.coordinator.resolveDimension("d.drop." + std::to_string(index)));
  }

  std::vector<CellRef> owners;
  std::vector<OwnerEdges> expectedEdges;
  owners.reserve(ownerCount);
  expectedEdges.reserve(ownerCount);
  for (std::size_t ownerIndex = 0; ownerIndex < ownerCount; ++ownerIndex) {
    const CellRef owner = h.arena.makeCell("owner");
    const CellRef keep  = h.arena.makeCell("keep");
    zigzag::expectWritten(
        h.arena.link(owner, keepDimension, DimVector::POS, keep));

    OwnerEdges edges{.owner = owner, .keep = keep};
    edges.dropped.reserve(dropDimensions.size());
    for (const DimRef dimension : dropDimensions) {
      const CellRef neighbor = h.arena.makeCell("drop");
      zigzag::expectWritten(
          h.arena.link(owner, dimension, DimVector::POS, neighbor));
      edges.dropped.emplace_back(dimension, neighbor);
    }
    owners.push_back(owner);
    expectedEdges.push_back(std::move(edges));
  }

  CompilationOptions options;
  options.initialBindings = {{"owners", owners}};
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto cellsBeforeCompile = h.arena.cellCount();
  const auto compileStart       = Clock::now();
  const auto program            = compiler.compile("$owners{+d.keep}", options);
  const auto compileElapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            compileStart);
  ASSERT_TRUE(program.success) << program.errorMessage;
  ASSERT_TRUE(program.cursor);
  const auto cellsAfterCompile = h.arena.cellCount();

  const std::uint64_t cyclesBefore = h.vm.totalCycles();
  const auto runStart              = Clock::now();
  const auto execution             = h.vm.run(*program.cursor, 20'000'000);
  const auto runElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      Clock::now() - runStart);
  const auto cellsAfterRun = h.arena.cellCount();
  h.vm.setCursorOpcode(*program.cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  std::vector<CellRef> results;
  for (const CellRef slot : program.resultSlots) {
    const auto values = h.vm.resultCells(slot);
    results.insert(results.end(), values.begin(), values.end());
  }
  EXPECT_EQ(results, owners);

  for (const auto &edges : expectedEdges) {
    EXPECT_EQ(h.arena.linked(edges.owner, keepDimension, DimVector::POS),
              edges.keep);
    EXPECT_EQ(h.arena.linked(edges.keep, keepDimension, DimVector::NEG),
              edges.owner);
    for (const auto &[dimension, neighbor] : edges.dropped) {
      EXPECT_FALSE(h.arena.linked(edges.owner, dimension, DimVector::POS));
      EXPECT_FALSE(h.arena.linked(neighbor, dimension, DimVector::NEG));
    }
  }
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  std::cout << "VQL_SHAPE_PROJECTION_PROFILE owners=" << ownerCount
            << " incident_dims=" << incidentDimensionCount
            << " compile_us=" << compileElapsed.count()
            << " run_us=" << runElapsed.count()
            << " cycles=" << h.vm.totalCycles() - cyclesBefore
            << " compile_cell_delta=" << cellsAfterCompile - cellsBeforeCompile
            << " run_cell_delta=" << cellsAfterRun - cellsAfterCompile
            << " total_cell_delta=" << cellsAfterRun - cellsBeforeCompile
            << " emitted_opcodes=" << program.generatedOpcodes.size() << '\n';
}

TEST(VQLShapeProjectionProfileTest, DISABLED_Scaling) {
  for (const std::size_t ownerCount : {32U, 128U}) {
    for (const std::size_t incidentDimensionCount : {4U, 16U}) {
      runMaskProfile(ownerCount, incidentDimensionCount);
    }
  }
}

} // namespace
} // namespace xanadu::vql
