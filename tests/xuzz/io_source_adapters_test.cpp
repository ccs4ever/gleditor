#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"
#include "common/xanadu/vortex/source_adapters.hpp"

namespace {
using namespace xanadu::vortex;

std::vector<std::byte> bytesOf(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i)
    out[i] = static_cast<std::byte>(text[i]);
  return out;
}

std::string textOf(const std::vector<std::byte> &bytes) {
  std::string out(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i)
    out[i] = static_cast<char>(bytes[i]);
  return out;
}

struct CapturingSink final : CompletionSink {
  void progress(PrivateRequestToken, IoProgress) noexcept override {}
  void terminalCandidate(PrivateRequestToken, BackendResult result,
                         std::optional<PayloadLease>) noexcept override {
    candidate = std::move(result);
  }
  void releaseAcknowledged(BackendReleaseAck ack) noexcept override {
    released = ack;
  }
  std::optional<BackendResult> candidate;
  std::optional<BackendReleaseAck> released;
};

BackendContext makeContext(PayloadResolver &payloads) {
  return BackendContext{.stop = {}, .payloads = payloads, .budget = {1 << 20}};
}

constexpr AdapterKey streamKey{1, 1};

CellRequest requestFor(AdapterKey key, std::uint32_t specification) {
  return CellRequest{.key           = key,
                     .capability    = CapabilityCell{},
                     .specification = specification};
}

// -- source generation / conflict (design/vortex-io-redesign.md §8.2;
//    design/vortex-io-implementation-plan.md §6 M4 exit gate) ---------------

TEST(StreamSourceAdapterTest, ReadSucceedsAndMatchesStagedGeneration) {
  InMemoryStreamAdapter adapter;
  adapter.setContent(bytesOf("hello world"));
  OwnerContext owner;
  PayloadResolver payloads;

  const auto key =
      adapter.stage(StreamOpSpec{.kind               = StreamOpKind::Read,
                                 .offset             = 0,
                                 .length             = 5,
                                 .expectedGeneration = adapter.generation()});
  const auto prepared = adapter.prepare(requestFor(streamKey, key), owner);
  ASSERT_TRUE(prepared);

  CapturingSink sink;
  auto context = makeContext(payloads);
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  const auto *bytes = std::get_if<std::vector<std::byte>>(&complete->value);
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(textOf(*bytes), "hello");
  ASSERT_TRUE(sink.released);

  const auto published = adapter.publish(
      NativeCompletion{{1}, *sink.candidate, {}, *sink.released}, owner);
  ASSERT_TRUE(published);
  EXPECT_TRUE(std::holds_alternative<Complete>(published->outcome));
}

TEST(StreamSourceAdapterTest, PrepareRefusesStaleGenerationBeforeAdmission) {
  InMemoryStreamAdapter adapter;
  adapter.setContent(bytesOf("v1"));
  OwnerContext owner;
  const auto staleGeneration = adapter.generation();
  adapter.setContent(bytesOf("v2")); // a concurrent writer moves the source

  const auto key =
      adapter.stage(StreamOpSpec{.kind               = StreamOpKind::Read,
                                 .offset             = 0,
                                 .length             = 2,
                                 .expectedGeneration = staleGeneration});
  const auto prepared = adapter.prepare(requestFor(streamKey, key), owner);
  ASSERT_FALSE(prepared);
  EXPECT_EQ(prepared.error().code, IoErrorCode::Stale);
}

TEST(StreamSourceAdapterTest,
     ExecuteReportsConflictWhenSourceMovesAfterAdmission) {
  InMemoryStreamAdapter adapter;
  adapter.setContent(bytesOf("before"));
  OwnerContext owner;
  PayloadResolver payloads;

  const auto key =
      adapter.stage(StreamOpSpec{.kind               = StreamOpKind::Write,
                                 .offset             = 0,
                                 .writeBytes         = bytesOf("XX"),
                                 .expectedGeneration = adapter.generation()});
  // Admitted while current: prepare succeeds.
  const auto prepared = adapter.prepare(requestFor(streamKey, key), owner);
  ASSERT_TRUE(prepared);

  // A second, already-admitted write lands on the owner between this
  // request's admission and its worker turn.
  adapter.setContent(bytesOf("raced"));

  CapturingSink sink;
  auto context = makeContext(payloads);
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{2}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *conflict = std::get_if<Conflict>(&sink.candidate->outcome);
  ASSERT_NE(conflict, nullptr);
  // The source was not mutated by the stale write.
  EXPECT_EQ(textOf(adapter.content()), "raced");
}

TEST(StreamSourceAdapterTest, EndOfStreamIsDistinctFromCompleteZeroBytes) {
  InMemoryStreamAdapter adapter;
  adapter.setContent(bytesOf("ab"));
  OwnerContext owner;
  PayloadResolver payloads;

  const auto key = adapter.stage(
      StreamOpSpec{.kind = StreamOpKind::Read, .offset = 2, .length = 10});
  const auto prepared = adapter.prepare(requestFor(streamKey, key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink;
  auto context = makeContext(payloads);
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{3}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  EXPECT_TRUE(std::holds_alternative<EndOfStream>(sink.candidate->outcome));
}

// -- process adapter (real host I/O moved onto the worker path) -------------

TEST(ProcessIoAdapterTest, CapturesBoundedChildStdout) {
  ProcessIoAdapter adapter("printf hello");
  OwnerContext owner;
  PayloadResolver payloads;
  const auto key =
      adapter.stage(StreamOpSpec{.kind = StreamOpKind::Read, .length = 64});
  const auto prepared =
      adapter.prepare(requestFor(AdapterKey{2, 1}, key), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink;
  auto context = makeContext(payloads);
  static_cast<IoAdapter &>(adapter).execute(BackendCommand{{1}, *prepared},
                                            context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto *complete = std::get_if<Complete>(&sink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  const auto *bytes = std::get_if<std::vector<std::byte>>(&complete->value);
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(textOf(*bytes), "hello");
}

// -- net adapter (loopback socketpair; real syscalls, off the owner thread) --

TEST(NetIoAdapterTest, RoundTripsBytesOverAConnectedSocketPair) {
  int fds[2]{};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  NetIoAdapter writer(fds[0]);
  NetIoAdapter reader(fds[1]);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto writeKey = writer.stage(
      StreamOpSpec{.kind = StreamOpKind::Write, .writeBytes = bytesOf("ping")});
  const auto preparedWrite =
      writer.prepare(requestFor(AdapterKey{3, 1}, writeKey), owner);
  ASSERT_TRUE(preparedWrite);
  CapturingSink writeSink;
  static_cast<IoAdapter &>(writer).execute(BackendCommand{{1}, *preparedWrite},
                                           context, writeSink);
  ASSERT_TRUE(writeSink.candidate);
  EXPECT_TRUE(std::holds_alternative<Complete>(writeSink.candidate->outcome));

  const auto readKey =
      reader.stage(StreamOpSpec{.kind = StreamOpKind::Read, .length = 4});
  const auto preparedRead =
      reader.prepare(requestFor(AdapterKey{3, 1}, readKey), owner);
  ASSERT_TRUE(preparedRead);
  CapturingSink readSink;
  static_cast<IoAdapter &>(reader).execute(BackendCommand{{2}, *preparedRead},
                                           context, readSink);
  ASSERT_TRUE(readSink.candidate);
  const auto *complete = std::get_if<Complete>(&readSink.candidate->outcome);
  ASSERT_NE(complete, nullptr);
  const auto *bytes = std::get_if<std::vector<std::byte>>(&complete->value);
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(textOf(*bytes), "ping");

  ::close(fds[0]);
  ::close(fds[1]);
}

// -- Router/Union composition does not resubmit -----------------------------

struct CountingAdapter final : IoAdapter {
  auto prepare(const CellRequest &request, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> override {
    ++prepareCalls;
    return PreparedOperation{request.key, {}};
  }
  void execute(const BackendCommand &, BackendContext &,
               CompletionSink &sink) noexcept override {
    ++executeCalls;
    sink.terminalCandidate({1}, BackendResult{Complete{{}, {}, {}}, {}}, {});
  }
  auto publish(const NativeCompletion &completion, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> override {
    ++publishCalls;
    return OwnerPublication{completion.result.outcome};
  }
  std::size_t prepareCalls{};
  std::size_t executeCalls{};
  std::size_t publishCalls{};
};

TEST(RouterSourceAdapterTest, DelegatesEveryCallExactlyOnceWithoutResubmit) {
  CountingAdapter underlying;
  RouterSourceAdapter router(underlying);
  OwnerContext owner;
  PayloadResolver payloads;
  auto context = makeContext(payloads);

  const auto prepared = router.prepare(requestFor(streamKey, 0), owner);
  ASSERT_TRUE(prepared);
  CapturingSink sink;
  router.execute(BackendCommand{{1}, *prepared}, context, sink);
  ASSERT_TRUE(sink.candidate);
  const auto published =
      router.publish(NativeCompletion{{1}, *sink.candidate, {}, {{1}}}, owner);
  ASSERT_TRUE(published);

  // One caller-visible submission produces exactly one of each adapter call
  // on the underlying source -- a router/union wrapper layer never multiplies
  // backend work.
  EXPECT_EQ(underlying.prepareCalls, 1U);
  EXPECT_EQ(underlying.executeCalls, 1U);
  EXPECT_EQ(underlying.publishCalls, 1U);
}

class FakeCells final : public IoCellStore {
public:
  auto authorizeOpen(const OpenSpec &)
      -> std::expected<void, IoErrorInfo> override {
    return {};
  }
  auto freezeRequest(CapabilityCell capability, OwnerCell spec)
      -> std::expected<CellRequest, IoErrorInfo> override {
    return CellRequest{.key                  = streamKey,
                       .capability           = capability,
                       .completionBoundBytes = 1024,
                       .payloadBoundBytes    = 1024,
                       .specification        = spec};
  }
  auto freezeClose(EndpointCell endpointCell, CloseMode)
      -> std::expected<CellRequest, IoErrorInfo> override {
    return CellRequest{.key = streamKey, .endpoint = endpointCell};
  }
  auto mintRequest(const CellRequest &)
      -> std::expected<RequestCell, IoErrorInfo> override {
    return RequestCell{nextCell++, 1};
  }
  auto mintRefusal(IoErrorInfo) -> OutcomeCell override {
    return OutcomeCell{nextCell++, 1};
  }
  auto publishTerminal(RequestCell, const IoOutcome &outcome,
                       const IoProgress &, const std::optional<PayloadLease> &)
      -> std::expected<OutcomeCell, IoErrorInfo> override {
    outcomes.push_back(outcome);
    return OutcomeCell{nextCell++, 1};
  }
  auto mintOpenedEndpoint(const OpenSpec &)
      -> std::expected<OpenedEndpoint, IoErrorInfo> override {
    return OpenedEndpoint{EndpointCell{nextCell++, 1},
                          CapabilityCell{nextCell++, 1}};
  }
  auto mintWaiter(RequestCell, OwnerWaiterRegistration) -> WaiterCell override {
    return WaiterCell{nextCell++, 1};
  }
  void retireWaiter(WaiterCell) noexcept override {}

  std::uint32_t nextCell{100};
  std::vector<IoOutcome> outcomes;
};

TEST(RouterSourceAdapterTest,
     RegisteredThroughIoServiceSubmitsBackendWorkExactlyOnce) {
  PayloadResolver payloads;
  FakeCells cells;
  CountingAdapter underlying;
  RouterSourceAdapter router(underlying);
  auto scheduler = std::make_unique<BoundedIoScheduler>(
      BoundedIoSchedulerOptions{4, 4, 4096, 4096}, payloads);
  IoService service({8, 4096, 4096, 4096, 8}, std::move(scheduler), payloads,
                    cells);
  service.registerAdapter(streamKey, router);

  const auto submitted = service.submit(CapabilityCell{1, 1}, OwnerCell{7});
  ASSERT_TRUE(std::holds_alternative<Admitted>(submitted.value));
  const auto request = std::get<Admitted>(submitted.value).request;

  for (int i = 0; i < 200 && !service.poll(request).outcome.has_value(); ++i) {
    static_cast<void>(service.pump(8));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(service.poll(request).outcome.has_value());
  EXPECT_EQ(underlying.prepareCalls, 1U);
  EXPECT_EQ(underlying.executeCalls, 1U);
  EXPECT_EQ(underlying.publishCalls, 1U);
  static_cast<void>(service.requestShutdown(ShutdownMode::CancelPending));
}

} // namespace
