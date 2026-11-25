#include "common/xanadu/vortex/io_scheduler.hpp" // IWYU pragma: associated

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace xanadu::vortex {

struct BoundedIoScheduler::Job final : CompletionSink {
  enum class State : std::uint8_t {
    Free,
    Reserved,
    Queued,
    Running,
    Completed
  };

  explicit Job(BoundedIoScheduler &owner) : owner(owner) {}

  void progress(const PrivateRequestToken request,
                const IoProgress value) noexcept override {
    std::scoped_lock lock(owner.mutex_);
    if (request != token || State::Running != state) {
      return;
    }
    latestProgress = value;
    hasProgress    = true;
  }

  void
  terminalCandidate(const PrivateRequestToken request, BackendResult result,
                    std::optional<PayloadLease> payload) noexcept override {
    if (request == token && sink != nullptr) {
      sink->terminalCandidate(request, std::move(result), std::move(payload));
    }
  }

  void releaseAcknowledged(
      const BackendReleaseAck acknowledgement) noexcept override {
    if (acknowledgement.token.request == token && sink != nullptr) {
      sink->releaseAcknowledged(acknowledgement);
    }
  }

  BoundedIoScheduler &owner;
  State state{State::Free};
  PrivateRequestToken token{};
  std::size_t reservedBytes{};
  std::size_t backendBytes{};
  std::optional<std::size_t> backendPayloadBytes{};
  BackendCommand command{};
  IoAdapter *adapter{};
  CompletionSink *sink{};
  std::stop_source cancellation;
  IoProgress latestProgress{};
  bool hasProgress{};
};

BoundedIoScheduler::BoundedIoScheduler(const BoundedIoSchedulerOptions options,
                                       PayloadResolver &payloads)
    : options_(options), payloads_(payloads) {
  if (0 == options_.workerCount || 0 == options_.maxOutstanding ||
      0 == options_.maxReservedBytes) {
    throw std::invalid_argument(
        "I/O scheduler bounds and worker count must be positive");
  }
  slots_.reserve(options_.maxOutstanding);
  for (std::size_t index = 0; index < options_.maxOutstanding; ++index) {
    slots_.push_back(std::make_unique<Job>(*this));
  }
  queue_.resize(options_.maxOutstanding);
  workers_.reserve(options_.workerCount);
  try {
    for (std::size_t index = 0; index < options_.workerCount; ++index) {
      workers_.emplace_back([this] { workerLoop(); });
    }
  } catch (...) {
    {
      const std::scoped_lock lock(mutex_);
      accepting_ = false;
    }
    wake_.notify_all();
    for (auto &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    throw;
  }
}

BoundedIoScheduler::~BoundedIoScheduler() {
  static_cast<void>(requestStop(ShutdownMode::CancelPending));
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

auto BoundedIoScheduler::reserve(const AdmissionCost cost)
    -> std::expected<SchedulerReservation, IoErrorInfo> {
  if (cost.terminalEntries != 1) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 1});
  }
  if (cost.commandBytes >
      std::numeric_limits<std::size_t>::max() - cost.completionBytes) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 2});
  }
  const auto bytes = cost.commandBytes + cost.completionBytes;
  const std::scoped_lock lock(mutex_);
  if (!accepting_) {
    return std::unexpected(IoErrorInfo{IoErrorCode::SchedulerUnavailable, 0});
  }
  if (bytes > options_.maxReservedBytes - reservedBytes_) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 3});
  }
  auto found = std::ranges::find_if(
      slots_, [](const auto &job) { return Job::State::Free == job->state; });
  if (found == slots_.end()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 4});
  }
  if (nextToken_ == 0) {
    return std::unexpected(IoErrorInfo{IoErrorCode::SchedulerUnavailable, 1});
  }

  auto &job         = **found;
  job.state         = Job::State::Reserved;
  job.token         = PrivateRequestToken{nextToken_++};
  job.reservedBytes = bytes;
  job.backendBytes  = cost.completionBytes;
  job.backendPayloadBytes = cost.payloadBytes;
  job.hasProgress   = false;
  job.cancellation  = std::stop_source{};
  reservedBytes_ += bytes;
  return SchedulerReservation{job.token, bytes};
}

auto BoundedIoScheduler::enqueue(const SchedulerReservation reservation,
                                 BackendCommand command, IoAdapter &adapter,
                                 CompletionSink &sink) noexcept
    -> EnqueueResult {
  {
    const std::scoped_lock lock(mutex_);
    auto *job = findJob(reservation.token);
    if (!accepting_ || job == nullptr || Job::State::Reserved != job->state ||
        reservation.reservedBytes != job->reservedBytes ||
        command.token != reservation.token || queueSize_ == queue_.size() ||
        command.operation.bytes.size() > job->reservedBytes) {
      return EnqueueResult::ReservationInvariantViolated;
    }
    job->command    = std::move(command);
    job->adapter    = &adapter;
    job->sink       = &sink;
    job->state      = Job::State::Queued;
    const auto tail = (queueHead_ + queueSize_) % queue_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (slots_[index].get() == job) {
        queue_[tail] = index;
        break;
      }
    }
    ++queueSize_;
  }
  wake_.notify_one();
  return EnqueueResult::Queued;
}

void BoundedIoScheduler::releaseUnused(
    const SchedulerReservation reservation) noexcept {
  const std::scoped_lock lock(mutex_);
  auto *job = findJob(reservation.token);
  if (job == nullptr || reservation.reservedBytes != job->reservedBytes ||
      (Job::State::Reserved != job->state &&
       Job::State::Completed != job->state)) {
    return;
  }
  reservedBytes_ -= job->reservedBytes;
  job->command       = BackendCommand{};
  job->adapter       = nullptr;
  job->sink          = nullptr;
  job->reservedBytes = 0;
  job->backendBytes  = 0;
  job->backendPayloadBytes.reset();
  job->hasProgress   = false;
  job->state         = Job::State::Free;
}

auto BoundedIoScheduler::cancel(const PrivateRequestToken token) noexcept
    -> CancelQueuedResult {
  Job *cancelled = nullptr;
  {
    const std::scoped_lock lock(mutex_);
    auto *job = findJob(token);
    if (job == nullptr) {
      return CancelQueuedResult::AlreadyTerminal;
    }
    if (Job::State::Completed == job->state) {
      return CancelQueuedResult::AlreadyTerminal;
    }
    if (Job::State::Running == job->state) {
      job->cancellation.request_stop();
      return CancelQueuedResult::AlreadyRunning;
    }
    if (Job::State::Queued != job->state) {
      return CancelQueuedResult::AlreadyTerminal;
    }
    std::size_t offset = queueSize_;
    for (std::size_t index = 0; index < queueSize_; ++index) {
      const auto queueIndex = (queueHead_ + index) % queue_.size();
      if (slots_[queue_[queueIndex]].get() == job) {
        offset = index;
        break;
      }
    }
    if (offset == queueSize_) {
      return CancelQueuedResult::AlreadyRunning;
    }
    for (std::size_t index = offset; index + 1 < queueSize_; ++index) {
      queue_[(queueHead_ + index) % queue_.size()] =
          queue_[(queueHead_ + index + 1) % queue_.size()];
    }
    --queueSize_;
    job->state = Job::State::Completed;
    cancelled  = job;
  }
  cancelQueued(*cancelled);
  return CancelQueuedResult::RemovedBeforeStart;
}

auto BoundedIoScheduler::pump(const std::size_t completionBudget)
    -> std::size_t {
  std::size_t delivered = 0;
  while (delivered < completionBudget) {
    CompletionSink *sink = nullptr;
    PrivateRequestToken token{};
    IoProgress progress{};
    {
      const std::scoped_lock lock(mutex_);
      std::size_t found = slots_.size();
      for (std::size_t offset = 0; offset < slots_.size(); ++offset) {
        const auto index = (progressCursor_ + offset) % slots_.size();
        if (slots_[index]->hasProgress && slots_[index]->sink != nullptr) {
          found = index;
          break;
        }
      }
      if (found == slots_.size()) {
        break;
      }
      auto &job       = *slots_[found];
      job.hasProgress = false;
      sink            = job.sink;
      token           = job.token;
      progress        = job.latestProgress;
      progressCursor_ = (found + 1) % slots_.size();
    }
    sink->progress(token, progress);
    ++delivered;
  }
  return delivered;
}

auto BoundedIoScheduler::requestStop(const ShutdownMode mode)
    -> ShutdownResult {
  std::vector<Job *> cancelled;
  cancelled.reserve(options_.maxOutstanding);
  ShutdownResult result{};
  {
    const std::scoped_lock lock(mutex_);
    accepting_ = false;
    if (ShutdownMode::CancelPending == mode) {
      while (queueSize_ > 0) {
        const auto slot = queue_[queueHead_];
        queueHead_      = (queueHead_ + 1) % queue_.size();
        --queueSize_;
        auto &job = *slots_[slot];
        if (Job::State::Queued == job.state) {
          job.state = Job::State::Completed;
          cancelled.push_back(&job);
        }
      }
      for (const auto &job : slots_) {
        if (Job::State::Running == job->state) {
          job->cancellation.request_stop();
        }
      }
    }
    result.queued   = queueSize_;
    result.reserved = static_cast<std::size_t>(
        std::ranges::count_if(slots_, [](const auto &job) {
          return Job::State::Free != job->state;
        }));
    result.running = static_cast<std::size_t>(
        std::ranges::count_if(slots_, [](const auto &job) {
          return Job::State::Running == job->state;
        }));
    result.stoppedAccepting = true;
  }
  for (auto *job : cancelled) {
    cancelQueued(*job);
  }
  wake_.notify_all();
  return result;
}

void BoundedIoScheduler::workerLoop() {
  while (true) {
    Job *job = nullptr;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return !accepting_ || queueSize_ > 0; });
      if (0 == queueSize_) {
        if (!accepting_) {
          return;
        }
        continue;
      }
      const auto slot = queue_[queueHead_];
      queueHead_      = (queueHead_ + 1) % queue_.size();
      --queueSize_;
      job = slots_[slot].get();
      if (Job::State::Queued != job->state) {
        continue;
      }
      job->state = Job::State::Running;
    }

    BackendContext context{
        job->cancellation.get_token(), payloads_,
        BackendBudget{std::min(options_.backendMaxBytes, job->backendBytes),
                      job->backendPayloadBytes}};
    job->adapter->execute(job->command, context, *job);

    const auto token = job->token;
    auto *sink       = job->sink;
    {
      const std::scoped_lock lock(mutex_);
      job->state = Job::State::Completed;
    }
    sink->releaseAcknowledged(BackendReleaseAck{PrivateReleaseToken{token}});
  }
}

auto BoundedIoScheduler::findJob(const PrivateRequestToken token) noexcept
    -> Job * {
  const auto found = std::ranges::find_if(slots_, [token](const auto &job) {
    return Job::State::Free != job->state && token == job->token;
  });
  return found == slots_.end() ? nullptr : found->get();
}

void BoundedIoScheduler::cancelQueued(Job &job) noexcept {
  auto *sink       = job.sink;
  const auto token = job.token;
  const IoProgress progress{0, 0, 0, std::nullopt, IoPhase::Terminal};
  BackendResult result{Cancelled{std::nullopt, progress, false}, {}};
  sink->terminalCandidate(token, std::move(result), std::nullopt);
  sink->releaseAcknowledged(BackendReleaseAck{PrivateReleaseToken{token}});
}

} // namespace xanadu::vortex
