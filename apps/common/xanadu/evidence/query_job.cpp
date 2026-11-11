#include "common/xanadu/evidence/query_job.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace xanadu::evidence {

struct SqliteJoinJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic_bool cancelled{};
  bool running{};
  std::optional<SqliteJoinCompletion> completed;
};

SqliteJoinJob::SqliteJoinJob() : impl_(std::make_unique<Impl>()) {}
SqliteJoinJob::~SqliteJoinJob() {
  cancel();
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool SqliteJoinJob::start(SqliteJoinEvaluation evaluation) {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->running || impl_->completed) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->cancelled = false;
  impl_->running   = true;
  try {
    impl_->worker =
        std::thread([state = impl_.get(), evaluation = std::move(evaluation)] {
          SqliteJoinCompletion completion;
          try {
            completion.result = evaluateSqliteObservationJoin(evaluation);
          } catch (const std::exception &) {
            completion.result = std::unexpected(
                QueryCompositionError{QueryCompositionErrorCode::KernelFailure,
                                      "query evaluation failed"});
          } catch (...) {
            completion.result = std::unexpected(
                QueryCompositionError{QueryCompositionErrorCode::KernelFailure,
                                      "unknown query evaluation failure"});
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

void SqliteJoinJob::cancel() { impl_->cancelled = true; }

bool SqliteJoinJob::busy() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

std::optional<SqliteJoinCompletion> SqliteJoinJob::poll() {
  const std::lock_guard lock(impl_->mutex);
  if (!impl_->completed) return {};
  auto completion = std::move(impl_->completed);
  impl_->completed.reset();
  if (impl_->worker.joinable()) impl_->worker.join();
  completion->cancelled = completion->cancelled || impl_->cancelled.load();
  return completion;
}

} // namespace xanadu::evidence
