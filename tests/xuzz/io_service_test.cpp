#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"

namespace {
using namespace xanadu::vortex;

constexpr AdapterKey testKey{7, 11};
constexpr CapabilityCell testCapability{4, 1};
constexpr OwnerCell testSpec{9};

class FakeCells final : public IoCellStore {
public:
  auto authorizeOpen(const OpenSpec &)
      -> std::expected<void, IoErrorInfo> override {
    return {};
  }
  auto freezeRequest(CapabilityCell capability, OwnerCell spec)
      -> std::expected<CellRequest, IoErrorInfo> override {
    if (rejectFreeze)
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    // Per-submit endpoints let lane tests give each request its own
    // endpoint; once the queue drains, `endpoint` is the fallback.
    const auto endpointCell = endpointQueueIndex < endpointQueue.size()
                                  ? endpointQueue[endpointQueueIndex++]
                                  : endpoint;
    return CellRequest{.key                  = testKey,
                       .capability           = capability,
                       .endpoint             = endpointCell,
                       .completionBoundBytes = completionBoundBytes,
                       .payloadBoundBytes    = payloadBoundBytes,
                       .direction            = direction,
                       .specification        = spec};
  }
  auto freezeClose(EndpointCell endpointCell, CloseMode)
      -> std::expected<CellRequest, IoErrorInfo> override {
    return CellRequest{.key                  = testKey,
                       .capability           = testCapability,
                       .endpoint             = endpointCell,
                       .completionBoundBytes = completionBoundBytes,
                       .payloadBoundBytes    = payloadBoundBytes,
                       .direction            = CellRequest::Direction::Control,
                       .specification        = OwnerCell{10}};
  }
  auto mintRequest(const CellRequest &request)
      -> std::expected<RequestCell, IoErrorInfo> override {
    if (failMintRequest)
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    const RequestCell cell{nextCell++, 1};
    mintedRequests.push_back(cell);
    (void)request;
    return cell;
  }
  auto mintRefusal(IoErrorInfo error) -> OutcomeCell override {
    return mintOutcome(
               Failed{error, {}, IoProgress{.phase = IoPhase::Terminal}})
        .value();
  }
  auto publishTerminal(RequestCell request, const IoOutcome &outcome,
                       const IoProgress &, const std::optional<PayloadLease> &)
      -> std::expected<OutcomeCell, IoErrorInfo> override {
    if (publishTerminalFailures > 0) {
      --publishTerminalFailures;
      throw std::runtime_error("publishTerminal boom");
    }
    if (publishTerminalRefusal) {
      return std::unexpected(*publishTerminalRefusal);
    }
    outcomes.push_back(outcome);
    const OutcomeCell cell{nextCell++, 1};
    publishedOutcomes.push_back(cell);
    requestOutcomes.emplace_back(request, cell);
    return cell;
  }
  [[nodiscard]] auto outcomeFor(const RequestCell request)
      -> std::optional<OutcomeCell> override {
    for (const auto &[req, cell] : requestOutcomes) {
      if (req == request) return cell;
    }
    return std::nullopt;
  }
  auto mintOutcome(const IoOutcome &outcome)
      -> std::expected<OutcomeCell, IoErrorInfo> {
    outcomes.push_back(outcome);
    return OutcomeCell{nextCell++, 1};
  }
  auto mintOpenedEndpoint(const OpenSpec &)
      -> std::expected<OpenedEndpoint, IoErrorInfo> override {
    ++mintedEndpoints;
    ++mintedCapabilities;
    return OpenedEndpoint{EndpointCell{nextCell++, 1},
                          CapabilityCell{nextCell++, 1}};
  }
  auto mintWaiter(RequestCell request, OwnerWaiterRegistration registration)
      -> std::expected<WaiterCell, IoErrorInfo> override {
    if (failMintWaiter) {
      return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded});
    }
    const WaiterCell waiter{nextCell++, 1};
    waiters.emplace_back(waiter, request, registration);
    return waiter;
  }
  void retireWaiter(WaiterCell waiter) noexcept override {
    retired.push_back(waiter);
  }
  void reapRequest(RequestCell request) noexcept override {
    reaped.push_back(request);
  }
  std::vector<RequestCell> reaped;

  bool rejectFreeze{};
  bool failMintRequest{};
  bool failMintWaiter{};
  int publishTerminalFailures{};
  std::optional<IoErrorInfo> publishTerminalRefusal;
  std::size_t completionBoundBytes{1024};
  std::size_t payloadBoundBytes{1024};
  std::optional<EndpointCell> endpoint;
  std::vector<EndpointCell> endpointQueue;
  std::size_t endpointQueueIndex{};
  CellRequest::Direction direction{CellRequest::Direction::Control};
  std::uint32_t nextCell{100};
  std::vector<RequestCell> mintedRequests;
  std::vector<IoOutcome> outcomes;
  std::vector<OutcomeCell> publishedOutcomes;
  std::vector<std::pair<RequestCell, OutcomeCell>> requestOutcomes;
  std::size_t mintedEndpoints{};
  std::size_t mintedCapabilities{};
  std::vector<std::tuple<WaiterCell, RequestCell, OwnerWaiterRegistration>>
      waiters;
  std::vector<WaiterCell> retired;
};

class FakeScheduler final : public IoScheduler {
public:
  auto reserve(AdmissionCost cost)
      -> std::expected<SchedulerReservation, IoErrorInfo> override {
    ++reserveCalls;
    if (reservationRefusal) return std::unexpected(*reservationRefusal);
    return SchedulerReservation{{nextToken++},
                                cost.commandBytes + cost.completionBytes};
  }
  auto enqueue(SchedulerReservation reservation, BackendCommand command,
               IoAdapter &adapter, CompletionSink &sink) noexcept
      -> EnqueueResult override {
    ++enqueueCalls;
    entries.push_back({reservation, command, &adapter, &sink});
    return enqueueFailure ? EnqueueResult::ReservationInvariantViolated
                          : EnqueueResult::Queued;
  }
  void releaseUnused(SchedulerReservation) noexcept override {
    ++unusedReleases;
  }
  auto cancel(PrivateRequestToken token) noexcept
      -> CancelQueuedResult override {
    ++cancelCalls;
    if (std::ranges::find(runningTokens, token.value) != runningTokens.end())
      return CancelQueuedResult::AlreadyRunning;
    if (std::ranges::find(cancelledTokens, token.value) !=
        cancelledTokens.end())
      return CancelQueuedResult::AlreadyTerminal;
    cancelledTokens.push_back(token.value);
    if (auto *entry = find(token)) {
      entry->sink->terminalCandidate(
          token, BackendResult{Cancelled{{}, {}, false}, {}}, {});
      entry->sink->releaseAcknowledged({PrivateReleaseToken{token}});
      return CancelQueuedResult::RemovedBeforeStart;
    }
    return CancelQueuedResult::AlreadyRunning;
  }
  auto pump(std::size_t) -> std::size_t override { return 0; }
  auto requestStop(ShutdownMode) -> xanadu::vortex::ShutdownResult override {
    stopped = true;
    return {0, activeCount, 0, true};
  }

  struct Entry {
    SchedulerReservation reservation;
    BackendCommand command;
    IoAdapter *adapter;
    CompletionSink *sink;
  };
  Entry *find(PrivateRequestToken token) {
    const auto it = std::find_if(
        entries.begin(), entries.end(),
        [token](const auto &entry) { return entry.command.token == token; });
    return it == entries.end() ? nullptr : &*it;
  }
  void candidate(PrivateRequestToken token, IoOutcome outcome,
                 std::vector<std::byte> staged = {}) {
    find(token)->sink->terminalCandidate(
        token, BackendResult{std::move(outcome), std::move(staged)}, {});
  }
  void acknowledge(PrivateRequestToken token) {
    find(token)->sink->releaseAcknowledged({PrivateReleaseToken{token}});
  }
  void progress(PrivateRequestToken token, IoProgress value) {
    find(token)->sink->progress(token, value);
  }

  std::optional<IoErrorInfo> reservationRefusal;
  bool enqueueFailure{};
  bool stopped{};
  std::size_t activeCount{};
  std::size_t reserveCalls{};
  std::size_t enqueueCalls{};
  std::size_t cancelCalls{};
  std::size_t unusedReleases{};
  std::uint64_t nextToken{1};
  std::vector<Entry> entries;
  std::vector<std::uint64_t> cancelledTokens;
  std::vector<std::uint64_t> runningTokens;
};

class FakeAdapter : public IoAdapter {
public:
  auto prepare(const CellRequest &request, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> override {
    ++prepareCalls;
    if (throwOnPrepare) throw std::runtime_error("prepare boom");
    auto bytes = request.openSpec
                     ? request.openSpec->locator
                     : std::vector<std::byte>{
                           static_cast<std::byte>(request.specification)};
    return PreparedOperation{request.key, std::move(bytes)};
  }
  void execute(const BackendCommand &, BackendContext &,
               CompletionSink &) noexcept override {
    ++executeCalls;
  }
  auto publish(const NativeCompletion &completion, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> override {
    ++publishCalls;
    publishThread = std::this_thread::get_id();
    if (throwOnPublish) throw std::runtime_error("publish boom");
    return OwnerPublication{completion.result.outcome};
  }
  void abandonStaged(OwnerCell specification) noexcept override {
    ++abandonCalls;
    abandoned.push_back(specification);
  }
  void
  openSettled(PrivateRequestToken,
              const std::optional<OpenedEndpoint> &opened) noexcept override {
    ++openSettledCalls;
    openSettledBound += opened.has_value() ? 1U : 0U;
  }
  std::size_t openSettledCalls{};
  std::size_t openSettledBound{};
  std::size_t prepareCalls{};
  std::size_t executeCalls{};
  std::size_t publishCalls{};
  std::size_t abandonCalls{};
  std::vector<OwnerCell> abandoned;
  std::thread::id publishThread;
  bool throwOnPublish{};
  bool throwOnPrepare{};
};

struct Harness {
  PayloadResolver payloads;
  FakeCells cells;
  FakeScheduler *scheduler{};
  FakeAdapter adapter;
  IoService service;

  explicit Harness(IoBudgetSnapshot budget = {8, 1024, 1024, 1024, 8})
      : service(budget, makeScheduler(), payloads, cells) {
    service.registerAdapter(testKey, adapter);
  }
  auto makeScheduler() -> std::unique_ptr<IoScheduler> {
    auto value = std::make_unique<FakeScheduler>();
    scheduler  = value.get();
    return value;
  }
  auto submit() -> RequestCell { return submit(testSpec); }
  auto submit(OwnerCell specification) -> RequestCell {
    auto result    = service.submit(testCapability, specification);
    auto *admitted = std::get_if<Admitted>(&result.value);
    if (!admitted) return {};
    return admitted->request;
  }
};

PrivateRequestToken tokenOf(const FakeScheduler &scheduler,
                            std::size_t index = 0) {
  return scheduler.entries.at(index).command.token;
}

TEST(IoServiceTest, ReservationRefusalStartsNoBackendAndMintsNoRequest) {
  Harness harness;
  harness.scheduler->reservationRefusal =
      IoErrorInfo{IoErrorCode::BudgetExceeded};
  const auto result = harness.service.submit(testCapability, testSpec);
  EXPECT_TRUE(std::holds_alternative<Refused>(result.value));
  EXPECT_TRUE(harness.scheduler->entries.empty());
  EXPECT_EQ(harness.adapter.executeCalls, 0U);
  EXPECT_TRUE(harness.cells.mintedRequests.empty());
}

TEST(IoServiceTest, CandidateAndReleaseAckBothGateOwnerPublication) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  const OwnerWaiterRegistration registration{OwnerCell{22}, 1, 31};
  const auto pending = harness.service.await(request, registration);
  ASSERT_TRUE(std::holds_alternative<Pending>(pending.value));

  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  static_cast<void>(harness.service.pump(8));
  EXPECT_EQ(harness.adapter.publishCalls, 0U);
  EXPECT_EQ(harness.scheduler->unusedReleases, 0U);
  EXPECT_FALSE(harness.service.poll(request).outcome.has_value());

  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_EQ(harness.adapter.publishCalls, 1U);
  EXPECT_EQ(harness.scheduler->unusedReleases, 1U);
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
  const auto ready = harness.service.await(request, registration);
  EXPECT_TRUE(std::holds_alternative<Ready>(ready.value));
}

TEST(IoServiceTest, WorkerSinkCallbacksNeverPublishCellsOffOwnerThread) {
  Harness harness;
  const auto request     = harness.submit();
  const auto token       = tokenOf(*harness.scheduler);
  const auto ownerThread = std::this_thread::get_id();
  std::thread worker([&] {
    harness.scheduler->candidate(token, Complete{{}, {}, {}});
    harness.scheduler->acknowledge(token);
  });
  worker.join();
  EXPECT_EQ(harness.adapter.publishCalls, 0U);
  EXPECT_TRUE(harness.cells.outcomes.empty());
  static_cast<void>(harness.service.pump(8));
  EXPECT_EQ(harness.adapter.publishThread, ownerThread);
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
}

TEST(IoServiceTest,
     AckBeforeCandidateAndDuplicatesPublishOnlyTheFirstTerminal) {
  Harness harness;
  static_cast<void>(harness.submit());
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_EQ(harness.adapter.publishCalls, 0U);
  harness.scheduler->candidate(token, EndOfStream{{}});
  harness.scheduler->candidate(
      token, Failed{{IoErrorCode::BackendFailure, 3}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  static_cast<void>(harness.service.pump(8));
  EXPECT_EQ(harness.adapter.publishCalls, 1U);
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  EXPECT_TRUE(
      std::holds_alternative<EndOfStream>(harness.cells.outcomes.front()));
}

TEST(IoServiceTest, PartialAndUnknownRemainTypedTerminalOutcomes) {
  Harness harness;
  const auto partialRequest = harness.submit();
  const auto partialToken   = tokenOf(*harness.scheduler);
  const auto unknownRequest = harness.submit();
  const auto unknownToken   = tokenOf(*harness.scheduler, 1);
  const IoProgress progress{.transferredBytes = 3, .phase = IoPhase::Terminal};
  // Partial always names a payload range; an unresolvable one degrades the
  // outcome to Failed, so this case registers real transient bytes.
  const std::vector<std::byte> payloadBytes{std::byte{'p'}, std::byte{'a'},
                                            std::byte{'r'}, std::byte{'t'}};
  const auto payloadOrigin = harness.payloads.registerSource(
      PayloadRef{.kind       = PayloadSourceKind::Transient,
                 .source     = 1,
                 .generation = 1,
                 .occurrence = 1},
      payloadBytes);
  ASSERT_TRUE(payloadOrigin);
  const PayloadRange partialRange{
      .origin = *payloadOrigin, .offset = 0, .length = payloadBytes.size()};
  harness.scheduler->candidate(
      partialToken,
      Partial{partialRange, PartialCause::BackendFailure, progress});
  harness.scheduler->acknowledge(partialToken);
  harness.scheduler->candidate(unknownToken, Unknown{progress});
  harness.scheduler->acknowledge(unknownToken);
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(harness.service.poll(partialRequest).outcome.has_value());
  EXPECT_TRUE(harness.service.poll(unknownRequest).outcome.has_value());
  ASSERT_EQ(harness.cells.outcomes.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<Partial>(harness.cells.outcomes[0]));
  EXPECT_TRUE(std::holds_alternative<Unknown>(harness.cells.outcomes[1]));
}

TEST(IoServiceTest, ProgressCoalescesWhileTerminalReserveRemainsAvailable) {
  Harness harness;
  const auto request = harness.submit();
  const auto token   = tokenOf(*harness.scheduler);
  harness.scheduler->progress(token, {1, 1, 0, 1, IoPhase::Running});
  harness.scheduler->progress(token, {5, 4, 2, 2, IoPhase::Running});
  static_cast<void>(harness.service.pump(8));
  const auto poll = harness.service.poll(request);
  ASSERT_TRUE(poll.progress.has_value());
  EXPECT_EQ(poll.progress->transferredBytes, 4U);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
}

TEST(IoServiceTest, PumpedProgressDoesNotEraseTheRequestsByteBounds) {
  Harness harness;
  const auto request = harness.submit();
  const auto token   = tokenOf(*harness.scheduler);
  const std::vector<std::byte> bytes(16, std::byte{'x'});
  const auto origin = harness.payloads.registerSource(
      PayloadRef{.kind       = PayloadSourceKind::Transient,
                 .source     = 1,
                 .generation = 1,
                 .occurrence = 1},
      bytes);
  ASSERT_TRUE(origin);

  // The owner drains the progress event -- which used to carry the admitted
  // limits and erase them -- before the worker's terminal candidate lands.
  harness.scheduler->progress(token, {1, 1, 0, 1, IoPhase::Running});
  static_cast<void>(harness.service.pump(8));
  ASSERT_TRUE(harness.service.poll(request).progress.has_value());

  const IoProgress done{.transferredBytes = bytes.size(),
                        .phase            = IoPhase::Terminal};
  harness.scheduler->candidate(
      token,
      Partial{
          PayloadRange{.origin = *origin, .offset = 0, .length = bytes.size()},
          PartialCause::BackendFailure, done});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  ASSERT_TRUE(harness.service.poll(request).outcome.has_value());
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<Partial>(harness.cells.outcomes.front()))
      << "a payload within the admitted bound must not become BudgetExceeded";
}

TEST(IoServiceTest, EnqueueInvariantFailurePublishesTypedSchedulerUnavailable) {
  Harness harness;
  harness.scheduler->enqueueFailure = true;
  const auto request                = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  EXPECT_EQ(harness.scheduler->enqueueCalls, 1U);
  EXPECT_EQ(harness.adapter.executeCalls, 0U);
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::SchedulerUnavailable);
}

TEST(IoServiceTest,
     WaiterCancellationDoesNotCancelRequestAndRepeatedAwaitIsIdempotent) {
  Harness harness;
  const auto request = harness.submit();
  const OwnerWaiterRegistration registration{OwnerCell{22}, 1, 31};
  const auto first  = harness.service.await(request, registration);
  const auto second = harness.service.await(request, registration);
  ASSERT_TRUE(std::holds_alternative<Pending>(first.value));
  ASSERT_TRUE(std::holds_alternative<Pending>(second.value));
  const auto waiter = std::get<Pending>(first.value).waiter;
  EXPECT_EQ(std::get<Pending>(second.value).waiter, waiter);
  EXPECT_EQ(harness.service.cancelWaiter(waiter).state,
            WaiterCancelState::Removed);
  EXPECT_EQ(harness.scheduler->cancelCalls, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
}

TEST(IoServiceTest, RequestCancellationBeforeStartUsesReservedTerminalPath) {
  Harness harness;
  const auto request = harness.submit();
  EXPECT_EQ(harness.service.cancelRequest(request).state,
            RequestCancelState::Accepted);
  EXPECT_EQ(harness.adapter.executeCalls, 0U);
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  EXPECT_TRUE(
      std::holds_alternative<Cancelled>(harness.cells.outcomes.front()));
  EXPECT_EQ(harness.service.cancelRequest(request).state,
            RequestCancelState::AlreadyTerminal);
}

TEST(IoServiceTest, TerminalRequestRetirementReturnsBoundedAdmissionCapacity) {
  Harness harness({1, 1024, 1024, 1024, 1});
  const auto request = harness.submit();
  const auto token   = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(std::holds_alternative<Refused>(
      harness.service.submit(testCapability, testSpec).value));

  EXPECT_TRUE(harness.cells.reaped.empty());
  harness.service.retireRequest(request);
  // The cell store is told once the state is gone, so it can take the
  // request out of the ranks later submits walk.
  EXPECT_EQ(harness.cells.reaped, std::vector<RequestCell>{request});
  const auto replacement = harness.service.submit(testCapability, testSpec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(replacement.value));
  const auto replacementToken = tokenOf(*harness.scheduler, 1);
  harness.scheduler->candidate(replacementToken, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(replacementToken);
  static_cast<void>(harness.service.pump(8));
}

TEST(IoServiceTest, RunningCancellationWaitsForBackendCandidateAndReleaseAck) {
  Harness harness;
  const auto request = harness.submit();
  const auto token   = tokenOf(*harness.scheduler);
  harness.scheduler->runningTokens.push_back(token.value);
  EXPECT_EQ(harness.service.cancelRequest(request).state,
            RequestCancelState::Accepted);
  static_cast<void>(harness.service.pump(8));
  EXPECT_FALSE(harness.service.poll(request).outcome.has_value());

  const IoProgress progress{.phase = IoPhase::Cancelling};
  harness.scheduler->candidate(token, Cancelled{{}, progress, true});
  static_cast<void>(harness.service.pump(8));
  EXPECT_FALSE(harness.service.poll(request).outcome.has_value());
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *cancelled =
      std::get_if<Cancelled>(&harness.cells.outcomes.front());
  ASSERT_NE(cancelled, nullptr);
  EXPECT_TRUE(cancelled->uncertain);
}

TEST(IoServiceTest, OpenIsAdmittedAndAwaitedThroughTheSameRequestLifecycle) {
  Harness harness;
  OpenSpec spec{.adapterKey      = testKey,
                .locator         = {std::byte{0x2f}},
                .requestedRights = 1,
                .consistency     = 0,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = harness.service.admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;
  ASSERT_EQ(harness.scheduler->entries.size(), 1U);
  EXPECT_TRUE(
      harness.scheduler->entries.front().command.operation.bytes.size() > 0);
  const auto awaiting = harness.service.await(request, {OwnerCell{22}, 1, 31});
  ASSERT_TRUE(std::holds_alternative<Pending>(awaiting.value));
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
  EXPECT_EQ(harness.adapter.prepareCalls, 1U);
  EXPECT_EQ(harness.cells.mintedEndpoints, 1U);
  EXPECT_EQ(harness.cells.mintedCapabilities, 1U);
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *complete = std::get_if<Complete>(&harness.cells.outcomes.front());
  ASSERT_NE(complete, nullptr);
  EXPECT_TRUE(std::holds_alternative<OpenedEndpoint>(complete->value));
}

TEST(IoServiceTest, OpenWhosePublishThrowsStillSettlesTheOpenUnbound) {
  Harness harness;
  harness.adapter.throwOnPublish = true;
  OpenSpec spec{.adapterKey      = testKey,
                .locator         = {std::byte{0x2f}},
                .requestedRights = 1,
                .consistency     = 0,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = harness.service.admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));

  // The worker already opened a native resource for this request; the hook is
  // how the adapter learns that no endpoint will ever name it.
  EXPECT_EQ(harness.adapter.openSettledCalls, 1U);
  EXPECT_EQ(harness.adapter.openSettledBound, 0U);
}

TEST(IoServiceTest, AnOrphanedOpenMintsNoEndpointButStillSettlesTheHook) {
  // M7-VQL gate r2, vortex minor 3: an orphan's request cell is gone, so an
  // endpoint minted for it could never be named or reaped.
  Harness harness;
  OpenSpec spec{.adapterKey      = testKey,
                .locator         = {std::byte{0x2f}},
                .requestedRights = 1,
                .consistency     = 0,
                .deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(5)};
  const auto submitted = harness.service.admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;
  EXPECT_EQ(harness.service.orphanRequest(request),
            IoService::OrphanResult::Orphaned);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  EXPECT_EQ(harness.cells.mintedEndpoints, 0U);
  EXPECT_EQ(harness.cells.mintedCapabilities, 0U);
  EXPECT_EQ(harness.adapter.openSettledCalls, 1U);
  EXPECT_EQ(harness.adapter.openSettledBound, 0U);
}

TEST(IoServiceTest, OrphaningIsTypedAndCounted) {
  // M7-VQL gate r2, vortex minor 2: a request with a waiter is refused by
  // name, not silently, and every orphan is counted.
  Harness harness;
  const auto request  = harness.submit();
  const auto awaiting = harness.service.await(request, {OwnerCell{22}, 1, 31});
  ASSERT_TRUE(std::holds_alternative<Pending>(awaiting.value));
  EXPECT_EQ(harness.service.orphanRequest(request),
            IoService::OrphanResult::HasWaiters);
  EXPECT_EQ(harness.service.orphanCount(), 0U);
  EXPECT_EQ(harness.service.orphanRequest(RequestCell{9999, 1}),
            IoService::OrphanResult::Unknown);
  const auto other = harness.submit();
  EXPECT_EQ(harness.service.orphanRequest(other),
            IoService::OrphanResult::Orphaned);
  EXPECT_EQ(harness.service.orphanCount(), 1U);
}

TEST(IoServiceTest,
     ShutdownStopsAdmissionAndCancelsQueuedRequestsWithoutRunning) {
  Harness harness;
  const auto request = harness.submit();
  const auto result =
      harness.service.requestShutdown(ShutdownMode::CancelPending);
  EXPECT_TRUE(result.stoppedAccepting);
  EXPECT_EQ(harness.scheduler->cancelCalls, 1U);
  EXPECT_EQ(harness.adapter.executeCalls, 0U);
  const auto refused = harness.service.submit(testCapability, testSpec);
  EXPECT_TRUE(std::holds_alternative<Refused>(refused.value));
  static_cast<void>(harness.service.pump(8));
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
}

TEST(IoServiceTest, TombstoneEvictionNeverDropsACloseStillInFlight) {
  Harness harness({2048, 1024, 1024, 1024, 8});
  const auto closeAndSettle = [&](const EndpointCell endpoint,
                                  const std::size_t index) {
    const auto closed = harness.service.closeEndpoint(endpoint, CloseMode{});
    EXPECT_TRUE(std::holds_alternative<Admitted>(closed.value));
    const auto token = tokenOf(*harness.scheduler, index);
    harness.scheduler->candidate(token, Complete{{}, {}, {}});
    harness.scheduler->acknowledge(token);
    static_cast<void>(harness.service.pump(8));
  };

  // The lowest-keyed endpoint closes first and never completes: its entry is
  // the one an eviction by key order used to pick.
  const EndpointCell inFlight{1, 1};
  const auto first = harness.service.closeEndpoint(inFlight, CloseMode{});
  ASSERT_TRUE(std::holds_alternative<Admitted>(first.value));
  constexpr std::size_t kMoreThanTheBound = 1100;
  for (std::size_t i = 0; i < kMoreThanTheBound; ++i) {
    closeAndSettle(EndpointCell{static_cast<zigzag::CellRef>(100 + i), 1},
                   i + 1);
  }

  const auto second = harness.service.closeEndpoint(inFlight, CloseMode{});
  ASSERT_TRUE(std::holds_alternative<Refused>(second.value));
  const auto refusal = harness.cells.outcomes.back();
  const auto *failed = std::get_if<Failed>(&refusal);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::Conflict);
}

TEST(IoServiceTest, EndpointQueuesFifoPerDirectionAndPermitFullDuplex) {
  Harness harness;
  harness.cells.endpoint  = EndpointCell{40, 2};
  harness.cells.direction = CellRequest::Direction::Read;
  const auto firstRead    = harness.submit(OwnerCell{1});
  const auto secondRead   = harness.submit(OwnerCell{2});
  harness.cells.direction = CellRequest::Direction::Write;
  const auto firstWrite   = harness.submit(OwnerCell{3});
  ASSERT_NE(firstRead.incarnation, 0U);
  ASSERT_NE(secondRead.incarnation, 0U);
  ASSERT_NE(firstWrite.incarnation, 0U);

  // The first read owns the read lane, while a write on the same endpoint can
  // start independently. The next read stays queued until the first releases.
  ASSERT_EQ(harness.scheduler->entries.size(), 2U);
  EXPECT_EQ(harness.scheduler->entries[0].command.operation.bytes.front(),
            std::byte{1});
  EXPECT_EQ(harness.scheduler->entries[1].command.operation.bytes.front(),
            std::byte{3});
  const auto firstReadToken = tokenOf(*harness.scheduler, 0);
  harness.scheduler->candidate(firstReadToken, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(firstReadToken);
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.scheduler->entries.size(), 3U);
  EXPECT_EQ(harness.scheduler->entries[2].command.operation.bytes.front(),
            std::byte{2});
  EXPECT_FALSE(harness.service.poll(secondRead).outcome.has_value());
  EXPECT_FALSE(harness.service.poll(firstWrite).outcome.has_value());
}

TEST(IoServiceTest, ThrowingPublishStillTerminatesAndAdvancesTheLane) {
  Harness harness;
  harness.adapter.throwOnPublish = true;
  harness.cells.endpoint         = EndpointCell{40, 2};
  harness.cells.direction        = CellRequest::Direction::Read;
  const auto firstRead           = harness.submit(OwnerCell{1});
  const auto secondRead          = harness.submit(OwnerCell{2});
  ASSERT_NE(firstRead.incarnation, 0U);
  ASSERT_NE(secondRead.incarnation, 0U);
  // FIFO lane: only the head is enqueued; the second read queues behind it.
  ASSERT_EQ(harness.scheduler->entries.size(), 1U);

  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));

  // The adapter's publish threw mid-materialization: the request must
  // still report exactly one typed Failed{BackendFailure} outcome, the
  // reservation must be released once, and the queued lane head must
  // start -- never a wedged lane behind a leaked request.
  EXPECT_EQ(harness.adapter.publishCalls, 1U);
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::BackendFailure);
  EXPECT_EQ(harness.scheduler->unusedReleases, 1U);
  ASSERT_EQ(harness.scheduler->entries.size(), 2U);
  EXPECT_EQ(harness.scheduler->entries[1].command.operation.bytes.front(),
            std::byte{2});
  const auto poll = harness.service.poll(firstRead);
  EXPECT_TRUE(poll.outcome.has_value());
  EXPECT_EQ(poll.phase, IoPhase::Terminal);
  EXPECT_FALSE(harness.service.poll(secondRead).outcome.has_value());
}

TEST(IoServiceTest, ThrowingPublishTerminalRematerializesOnPoll) {
  Harness harness;
  // pump()'s publishTerminal throws once; the durable terminal record must
  // let poll() rematerialize the real outcome instead of losing it.
  harness.cells.publishTerminalFailures = 1;
  const auto request                    = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.cells.outcomes.size(), 0U);

  const auto poll = harness.service.poll(request);
  EXPECT_TRUE(poll.outcome.has_value());
  EXPECT_EQ(poll.phase, IoPhase::Terminal);
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<Complete>(harness.cells.outcomes.front()));
  EXPECT_EQ(*poll.outcome, harness.cells.publishedOutcomes.front());
}

TEST(IoServiceTest, PersistentPublishTerminalThrowDegradesToRefusal) {
  Harness harness;
  harness.cells.publishTerminalFailures = 100;
  const auto request                    = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));

  // Every publishTerminal throws: poll must still surface a typed outcome
  // (the refusal-cell fallback) rather than propagating the exception.
  const auto poll = harness.service.poll(request);
  EXPECT_TRUE(poll.outcome.has_value());
  EXPECT_EQ(poll.phase, IoPhase::Terminal);
  ASSERT_FALSE(harness.cells.outcomes.empty());
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.back());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::BackendFailure);
}

TEST(IoServiceTest, RealArenaBridgeMintsAndPublishesTypedRequestOutcome) {
  zigzag::ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  const auto endpoint = makeEndpointCell(arena, *schema);
  ASSERT_TRUE(endpoint);
  const auto capability = makeCapabilityCell(arena, *schema, *endpoint);
  ASSERT_TRUE(capability);
  PayloadResolver payloads;
  FakeScheduler *schedulerRaw{};
  auto scheduler = std::make_unique<FakeScheduler>();
  schedulerRaw   = scheduler.get();
  FakeAdapter adapter;
  ArenaIoCellStore cells(
      arena, *schema, payloads,
      [endpoint = *endpoint](CapabilityCell requestCapability, OwnerCell spec)
          -> std::expected<CellRequest, IoErrorInfo> {
        return CellRequest{.key                  = testKey,
                           .capability           = requestCapability,
                           .endpoint             = endpoint,
                           .completionBoundBytes = 1024,
                           .payloadBoundBytes    = 1024,
                           .direction            = CellRequest::Direction::Read,
                           .specification        = spec};
      });
  IoService service({8, 1024, 1024, 1024, 8}, std::move(scheduler), payloads,
                    cells);
  service.registerAdapter(testKey, adapter);
  const auto specification = arena.makeCell("io request spec");
  const auto submitted     = service.submit(*capability, specification);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;
  EXPECT_EQ(ioCellTag(arena, request.cell, request.incarnation),
            IoCellTag::Request);

  const auto token = tokenOf(*schedulerRaw);
  schedulerRaw->candidate(token,
                          Failed{{IoErrorCode::BackendFailure, 42}, {}, {}});
  schedulerRaw->acknowledge(token);
  static_cast<void>(service.pump(8));
  const auto poll = service.poll(request);
  ASSERT_TRUE(poll.outcome.has_value());
  EXPECT_EQ(ioCellTag(arena, poll.outcome->cell, poll.outcome->incarnation),
            IoCellTag::Outcome);
  EXPECT_EQ(ioOutcomeTag(arena, *poll.outcome), IoOutcomeTag::Failed);
}

TEST(IoServiceTest, RetiredRequestIsFreedAndALatePollFindsItStale) {
  // The real ArenaIoCellStore, not FakeCells: retiring is the consumer saying
  // it has read the outcome, and the reaper frees the request's cells through
  // ArenaManifold::freeCell (P4), so a late poll has nothing left to resolve.
  zigzag::ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  const auto endpoint = makeEndpointCell(arena, *schema);
  ASSERT_TRUE(endpoint);
  const auto capability = makeCapabilityCell(arena, *schema, *endpoint);
  ASSERT_TRUE(capability);
  PayloadResolver payloads;
  FakeScheduler *schedulerRaw{};
  auto scheduler = std::make_unique<FakeScheduler>();
  schedulerRaw   = scheduler.get();
  FakeAdapter adapter;
  ArenaIoCellStore cells(
      arena, *schema, payloads,
      [endpoint = *endpoint](CapabilityCell requestCapability, OwnerCell spec)
          -> std::expected<CellRequest, IoErrorInfo> {
        return CellRequest{.key                  = testKey,
                           .capability           = requestCapability,
                           .endpoint             = endpoint,
                           .completionBoundBytes = 1024,
                           .payloadBoundBytes    = 1024,
                           .direction            = CellRequest::Direction::Read,
                           .specification        = spec};
      });
  IoService service({8, 1024, 1024, 1024, 8}, std::move(scheduler), payloads,
                    cells);
  service.registerAdapter(testKey, adapter);
  const auto specification = arena.makeCell("io request spec");
  const auto submitted     = service.submit(*capability, specification);
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;

  const auto token = tokenOf(*schedulerRaw);
  schedulerRaw->candidate(token, Complete{{}, {}, {}});
  schedulerRaw->acknowledge(token);
  static_cast<void>(service.pump(8));
  const auto outcome = service.poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(arena, *outcome), IoOutcomeTag::Complete);

  const auto liveBefore = arena.liveCells();
  service.retireRequest(request);
  EXPECT_LT(arena.liveCells(), liveBefore) << "the request's cells are freed";
  EXPECT_FALSE(arena.matches(request.cell, request.incarnation));
  EXPECT_FALSE(arena.matches(outcome->cell, outcome->incarnation));
  const auto late = service.poll(request);
  EXPECT_FALSE(late.outcome.has_value());
  EXPECT_EQ(ioOutcomeTag(arena, *outcome).error(), IoCellError::StaleCell);
}

/// Submit, optionally await (a parked waiter, retired by the terminal pass or
/// by a cancel), complete, poll and retire @p stream requests, and require the
/// arena to hold no more after them than after the first. Only the caller's own
/// spec and cursor cells are the test's to free.
void runBoundedCycles(const bool awaiting) {
  zigzag::ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  const auto endpoint = makeEndpointCell(arena, *schema);
  ASSERT_TRUE(endpoint);
  const auto capability = makeCapabilityCell(arena, *schema, *endpoint);
  ASSERT_TRUE(capability);
  PayloadResolver payloads;
  auto scheduler          = std::make_unique<FakeScheduler>();
  FakeScheduler *scripted = scheduler.get();
  FakeAdapter adapter;
  ArenaIoCellStore cells(
      arena, *schema, payloads,
      [endpoint = *endpoint](CapabilityCell requestCapability, OwnerCell spec)
          -> std::expected<CellRequest, IoErrorInfo> {
        return CellRequest{.key                  = testKey,
                           .capability           = requestCapability,
                           .endpoint             = endpoint,
                           .completionBoundBytes = 1024,
                           .payloadBoundBytes    = 1024,
                           .direction            = CellRequest::Direction::Read,
                           .specification        = spec};
      });
  IoService service({8, 1024, 1024, 1024, 8}, std::move(scheduler), payloads,
                    cells);
  service.registerAdapter(testKey, adapter);

  const auto cycle = [&](const std::size_t index) {
    const auto liveBeforeSubmit = arena.liveCells() + 1U;
    const auto specification    = arena.makeCell("io request spec");
    const auto submitted        = service.submit(*capability, specification);
    ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
    const auto request = std::get<Admitted>(submitted.value).request;
    const auto token   = tokenOf(*scripted, index);
    const auto cursor  = arena.makeCell("await cursor");
    if (awaiting) {
      const OwnerWaiterRegistration registration{
          cursor, arena.incarnationOf(cursor).value(), index + 1U};
      const auto pending = service.await(request, registration);
      ASSERT_TRUE(std::holds_alternative<Pending>(pending.value));
      if (index % 2U == 1U) {
        // Half the waiters leave by cancel, half by the terminal pass.
        ASSERT_EQ(
            service.cancelWaiter(std::get<Pending>(pending.value).waiter).state,
            WaiterCancelState::Removed);
      }
    }
    scripted->candidate(token, Complete{{}, {}, {}});
    scripted->acknowledge(token);
    static_cast<void>(service.pump(8));
    ASSERT_TRUE(service.poll(request).outcome.has_value());
    if (index == 0 && !awaiting) {
      // What one served request holds while it is live, for the plan's price:
      // everything it minted but the caller's own spec cell.
      ::testing::Test::RecordProperty(
          "liveCellsPerServedRequest",
          static_cast<int>(arena.liveCells() - liveBeforeSubmit));
    }
    service.retireRequest(request);
    ASSERT_TRUE(arena.freeCell(specification,
                               arena.incarnationOf(specification).value()));
    ASSERT_TRUE(arena.freeCell(cursor, arena.incarnationOf(cursor).value()));
  };

  cycle(0);
  const auto liveAfterOne      = arena.liveCells();
  const auto slotsAfterOne     = arena.cellCount();
  constexpr std::size_t stream = 10'000;
  for (std::size_t i = 1; i <= stream; ++i) {
    cycle(i);
    if (::testing::Test::HasFatalFailure()) return;
  }
  EXPECT_EQ(arena.liveCells(), liveAfterOne);
  EXPECT_EQ(arena.cellCount(), slotsAfterOne);
  EXPECT_EQ(queuedRequests(arena, *schema, *endpoint)->size(), 0U);
}

TEST(IoServiceTest, TenThousandSubmitAndRetireCyclesLeaveTheArenaBounded) {
  // P4: a service that retires what it serves does not grow its arena. The
  // request's cells (request, field occurrences and views, outcome) are freed
  // by the reaper and the slots reused.
  runBoundedCycles(false);
}

TEST(IoServiceTest, TenThousandAwaitCyclesLeaveTheArenaBoundedToo) {
  // M1 of the M7a review: the waiter graph (cell, request occurrence and view,
  // cursor/incarnation/continuation records) is freed when the waiter retires,
  // by cancel or by the terminal pass, not left behind per parked await.
  runBoundedCycles(true);
}

TEST(IoSchedulerTest, BoundedPoolNeverRunsAdapterOnTheSubmittingThread) {
  PayloadResolver payloads;
  BoundedIoScheduler scheduler({1, 2, 4096, 1024}, payloads);
  struct Sink final : CompletionSink {
    void progress(PrivateRequestToken, IoProgress) noexcept override {}
    void terminalCandidate(PrivateRequestToken, BackendResult,
                           std::optional<PayloadLease>) noexcept override {}
    void releaseAcknowledged(BackendReleaseAck) noexcept override {}
  } sink;
  struct ThreadAdapter final : FakeAdapter {
    std::mutex mutex;
    std::condition_variable wake;
    bool executed{};
    std::thread::id executeThread;
    void execute(const BackendCommand &, BackendContext &,
                 CompletionSink &) noexcept override {
      {
        std::lock_guard lock(mutex);
        executed      = true;
        executeThread = std::this_thread::get_id();
      }
      wake.notify_one();
    }
  } adapter;

  const auto reservation = scheduler.reserve({1, 1, 1});
  ASSERT_TRUE(reservation);
  const std::thread::id caller = std::this_thread::get_id();
  ASSERT_EQ(
      scheduler.enqueue(*reservation,
                        {{reservation->token}, {{1, 2}, {std::byte{0x41}}}},
                        adapter, sink),
      EnqueueResult::Queued);
  std::unique_lock lock(adapter.mutex);
  ASSERT_TRUE(adapter.wake.wait_for(lock, std::chrono::seconds(2),
                                    [&] { return adapter.executed; }));
  EXPECT_NE(adapter.executeThread, caller);
  lock.unlock();
  scheduler.requestStop(ShutdownMode::CancelPending);
}

TEST(IoSchedulerTest, ReservationsBoundOutstandingTerminalAndQueueCapacity) {
  PayloadResolver payloads;
  BoundedIoScheduler scheduler({1, 1, 10, 1024}, payloads);
  const auto first = scheduler.reserve({1, 1, 8});
  ASSERT_TRUE(first);
  EXPECT_FALSE(scheduler.reserve({1, 1, 1}));
  scheduler.releaseUnused(*first);
  const auto replacement = scheduler.reserve({1, 1, 8});
  EXPECT_TRUE(replacement);
  scheduler.requestStop(ShutdownMode::CancelPending);
}

TEST(IoServiceTest,
     DestructionJoinsDelayedBackendBeforeReleasingItsSinkAndDependencies) {
  PayloadResolver payloads;
  FakeCells cells;
  std::binary_semaphore entered{0};
  std::latch release{1};
  std::latch teardownStarted{1};
  std::atomic_bool observedStop{};
  std::atomic_bool destroyed{};

  class DelayedAdapter final : public FakeAdapter {
  public:
    DelayedAdapter(std::binary_semaphore &entered, std::latch &release,
                   std::atomic_bool &observedStop)
        : entered(entered), release(release), observedStop(observedStop) {}

    void execute(const BackendCommand &command, BackendContext &context,
                 CompletionSink &sink) noexcept override {
      entered.release();
      release.wait();
      observedStop.store(context.stop.stop_requested());
      sink.terminalCandidate(command.token,
                             BackendResult{Complete{{}, {}, {}}, {}}, {});
    }

    std::binary_semaphore &entered;
    std::latch &release;
    std::atomic_bool &observedStop;
  } adapter(entered, release, observedStop);

  auto service = std::make_unique<IoService>(
      IoBudgetSnapshot{8, 1024, 1024, 1024, 8},
      std::make_unique<BoundedIoScheduler>(
          BoundedIoSchedulerOptions{1, 2, 4096, 1024}, payloads),
      payloads, cells);
  service->registerAdapter(testKey, adapter);
  const auto request = service->submit(testCapability, testSpec);
  ASSERT_TRUE(std::holds_alternative<Admitted>(request.value));
  if (!entered.try_acquire_for(std::chrono::seconds(2))) {
    release.count_down();
    FAIL() << "bounded scheduler did not start the admitted backend";
  }

  const auto stopped = service->requestShutdown(ShutdownMode::CancelPending);
  EXPECT_TRUE(stopped.stoppedAccepting);
  EXPECT_EQ(stopped.running, 1U);
  EXPECT_FALSE(destroyed.load());

  std::thread destroyer([&] {
    teardownStarted.count_down();
    service.reset();
    destroyed.store(true);
  });
  teardownStarted.wait();
  EXPECT_FALSE(destroyed.load());
  release.count_down();
  destroyer.join();

  EXPECT_TRUE(destroyed.load());
  EXPECT_TRUE(observedStop.load());
  EXPECT_EQ(adapter.publishCalls, 0U);
  EXPECT_TRUE(cells.outcomes.empty());
}

TEST(IoServiceTest, OversizedCompletionBecomesTypedBudgetFailure) {
  Harness harness;
  harness.cells.completionBoundBytes = 2;
  const auto request                 = harness.submit();
  const auto token                   = tokenOf(*harness.scheduler);

  // The completion bound sees the worker->owner `staged` channel; outcome
  // payload bytes are bounded separately by payloadBoundBytes.
  harness.scheduler->candidate(token, Complete{{}, {}, {}},
                               std::vector<std::byte>(3));
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));

  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.back());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::BudgetExceeded);
  EXPECT_TRUE(harness.service.poll(request).outcome.has_value());
  EXPECT_EQ(harness.adapter.publishCalls, 1U);
}

TEST(IoServiceTest, CloseWaitsForEndpointWorkAndBlocksLaterRequests) {
  Harness harness;
  const EndpointCell endpoint{31, 1};
  harness.cells.endpoint  = endpoint;
  harness.cells.direction = CellRequest::Direction::Read;
  const auto readRequest  = harness.submit();
  ASSERT_NE(readRequest.incarnation, 0U);
  const auto readToken = tokenOf(*harness.scheduler);

  const auto close =
      harness.service.closeEndpoint(endpoint, CloseMode::Graceful);
  ASSERT_TRUE(std::holds_alternative<Admitted>(close.value));
  EXPECT_EQ(harness.scheduler->entries.size(), 1U);

  const auto laterRequest =
      harness.service.submit(testCapability, OwnerCell{12});
  EXPECT_TRUE(std::holds_alternative<Refused>(laterRequest.value));
  EXPECT_EQ(harness.scheduler->entries.size(), 1U);

  harness.scheduler->candidate(readToken, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(readToken);
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.scheduler->entries.size(), 2U);
  const auto closeToken = tokenOf(*harness.scheduler, 1);

  harness.scheduler->candidate(closeToken, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(closeToken);
  static_cast<void>(harness.service.pump(8));
  const auto afterClose = harness.service.submit(testCapability, OwnerCell{13});
  EXPECT_TRUE(std::holds_alternative<Refused>(afterClose.value));
  EXPECT_EQ(harness.scheduler->entries.size(), 2U);
}

// -- M2/H2/M3/M4/M5/M12/M14/M15 regression coverage (plan §8.1) ------------

TEST(IoServiceTest, RequestDeclaringNoOutputEnvelopeIsRefused) {
  Harness harness;
  harness.cells.completionBoundBytes = 0;
  harness.cells.payloadBoundBytes    = 0;
  const auto result = harness.service.submit(testCapability, testSpec);
  ASSERT_TRUE(std::holds_alternative<Refused>(result.value));
  EXPECT_TRUE(harness.scheduler->entries.empty());
  EXPECT_TRUE(harness.cells.mintedRequests.empty());
  // The refusal still mints a typed Rejected outcome for the caller.
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::InvalidRequest);
}

TEST(IoServiceTest, OneZeroBoundStillAdmitsTheOtherChannel) {
  Harness harness;
  harness.cells.payloadBoundBytes = 0; // the completion channel stays open
  const auto request              = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  EXPECT_EQ(harness.scheduler->entries.size(), 1U);
}

TEST(IoServiceTest, MintedRefusalCarriesItsTypedErrorFields) {
  zigzag::ArenaManifold arena;
  const auto schema = makeIoCellSchema(arena);
  ASSERT_TRUE(schema);
  PayloadResolver payloads;
  ArenaIoCellStore cells(
      arena, *schema, payloads,
      [](CapabilityCell, OwnerCell) -> std::expected<CellRequest, IoErrorInfo> {
        return CellRequest{};
      });
  const auto refusal = cells.mintRefusal({IoErrorCode::BudgetExceeded, 7});
  ASSERT_TRUE(ioCellTag(arena, refusal.cell, refusal.incarnation));
  EXPECT_EQ(*ioCellTag(arena, refusal.cell, refusal.incarnation),
            IoCellTag::Outcome);
  EXPECT_EQ(ioOutcomeTag(arena, refusal), IoOutcomeTag::Rejected);

  // §3.2: a Rejected outcome names the IoErrorInfo that refused it, so
  // BudgetExceeded and Unsupported stay distinguishable to readers.
  const auto fieldValue =
      [&](const std::string_view label) -> std::optional<std::string> {
    auto at = arena.linked(refusal.cell, schema->dim(IoDimension::Occurrence),
                           zigzag::DimVector::POS);
    while (at) {
      if (arena.textOf(*at) == label) {
        const auto value = arena.linked(*at, schema->dim(IoDimension::State),
                                        zigzag::DimVector::POS);
        return value ? std::optional{arena.textOf(*value)} : std::nullopt;
      }
      at = arena.linked(*at, schema->dim(IoDimension::Occurrence),
                        zigzag::DimVector::POS);
    }
    return std::nullopt;
  };
  EXPECT_EQ(fieldValue("errorCode"),
            std::optional<std::string>{"BudgetExceeded"});
  EXPECT_EQ(fieldValue("stableDetail"), std::optional<std::string>{"7"});
}

TEST(IoServiceTest, RefusedAdmissionAbandonsTheStagedSpecification) {
  Harness harness;
  harness.scheduler->reservationRefusal =
      IoErrorInfo{IoErrorCode::BudgetExceeded};
  const auto result = harness.service.submit(testCapability, testSpec);
  ASSERT_TRUE(std::holds_alternative<Refused>(result.value));
  // A refused submission consumes its staged entry exactly like execute
  // would have, so staging tables cannot fill with dead specs.
  ASSERT_EQ(harness.adapter.abandonCalls, 1U);
  EXPECT_EQ(harness.adapter.abandoned, (std::vector<OwnerCell>{testSpec}));
}

TEST(IoServiceTest, TerminalWithoutExecuteStillAbandonsStagedSpecification) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  // Cancel before the worker turn: execute never consumes the staged
  // entry, so the terminal path must hand it back to the adapter.
  EXPECT_EQ(harness.service.cancelRequest(request).state,
            RequestCancelState::Accepted);
  static_cast<void>(harness.service.pump(8));
  ASSERT_TRUE(harness.service.poll(request).outcome.has_value());
  ASSERT_EQ(harness.adapter.abandonCalls, 1U);
  EXPECT_EQ(harness.adapter.abandoned, (std::vector<OwnerCell>{testSpec}));
}

TEST(IoServiceTest, PayloadOnPayloadlessOutcomeBecomesTypedRefusal) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  const auto bytes = publishTransientBytes(
      harness.payloads, std::array<std::byte, 1>{std::byte{'x'}});
  ASSERT_TRUE(bytes.has_value());
  auto *entry = harness.scheduler->find(token);
  ASSERT_NE(entry, nullptr);
  // EndOfStream has no payload field (§3.2): a lease attached to it is a
  // contract violation and must surface as a typed refusal, not a cell
  // the schema says cannot carry bytes.
  entry->sink->terminalCandidate(token, BackendResult{EndOfStream{{}}, {}},
                                 *bytes);
  entry->sink->releaseAcknowledged({PrivateReleaseToken{token}});
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::InvalidRequest);
  EXPECT_EQ(failed->reason.stableDetail, 4U);
}

TEST(IoServiceTest, DuplicateTerminalCandidateReleasesItsPayloadLease) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  const auto first = publishTransientBytes(
      harness.payloads, std::array<std::byte, 1>{std::byte{'a'}});
  const auto second = publishTransientBytes(
      harness.payloads, std::array<std::byte, 1>{std::byte{'b'}});
  ASSERT_TRUE(first.has_value() && second.has_value());
  EXPECT_EQ(harness.payloads.liveLeases(), 2U);
  auto *entry = harness.scheduler->find(token);
  ASSERT_NE(entry, nullptr);
  entry->sink->terminalCandidate(token, BackendResult{Complete{{}, {}, {}}, {}},
                                 *first);
  // The second candidate loses to the first, but its payload lease must
  // still be released -- a dropped candidate cannot pin bytes forever.
  entry->sink->terminalCandidate(token, BackendResult{Complete{{}, {}, {}}, {}},
                                 *second);
  entry->sink->releaseAcknowledged({PrivateReleaseToken{token}});
  static_cast<void>(harness.service.pump(8));
  // Only the winning candidate's payload survives, as the outcome's
  // owner token.
  EXPECT_EQ(harness.payloads.liveLeases(), 1U);
  harness.service.retireRequest(request);
  EXPECT_EQ(harness.payloads.liveLeases(), 0U);
}

TEST(IoServiceTest, DeferredLaneHeadsDoNotStarveEndpointConcurrency) {
  // maxEndpointConcurrency = 1 with three endpoints: E1 runs, E2 and E3
  // defer. Deferred heads must not count as busy -- otherwise every
  // waiting head sees cap+1 endpoints and none ever starts.
  Harness harness({8, 1024, 1024, 1024, 1});
  harness.cells.direction     = CellRequest::Direction::Read;
  harness.cells.endpointQueue = {EndpointCell{50, 1}, EndpointCell{51, 1},
                                 EndpointCell{52, 1}};
  const auto first            = harness.submit(OwnerCell{1});
  const auto second           = harness.submit(OwnerCell{2});
  const auto third            = harness.submit(OwnerCell{3});
  ASSERT_NE(first.incarnation, 0U);
  ASSERT_NE(second.incarnation, 0U);
  ASSERT_NE(third.incarnation, 0U);
  ASSERT_EQ(harness.scheduler->entries.size(), 1U);

  const auto token = tokenOf(*harness.scheduler, 0);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));

  // E1's slot admits exactly one deferred head: the next endpoint in
  // lane order, while the third keeps waiting.
  ASSERT_EQ(harness.scheduler->entries.size(), 2U);
  EXPECT_EQ(harness.scheduler->entries[1].command.operation.bytes.front(),
            std::byte{2});
  EXPECT_FALSE(harness.service.poll(second).outcome.has_value());
  EXPECT_FALSE(harness.service.poll(third).outcome.has_value());
}

TEST(IoServiceTest, ClosingEndpointTombstonesAreBounded) {
  Harness harness({4096, 1024, 1024, 1024, 8});
  // Each Admitted close leaves a tombstone; every close below stays in
  // flight because FakeScheduler never runs them, so none may be evicted
  // even past the bound -- an in-flight close keeps refusing a second.
  for (std::uint32_t i = 1; i <= 1025; ++i) {
    const auto closed =
        harness.service.closeEndpoint(EndpointCell{i, 1}, CloseMode::Graceful);
    ASSERT_TRUE(std::holds_alternative<Admitted>(closed.value)) << i;
  }
  harness.cells.endpoint = EndpointCell{1, 1};
  EXPECT_TRUE(std::holds_alternative<Refused>(
      harness.service.submit(testCapability, OwnerCell{9}).value));

  // Once the oldest close settles it is the one a later close evicts, so
  // endpoint churn cannot grow the set forever.
  const auto token = tokenOf(*harness.scheduler, 0);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  const auto next =
      harness.service.closeEndpoint(EndpointCell{2000, 1}, CloseMode::Graceful);
  ASSERT_TRUE(std::holds_alternative<Admitted>(next.value));
  const auto reopened = harness.service.submit(testCapability, OwnerCell{9});
  EXPECT_TRUE(std::holds_alternative<Admitted>(reopened.value));
}

TEST(IoServiceTest, PollAfterRetireStillResolvesThePublishedOutcome) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  const auto outcome = harness.service.poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());

  harness.service.retireRequest(request);
  // The live request state is gone, but the arena request->outcome link
  // survives retirement: a late poll still resolves the terminal cell.
  const auto late = harness.service.poll(request);
  EXPECT_EQ(late.phase, IoPhase::Terminal);
  ASSERT_TRUE(late.outcome.has_value());
  EXPECT_EQ(*late.outcome, *outcome);
  EXPECT_FALSE(late.progress.has_value());
}

TEST(IoServiceTest, AwaitWaiterMintFailureIsBudgetExceeded) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  harness.cells.failMintWaiter = true;
  const auto result = harness.service.await(request, {OwnerCell{22}, 1, 31});
  ASSERT_TRUE(std::holds_alternative<Ready>(result.value));
  // mintWaiter types its refusal through the expected: the fake reports
  // BudgetExceeded, which await() must surface verbatim -- not collapse.
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::BudgetExceeded);
}

TEST(IoServiceTest, ThrowingPrepareRefusesAsBackendFailureAndAbandonsSpec) {
  Harness harness;
  harness.adapter.throwOnPrepare = true;
  const auto result = harness.service.submit(testCapability, testSpec);
  ASSERT_TRUE(std::holds_alternative<Refused>(result.value));
  // prepare() threw: admit() converts it to a typed BackendFailure
  // refusal, starts no scheduler work, and still consumes the staged
  // spec like every post-resolution refusal.
  EXPECT_EQ(harness.adapter.prepareCalls, 1U);
  EXPECT_TRUE(harness.scheduler->entries.empty());
  EXPECT_EQ(harness.adapter.executeCalls, 0U);
  ASSERT_EQ(harness.adapter.abandonCalls, 1U);
  EXPECT_EQ(harness.adapter.abandoned, (std::vector<OwnerCell>{testSpec}));
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::BackendFailure);
}

TEST(IoServiceTest, PublishTerminalRefusalPropagatesItsTypedError) {
  Harness harness;
  harness.cells.publishTerminalRefusal =
      IoErrorInfo{IoErrorCode::InvalidRequest, 9};
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));

  // publishTerminal refused with InvalidRequest{9}: pump() must mint the
  // refusal carrying THAT typed error -- collapsing it to BackendFailure
  // would misreport why the terminal could not publish.
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.back());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::InvalidRequest);
  EXPECT_EQ(failed->reason.stableDetail, 9U);
}

TEST(IoServiceTest, RetireDuringPublishThrowStillFreesStateAfterMaterialize) {
  Harness harness;
  // pump()'s publishTerminal throws once: the terminal record stays
  // durable in state but no outcome cell exists yet.
  harness.cells.publishTerminalFailures = 1;
  const auto request                    = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  ASSERT_TRUE(harness.cells.outcomes.empty());

  // The owner retires while the outcome is still unmaterialized:
  // retireAfterTerminal must not strand the durable terminal -- the next
  // materialization is the last chance to drop the request state.
  harness.service.retireRequest(request);

  const auto first = harness.service.poll(request);
  ASSERT_TRUE(first.outcome.has_value());
  // ensureTerminalOutcome retried publishTerminal, which now succeeds;
  // the retireAfterTerminal flag then drops the request state. The next
  // poll resolves through the arena request->outcome link instead, which
  // reports Terminal with no live progress.
  const auto second = harness.service.poll(request);
  ASSERT_TRUE(second.outcome.has_value());
  EXPECT_EQ(*second.outcome, *first.outcome);
  EXPECT_FALSE(second.progress.has_value());
}

TEST(IoServiceTest, RetireBeforeTerminalKeepsThrownPublishResolvable) {
  Harness harness;
  // Every publishTerminal attempt throws: the terminal record stays
  // durable in state but no outcome cell -- and no arena request->outcome
  // link -- is ever written. retireRequest lands while the request is
  // still outstanding, so the terminal pass erases state under
  // retireAfterTerminal; the erase must keep the state because
  // outcomeFor cannot resolve what was never linked.
  harness.cells.publishTerminalFailures = 100;
  const auto request                    = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  harness.service.retireRequest(request);
  static_cast<void>(harness.service.pump(8));
  ASSERT_TRUE(harness.cells.outcomes.empty());

  // The retained state keeps rematerializing: each poll/await retries
  // publishTerminal, which keeps throwing, and degrades to the typed
  // BackendFailure refusal mint -- never the Stale a stranded request
  // would report, and never an empty poll.
  const auto poll = harness.service.poll(request);
  EXPECT_TRUE(poll.outcome.has_value());
  const auto awaiting = harness.service.await(request, {OwnerCell{22}, 1, 31});
  ASSERT_TRUE(std::holds_alternative<Ready>(awaiting.value));
  const auto *ready = std::get_if<Ready>(&awaiting.value);
  ASSERT_NE(ready, nullptr);
  EXPECT_NE(ready->outcome, OutcomeCell{});
  ASSERT_GE(harness.cells.outcomes.size(), 2U);
  for (const auto &outcome : harness.cells.outcomes) {
    const auto *failed = std::get_if<Failed>(&outcome);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->reason.code, IoErrorCode::BackendFailure);
  }
}

TEST(IoServiceTest, RetireBeforeTerminalKeepsRefusedPublishResolvable) {
  Harness harness;
  // publishTerminal refuses with a typed error: pump() mints the
  // rejection into state.outcome, but that cell is NOT linked from the
  // request -- outcomeFor still cannot resolve it after the state is
  // gone, so retireAfterTerminal must retain the state the same way.
  harness.cells.publishTerminalRefusal =
      IoErrorInfo{IoErrorCode::InvalidRequest, 9};
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  harness.service.retireRequest(request);
  static_cast<void>(harness.service.pump(8));
  ASSERT_EQ(harness.cells.outcomes.size(), 1U);

  const auto poll = harness.service.poll(request);
  ASSERT_TRUE(poll.outcome.has_value());
  // state.outcome is current, so poll/await report the same typed
  // InvalidRequest{9} refusal cell the terminal pass minted -- a late
  // await is Ready with that outcome, not a fresh Stale mint.
  const auto awaiting = harness.service.await(request, {OwnerCell{22}, 1, 31});
  ASSERT_TRUE(std::holds_alternative<Ready>(awaiting.value));
  const auto *ready = std::get_if<Ready>(&awaiting.value);
  ASSERT_NE(ready, nullptr);
  EXPECT_EQ(ready->outcome, *poll.outcome);
  const auto *failed = std::get_if<Failed>(&harness.cells.outcomes.front());
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->reason.code, IoErrorCode::InvalidRequest);
  EXPECT_EQ(failed->reason.stableDetail, 9U);
}

TEST(IoServiceTest, AwaitAfterRetireResolvesThePublishedOutcome) {
  Harness harness;
  const auto request = harness.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto token = tokenOf(*harness.scheduler);
  harness.scheduler->candidate(token, Complete{{}, {}, {}});
  harness.scheduler->acknowledge(token);
  static_cast<void>(harness.service.pump(8));
  const auto outcome = harness.service.poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());

  harness.service.retireRequest(request);
  // The same arena-link fallback poll() keeps: a late await reports the
  // terminal outcome Ready rather than minting a Stale refusal for
  // work that actually completed.
  const auto late = harness.service.await(request, {OwnerCell{22}, 1, 31});
  ASSERT_TRUE(std::holds_alternative<Ready>(late.value));
  const auto *ready = std::get_if<Ready>(&late.value);
  ASSERT_NE(ready, nullptr);
  EXPECT_EQ(ready->outcome, *outcome);
}

} // namespace
