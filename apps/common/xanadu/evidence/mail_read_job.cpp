#include "common/xanadu/evidence/mail_read_job.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace xanadu::evidence {

struct MailReadJob::Impl {
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic_bool cancelled{};
  bool running{};
  std::optional<MailReadCompletion> completed;
};

MailReadJob::MailReadJob() : impl_(std::make_unique<Impl>()) {}

MailReadJob::~MailReadJob() {
  cancel();
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool MailReadJob::start(ImapReadProfile profile, ImapReadRequest request,
                        MailCredentialLease credential) {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->running || impl_->completed) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->cancelled = false;
  impl_->running   = true;
  try {
    impl_->worker = std::thread(
        [state = impl_.get(), profile = std::move(profile),
         request = std::move(request), credential = std::move(credential)] {
          MailReadCompletion completion;
          try {
            completion.observation =
                readImapMailbox(profile, request, credential, state->cancelled);
          } catch (...) {
            completion.observation.state  = CaptureState::Failed;
            completion.observation.bounds = request.retentionBounds;
            try {
              if (!imapReadProblem(profile, request)) {
                const auto authority = imapServerAuthority(profile.serverUrl);
                if (authority && *authority == profile.serverAuthority) {
                  completion.observation.serverAuthority = *authority;
                  completion.observation.accountScope    = profile.accountScope;
                  completion.observation.mailbox         = profile.mailbox;
                  completion.observation.coverage =
                      "IMAP read failed before UIDVALIDITY was observed";
                }
              }
              completion.observation.gaps.push_back(
                  {"transport", "IMAP read failed unexpectedly", {}});
            } catch (...) {
            }
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

void MailReadJob::cancel() { impl_->cancelled = true; }

bool MailReadJob::busy() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

std::optional<MailReadCompletion> MailReadJob::poll() {
  const std::lock_guard lock(impl_->mutex);
  if (!impl_->completed) return {};
  auto completion = std::move(impl_->completed);
  impl_->completed.reset();
  if (impl_->worker.joinable()) impl_->worker.join();
  completion->cancelled = completion->cancelled || impl_->cancelled.load();
  return completion;
}

} // namespace xanadu::evidence
