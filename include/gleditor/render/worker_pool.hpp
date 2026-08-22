/**
 * @file worker_pool.hpp
 * @brief A fixed set of threads a caller hands a batch of indexed work to.
 *
 * Deliberately not a general task queue. The one thing it does -- run the same
 * function for indices 0..count-1 and return when all of them have finished --
 * is what recording part of a frame on several threads needs, and nothing more:
 * the caller is blocked for the duration, so there is no lifetime question
 * about what the work captured, and results are visible to the caller when it
 * resumes.
 *
 * The threads are created once and parked between batches, because a frame's
 * recording is measured in hundreds of microseconds and starting a thread costs
 * a good fraction of that.
 */
#ifndef GLEDITOR_RENDER_WORKER_POOL_H
#define GLEDITOR_RENDER_WORKER_POOL_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace render {

/**
 * @class WorkerPool
 * @brief Runs indexed work across a fixed set of threads.
 */
class WorkerPool {
public:
  /**
   * @brief Start @p threads workers.
   *
   * A pool of one starts no threads at all and runs everything on the caller,
   * which is what makes "parallel recording is not available" cost nothing
   * rather than cost a handoff.
   */
  explicit WorkerPool(std::uint32_t threads);
  ~WorkerPool();

  WorkerPool(const WorkerPool &)            = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;
  WorkerPool(WorkerPool &&)                 = delete;
  WorkerPool &operator=(WorkerPool &&)      = delete;

  /// Workers this pool owns, not counting the calling thread.
  [[nodiscard]] std::uint32_t threadCount() const {
    return static_cast<std::uint32_t>(workers.size());
  }

  /// The most work run() can spread out at once: the workers plus the caller,
  /// which takes a share rather than waiting.
  [[nodiscard]] std::uint32_t parallelism() const {
    return static_cast<std::uint32_t>(workers.size()) + 1;
  }

  /**
   * @brief Call @p work(i) for every i in [0, @p count), and return once all
   *        of them have returned.
   *
   * The caller runs at least one share itself instead of blocking on the
   * workers, so a pool of N threads splits the work N+1 ways -- and a batch of
   * one is run here without waking anybody.
   *
   * If any call throws, one of the exceptions is rethrown here after every
   * other call has finished. That matters for a graphics backend: abandoning
   * half-recorded command buffers while other threads are still writing to
   * them would be worse than the original failure.
   */
  void run(std::uint32_t count, const std::function<void(std::uint32_t)> &work);

private:
  void workerLoop();
  /// Run one index, remembering the first exception rather than letting it out.
  /// Called with the mutex released.
  void runOne(const std::function<void(std::uint32_t)> &work,
              std::uint32_t index);

  std::vector<std::thread> workers;

  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable done;

  /// The batch currently being run. Null between batches.
  const std::function<void(std::uint32_t)> *current{};
  /// Indices of the current batch not yet claimed, and how many are still
  /// running. A batch is finished when both reach zero.
  std::uint32_t unclaimed{};
  std::uint32_t outstanding{};
  /// Incremented per batch so a worker that wakes spuriously can tell whether
  /// there is anything new to do.
  std::uint64_t generation{};
  std::exception_ptr failure;
  bool stopping{};
};

} // namespace render

#endif // GLEDITOR_RENDER_WORKER_POOL_H
