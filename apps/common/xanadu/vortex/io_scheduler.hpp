#ifndef XANADU_VORTEX_IO_SCHEDULER_HPP
#define XANADU_VORTEX_IO_SCHEDULER_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/xanadu/vortex/io_protocol.hpp"

namespace xanadu::vortex {

struct SchedulerReservation {
  PrivateRequestToken token;
  std::size_t reservedBytes{};
};

enum class EnqueueResult : std::uint8_t {
  Queued,
  ReservationInvariantViolated,
};
enum class CancelQueuedResult : std::uint8_t {
  RemovedBeforeStart,
  AlreadyRunning,
  AlreadyTerminal,
};
enum class ShutdownMode : std::uint8_t { Drain, CancelPending };

struct ShutdownResult {
  std::size_t queued{};
  std::size_t running{};
  std::size_t reserved{};
  bool stoppedAccepting{};
};

class IoScheduler {
public:
  virtual auto reserve(AdmissionCost cost)
      -> std::expected<SchedulerReservation, IoErrorInfo> = 0;
  virtual auto enqueue(SchedulerReservation reservation, BackendCommand command,
                       IoAdapter &adapter, CompletionSink &sink) noexcept
      -> EnqueueResult                                                  = 0;
  virtual void releaseUnused(SchedulerReservation reservation) noexcept = 0;
  virtual auto cancel(PrivateRequestToken token) noexcept
      -> CancelQueuedResult                                      = 0;
  virtual auto pump(std::size_t completionBudget) -> std::size_t = 0;
  virtual auto requestStop(ShutdownMode mode) -> ShutdownResult  = 0;
  virtual ~IoScheduler()                                         = default;
};

struct BoundedIoSchedulerOptions {
  std::size_t workerCount{2};
  std::size_t maxOutstanding{64};
  std::size_t maxReservedBytes{16U * 1024U * 1024U};
  std::size_t backendMaxBytes{16U * 1024U * 1024U};
};

/// A fixed worker pool with reservations charged through owner publication.
/// Workers execute only immutable prepared commands; callbacks into the service
/// are the thread-safe CompletionSink methods.
class BoundedIoScheduler final : public IoScheduler {
public:
  BoundedIoScheduler(BoundedIoSchedulerOptions options,
                     PayloadResolver &payloads);
  ~BoundedIoScheduler() override;
  BoundedIoScheduler(const BoundedIoScheduler &)            = delete;
  BoundedIoScheduler &operator=(const BoundedIoScheduler &) = delete;

  auto reserve(AdmissionCost cost)
      -> std::expected<SchedulerReservation, IoErrorInfo> override;
  auto enqueue(SchedulerReservation reservation, BackendCommand command,
               IoAdapter &adapter, CompletionSink &sink) noexcept
      -> EnqueueResult override;
  void releaseUnused(SchedulerReservation reservation) noexcept override;
  auto cancel(PrivateRequestToken token) noexcept
      -> CancelQueuedResult override;
  auto pump(std::size_t completionBudget) -> std::size_t override;
  auto requestStop(ShutdownMode mode) -> ShutdownResult override;

private:
  struct Job;
  void workerLoop();
  [[nodiscard]] Job *findJob(PrivateRequestToken token) noexcept;
  void cancelQueued(Job &job) noexcept;

  BoundedIoSchedulerOptions options_;
  PayloadResolver &payloads_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<std::unique_ptr<Job>> slots_;
  std::vector<std::size_t> queue_;
  std::size_t queueHead_{};
  std::size_t queueSize_{};
  std::size_t progressCursor_{};
  std::vector<std::thread> workers_;
  std::uint64_t nextToken_{1};
  std::size_t reservedBytes_{};
  bool accepting_{true};
};

} // namespace xanadu::vortex

#endif
