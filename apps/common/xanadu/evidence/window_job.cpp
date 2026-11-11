#include "common/xanadu/evidence/window_job.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace xanadu::evidence {

struct ObservationWindowJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic_bool cancelled{};
  bool running{};
  std::optional<ObservationWindowCompletion> completed;
};

ObservationWindowJob::ObservationWindowJob()
    : impl_(std::make_unique<Impl>()) {}
ObservationWindowJob::~ObservationWindowJob() {
  cancel();
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool ObservationWindowJob::start(ObservationWindowEvaluation evaluation) {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->running || impl_->completed) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->cancelled = false;
  impl_->running   = true;
  try {
    impl_->worker =
        std::thread([state = impl_.get(), evaluation = std::move(evaluation)] {
          ObservationWindowCompletion completion;
          try {
            completion.result = evaluateObservationWindow(evaluation);
          } catch (...) {
            completion.result = std::unexpected(WindowCompositionError{
                WindowCompositionErrorCode::KernelFailure,
                "window evaluation failed"});
          }
          const std::lock_guard completionLock(state->mutex);
          completion.cancelled = state->cancelled.load();
          state->completed     = std::move(completion);
          state->running       = false;
        });
  } catch (...) {
    impl_->running = false;
    return false;
  }
  return true;
}

void ObservationWindowJob::cancel() { impl_->cancelled = true; }

bool ObservationWindowJob::busy() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

std::optional<ObservationWindowCompletion> ObservationWindowJob::poll() {
  const std::lock_guard lock(impl_->mutex);
  if (!impl_->completed) return {};
  auto completion = std::move(impl_->completed);
  impl_->completed.reset();
  if (impl_->worker.joinable()) impl_->worker.join();
  completion->cancelled = completion->cancelled || impl_->cancelled.load();
  return completion;
}

} // namespace xanadu::evidence
