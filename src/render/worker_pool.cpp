/**
 * @file worker_pool.cpp
 * @brief Fixed-size pool of threads for indexed work.
 */
#include <gleditor/render/worker_pool.hpp> // IWYU pragma: associated

#include <utility>

namespace render {

WorkerPool::WorkerPool(const std::uint32_t threads) {
  // One worker fewer than the requested parallelism: the caller takes a share
  // rather than parking while others work.
  const auto spawn = threads > 1 ? threads - 1 : 0;
  workers.reserve(spawn);
  for (std::uint32_t i = 0; i < spawn; i++) {
    workers.emplace_back([this] { workerLoop(); });
  }
}

WorkerPool::~WorkerPool() {
  {
    const std::lock_guard locker(mutex);
    stopping = true;
  }
  wake.notify_all();
  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void WorkerPool::runOne(const std::function<void(std::uint32_t)> &work,
                        const std::uint32_t index) {
  try {
    work(index);
  } catch (...) {
    const std::lock_guard failing(mutex);
    if (!failure) {
      failure = std::current_exception();
    }
  }
}

void WorkerPool::workerLoop() {
  std::uint64_t seen = 0;
  std::unique_lock locker(mutex);
  while (true) {
    wake.wait(locker, [this, &seen] {
      return stopping || (generation != seen && 0 != unclaimed);
    });
    if (stopping) {
      return;
    }
    // Claim one index and run it outside the lock, then come back for more.
    // Claiming one at a time rather than a contiguous share is what keeps an
    // uneven batch from leaving one thread with all the expensive entries.
    while (0 != unclaimed) {
      const auto index = --unclaimed;
      const auto *work = current;
      locker.unlock();
      runOne(*work, index);
      locker.lock();
      if (0 == --outstanding) {
        done.notify_all();
      }
    }
    seen = generation;
  }
}

void WorkerPool::run(const std::uint32_t count,
                     const std::function<void(std::uint32_t)> &work) {
  if (0 == count) {
    return;
  }
  if (workers.empty()) {
    // No handoff at all when there is nobody to hand off to.
    for (std::uint32_t i = 0; i < count; i++) {
      work(i);
    }
    return;
  }

  std::uint32_t mine = 0;
  {
    const std::lock_guard publishing(mutex);
    current     = &work;
    unclaimed   = count;
    outstanding = count;
    failure     = nullptr;
    generation++;
    // The caller's own share, claimed before anybody is woken. Waking first and
    // then racing the workers for an index would let them take the whole batch
    // while this thread was still acquiring the lock -- which for a batch of
    // one means paying for a handoff to run a single item.
    mine = --unclaimed;
  }
  // Notified with the lock released. Waking a thread that then has to block on
  // the mutex this thread still holds costs a second context switch per worker,
  // and the whole point of the split is measured in tens of microseconds.
  wake.notify_all();

  std::unique_lock locker(mutex);
  while (true) {
    locker.unlock();
    runOne(work, mine);
    locker.lock();
    if (0 == --outstanding) {
      done.notify_all();
    }
    if (0 == unclaimed) {
      break;
    }
    mine = --unclaimed;
  }

  done.wait(locker, [this] { return 0 == outstanding; });
  current = nullptr;

  if (failure) {
    // Every index has finished by now, so nothing is still writing to whatever
    // the work was building when this propagates.
    const auto raised = std::exchange(failure, nullptr);
    locker.unlock();
    std::rethrow_exception(raised);
  }
}

} // namespace render
