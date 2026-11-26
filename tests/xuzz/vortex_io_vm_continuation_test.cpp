#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {

using namespace xanadu::vortex;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr AdapterKey testKey{7, 11};

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

  std::uint64_t nextToken{1};
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
    ++publicationCount;
    return OwnerPublication{completion.result.outcome};
  }

  std::size_t publicationCount{};
};

struct Harness {
  ArenaManifold arena;
  IoCellSchema schema;
  EndpointCell endpoint;
  CapabilityCell capability;
  PayloadResolver payloads;
  ArenaIoCellStore cells;
  DelayedScheduler *scheduler{};
  DelayedAdapter adapter;
  IoService service;
  VortexCore core;
  VortexVM vm;
  IoHost host;

  Harness()
      : schema(makeIoCellSchema(arena).value()),
        endpoint(makeEndpointCell(arena, schema).value()),
        capability(makeCapabilityCell(arena, schema, endpoint).value()),
        cells(arena, schema, payloads,
              [this](CapabilityCell cap, OwnerCell specification)
                  -> std::expected<CellRequest, IoErrorInfo> {
                return CellRequest{.key                  = testKey,
                                   .capability           = cap,
                                   .endpoint             = endpoint,
                                   .completionBoundBytes = 64,
                                   .payloadBoundBytes    = 64,
                                   .specification        = specification};
              }),
        service({8, 1024, 1024, 1024, 8}, makeScheduler(), payloads, cells),
        core(arena), vm(core), host(service, arena, vm) {
    service.registerAdapter(testKey, adapter);
  }

  auto makeScheduler() -> std::unique_ptr<IoScheduler> {
    auto instance = std::make_unique<DelayedScheduler>();
    scheduler     = instance.get();
    return instance;
  }

  RequestCell submit() {
    auto result = service.submit(capability, arena.makeCell("request"));
    const auto *admitted = std::get_if<Admitted>(&result.value);
    return admitted ? admitted->request : RequestCell{};
  }

  CellRef cursor() {
    const auto entry = vm.mintOpcode(OpcodeKind::Nop);
    return vm.spawnCursor(entry, "io continuation test");
  }
};

TEST(VortexIoVmContinuationTest, ParkPreservesChoiceAndDoesNotBurnCycles) {
  Harness h;
  const auto request = h.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto cursor      = h.cursor();
  const auto alternative = h.vm.mintOpcode(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, alternative);
  const auto mark = h.arena.mark();

  const auto pending = h.host.awaitCell(request, cursor);
  ASSERT_TRUE(std::holds_alternative<Pending>(pending.value));
  const auto cycles  = h.vm.totalCycles();
  const auto stepped = h.vm.step(cursor);
  EXPECT_TRUE(stepped.success);
  EXPECT_EQ(stepped.disposition, zigzag::vortex::StepDisposition::Pending);
  EXPECT_EQ(h.vm.totalCycles(), cycles);
  static_cast<void>(h.vm.run(cursor, 100));
  EXPECT_EQ(h.vm.totalCycles(), cycles);
  ASSERT_EQ(h.vm.choiceStack().size(), 1U);
  EXPECT_EQ(h.vm.choiceStack().front().cursor, cursor);
  EXPECT_TRUE(h.arena.release(mark));
}

TEST(VortexIoVmContinuationTest,
     AwaitOpcodeResumesAtTheSameInstructionAndPublishesItsOutputOnce) {
  Harness h;
  const auto request = h.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto await  = h.vm.mintOpcode(OpcodeKind::Await);
  const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const auto output = h.arena.makeCell();
  h.core.bindInput(await, request.cell);
  h.core.bindOutput(await, output);
  ASSERT_TRUE(h.arena.link(await, h.core.dims().spin, DimVector::POS, halt));
  const auto cursor = h.vm.spawnCursor(await);

  const auto cycles  = h.vm.totalCycles();
  const auto pending = h.vm.step(cursor);
  ASSERT_TRUE(pending.success) << pending.errorMessage;
  ASSERT_EQ(pending.disposition, zigzag::vortex::StepDisposition::Pending);
  ASSERT_TRUE(pending.pending);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), await);
  EXPECT_FALSE(h.vm.cellReferenceOf(output));
  EXPECT_EQ(h.vm.totalCycles(), cycles);

  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), await);
  EXPECT_FALSE(h.vm.cellReferenceOf(output));
  EXPECT_EQ(h.vm.totalCycles(), cycles);

  const auto resumed = h.vm.step(cursor);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(resumed.disposition, zigzag::vortex::StepDisposition::Progress);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), halt);
  const auto outputReference = h.vm.cellReferenceOf(output);
  ASSERT_TRUE(outputReference);
  EXPECT_EQ(*outputReference, h.service.poll(request).outcome->cell);
  EXPECT_EQ(h.vm.totalCycles(), cycles + 1);
}

TEST(VortexIoVmContinuationTest,
     AwaitRematerializesRolledBackOutcomeWithoutTouchingReusedCell) {
  Harness h;
  const auto request = h.submit();
  const auto await   = h.vm.mintOpcode(OpcodeKind::Await);
  const auto halt    = h.vm.mintOpcode(OpcodeKind::Halt);
  const auto output  = h.arena.makeCell();
  h.core.bindInput(await, request.cell);
  h.core.bindOutput(await, output);
  ASSERT_TRUE(h.arena.link(await, h.core.dims().spin, DimVector::POS, halt));
  const auto cursor = h.vm.spawnCursor(await);
  const auto mark   = h.arena.mark();
  const auto cycles = h.vm.totalCycles();

  const auto pending = h.vm.step(cursor);
  ASSERT_TRUE(pending.success) << pending.errorMessage;
  ASSERT_EQ(pending.disposition, zigzag::vortex::StepDisposition::Pending);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), await);
  EXPECT_FALSE(h.vm.cellReferenceOf(output));
  EXPECT_EQ(h.vm.totalCycles(), cycles);

  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  const auto staleOutcome = h.service.poll(request).outcome;
  ASSERT_TRUE(staleOutcome);
  ASSERT_TRUE(ioOutcomeTag(h.arena, *staleOutcome));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), await);
  EXPECT_FALSE(h.vm.cellReferenceOf(output));
  EXPECT_EQ(h.vm.totalCycles(), cycles);

  ASSERT_TRUE(h.arena.release(mark));
  EXPECT_FALSE(ioOutcomeTag(h.arena, *staleOutcome));
  CellRef reused = 0;
  while (reused < staleOutcome->cell)
    reused = h.arena.makeCell("unrelated reused allocation");
  ASSERT_EQ(reused, staleOutcome->cell);
  const auto reusedText = h.arena.textOf(reused);

  const auto resumed = h.vm.step(cursor);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(resumed.disposition, zigzag::vortex::StepDisposition::Progress);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), halt);
  const auto freshOutcome = h.service.poll(request).outcome;
  ASSERT_TRUE(freshOutcome);
  EXPECT_NE(*freshOutcome, *staleOutcome);
  ASSERT_TRUE(ioOutcomeTag(h.arena, *freshOutcome));
  const auto outputReference = h.vm.cellReferenceOf(output);
  ASSERT_TRUE(outputReference);
  EXPECT_EQ(*outputReference, freshOutcome->cell);
  EXPECT_EQ(h.arena.textOf(reused), reusedText);
  EXPECT_EQ(h.adapter.publicationCount, 1U);
  ASSERT_EQ(h.scheduler->entries.size(), 1U);
  EXPECT_EQ(h.vm.totalCycles(), cycles + 1);
}

TEST(VortexIoVmContinuationTest,
     CompletionBeforePumpAndAfterPumpBothReachTheAwaitOwner) {
  Harness h;
  const auto beforeRequest = h.submit();
  const auto beforeCursor  = h.cursor();
  h.scheduler->complete(0, EndOfStream{{}});
  EXPECT_FALSE(h.service.poll(beforeRequest).outcome);
  EXPECT_EQ(h.adapter.publicationCount, 0U);
  const auto before = h.host.awaitCell(beforeRequest, beforeCursor);
  ASSERT_TRUE(std::holds_alternative<Pending>(before.value));
  const auto beforePending = std::get<Pending>(before.value);
  static_cast<void>(h.host.pumpIo(8));
  EXPECT_EQ(h.adapter.publicationCount, 1U);
  EXPECT_TRUE(h.vm.takeAwaitOutcome(beforeCursor,
                                    *h.arena.incarnationOf(beforeCursor),
                                    beforePending.continuation));

  const auto afterRequest = h.submit();
  h.scheduler->complete(1, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  ASSERT_TRUE(h.service.poll(afterRequest).outcome);
  const auto after = h.host.awaitCell(afterRequest, beforeCursor);
  ASSERT_TRUE(std::holds_alternative<Ready>(after.value));
  EXPECT_EQ(h.adapter.publicationCount, 2U);
}

TEST(VortexIoVmContinuationTest,
     DuplicateAwaitAndRetireKeepOneParkedContinuation) {
  Harness h;
  const auto request = h.submit();
  const auto cursor  = h.cursor();
  const auto first   = h.host.awaitCell(request, cursor);
  const auto second  = h.host.awaitCell(request, cursor);
  ASSERT_TRUE(std::holds_alternative<Pending>(first.value));
  ASSERT_TRUE(std::holds_alternative<Pending>(second.value));
  const auto firstPending  = std::get<Pending>(first.value);
  const auto secondPending = std::get<Pending>(second.value);
  EXPECT_EQ(firstPending.continuation, secondPending.continuation);
  EXPECT_EQ(firstPending.waiter, secondPending.waiter);
  h.host.retireWaiter(firstPending.waiter);
  h.host.retireWaiter(firstPending.waiter);
  EXPECT_FALSE(h.vm.takeAwaitOutcome(cursor, *h.arena.incarnationOf(cursor),
                                     firstPending.continuation));

  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  EXPECT_FALSE(h.vm.takeAwaitOutcome(cursor, *h.arena.incarnationOf(cursor),
                                     firstPending.continuation));
}

TEST(VortexIoVmContinuationTest,
     SameRequestCanBeReawaitedAfterBacktrackingItsOutcomeCell) {
  Harness h;
  const auto request = h.submit();
  const auto cursor  = h.cursor();
  const auto mark    = h.arena.mark();
  const auto pending = h.host.awaitCell(request, cursor);
  ASSERT_TRUE(std::holds_alternative<Pending>(pending.value));

  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  const auto firstOutcome =
      h.vm.takeAwaitOutcome(cursor, *h.arena.incarnationOf(cursor),
                            std::get<Pending>(pending.value).continuation);
  ASSERT_TRUE(firstOutcome);
  ASSERT_TRUE(ioOutcomeTag(h.arena, *firstOutcome));
  ASSERT_TRUE(h.arena.release(mark));

  const auto repeated = h.host.awaitCell(request, cursor);
  ASSERT_TRUE(std::holds_alternative<Ready>(repeated.value));
  EXPECT_TRUE(ioOutcomeTag(h.arena, std::get<Ready>(repeated.value).outcome));
}

TEST(VortexIoVmContinuationTest,
     StaleParkedCursorCannotReceiveCompletionAfterIndexReuse) {
  Harness h;
  const auto request          = h.submit();
  const auto mark             = h.arena.mark();
  const auto staleCursor      = h.cursor();
  const auto staleIncarnation = h.arena.incarnationOf(staleCursor);
  ASSERT_TRUE(staleIncarnation);
  const auto pending = h.host.awaitCell(request, staleCursor);
  ASSERT_TRUE(std::holds_alternative<Pending>(pending.value));
  ASSERT_TRUE(h.arena.release(mark));
  static_cast<void>(h.arena.makeCell("released opcode slot"));
  const auto replacement = h.arena.makeCell("replacement");
  ASSERT_EQ(replacement, staleCursor);
  ASSERT_NE(h.arena.incarnationOf(replacement), staleIncarnation);

  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  EXPECT_FALSE(
      h.vm.takeAwaitOutcome(replacement, *h.arena.incarnationOf(replacement),
                            std::get<Pending>(pending.value).continuation));
  EXPECT_TRUE(h.service.poll(request).outcome);
}

TEST(VortexIoVmContinuationTest,
     CancelWaiterAfterTerminalKeepsParkedAwaitDeliverable) {
  Harness h;
  const auto request = h.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto cursor = h.cursor();
  // Park the await BEFORE the terminal events are pumped: the service has
  // no outcome yet, so await mints a live waiter and parks the cursor.
  const auto pending = h.host.awaitCell(request, cursor);
  ASSERT_TRUE(std::holds_alternative<Pending>(pending.value));
  const auto parkedPending = std::get<Pending>(pending.value);

  // The backend completes and the whole terminal pass runs -- but the
  // work budget is spent entirely inside service_.pump, so pumpIo never
  // reaches its resume loop. pump() retires the waiter as part of
  // publishing the outcome, leaving the parked entry to deliver it.
  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(3));
  ASSERT_TRUE(h.service.poll(request).outcome.has_value());

  // The service already retired the waiter: cancelWaiter reports
  // AlreadyComplete. The parked entry must survive -- the outcome is
  // still deliverable through poll(), and erasing it here would strand
  // the continuation forever.
  const auto cancelled = h.host.cancelWaiter(parkedPending.waiter);
  EXPECT_EQ(cancelled.state, WaiterCancelState::AlreadyComplete);

  static_cast<void>(h.host.pumpIo(8));
  EXPECT_TRUE(h.vm.takeAwaitOutcome(cursor, *h.arena.incarnationOf(cursor),
                                    parkedPending.continuation));
}

TEST(VortexIoVmContinuationTest,
     AwaitHostFailureIsAStepFailureNotAnExceptionAndLeaksNoContinuation) {
  Harness h;
  const auto request = h.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto await  = h.vm.mintOpcode(OpcodeKind::Await);
  const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const auto output = h.arena.makeCell();
  h.core.bindInput(await, request.cell);
  h.core.bindOutput(await, output);
  ASSERT_TRUE(h.arena.link(await, h.core.dims().spin, DimVector::POS, halt));
  const auto cursor = h.vm.spawnCursor(await);
  const auto inc    = *h.arena.incarnationOf(cursor);

  // No cell can be minted any more, so the waiter the await needs cannot be
  // either: the host throws from inside the VM's handler call.
  const auto limit = h.arena.allocationLimit();
  h.arena.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(h.arena.cellCount()));
  zigzag::vortex::VmExecutionResult stepped;
  EXPECT_NO_THROW(stepped = h.vm.step(cursor));
  h.arena.setAllocationLimitForTesting(limit);
  EXPECT_FALSE(stepped.success);
  EXPECT_EQ(stepped.disposition, zigzag::vortex::StepDisposition::Failed);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), await);

  // The continuation the host issued for the failed await was retired: the
  // VM no longer recognises it as belonging to the cursor.
  const auto parked = h.vm.parkAwait(cursor, inc, 1, WaiterCell{});
  EXPECT_FALSE(parked.success);
  EXPECT_NE(parked.errorMessage.find("does not belong"), std::string::npos)
      << parked.errorMessage;
}

TEST(VortexIoVmContinuationTest, AwaitFromAForeignThreadThrowsAndMintsNothing) {
  Harness h;
  const auto request = h.submit();
  ASSERT_NE(request.incarnation, 0U);
  const auto await  = h.vm.mintOpcode(OpcodeKind::Await);
  const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
  const auto output = h.arena.makeCell();
  h.core.bindInput(await, request.cell);
  h.core.bindOutput(await, output);
  ASSERT_TRUE(h.arena.link(await, h.core.dims().spin, DimVector::POS, halt));
  const auto cursor = h.vm.spawnCursor(await);

  // The owner discipline is the contract: a refusal minted from the wrong
  // thread would be an arena mutation off the owner.
  const auto cellsBefore = h.arena.cellCount();
  bool threw             = false;
  std::thread foreign([&] {
    try {
      static_cast<void>(h.vm.step(cursor));
    } catch (const std::logic_error &) {
      threw = true;
    }
  });
  foreign.join();
  EXPECT_TRUE(threw);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
}

// ---- The Submit opcode over a counting fake host --------------------------

/// A Submit host that admits through the harness service (so an Await over its
/// request is real) and counts every call; `refuseNext` turns the arm to a
/// Rejected outcome, `throwNext` makes the host throw.
struct SubmitProbe {
  Harness &h;
  std::size_t calls{};
  bool refuseNext{};
  bool throwNext{};
  std::vector<CellRef> submissions;

  explicit SubmitProbe(Harness &harness) : h(harness) {
    h.vm.setSubmitHandler(
        [this](const CapabilityCell capability, const CellRef description,
               const CellRef) -> zigzag::vortex::VmSubmission {
          ++calls;
          if (throwNext) throw std::runtime_error("fake submit host failed");
          if (refuseNext) {
            const auto outcome = h.service.refuse(IoErrorInfo{});
            const auto sum =
                makeRefusedSubmissionCell(h.arena, h.schema, outcome);
            submissions.push_back(sum->cell);
            return {*sum, 1, outcome.cell};
          }
          auto result          = h.service.submit(capability, description);
          const auto &admitted = std::get<Admitted>(result.value);
          const auto sum =
              makeAdmittedSubmissionCell(h.arena, h.schema, admitted.request);
          submissions.push_back(sum->cell);
          return {*sum, 0, admitted.request.cell};
        });
  }
  ~SubmitProbe() { h.vm.setSubmitHandler({}); }
};

struct SubmitOps {
  CellRef submit;
  CellRef submission;
  CellRef arm;
  CellRef payload;
};

auto mintSubmit(Harness &h) -> SubmitOps {
  SubmitOps ops{.submit     = h.vm.mintOpcode(OpcodeKind::Submit),
                .submission = h.arena.makeCell(),
                .arm        = h.arena.makeCell(),
                .payload    = h.arena.makeCell()};
  h.core.bindInput(ops.submit, h.capability.cell);
  h.core.bindInput(ops.submit, h.arena.makeCell("description"));
  h.core.bindOutput(ops.submit, ops.submission);
  h.core.bindOutput(ops.submit, ops.arm);
  h.core.bindOutput(ops.submit, ops.payload);
  return ops;
}

auto armOf(Harness &h, const SubmitOps &ops) -> std::int64_t {
  const auto value = h.core.render(ops.arm);
  const auto *arm  = value ? std::get_if<std::int64_t>(&*value) : nullptr;
  return arm ? *arm : -1;
}

void spinTo(Harness &h, const CellRef from, const CellRef to) {
  ASSERT_TRUE(h.arena.link(from, h.core.dims().spin, DimVector::POS, to));
}

TEST(VortexSubmitOpcodeTest, AdmissionLeavesTheSumItsArmAndItsPayload) {
  Harness h;
  SubmitProbe host(h);
  const auto ops  = mintSubmit(h);
  const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
  spinTo(h, ops.submit, halt);
  const auto cursor = h.vm.spawnCursor(ops.submit);

  const auto cycles  = h.vm.totalCycles();
  const auto stepped = h.vm.step(cursor);
  ASSERT_TRUE(stepped.success) << stepped.errorMessage;
  EXPECT_EQ(stepped.disposition, zigzag::vortex::StepDisposition::Progress);
  EXPECT_EQ(host.calls, 1U);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), halt);
  EXPECT_EQ(h.vm.totalCycles(), cycles + 1);
  ASSERT_EQ(host.submissions.size(), 1U);
  EXPECT_EQ(h.vm.cellReferenceOf(ops.submission), host.submissions.front());
  EXPECT_EQ(armOf(h, ops), 0);
  const auto payload = h.vm.cellReferenceOf(ops.payload);
  ASSERT_TRUE(payload);
  EXPECT_EQ(ioCellTag(h.arena, *payload, *h.arena.incarnationOf(*payload)),
            IoCellTag::Request);
}

TEST(VortexSubmitOpcodeTest, RefusalIsAValueOnTheSecondArmNeverATrap) {
  Harness h;
  SubmitProbe host(h);
  host.refuseNext = true;
  const auto ops  = mintSubmit(h);
  const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
  spinTo(h, ops.submit, halt);
  const auto cursor = h.vm.spawnCursor(ops.submit);

  const auto stepped = h.vm.step(cursor);
  ASSERT_TRUE(stepped.success) << stepped.errorMessage;
  EXPECT_EQ(armOf(h, ops), 1);
  const auto payload = h.vm.cellReferenceOf(ops.payload);
  ASSERT_TRUE(payload);
  EXPECT_EQ(ioCellTag(h.arena, *payload, *h.arena.incarnationOf(*payload)),
            IoCellTag::Outcome);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), halt);
}

TEST(VortexSubmitOpcodeTest, AHostThatThrowsIsATypedStepFailure) {
  Harness h;
  SubmitProbe host(h);
  host.throwNext  = true;
  const auto ops  = mintSubmit(h);
  const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
  spinTo(h, ops.submit, halt);
  const auto cursor = h.vm.spawnCursor(ops.submit);

  const auto cycles = h.vm.totalCycles();
  zigzag::vortex::VmExecutionResult stepped;
  EXPECT_NO_THROW(stepped = h.vm.step(cursor));
  EXPECT_FALSE(stepped.success);
  EXPECT_EQ(stepped.disposition, zigzag::vortex::StepDisposition::Failed);
  EXPECT_NE(stepped.errorMessage.find("fake submit host failed"),
            std::string::npos);
  EXPECT_EQ(host.calls, 1U);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), ops.submit) << "no advance";
  EXPECT_EQ(h.vm.totalCycles(), cycles);
  EXPECT_FALSE(h.vm.cellReferenceOf(ops.submission)) << "nothing published";
  EXPECT_TRUE(host.submissions.empty());
}

TEST(VortexSubmitOpcodeTest, AnInputThatIsNotACapabilityFailsBeforeTheHost) {
  Harness h;
  SubmitProbe host(h);
  const auto ops = mintSubmit(h);
  // Rebind the capability slot to an ordinary cell.
  const auto other = h.vm.mintOpcode(OpcodeKind::Submit);
  h.core.bindInput(other, h.arena.makeCell("plain"));
  h.core.bindInput(other, h.arena.makeCell("description"));
  h.core.bindOutput(other, h.arena.makeCell());
  h.core.bindOutput(other, h.arena.makeCell());
  h.core.bindOutput(other, h.arena.makeCell());
  const auto cursor  = h.vm.spawnCursor(other);
  const auto stepped = h.vm.step(cursor);
  EXPECT_FALSE(stepped.success);
  EXPECT_EQ(host.calls, 0U);
  static_cast<void>(ops);

  Harness bare;
  const auto lone    = mintSubmit(bare);
  const auto cursor2 = bare.vm.spawnCursor(lone.submit);
  const auto noHost  = bare.vm.step(cursor2);
  EXPECT_FALSE(noHost.success);
  EXPECT_NE(noHost.errorMessage.find("no I/O host"), std::string::npos);
}

/// Submit, then Branch on the arm: the admitted arm falls through to an Await
/// of the payload, the refusal arm jumps over it; both end in Halt.
struct MatchProgram {
  SubmitOps ops;
  CellRef branch;
  CellRef await;
  CellRef awaitOut;
  CellRef admittedHalt;
  CellRef refusedHalt;
  CellRef cursor;
};

auto matchProgram(Harness &h) -> MatchProgram {
  MatchProgram p;
  p.ops          = mintSubmit(h);
  p.branch       = h.vm.mintOpcode(OpcodeKind::Branch);
  p.await        = h.vm.mintOpcode(OpcodeKind::Await);
  p.awaitOut     = h.arena.makeCell();
  p.admittedHalt = h.vm.mintOpcode(OpcodeKind::Halt);
  p.refusedHalt  = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(p.branch, p.ops.arm);
  h.core.bindInput(p.branch, h.arena.makeScalarCell(
                                 static_cast<std::int64_t>(p.refusedHalt)));
  h.core.bindInput(p.await, p.ops.payload);
  h.core.bindOutput(p.await, p.awaitOut);
  spinTo(h, p.ops.submit, p.branch);
  spinTo(h, p.branch, p.await);
  spinTo(h, p.await, p.admittedHalt);
  p.cursor = h.vm.spawnCursor(p.ops.submit);
  return p;
}

TEST(VortexSubmitOpcodeTest, ABranchOnTheArmMatchesAdmittedAndRefusedArms) {
  {
    Harness h;
    SubmitProbe host(h);
    const auto p = matchProgram(h);
    const auto r = h.vm.run(p.cursor, 100);
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_EQ(r.disposition, zigzag::vortex::StepDisposition::Pending)
        << "the admitted arm falls through to the Await and parks";
    EXPECT_EQ(host.calls, 1U);
    EXPECT_EQ(h.vm.getCursorOpcode(p.cursor), p.await);
  }
  {
    Harness h;
    SubmitProbe host(h);
    host.refuseNext = true;
    const auto p    = matchProgram(h);
    const auto r    = h.vm.run(p.cursor, 100);
    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_EQ(r.errorMessage, "Halted");
    EXPECT_EQ(h.vm.getCursorOpcode(p.cursor), p.refusedHalt)
        << "the refusal arm never reaches the Await";
    EXPECT_EQ(host.calls, 1U);
  }
}

TEST(VortexSubmitOpcodeTest, ResumingAfterTheAwaitParkNeverSubmitsAgain) {
  Harness h;
  SubmitProbe host(h);
  const auto p     = matchProgram(h);
  const auto first = h.vm.run(p.cursor, 100);
  ASSERT_EQ(first.disposition, zigzag::vortex::StepDisposition::Pending);
  ASSERT_EQ(host.calls, 1U);

  // Running again while parked is the same park, not a re-execution.
  const auto again = h.vm.run(p.cursor, 100);
  EXPECT_EQ(again.disposition, zigzag::vortex::StepDisposition::Pending);
  EXPECT_EQ(host.calls, 1U);
  EXPECT_EQ(h.vm.getCursorOpcode(p.cursor), p.await)
      << "the continuation resumes at the Await, after the Submit";

  h.scheduler->complete(0, EndOfStream{{}});
  static_cast<void>(h.host.pumpIo(8));
  const auto resumed = h.vm.run(p.cursor, 100);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(resumed.errorMessage, "Halted");
  EXPECT_EQ(host.calls, 1U) << "one Submit executed, one host admission";
  EXPECT_TRUE(h.vm.cellReferenceOf(p.awaitOut));
}

TEST(VortexSubmitOpcodeTest, BacktrackingOverASubmitSubmitsAgainOnReexecution) {
  // Choice leaves the second Submit as the alternative; the first Submit then
  // Fails into it. Each executed Submit is its own admission: a request is an
  // effect on the world and backtracking does not recall it, so a new
  // execution creates a new operation (plan §5).
  Harness h;
  SubmitProbe host(h);
  const auto choice = h.vm.mintOpcode(OpcodeKind::Choice);
  const auto first  = mintSubmit(h);
  const auto fail   = h.vm.mintOpcode(OpcodeKind::Fail);
  const auto second = mintSubmit(h);
  const auto halt   = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(
      choice, h.arena.makeScalarCell(static_cast<std::int64_t>(second.submit)));
  spinTo(h, choice, first.submit);
  spinTo(h, first.submit, fail);
  spinTo(h, second.submit, halt);
  const auto cursor = h.vm.spawnCursor(choice);

  const auto r = h.vm.run(cursor, 100);
  ASSERT_TRUE(r.success) << r.errorMessage;
  EXPECT_EQ(r.errorMessage, "Halted");
  EXPECT_EQ(host.calls, 2U) << "each executed Submit admits once";

  // And the same Submit instruction executed twice (a loop body, a re-entered
  // frame) is two admissions, not one cached one.
  Harness again;
  SubmitProbe loopHost(again);
  const auto op   = mintSubmit(again);
  const auto stop = again.vm.mintOpcode(OpcodeKind::Halt);
  spinTo(again, op.submit, stop);
  const auto loopCursor = again.vm.spawnCursor(op.submit);
  ASSERT_TRUE(again.vm.step(loopCursor).success);
  again.vm.setCursorOpcode(loopCursor, op.submit);
  ASSERT_TRUE(again.vm.step(loopCursor).success);
  EXPECT_EQ(loopHost.calls, 2U);
  EXPECT_NE(loopHost.submissions[0], loopHost.submissions[1]);
}

TEST(VortexSubmitOpcodeTest,
     ARetryLoopOverASubmitLeavesTheServiceAndArenaBounded) {
  // M7-VQL gate r1, vortex M1. Each pass admits a request and then Fails past
  // it, so the arena rollback undoes the cells that named the request while
  // the request is still running. The host is told before the cells go and
  // forgets the request (it is not recalled), so N passes neither exhaust the
  // request budget nor leave cells behind.
  Harness h;
  SubmitProbe host(h);
  constexpr std::size_t passes = 6; // below the harness's 8-request budget
  std::size_t notified         = 0;
  h.vm.setSubmitRollbackHandler([&](const zigzag::vortex::VmSubmission &lost) {
    ++notified;
    ASSERT_EQ(lost.arm, 0);
    h.host.orphanRequest(
        RequestCell{lost.payload, *h.arena.incarnationOf(lost.payload)});
  });
  std::vector<CellRef> choices;
  std::vector<SubmitOps> submits;
  std::vector<CellRef> fails;
  for (std::size_t i = 0; i < passes; ++i) {
    choices.push_back(h.vm.mintOpcode(OpcodeKind::Choice));
    submits.push_back(mintSubmit(h));
    fails.push_back(h.vm.mintOpcode(OpcodeKind::Fail));
  }
  const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
  for (std::size_t i = 0; i < passes; ++i) {
    const auto alternative = i + 1 < passes ? choices[i + 1] : halt;
    h.core.bindInput(choices[i], h.arena.makeScalarCell(
                                     static_cast<std::int64_t>(alternative)));
    spinTo(h, choices[i], submits[i].submit);
    spinTo(h, submits[i].submit, fails[i]);
  }
  const auto cursor = h.vm.spawnCursor(choices[0]);
  const auto cells  = h.arena.cellCount();

  const auto r = h.vm.run(cursor, 1000);
  ASSERT_TRUE(r.success) << r.errorMessage;
  EXPECT_EQ(r.errorMessage, "Halted");
  EXPECT_EQ(host.calls, passes) << "every pass is a new operation";
  EXPECT_EQ(notified, passes) << "each rollback was announced before its cells";
  EXPECT_EQ(h.arena.cellCount(), cells) << "the arena holds no leftover cells";

  // The requests run to their end and are then dropped, not published.
  // The endpoint runs one request at a time: each end starts the next.
  for (std::size_t i = 0; i < passes; ++i) {
    ASSERT_LT(i, h.scheduler->entries.size());
    h.scheduler->complete(i, EndOfStream{{}});
    static_cast<void>(h.service.pump(64));
  }
  const auto after = h.arena.cellCount();
  EXPECT_EQ(after, cells) << "a dropped terminal mints no refusal cell";
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_NE(h.submit().incarnation, 0U)
        << "the budget is whole again: admission " << i;
  }
}

TEST(VortexSubmitOpcodeTest, AnUnknownArmOrdinalIsATypedStepFailure) {
  Harness h;
  h.vm.setSubmitHandler([&](const CapabilityCell capability,
                            const CellRef description,
                            const CellRef) -> zigzag::vortex::VmSubmission {
    auto result          = h.service.submit(capability, description);
    const auto &admitted = std::get<Admitted>(result.value);
    const auto sum =
        makeAdmittedSubmissionCell(h.arena, h.schema, admitted.request);
    return {*sum, 7, admitted.request.cell};
  });
  const auto ops  = mintSubmit(h);
  const auto halt = h.vm.mintOpcode(OpcodeKind::Halt);
  spinTo(h, ops.submit, halt);
  const auto stepped = h.vm.step(h.vm.spawnCursor(ops.submit));
  EXPECT_FALSE(stepped.success);
  EXPECT_NE(stepped.errorMessage.find("unknown arm"), std::string::npos);
  EXPECT_FALSE(h.vm.cellReferenceOf(ops.submission)) << "nothing published";
  h.vm.setSubmitHandler({});
}

} // namespace
