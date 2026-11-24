#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/io_profile.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

// design/vortex-io-implementation-plan.md §9: IoExecutionProfile is a
// generic immutable host contract; an actor turn's direct Submit/Await is
// refused before admission, and completion for that profile is defined to
// arrive only as a later message. These tests exercise IoHost's gate, not
// any language frontend.

namespace {
using namespace xanadu::vortex;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr AdapterKey testKey{7, 11};

class RecordingScheduler final : public IoScheduler {
public:
  auto reserve(AdmissionCost cost)
      -> std::expected<SchedulerReservation, IoErrorInfo> override {
    ++reserveCalls;
    return SchedulerReservation{{nextToken++},
                                cost.commandBytes + cost.completionBytes};
  }
  auto enqueue(SchedulerReservation reservation, BackendCommand command,
               IoAdapter &, CompletionSink &sink) noexcept
      -> EnqueueResult override {
    ++enqueueCalls;
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
  std::uint64_t nextToken{1};
  std::size_t reserveCalls{};
  std::size_t enqueueCalls{};
  std::vector<Entry> entries;
};

class RecordingAdapter final : public IoAdapter {
public:
  auto prepare(const CellRequest &request, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> override {
    ++prepareCalls;
    return PreparedOperation{request.key, {std::byte{1}}};
  }
  void execute(const BackendCommand &, BackendContext &,
               CompletionSink &) noexcept override {}
  auto publish(const NativeCompletion &completion, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> override {
    return OwnerPublication{completion.result.outcome};
  }
  std::size_t prepareCalls{};
};

struct Harness {
  ArenaManifold arena;
  IoCellSchema schema;
  EndpointCell endpoint;
  CapabilityCell capability;
  PayloadResolver payloads;
  ArenaIoCellStore cells;
  RecordingScheduler *scheduler{};
  RecordingAdapter adapter;
  IoService service;
  VortexCore core;
  VortexVM vm;
  IoHost host;

  explicit Harness(const IoExecutionProfile profile)
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
        core(arena), vm(core), host(service, arena, vm, profile) {
    service.registerAdapter(testKey, adapter);
  }

  auto makeScheduler() -> std::unique_ptr<IoScheduler> {
    auto instance = std::make_unique<RecordingScheduler>();
    scheduler     = instance.get();
    return instance;
  }

  CellRef cursor() {
    const auto entry = vm.mintOpcode(OpcodeKind::Nop);
    return vm.spawnCursor(entry, "io profile gate test");
  }
};

TEST(IoProfileGateTest, InteractiveProfileAdmitsDirectSubmitAndAwait) {
  Harness h(IoExecutionProfile::Interactive);
  const auto submitted = h.host.submitCell(h.capability, h.arena.makeCell());
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;
  const auto awaited = h.host.awaitCell(request, h.cursor());
  EXPECT_TRUE(std::holds_alternative<Pending>(awaited.value));
  EXPECT_EQ(h.scheduler->enqueueCalls, 1U);
}

TEST(IoProfileGateTest,
     ActorTurnProfileRefusesDirectSubmitBeforeAnyBackendWork) {
  Harness h(IoExecutionProfile::ActorTurn);
  const auto submitted = h.host.submitCell(h.capability, h.arena.makeCell());
  ASSERT_TRUE(std::holds_alternative<Refused>(submitted.value));
  const auto outcome = std::get<Refused>(submitted.value).outcome;
  const auto record  = ioOutcomeTag(h.arena, outcome);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(h.scheduler->reserveCalls, 0U);
  EXPECT_EQ(h.scheduler->enqueueCalls, 0U);
  EXPECT_EQ(h.adapter.prepareCalls, 0U);
}

TEST(IoProfileGateTest, ActorTurnProfileRefusesDirectAwaitWithoutParking) {
  Harness h(IoExecutionProfile::ActorTurn);
  // Admit the request through a non-gated host to get a real request to
  // attempt to await; an actor turn would never reach this itself, but
  // the gate must hold even if a stray request cell is handed to it.
  const auto submitted = h.service.submit(h.capability, h.arena.makeCell());
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;

  const auto awaited = h.host.awaitCell(request, h.cursor());
  ASSERT_TRUE(std::holds_alternative<Ready>(awaited.value));
  EXPECT_EQ(h.host.pumpIo(8), 0U);
}

TEST(IoProfileGateTest, ActorTurnProfileRefusesAdmitOpenBeforeAnyWork) {
  Harness h(IoExecutionProfile::ActorTurn);
  OpenSpec spec{.adapterKey = testKey, .locator = {std::byte{1}}};
  const auto opened = h.host.admitOpen(spec);
  ASSERT_TRUE(std::holds_alternative<Refused>(opened.value));
  EXPECT_EQ(h.scheduler->enqueueCalls, 0U);
}

TEST(IoProfileGateTest, DurableEffectProfileAllowsSubmitAndAwaitSuspension) {
  Harness h(IoExecutionProfile::DurableEffect);
  const auto submitted = h.host.submitCell(h.capability, h.arena.makeCell());
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;
  const auto awaited = h.host.awaitCell(request, h.cursor());
  EXPECT_TRUE(std::holds_alternative<Pending>(awaited.value));
}

TEST(IoProfileGateTest, PolicyTablesMatchTheDesignedProfileContract) {
  EXPECT_TRUE(policyFor(IoExecutionProfile::Interactive).allowDirectSubmit);
  EXPECT_TRUE(policyFor(IoExecutionProfile::Interactive).allowAwaitSuspension);
  EXPECT_FALSE(
      policyFor(IoExecutionProfile::Interactive).completionIsLaterMessage);

  EXPECT_FALSE(policyFor(IoExecutionProfile::ActorTurn).allowDirectSubmit);
  EXPECT_FALSE(policyFor(IoExecutionProfile::ActorTurn).allowAwaitSuspension);
  EXPECT_TRUE(
      policyFor(IoExecutionProfile::ActorTurn).completionIsLaterMessage);
}

} // namespace
