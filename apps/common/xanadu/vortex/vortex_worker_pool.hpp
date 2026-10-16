/**
 * @file vortex_worker_pool.hpp
 * @brief A fixed set of threads a Vortex caller hands a batch of indexed work
 *        to.
 *
 * A sibling of render::WorkerPool, kept separate on purpose: the engine
 * (apps/common/xanadu) has no graphics device and must not depend on the render
 * library. The design is the same proven one -- fixed threads started once and
 * parked between batches, a pool of one that starts no threads and runs
 * everything on the caller, and a run() that spreads indices 0..count-1 over
 * the workers plus the caller and returns when all of them have finished.
 *
 * Deliberately not a general task queue. The caller is blocked for the duration
 * of run(), so there is no lifetime question about what the work captured, and
 * results are visible to the caller when it resumes.
 */
#ifndef COMMON_XANADU_VORTEX_WORKER_POOL_HPP
#define COMMON_XANADU_VORTEX_WORKER_POOL_HPP

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <gleditor/cpp26.hpp>

namespace zigzag::vortex {

/**
 * @class VortexWorkerPool
 * @brief Runs indexed work across a fixed set of threads for the Vortex VM.
 */
class VortexWorkerPool {
public:
  /**
   * @brief Start @p threads workers.
   *
   * A pool of one starts no threads at all and runs everything on the caller,
   * which is what makes "parallel execution is not available" cost nothing
   * rather than cost a handoff.
   */
  explicit VortexWorkerPool(std::uint32_t threads);
  ~VortexWorkerPool();

  VortexWorkerPool(const VortexWorkerPool &)            = delete;
  VortexWorkerPool &operator=(const VortexWorkerPool &) = delete;
  VortexWorkerPool(VortexWorkerPool &&)                 = delete;
  VortexWorkerPool &operator=(VortexWorkerPool &&)      = delete;

  /// Workers this pool owns, not counting the calling thread.
  [[nodiscard]] std::uint32_t threadCount() const {
    return static_cast<std::uint32_t>(workers.size());
  }

  /// The most work run() can spread out at once: the workers plus the caller,
  /// which takes a share rather than waiting.
  [[nodiscard]] std::uint32_t parallelism() const {
    return static_cast<std::uint32_t>(workers.size()) + 1;
  }

  /// One index of a batch. Borrowed for the duration of run().
  using Work = gleditor::cpp26::function_ref<void(std::uint32_t)>;

  /**
   * @brief Call @p work(i) for every i in [0, @p count), and return once all
   *        of them have returned.
   *
   * The caller runs at least one share itself instead of blocking on the
   * workers, so a pool of N threads splits the work N+1 ways -- and a batch of
   * one is run here without waking anybody.
   *
   * If any call throws, one of the exceptions is rethrown here after every
   * other call has finished, so nothing the batch was building is left in a
   * half-written state while another thread is still touching it.
   *
   * @p work is borrowed, not owned: a function_ref rather than a
   * std::function. The pool keeps only a pointer to it, and only until this
   * call returns.
   */
  void run(std::uint32_t count, Work work);

private:
  void workerLoop();
  /// Run one index, remembering the first exception rather than letting it out.
  /// Called with the mutex released.
  void runOne(Work work, std::uint32_t index);

  std::vector<std::thread> workers;

  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable done;

  /// The batch currently being run. Null between batches.
  const Work *current{};
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

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_WORKER_POOL_HPP
