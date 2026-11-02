#include "common/xanadu/evidence/communication_read.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace xanadu::evidence {
namespace {

bool addWithin(std::size_t &total, const std::size_t amount,
               const std::size_t limit) {
  if (amount > limit - std::min(total, limit)) return false;
  total += amount;
  return true;
}

} // namespace

struct CommunicationReadJob::Impl {
  explicit Impl(const FediverseTransportPolicy policy) : transport(policy) {}

  FediverseTransport transport;
  mutable std::mutex mutex;
  std::thread worker;
  std::atomic_bool cancelRequested{};
  bool running{};
  std::optional<FediverseTransportResult> completed;
};

CommunicationReadJob::CommunicationReadJob(
    const FediverseTransportPolicy policy)
    : impl_(std::make_unique<Impl>(policy)) {}

CommunicationReadJob::~CommunicationReadJob() {
  cancel();
  if (impl_->worker.joinable()) impl_->worker.join();
}

bool CommunicationReadJob::start(
    FediverseReadRequest request,
    std::shared_ptr<FediverseCredentialLease> credential,
    std::shared_ptr<FediverseAuthority> authority) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->running || impl_->completed) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->cancelRequested.store(false, std::memory_order_relaxed);
  impl_->running = true;
  try {
    impl_->worker = std::thread(
        [state = impl_.get(), request = std::move(request),
         credential = std::move(credential), authority = std::move(authority)] {
          FediverseTransportResult result;
          try {
            result = state->transport.read(request, credential, authority,
                                           state->cancelRequested);
          } catch (const std::exception &error) {
            result.observation.instanceAuthority = request.instanceOrigin;
            result.observation.accountId         = request.accountId;
            result.observation.accountActorUri   = request.accountActorUri;
            result.observation.collectionUri     = request.instanceOrigin +
                                               "/api/v1/accounts/" +
                                               request.accountId + "/statuses";
            result.observation.bounds = request.limits.records;
            result.observation.state  = state->cancelRequested.load()
                                            ? CaptureState::Cancelled
                                            : CaptureState::Failed;
            result.error              = error.what();
          } catch (...) {
            result.observation.state = CaptureState::Failed;
            result.error             = "unknown communication read failure";
          }
          std::lock_guard completionLock(state->mutex);
          state->completed = std::move(result);
          state->running   = false;
        });
  } catch (...) {
    impl_->running = false;
    return false;
  }
  return true;
}

void CommunicationReadJob::cancel() {
  impl_->cancelRequested.store(true, std::memory_order_relaxed);
}

bool CommunicationReadJob::busy() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

std::optional<FediverseTransportResult> CommunicationReadJob::poll() {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->completed) return {};
  auto result = std::move(impl_->completed);
  impl_->completed.reset();
  if (impl_->worker.joinable()) impl_->worker.join();
  return result;
}

CommunicationRetention
retainFediverseRead(EvidenceCase &evidence,
                    const FediverseTransportResult &result) {
  auto bounds = result.observation.bounds;
  if (bounds.maxRecords == 0 || bounds.maxBytes == 0 ||
      bounds.maxStringBytes == 0)
    throw std::invalid_argument("communication bounds must be positive");

  // Reserve page and gap capacity first. The semantic recorder then validates
  // its entire plan against the remaining budget before its first write.
  auto gaps = result.gaps;
  if (!result.error.empty() && std::ranges::none_of(gaps, [&](const auto &gap) {
        return gap.reason == result.error;
      }))
    gaps.push_back({result.observation.collectionUri, result.error});
  if (result.pages.size() > bounds.maxRecords ||
      gaps.size() > bounds.maxRecords - result.pages.size())
    throw std::invalid_argument("communication record limit exceeded");
  const auto artifactRecords = result.pages.size() + gaps.size();
  std::size_t artifactBytes{};
  const auto charge = [&](const std::string_view value) {
    if (value.size() > bounds.maxStringBytes ||
        !addWithin(artifactBytes, value.size(), bounds.maxBytes))
      throw std::invalid_argument("communication byte limit exceeded");
  };
  for (std::size_t index = 0; index < result.pages.size(); ++index) {
    const auto &page = result.pages[index];
    charge(page.locator);
    charge(page.body);
    charge(page.contentType);
    charge(page.etag);
    charge(page.lastModified);
    charge(page.next);
    charge(page.error);
    charge(page.redacted ? "withheld" : "exact-entity-bytes");
    charge(page.redacted ? "true" : "false");
    charge(std::to_string(index));
    charge(std::to_string(page.status));
    charge(stateName(page.state));
  }
  for (std::size_t index = 0; index < gaps.size(); ++index) {
    const auto &gap = gaps[index];
    charge(gap.locator);
    charge(gap.reason);
    charge(gap.locator);
    charge(gap.reason);
    charge(std::to_string(index));
  }
  bounds.maxRecords -= artifactRecords;
  bounds.maxBytes -= artifactBytes;

  auto observation   = result.observation;
  observation.bounds = bounds;
  auto retained      = retainFediverseObservation(evidence, observation);
  const auto root    = retained.observation;
  for (std::size_t index = 0; index < result.pages.size(); ++index) {
    const auto &page = result.pages[index];
    const auto id    = evidence.appendRecord(
        "fediverse-response-page", page.locator,
        page.redacted ? "Response entity withheld by credential policy"
                         : page.body,
        root,
        {{"page-index", std::to_string(index)},
            {"status", std::to_string(page.status)},
            {"state", stateName(page.state)},
            {"artifact", page.redacted ? "withheld" : "exact-entity-bytes"},
            {"redacted", page.redacted ? "true" : "false"},
            {"content-type", page.contentType},
            {"etag", page.etag},
            {"last-modified", page.lastModified},
            {"next", page.next},
            {"error", page.error}});
    retained.records.emplace("page:" + std::to_string(index), id);
  }
  for (std::size_t index = 0; index < gaps.size(); ++index) {
    const auto &gap = gaps[index];
    const auto id   = evidence.appendRecord(
        "fediverse-read-gap", gap.locator, gap.reason, root,
        {{"locator", gap.locator},
           {"reason", gap.reason},
           {"gap-index", std::to_string(index)}});
    retained.records.emplace("gap:" + std::to_string(index), id);
  }
  return retained;
}

} // namespace xanadu::evidence
