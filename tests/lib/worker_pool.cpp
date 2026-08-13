/**
 * @file worker_pool.cpp
 * @brief Tests for the fixed-size recording thread pool.
 */
#include <gtest/gtest.h>

#include <gleditor/render/worker_pool.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

/// How long a test waits for the other shares of a batch to start before
/// giving up. Generous: this is a liveness bound, not a timing assertion.
constexpr auto startupLimit = std::chrono::seconds{5};

} // namespace

TEST(WorkerPool, runsEveryIndexExactlyOnce) {
  for (const std::uint32_t threads : {1U, 2U, 3U, 8U}) {
    for (const std::uint32_t count : {1U, 2U, 7U, 64U, 1000U}) {
      render::WorkerPool pool(threads);
      std::vector<std::atomic_int> runs(count);
      pool.run(count, [&runs](const std::uint32_t i) { runs[i]++; });
      for (std::uint32_t i = 0; i < count; i++) {
        EXPECT_EQ(1, runs[i].load())
            << "index " << i << " with " << threads << " threads";
      }
    }
  }
}

TEST(WorkerPool, doesNothingForAnEmptyBatch) {
  render::WorkerPool pool(4);
  std::atomic_int runs{0};
  pool.run(0, [&runs](std::uint32_t) { runs++; });
  EXPECT_EQ(0, runs.load());
}

// A pool of one is the shape a device without parallel recording asks for. It
// must still run the work rather than quietly drop it or deadlock waiting for
// a worker that was never started.
TEST(WorkerPool, aPoolOfOneRunsOnTheCallingThread) {
  render::WorkerPool pool(1);
  EXPECT_EQ(0U, pool.threadCount());
  EXPECT_EQ(1U, pool.parallelism());

  std::set<std::thread::id> seen;
  pool.run(16,
           [&seen](std::uint32_t) { seen.insert(std::this_thread::get_id()); });
  ASSERT_EQ(1U, seen.size());
  EXPECT_EQ(std::this_thread::get_id(), *seen.begin());
}

// The point of the pool. Each share blocks until every other share has started,
// which only completes if they really are running at the same time.
TEST(WorkerPool, sharesRunConcurrently) {
  render::WorkerPool pool(4);
  const auto count = pool.parallelism();

  std::atomic_uint32_t started{0};
  std::atomic_bool timedOut{false};
  std::mutex guard;
  std::set<std::thread::id> threads;

  pool.run(count, [&](std::uint32_t) {
    {
      const std::lock_guard locker(guard);
      threads.insert(std::this_thread::get_id());
    }
    started++;
    const auto deadline = std::chrono::steady_clock::now() + startupLimit;
    while (started.load() < count) {
      if (std::chrono::steady_clock::now() > deadline) {
        timedOut = true;
        return;
      }
      std::this_thread::yield();
    }
  });

  EXPECT_FALSE(timedOut.load())
      << "shares did not overlap; the pool serialised them";
  EXPECT_EQ(count, threads.size());
}

// The caller takes a share instead of parking, so a batch of one does not pay
// for a handoff and a pool larger than the batch does not idle.
TEST(WorkerPool, theCallerTakesAShare) {
  render::WorkerPool pool(4);
  std::mutex guard;
  std::set<std::thread::id> threads;
  pool.run(pool.parallelism(), [&](std::uint32_t) {
    const std::lock_guard locker(guard);
    threads.insert(std::this_thread::get_id());
  });
  EXPECT_TRUE(threads.contains(std::this_thread::get_id()));
}

// A half-recorded command buffer that another thread is still writing to is
// worse than the failure that started it, so every share finishes before the
// exception reaches the caller.
TEST(WorkerPool, everyShareFinishesBeforeAThrowPropagates) {
  render::WorkerPool pool(4);
  constexpr std::uint32_t count = 200;
  std::atomic_uint32_t finished{0};

  EXPECT_THROW(
      {
        pool.run(count, [&finished](const std::uint32_t i) {
          if (0 == i % 7) {
            throw std::runtime_error("recording failed");
          }
          finished++;
        });
      },
      std::runtime_error);

  // 29 of the 200 indices are multiples of seven and threw; the rest ran.
  EXPECT_EQ(count - 29, finished.load());
}

// A pool is reused for every frame, so a batch that threw must leave it usable.
TEST(WorkerPool, survivesAFailedBatch) {
  render::WorkerPool pool(4);
  EXPECT_THROW(pool.run(8,
                        [](std::uint32_t) {
                          throw std::runtime_error("recording failed");
                        }),
               std::runtime_error);

  std::vector<std::atomic_int> runs(8);
  pool.run(8, [&runs](const std::uint32_t i) { runs[i]++; });
  for (const auto &run : runs) {
    EXPECT_EQ(1, run.load());
  }
}

// Batch after batch, since the pool parks its threads between them and a
// missed wake-up would show up as a hang or a dropped index rather than as a
// wrong answer.
TEST(WorkerPool, handlesManyBatchesInARow) {
  render::WorkerPool pool(4);
  std::atomic_int total{0};
  for (int batch = 0; batch < 500; batch++) {
    pool.run(9,
             [&total](const std::uint32_t i) { total += static_cast<int>(i); });
  }
  // 0 + 1 + ... + 8 is 36, five hundred times over.
  EXPECT_EQ(36 * 500, total.load());
}

// vi: set sw=2 sts=2 ts=2 et:
