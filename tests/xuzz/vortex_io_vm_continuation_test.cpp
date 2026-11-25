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

} // namespace
