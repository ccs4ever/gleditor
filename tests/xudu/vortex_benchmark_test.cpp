/**
 * @file vortex_benchmark_test.cpp
 * @brief Performance and throughput benchmarks for the Vortex Hyperstructural
 * Runtime Core.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "xudu/core/vortex.hpp"
#include "zigzag/core/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using namespace zigzag::vortex;

TEST(VortexBenchmarkTest, LinkPrimitiveThroughput) {
  constexpr std::size_t OpsCount = 50000;
  ArenaManifold arena;
  VortexCore core{arena};
  DimRef testDim = core.dims().step;

  CellRef root = arena.makeCell("bench_root");

  const auto t0 = std::chrono::steady_clock::now();
  CellRef cur   = root;
  for (std::size_t i = 0; i < OpsCount; ++i) {
    auto next =
        core.link(cur, testDim, DimVector::POS, static_cast<CellRef>(-1));
    cur = *next;
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto durationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  const double opsPerSec =
      (static_cast<double>(OpsCount) / static_cast<double>(durationUs)) *
      1000000.0;
  std::cout << "[ BENCHMARK ] VortexCore link allocation: " << OpsCount
            << " links in " << durationUs << " us ("
            << static_cast<std::uint64_t>(opsPerSec) << " ops/sec)\n";

  EXPECT_GT(opsPerSec, 100000.0);
}

TEST(VortexBenchmarkTest, ValueSpliceAndSliceThroughput) {
  constexpr std::size_t OpsCount = 20000;
  ArenaManifold arena;
  VortexCore core{arena};

  CellRef doc = arena.makeCell("Alpha Beta Gamma Delta Epsilon Zeta Eta Theta");

  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < OpsCount; ++i) {
    // Splicing
    core.value(doc, 6, 4, CellValue(std::string("Omega")));
    // Slicing
    auto slice = core.value(doc, 0, 15);
    (void)slice;
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto durationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  const double opsPerSec =
      (static_cast<double>(OpsCount * 2) / static_cast<double>(durationUs)) *
      1000000.0;
  std::cout << "[ BENCHMARK ] VortexCore value splice & slice: "
            << (OpsCount * 2) << " ops in " << durationUs << " us ("
            << static_cast<std::uint64_t>(opsPerSec) << " ops/sec)\n";

  EXPECT_GT(opsPerSec, 50000.0);
}

TEST(VortexBenchmarkTest, VMInstructionExecutionRate) {
  constexpr std::size_t LoopSteps = 20000;
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  // Build a tight loop:
  // #ADD acc one acc
  // #SUB count one count
  // #BRANCH count loop
  CellRef acc   = arena.makeScalarCell(static_cast<std::int64_t>(0));
  CellRef one   = arena.makeScalarCell(static_cast<std::int64_t>(1));
  CellRef count = arena.makeScalarCell(static_cast<std::int64_t>(LoopSteps));

  CellRef addOp = vm.mintOpcode(OpcodeKind::Add, "#ADD");
  core.bindInput(addOp, acc);
  core.bindInput(addOp, one);
  core.bindOutput(addOp, acc);

  CellRef subOp = vm.mintOpcode(OpcodeKind::Sub, "#SUB");
  core.bindInput(subOp, count);
  core.bindInput(subOp, one);
  core.bindOutput(subOp, count);

  CellRef branchOp = vm.mintOpcode(OpcodeKind::Branch, "#BRANCH");
  core.bindInput(branchOp, count);
  CellRef loopTarget = arena.makeScalarCell(static_cast<std::int64_t>(addOp));
  core.bindInput(branchOp, loopTarget);

  arena.link(addOp, core.dims().spin, false, subOp);
  arena.link(subOp, core.dims().spin, false, branchOp);

  CellRef cursor = vm.spawnCursor(addOp, "perf_loop");

  const auto t0 = std::chrono::steady_clock::now();
  auto res      = vm.run(cursor, LoopSteps * 4);
  const auto t1 = std::chrono::steady_clock::now();
  const auto durationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  EXPECT_TRUE(res.success);
  EXPECT_EQ(core.render(acc), CellValue(static_cast<std::int64_t>(LoopSteps)));

  const std::size_t totalInstructions = LoopSteps * 3;
  const double instrsPerSec = (static_cast<double>(totalInstructions) /
                               static_cast<double>(durationUs)) *
                              1000000.0;
  std::cout << "[ BENCHMARK ] VortexVM execution rate: " << totalInstructions
            << " instructions in " << durationUs << " us ("
            << static_cast<std::uint64_t>(instrsPerSec) << " instrs/sec)\n";

  EXPECT_GT(instrsPerSec, 50000.0);
}

TEST(VortexBenchmarkTest, MemoizedVsUnmemoizedExecution) {
  constexpr std::size_t QueryCount = 5000;
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  // Complex computation: #MUL a b
  CellRef mulOp = vm.mintOpcode(OpcodeKind::Mul, "#EXPENSIVE_MUL");
  CellRef a     = arena.makeScalarCell(static_cast<std::int64_t>(12345));
  CellRef b     = arena.makeScalarCell(static_cast<std::int64_t>(67890));
  core.bindInput(mulOp, a);
  core.bindInput(mulOp, b);
  CellRef outTarget = arena.makeCell();
  core.bindOutput(mulOp, outTarget);

  vm.enableMemoization(mulOp, "bench:mul");

  // First run primes the cache
  CellRef c0 = vm.spawnCursor(mulOp, "prime");
  vm.step(c0);

  // Measure 5000 cache-hit executions
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t q = 0; q < QueryCount; ++q) {
    CellRef cursor = vm.spawnCursor(mulOp, "query");
    vm.step(cursor);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto durationUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  const double hitsPerSec =
      (static_cast<double>(QueryCount) / static_cast<double>(durationUs)) *
      1000000.0;
  std::cout << "[ BENCHMARK ] Vortex memoization cache hit rate: " << QueryCount
            << " lookups in " << durationUs << " us ("
            << static_cast<std::uint64_t>(hitsPerSec) << " hits/sec)\n";

  EXPECT_GT(hitsPerSec, 50000.0);
}

} // namespace
