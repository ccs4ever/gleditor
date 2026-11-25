#ifndef XUZZ_TESTS_IO_FRONTEND_CONFORMANCE_HPP
#define XUZZ_TESTS_IO_FRONTEND_CONFORMANCE_HPP

/**
 * @file io_frontend_conformance.hpp
 * @brief The reusable frontend conformance harness (plan §13, M7a): every
 *        language milestone instantiates it with its own runner and must pass
 *        the same checks over the M1 delayed fake backend.
 *
 * A language writes a `ConformanceDriver` that runs its own "submit one read,
 * match the submission, await it" program (in the language's real syntax,
 * lowered through its real runner) and reports the result as a
 * `FrontendRunResult<OutcomeCell>`. Then, in its own test file:
 *
 *     XUZZ_FRONTEND_CONFORMANCE(VplConformance, myDriverFactory)
 *
 * which expands to one TEST per contract below. The harness owns everything
 * the language must not: the service, scheduler, arena, VM and host.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/frontend_bridge.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xuzz_conformance {

using namespace xanadu::vortex;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

inline constexpr AdapterKey conformanceKey{7, 11};
/// The fake backend's private request tokens start here, far from any small
/// counter a cell legitimately holds, so a raw token that leaks into a
/// language-visible cell is recognisable by value.
inline constexpr std::uint64_t privateTokenBase = 0xC0FFEE00ULL;

/// The M1 delayed fake backend: admission queues the work, and the test
/// decides when (and how) each request finishes.
class DelayedScheduler final : public IoScheduler {
public:
  auto reserve(AdmissionCost cost)
      -> std::expected<SchedulerReservation, IoErrorInfo> override {
    return SchedulerReservation{{nextToken++},
                                cost.commandBytes + cost.completionBytes};
  }
  auto enqueue(SchedulerReservation reservation, BackendCommand command,
               IoAdapter &, CompletionSink &sink) noexcept
      -> EnqueueResult override {
    entries.push_back({reservation, command, &sink});
    return EnqueueResult::Queued;
  }
  void releaseUnused(SchedulerReservation) noexcept override {}
  auto cancel(PrivateRequestToken) noexcept -> CancelQueuedResult override {
    return CancelQueuedResult::AlreadyRunning;
  }
  auto pump(std::size_t) -> std::size_t override { return 0; }
  auto requestStop(ShutdownMode) -> ShutdownResult override {
    return {0, entries.size(), 0, true};
  }

  struct Entry {
    SchedulerReservation reservation;
    BackendCommand command;
    CompletionSink *sink;
  };
  void complete(const std::size_t index, IoOutcome outcome) {
    const auto &entry = entries.at(index);
    entry.sink->terminalCandidate(entry.command.token,
                                  BackendResult{std::move(outcome), {}}, {});
    entry.sink->releaseAcknowledged({PrivateReleaseToken{entry.command.token}});
  }

  std::uint64_t nextToken{privateTokenBase};
  std::vector<Entry> entries;
};

class DelayedAdapter final : public IoAdapter {
public:
  auto prepare(const CellRequest &request, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> override {
    return PreparedOperation{request.key, {std::byte{1}}};
  }
  void execute(const BackendCommand &, BackendContext &,
               CompletionSink &) noexcept override {}
  auto publish(const NativeCompletion &completion, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> override {
    return OwnerPublication{completion.result.outcome};
  }
};

/// Everything a runner under test is given and must not own.
struct World {
  ArenaManifold arena;
  IoCellSchema schema;
  EndpointCell endpoint;
  CapabilityCell capability;
  PayloadResolver payloads;
  bool refuseAdmission{};
  ArenaIoCellStore cells;
  DelayedScheduler *scheduler{};
  DelayedAdapter adapter;
  IoService service;
  VortexCore core;
  VortexVM vm;
  IoHost host;
  FrontendDiagnosticRegistry diagnostics;
  FrontendBridge bridge;

  World()
      : schema(makeIoCellSchema(arena).value()),
        endpoint(makeEndpointCell(arena, schema).value()),
        capability(makeCapabilityCell(arena, schema, endpoint).value()),
        cells(arena, schema, payloads,
              [this](CapabilityCell cap, OwnerCell specification)
                  -> std::expected<CellRequest, IoErrorInfo> {
                if (refuseAdmission) {
                  return std::unexpected{
                      IoErrorInfo{IoErrorCode::InvalidRequest, 9}};
                }
                return CellRequest{.key                  = conformanceKey,
                                   .capability           = cap,
                                   .endpoint             = endpoint,
                                   .completionBoundBytes = 64,
                                   .payloadBoundBytes    = 64,
                                   .specification        = specification};
              }),
        service({8, 1024, 1024, 1024, 8}, makeScheduler(), payloads, cells),
        core(arena), vm(core), host(service, arena, vm),
        diagnostics(FrontendDiagnosticRegistry::withBuiltins()),
        bridge(host, arena, schema, diagnostics) {
    service.registerAdapter(conformanceKey, adapter);
  }

  [[nodiscard]] CellRef specification() { return arena.makeCell("read spec"); }
  void complete(const std::size_t index, IoOutcome outcome) {
    scheduler->complete(index, std::move(outcome));
  }
  auto pump() -> std::size_t { return host.pumpIo(16); }
  [[nodiscard]] std::size_t submissions() const {
    return scheduler->entries.size();
  }
  /// Real retained bytes, so a Partial or Failed outcome has a payload.
  [[nodiscard]] auto payloadRange() -> PayloadRange {
    static const std::vector<std::byte> bytes{std::byte{'o'}, std::byte{'k'}};
    const auto origin =
        payloads.registerSource(PayloadRef{.kind = PayloadSourceKind::Transient,
                                           .source     = ++nextPayloadSource,
                                           .generation = 1,
                                           .occurrence = 1},
                                bytes);
    return PayloadRange{
        .origin = origin.value(), .offset = 0, .length = bytes.size()};
  }

private:
  auto makeScheduler() -> std::unique_ptr<IoScheduler> {
    auto instance = std::make_unique<DelayedScheduler>();
    scheduler     = instance.get();
    return instance;
  }
  std::uint64_t nextPayloadSource{};
};

using RunResult = FrontendRunResult<OutcomeCell>;

/// The runner under test. `begin` runs the language's submit-match-await
/// program once; `resume` continues the same activation after a wake and must
/// not submit again. Both report through FrontendRunResult.
class ConformanceDriver : public FrontendRunner {
public:
  virtual auto begin(World &, CapabilityCell, CellRef specification)
      -> RunResult                                         = 0;
  virtual auto resume(World &, FrontendToken) -> RunResult = 0;
  /// Every cell the language is currently holding as a value (the
  /// submission, a bound outcome, ...): the roots of the native-id audit.
  [[nodiscard]] virtual auto visibleCells() const -> std::vector<CellRef> = 0;
  /// The submission cell the language bound, if it got that far.
  [[nodiscard]] virtual auto submission() const
      -> std::optional<SubmissionCell> = 0;

  void wake(FrontendToken token, const FrontendWake why) override {
    woken.emplace_back(std::move(token), why);
  }
  std::vector<std::pair<FrontendToken, FrontendWake>> woken;
};

using DriverFactory =
    std::function<std::unique_ptr<ConformanceDriver>(World &)>;

namespace detail {

inline auto parked(const RunResult &result) -> const FrontendToken * {
  const auto *p = std::get_if<RunResult::Parked>(&result.value);
  return p ? &p->token : nullptr;
}
inline auto completed(const RunResult &result) -> std::optional<OutcomeCell> {
  const auto *c = std::get_if<RunResult::Completed>(&result.value);
  if (!c) return std::nullopt;
  return c->value;
}

} // namespace detail

/// Pending is runner state; resuming re-awaits the SAME request after the
/// host wakes the SAME token, and submits nothing.
inline void checkSameRequestPendingResume(const DriverFactory &make) {
  World w;
  const auto driver = make(w);
  const auto begun  = driver->begin(w, w.capability, w.specification());
  const auto *token = detail::parked(begun);
  ASSERT_NE(token, nullptr) << "a pending request must park, not complete";
  ASSERT_EQ(w.submissions(), 1U);
  const auto parkedToken = *token;

  static_cast<void>(w.pump());
  EXPECT_TRUE(driver->woken.empty())
      << "no wake before the request is terminal";

  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  ASSERT_EQ(driver->woken.size(), 1U);
  EXPECT_EQ(driver->woken[0].second, FrontendWake::Resumable);
  EXPECT_EQ(driver->woken[0].first.continuation, parkedToken.continuation);
  EXPECT_EQ(driver->woken[0].first.request, parkedToken.request);

  const auto resumed = driver->resume(w, driver->woken[0].first);
  const auto outcome = detail::completed(resumed);
  ASSERT_TRUE(outcome) << "the resumed run must complete with the outcome";
  EXPECT_EQ(w.submissions(), 1U) << "resume must never submit again";
  const auto polled = w.service.poll(parkedToken.request).outcome;
  ASSERT_TRUE(polled);
  EXPECT_EQ(*polled, *outcome) << "the outcome is the same request's";
  EXPECT_EQ(ioOutcomeTag(w.arena, *outcome), IoOutcomeTag::EndOfStream);
}

/// A refused admission is a value of the language: the run completes with the
/// Rejected outcome, nothing was started, and awaiting the refusal is refused.
inline void checkRefusalRemainsAValue(const DriverFactory &make) {
  World w;
  w.refuseAdmission = true;
  const auto driver = make(w);
  const auto begun  = driver->begin(w, w.capability, w.specification());
  EXPECT_EQ(detail::parked(begun), nullptr);
  ASSERT_FALSE(std::holds_alternative<RunResult::Failed>(begun.value))
      << "a refusal must not become a run failure";
  const auto outcome = detail::completed(begun);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(ioOutcomeTag(w.arena, *outcome), IoOutcomeTag::Rejected);
  EXPECT_EQ(w.submissions(), 0U) << "no external work for a refusal";

  const auto submission = driver->submission();
  ASSERT_TRUE(submission);
  EXPECT_EQ(submissionTag(w.arena, w.schema, *submission),
            IoSubmissionTag::Refused);
  const auto awaited = w.bridge.awaitSubmission(
      *submission,
      w.vm.spawnCursor(w.vm.mintOpcode(zigzag::vortex::OpcodeKind::Nop)),
      *driver, nullptr);
  const auto *failed = std::get_if<RunResult::Failed>(&awaited.value);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->diagnostic.code, frontend_codes::awaitNotAdmitted);
}

/// Every terminal tag is a value the language receives; none is a run
/// failure, and payload access follows the shared status gate.
inline void
checkTerminalOutcomesAreNotLogicalFailure(const DriverFactory &make) {
  struct Case {
    const char *name;
    std::function<IoOutcome(World &)> outcome;
    IoOutcomeTag tag;
    bool payload;
  };
  const IoProgress progress{.transferredBytes = 2, .phase = IoPhase::Terminal};
  const std::vector<Case> cases{
      {"Complete",
       [&](World &) -> IoOutcome { return Complete{{}, {}, progress}; },
       IoOutcomeTag::Complete, false},
      {"EndOfStream",
       [&](World &) -> IoOutcome { return EndOfStream{progress}; },
       IoOutcomeTag::EndOfStream, false},
      {"Partial",
       [&](World &w) -> IoOutcome {
         return Partial{w.payloadRange(), PartialCause::BackendFailure,
                        progress};
       },
       IoOutcomeTag::Partial, true},
      {"Failed",
       [&](World &) -> IoOutcome {
         return Failed{{IoErrorCode::BackendFailure, 3}, {}, progress};
       },
       IoOutcomeTag::Failed, false},
      {"FailedWithPayload",
       [&](World &w) -> IoOutcome {
         return Failed{
             {IoErrorCode::BackendFailure, 3}, w.payloadRange(), progress};
       },
       IoOutcomeTag::Failed, true},
      {"Cancelled",
       [&](World &) -> IoOutcome { return Cancelled{{}, progress, false}; },
       IoOutcomeTag::Cancelled, false},
      {"TimedOut",
       [&](World &) -> IoOutcome { return TimedOut{{}, progress, true}; },
       IoOutcomeTag::TimedOut, false},
      {"Unknown", [&](World &) -> IoOutcome { return Unknown{progress}; },
       IoOutcomeTag::Unknown, false},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    World w;
    const auto driver = make(w);
    const auto begun  = driver->begin(w, w.capability, w.specification());
    ASSERT_NE(detail::parked(begun), nullptr);
    w.complete(0, test.outcome(w));
    static_cast<void>(w.pump());
    ASSERT_EQ(driver->woken.size(), 1U);
    const auto resumed = driver->resume(w, driver->woken[0].first);
    ASSERT_FALSE(std::holds_alternative<RunResult::Failed>(resumed.value))
        << "a terminal outcome is not a logical failure";
    const auto outcome = detail::completed(resumed);
    ASSERT_TRUE(outcome);
    const auto gated = w.bridge.gate(*outcome);
    ASSERT_TRUE(gated);
    EXPECT_EQ(gated->tag, test.tag);
    EXPECT_EQ(gated->payload.has_value(), test.payload);
    const auto payload = w.bridge.payloadOf(*outcome);
    EXPECT_EQ(payload.has_value(), test.payload);
    if (!test.payload) {
      ASSERT_FALSE(payload);
      EXPECT_EQ(payload.error().code, frontend_codes::payloadBeforeStatus);
    }
  }
}

/// No raw RequestId, WaiterId or ContinuationId in any cell a language can
/// see, while parked and after the outcome arrived; and the audit itself can
/// fail (it flags the owner-only waiter graph).
inline void checkNoRawIdsInLanguageValues(const DriverFactory &make) {
  World w;
  const auto driver = make(w);
  const auto begun  = driver->begin(w, w.capability, w.specification());
  const auto *token = detail::parked(begun);
  ASSERT_NE(token, nullptr);
  const auto parkedToken = *token;

  std::vector<std::uint64_t> forbidden;
  for (const auto &entry : w.scheduler->entries) {
    forbidden.push_back(entry.command.token.value);
  }
  const auto roots = driver->visibleCells();
  ASSERT_FALSE(roots.empty());
  EXPECT_TRUE(auditLanguageValue(w.arena, w.schema, roots, forbidden).empty());

  const std::vector<CellRef> waiterRoot{parkedToken.waiter.cell};
  EXPECT_FALSE(auditLanguageValue(w.arena, w.schema, waiterRoot).empty())
      << "the audit must flag the owner-only waiter graph";

  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  ASSERT_EQ(driver->woken.size(), 1U);
  const auto resumed = driver->resume(w, driver->woken[0].first);
  ASSERT_TRUE(detail::completed(resumed));
  auto after = driver->visibleCells();
  after.push_back(detail::completed(resumed)->cell);
  EXPECT_TRUE(auditLanguageValue(w.arena, w.schema, after, forbidden).empty());
}

/// Cancelling the activation cancels its waiter only: the route is dropped
/// (no wake), and the request itself still completes independently.
inline void
checkCancelledWaiterDropsRouteNotRequest(const DriverFactory &make) {
  World w;
  const auto driver = make(w);
  const auto begun  = driver->begin(w, w.capability, w.specification());
  const auto *token = detail::parked(begun);
  ASSERT_NE(token, nullptr);
  const auto parkedToken = *token;

  EXPECT_EQ(w.host.cancelWaiter(parkedToken.waiter).state,
            WaiterCancelState::Removed);
  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  EXPECT_TRUE(driver->woken.empty()) << "a cancelled waiter is never woken";
  EXPECT_TRUE(w.service.poll(parkedToken.request).outcome)
      << "request cancellation is a separate, explicit action";
}

} // namespace xuzz_conformance

#define XUZZ_FRONTEND_CONFORMANCE(Suite, makeDriver)                           \
  TEST(Suite, SameRequestPendingResume) {                                      \
    xuzz_conformance::checkSameRequestPendingResume(makeDriver);               \
  }                                                                            \
  TEST(Suite, RefusalRemainsAValue) {                                          \
    xuzz_conformance::checkRefusalRemainsAValue(makeDriver);                   \
  }                                                                            \
  TEST(Suite, TerminalOutcomesAreNotLogicalFailure) {                          \
    xuzz_conformance::checkTerminalOutcomesAreNotLogicalFailure(makeDriver);   \
  }                                                                            \
  TEST(Suite, NoRawIdsInLanguageValues) {                                      \
    xuzz_conformance::checkNoRawIdsInLanguageValues(makeDriver);               \
  }                                                                            \
  TEST(Suite, CancelledWaiterDropsRouteNotRequest) {                           \
    xuzz_conformance::checkCancelledWaiterDropsRouteNotRequest(makeDriver);    \
  }

#endif
