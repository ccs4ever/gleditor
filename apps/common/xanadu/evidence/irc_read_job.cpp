#include "common/xanadu/evidence/irc_read_job.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace xanadu::evidence {

struct IrcReadJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic_bool cancelled{};
  bool running{};
  std::optional<IrcReadCompletion> completed;
};

IrcReadJob::IrcReadJob() : impl_(std::make_unique<Impl>()) {}

IrcReadJob::~IrcReadJob() {
  cancel();
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool IrcReadJob::start(std::shared_ptr<IrcSession> session,
                       IrcReadOperation operation) {
  const std::lock_guard lock(impl_->mutex);
  if (!session || impl_->running || impl_->completed) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->cancelled = false;
  impl_->running   = true;
  try {
    impl_->worker = std::thread([state     = impl_.get(),
                                 session   = std::move(session),
                                 operation = std::move(operation)]() mutable {
      IrcReadCompletion completion;
      try {
        completion.identity      = session->identity();
        completion.pendingBounds = session->pendingReadBounds();
        std::visit(
            [&](auto &request) {
              using Request = std::decay_t<decltype(request)>;
              if constexpr (std::is_same_v<Request, IrcConnectOperation>) {
                auto result            = session->connect(state->cancelled);
                completion.connected   = result.connected;
                completion.observation = std::move(result.observation);
              } else if constexpr (std::is_same_v<Request, IrcJoinAuthority>) {
                completion.join = session->join(request, state->cancelled);
                completion.observation = completion.join->observation;
                if (completion.observation.serverAuthority.empty()) {
                  completion.observation.serverAuthority =
                      completion.identity.serverAuthority;
                  completion.observation.networkIncarnation =
                      completion.identity.networkIncarnation;
                  completion.observation.connectionIncarnation =
                      completion.identity.connectionIncarnation;
                  completion.observation.state = CaptureState::Partial;
                  completion.observation.gaps.push_back(
                      {{},
                       {},
                       "IRC membership operation had no captured response"});
                }
              } else if constexpr (std::is_same_v<Request,
                                                  IrcLiveReadRequest>) {
                auto result = session->pendingLive(
                    request.target, request.maxMessages, state->cancelled);
                completion.observation = std::move(result.observation);
                completion.readTicket  = std::move(result.readTicket);
              } else {
                auto result =
                    session->pendingHistory(request, state->cancelled);
                completion.observation = std::move(result.observation);
                completion.readTicket  = std::move(result.readTicket);
              }
            },
            operation);
      } catch (...) {
        completion.observation.state = CaptureState::Failed;
        completion.observation.serverAuthority =
            completion.identity.serverAuthority;
        completion.observation.networkIncarnation =
            completion.identity.networkIncarnation;
        completion.observation.connectionIncarnation =
            completion.identity.connectionIncarnation;
        completion.observation.gaps.push_back(
            {{}, {}, "IRC read operation failed unexpectedly"});
      }
      completion.cancelled = state->cancelled.load();
      const std::lock_guard completionLock(state->mutex);
      state->completed = std::move(completion);
      state->running   = false;
    });
  } catch (...) {
    impl_->running = false;
    return false;
  }
  return true;
}

void IrcReadJob::cancel() { impl_->cancelled = true; }

bool IrcReadJob::busy() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

std::optional<IrcReadCompletion> IrcReadJob::poll() {
  const std::lock_guard lock(impl_->mutex);
  if (!impl_->completed) return {};
  auto completion = std::move(impl_->completed);
  impl_->completed.reset();
  if (impl_->worker.joinable()) impl_->worker.join();
  completion->cancelled = completion->cancelled || impl_->cancelled.load();
  return completion;
}

} // namespace xanadu::evidence
