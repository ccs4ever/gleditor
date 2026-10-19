/**
 * @file vortex_worker_pool_test.cpp
 * @brief Unit tests for the Vortex worker pool (Stage 2 multithreading).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "common/xanadu/vortex/vortex_worker_pool.hpp"

namespace {

using zigzag::vortex::VortexWorkerPool;

// (1) A pool of one starts no threads and runs every index on the caller.
TEST(VortexWorkerPoolTest, PoolOfOneRunsOnCallerWithNoThreads) {
  VortexWorkerPool pool(1);
  EXPECT_EQ(pool.threadCount(), 0u);
  EXPECT_EQ(pool.parallelism(), 1u);

  const std::thread::id caller  = std::this_thread::get_id();
  constexpr std::uint32_t count = 32;
  std::vector<char> visited(count, 0);
  std::vector<std::thread::id> ran(count);

  pool.run(count, [&](std::uint32_t i) {
    visited[i] = 1;
    ran[i]     = std::this_thread::get_id();
  });

  for (std::uint32_t i = 0; i < count; i++) {
    EXPECT_EQ(visited[i], 1) << "index " << i << " not visited";
    EXPECT_EQ(ran[i], caller) << "index " << i << " ran off the caller thread";
  }
}

// (2) run() over N indices visits every index exactly once, across thread
// counts. Each index is distinct, so writing visited[i] is race-free.
TEST(VortexWorkerPoolTest, VisitsEveryIndexExactlyOnce) {
  for (std::uint32_t threads : {2u, 4u, 8u}) {
    VortexWorkerPool pool(threads);
    EXPECT_EQ(pool.threadCount(), threads - 1);
    EXPECT_EQ(pool.parallelism(), threads);

    constexpr std::uint32_t count = 1000;
    std::vector<char> visited(count, 0);
    std::atomic<std::uint32_t> total{0};

    pool.run(count, [&](std::uint32_t i) {
      visited[i] = static_cast<char>(visited[i] + 1);
      total.fetch_add(1, std::memory_order_relaxed);
    });

    EXPECT_EQ(total.load(std::memory_order_relaxed), count)
        << "threads=" << threads;
    for (std::uint32_t i = 0; i < count; i++) {
      EXPECT_EQ(visited[i], 1)
          << "threads=" << threads << " index=" << i << " visit count wrong";
    }
  }
}

// (3) A throwing work item causes run() to rethrow after all others complete.
TEST(VortexWorkerPoolTest, RethrowsAfterAllComplete) {
  VortexWorkerPool pool(4);

  constexpr std::uint32_t count   = 256;
  constexpr std::uint32_t thrower = 100;
  std::atomic<std::uint32_t> completed{0};

  EXPECT_THROW(
      {
        pool.run(count, [&](std::uint32_t i) {
          if (i == thrower) {
            throw std::runtime_error("boom");
          }
          completed.fetch_add(1, std::memory_order_relaxed);
        });
      },
      std::runtime_error);

  // Every non-throwing index still ran before run() returned.
  EXPECT_EQ(completed.load(std::memory_order_relaxed), count - 1);
}

// (4) Per-index results written inside work() are all visible once run()
// returns: run() establishes a happens-before edge from each worker to the
// caller.
TEST(VortexWorkerPoolTest, ResultsVisibleAfterRun) {
  VortexWorkerPool pool(8);

  constexpr std::uint32_t count = 4096;
  std::vector<std::uint64_t> results(count, 0);

  pool.run(count, [&](std::uint32_t i) {
    results[i] = static_cast<std::uint64_t>(i) * 2 + 1;
  });

  for (std::uint32_t i = 0; i < count; i++) {
    EXPECT_EQ(results[i], static_cast<std::uint64_t>(i) * 2 + 1)
        << "index " << i << " result not visible";
  }
}

} // namespace
