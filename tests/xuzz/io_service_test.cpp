#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <mutex>
#include <semaphore>
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
    return CellRequest{.key                  = testKey,
                       .capability           = capability,
                       .endpoint             = endpoint,
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
  auto publishTerminal(RequestCell, const IoOutcome &outcome,
                       const IoProgress &, const std::optional<PayloadLease> &)
      -> std::expected<OutcomeCell, IoErrorInfo> override {
    outcomes.push_back(outcome);
    const OutcomeCell cell{nextCell++, 1};
    publishedOutcomes.push_back(cell);
    return cell;
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
      -> WaiterCell override {
    const WaiterCell waiter{nextCell++, 1};
    waiters.emplace_back(waiter, request, registration);
    return waiter;
  }
  void retireWaiter(WaiterCell waiter) noexcept override {
    retired.push_back(waiter);
  }

  bool rejectFreeze{};
  bool failMintRequest{};
  std::size_t completionBoundBytes{1024};
  std::size_t payloadBoundBytes{1024};
  std::optional<EndpointCell> endpoint;
  CellRequest::Direction direction{CellRequest::Direction::Control};
  std::uint32_t nextCell{100};
  std::vector<RequestCell> mintedRequests;
  std::vector<IoOutcome> outcomes;
  std::vector<OutcomeCell> publishedOutcomes;
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
  void candidate(PrivateRequestToken token, IoOutcome outcome) {
    find(token)->sink->terminalCandidate(
        token, BackendResult{std::move(outcome), {}}, {});
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
    return OwnerPublication{completion.result.outcome};
  }
  std::size_t prepareCalls{};
  std::size_t executeCalls{};
  std::size_t publishCalls{};
  std::thread::id publishThread;
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

  harness.service.retireRequest(request);
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
        return CellRequest{.key           = testKey,
                           .capability    = requestCapability,
                           .endpoint      = endpoint,
                           .direction     = CellRequest::Direction::Read,
                           .specification = spec};
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

  harness.scheduler->candidate(token,
                               Complete{{}, std::vector<std::byte>(3), {}});
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

} // namespace
