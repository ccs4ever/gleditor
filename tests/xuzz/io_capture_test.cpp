#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/capture_coordinator.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"
#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"
#include "common/xanadu/vortex/io_service.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"

namespace {
using namespace xanadu::vortex;

std::vector<std::byte> bytesOf(const std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(text[i]);
  }
  return out;
}

std::string textOf(const std::span<const std::byte> bytes) {
  std::string out(bytes.size(), '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i] = static_cast<char>(bytes[i]);
  }
  return out;
}

PayloadRef transientRef(const std::uint64_t source) {
  return PayloadRef{.kind       = PayloadSourceKind::Transient,
                    .source     = source,
                    .generation = 1,
                    .occurrence = 1};
}

PayloadRange rangeOf(const PayloadRef &origin, const std::uint64_t offset,
                     const std::uint64_t length) {
  return PayloadRange{.origin = origin, .offset = offset, .length = length};
}

/// The "permascroll" the capture appends into: a flat byte vector that
/// survives coordinator restarts, handing out sequential PrimediaSpans.
/// throwAfterAppend models a sink that landed bytes and then failed --
/// the uncertain-abort case recovery must pin, never alias.
class RecordingSink final : public CaptureByteSink {
public:
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    if (throwBeforeAppend) {
      throwBeforeAppend = false;
      throw std::runtime_error("sink boom before write");
    }
    const auto start = static_cast<std::uint64_t>(bytes_.size());
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    if (throwAfterAppend) {
      throwAfterAppend = false;
      throw std::runtime_error("sink boom after write");
    }
    return xanadu::PrimediaSpan{
        .scroll = xanadu::localScroll, .start = start, .length = bytes.size()};
  }
  auto sync() -> bool override {
    ++syncCalls;
    if (failSync) return false;
    durable_ = bytes_.size();
    return true;
  }
  auto highWaterBytes() const -> std::uint64_t override {
    return bytes_.size();
  }
  /// Simulated crash: bytes appended but never synced do not survive.
  void loseUnsynced() { bytes_.resize(durable_); }

  std::vector<std::byte> bytes_;
  std::size_t durable_{};
  int syncCalls{};
  bool failSync{};
  bool throwBeforeAppend{};
  bool throwAfterAppend{};
};

/// Deterministic IoScheduler: enqueue only records the command; runNext()
/// performs the worker turn synchronously so tests choose exactly where a
/// "crash" (world teardown) lands relative to the durable steps.
class DirectScheduler final : public IoScheduler {
public:
  explicit DirectScheduler(PayloadResolver &payloads,
                           const std::size_t backendMaxBytes = 1u << 20)
      : payloads_(payloads), backendMaxBytes_(backendMaxBytes) {}

  auto reserve(const AdmissionCost cost)
      -> std::expected<SchedulerReservation, IoErrorInfo> override {
    return SchedulerReservation{{nextToken_++},
                                cost.commandBytes + cost.completionBytes};
  }
  auto enqueue(SchedulerReservation reservation, BackendCommand command,
               IoAdapter &adapter, CompletionSink &sink) noexcept
      -> EnqueueResult override {
    entries_.push_back({reservation, std::move(command), &adapter, &sink});
    return EnqueueResult::Queued;
  }
  void releaseUnused(SchedulerReservation) noexcept override {}
  auto cancel(const PrivateRequestToken token) noexcept
      -> CancelQueuedResult override {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->command.token == token) {
        // Same contract as the bounded scheduler's pre-start cancel: a
        // Cancelled candidate plus release ack, no worker turn.
        it->sink->terminalCandidate(
            token, BackendResult{Cancelled{{}, {}, false}, {}}, {});
        it->sink->releaseAcknowledged({PrivateReleaseToken{token}});
        entries_.erase(it);
        return CancelQueuedResult::RemovedBeforeStart;
      }
    }
    return CancelQueuedResult::AlreadyRunning;
  }
  auto pump(std::size_t) -> std::size_t override { return 0; }
  auto requestStop(ShutdownMode) -> xanadu::vortex::ShutdownResult override {
    return {0, 0, 0, true};
  }

  /// The worker turn: run the oldest queued command to its terminal
  /// candidate synchronously. The service publishes on pump().
  void runNext() {
    EXPECT_FALSE(entries_.empty());
    if (entries_.empty()) return;
    Entry entry = std::move(entries_.front());
    entries_.pop_front();
    BackendContext context{.stop     = {},
                           .payloads = payloads_,
                           .budget   = {.maxBytes = backendMaxBytes_}};
    entry.adapter->execute(entry.command, context, *entry.sink);
  }
  [[nodiscard]] auto queued() const -> std::size_t { return entries_.size(); }

private:
  struct Entry {
    SchedulerReservation reservation;
    BackendCommand command;
    IoAdapter *adapter;
    CompletionSink *sink;
  };
  PayloadResolver &payloads_;
  std::size_t backendMaxBytes_;
  std::uint64_t nextToken_{1};
  std::deque<Entry> entries_;
};

/// One boot of the capture stack: fresh arena, payloads, service and
/// coordinator over the caller-owned sink and journal path, so a test can
/// destroy the world and "restart" onto the same durable state.
struct World {
  zigzag::ArenaManifold arena;
  PayloadResolver payloads;
  DirectScheduler *scheduler{};
  std::unique_ptr<ArenaIoCellStore> cells;
  std::unique_ptr<IoService> service;
  std::unique_ptr<CaptureCoordinator> coordinator;
  /// Whole-source leases standing in for the reader that keeps a
  /// transient source alive across captures: the resolver recycles a
  /// transient chunk once its last lease goes (plan §8.1).
  std::vector<PayloadLease> pins;
};

auto registerPinned(World &world, const PayloadRef ref,
                    const std::span<const std::byte> bytes)
    -> std::expected<PayloadRef, IoError> {
  auto origin = world.payloads.registerSource(ref, bytes);
  if (!origin) return origin;
  auto pin = world.payloads.retain(rangeOf(*origin, 0, bytes.size()));
  if (!pin) return std::unexpected(pin.error());
  world.pins.push_back(*pin);
  return origin;
}

std::unique_ptr<World> boot(RecordingSink &sink,
                            const std::filesystem::path &journalPath,
                            const std::size_t journalMaxBytes     = 4u << 20,
                            const std::size_t backendMaxBytes     = 1u << 20,
                            const bool recoverNow                 = true,
                            const CaptureCoordinatorLimits limits = {}) {
  auto world  = std::make_unique<World>();
  auto schema = makeIoCellSchema(world->arena);
  EXPECT_TRUE(schema.has_value());
  if (!schema) return nullptr;
  world->cells = std::make_unique<ArenaIoCellStore>(
      world->arena, *schema, world->payloads,
      [world = world.get()](const CapabilityCell capability,
                            const OwnerCell specification)
          -> std::expected<CellRequest, IoErrorInfo> {
        if (world->coordinator) {
          if (auto request = world->coordinator->decodeAppendRequest(
                  capability, specification)) {
            return *request;
          }
        }
        return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
      });
  auto scheduler =
      std::make_unique<DirectScheduler>(world->payloads, backendMaxBytes);
  world->scheduler = scheduler.get();
  world->service   = std::make_unique<IoService>(
      IoBudgetSnapshot{64, 1u << 20, 1u << 20, 1u << 20, 16},
      std::move(scheduler), world->payloads, *world->cells);
  world->coordinator = std::make_unique<CaptureCoordinator>(
      world->arena, *schema, world->payloads, *world->service, sink,
      journalPath, journalMaxBytes, limits);
  world->service->registerAdapter(world->coordinator->appendKey(),
                                  world->coordinator->appendAdapter());
  // recover() is idempotent, so tests that inspect the restart report call
  // it again and see the same one; a corrupt journal leaves the coordinator
  // unrecovered and that test asserts the refusal itself.
  if (recoverNow) static_cast<void>(world->coordinator->recover());
  return world;
}

/// prepare + appendRequest + worker turn + owner publish; returns the
/// append request's terminal outcome cell, or nullopt on any refusal.
std::optional<OutcomeCell> driveAppendToOutcome(World &world,
                                                const CaptureCell capture) {
  auto appended = world.coordinator->appendRequest(capture);
  if (!std::holds_alternative<Admitted>(appended.admission.value)) {
    return std::nullopt;
  }
  const auto request = std::get<Admitted>(appended.admission.value).request;
  world.scheduler->runNext();
  static_cast<void>(world.service->pump(8));
  const auto poll = world.service->poll(request);
  if (!poll.outcome) return std::nullopt;
  return *poll.outcome;
}

/// The full happy-path transaction; returns the receipt cell.
std::optional<CaptureReceiptCell>
captureCommit(World &world, const std::span<const PayloadRange> selection) {
  auto prepared = world.coordinator->prepare(selection);
  if (!std::holds_alternative<Prepared>(prepared)) {
    return std::nullopt;
  }
  const auto capture = std::get<Prepared>(prepared).capture;
  const auto outcome = driveAppendToOutcome(world, capture);
  if (!outcome) return std::nullopt;
  auto committed = world.coordinator->commit(capture, *outcome);
  if (!std::holds_alternative<Committed>(committed)) {
    return std::nullopt;
  }
  return std::get<Committed>(committed).receipt;
}

CaptureSourceInterval intervalOf(const PayloadRef &origin,
                                 const std::uint64_t srcOffset,
                                 const std::uint64_t srcLength,
                                 const std::uint64_t dstOffset) {
  return CaptureSourceInterval{.sourceKind = origin.kind,
                               .source     = origin.source,
                               .generation = origin.generation,
                               .occurrence = origin.occurrence,
                               .srcOffset  = srcOffset,
                               .srcLength  = srcLength,
                               .dstOffset  = dstOffset};
}

/// Hand-write one capture's Append (+ optional Appended) record the way a
/// coordinator would have, for recovery tests that need an exact journal.
void writeAttempt(CaptureJournal &journal, const CaptureId id,
                  const std::vector<CaptureSourceInterval> &intervals,
                  const std::uint64_t appendLength,
                  const std::optional<xanadu::PrimediaSpan> span) {
  CaptureJournalRecord record;
  record.appendLength = appendLength;
  record.intervals    = intervals;
  ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Append, id, record)
                  .has_value());
  if (span) {
    record.span = *span;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Appended, id, record)
            .has_value());
  }
}

xanadu::PrimediaSpan localSpan(const std::uint64_t start,
                               const std::uint64_t length) {
  return xanadu::PrimediaSpan{
      .scroll = xanadu::localScroll, .start = start, .length = length};
}

class IoCaptureTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<std::uint64_t> next{};
    dir_ = std::filesystem::temp_directory_path() /
           ("xanadu-io-capture-" + std::to_string(next.fetch_add(1)) + "-" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
    journalPath_ = dir_ / "capture.journal";
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::filesystem::path journalPath_;
};

// -- the two-resource transaction --------------------------------------------

TEST_F(IoCaptureTest, PrepareAppendCommitPublishesReceiptAndSpan) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("the quick brown fox");
  auto origin            = registerPinned(*world, transientRef(1), sourceBytes);
  ASSERT_TRUE(origin.has_value());

  const PayloadRange selection[]{rangeOf(*origin, 4, 5)}; // "quick"
  auto receipt = captureCommit(*world, selection);
  ASSERT_TRUE(receipt.has_value());
  EXPECT_EQ(ioCellTag(world->arena, receipt->cell, receipt->incarnation),
            IoCellTag::CaptureReceipt);

  // The append landed once, at the start of the permascroll.
  ASSERT_EQ(sink.bytes_.size(), 5U);
  EXPECT_EQ(textOf(sink.bytes_), "quick");

  // Journal order: Append intent, Appended (span+intent), Committed.
  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  ASSERT_EQ(view.records.size(), 3U);
  EXPECT_EQ(view.records[0].kind, CaptureJournalRecordKind::Append);
  EXPECT_EQ(view.records[1].kind, CaptureJournalRecordKind::Appended);
  EXPECT_EQ(view.records[2].kind, CaptureJournalRecordKind::Committed);
  EXPECT_EQ(view.records[1].span.start, std::uint64_t{0});
  EXPECT_EQ(view.records[1].span.length, std::uint64_t{5});
  ASSERT_EQ(view.records[1].intervals.size(), 1U);
  EXPECT_EQ(view.records[1].intervals[0].srcOffset, std::uint64_t{4});
  EXPECT_EQ(view.records[1].intervals[0].srcLength, std::uint64_t{5});
  EXPECT_EQ(view.records[1].intervals[0].dstOffset, std::uint64_t{0});
}

TEST_F(IoCaptureTest, OverlappingLaterSelectionReusesCommittedSpan) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("abcdefghijklmnop"); // 16 bytes
  auto origin            = registerPinned(*world, transientRef(2), sourceBytes);
  ASSERT_TRUE(origin.has_value());

  // First capture: [0,10) -> appends all ten bytes.
  const PayloadRange first[]{rangeOf(*origin, 0, 10)};
  auto firstReceipt = captureCommit(*world, first);
  ASSERT_TRUE(firstReceipt.has_value());
  ASSERT_EQ(sink.bytes_.size(), 10U);

  // Second capture: [5,15) overlaps [5,10) -> only [10,15) is appended.
  const PayloadRange second[]{rangeOf(*origin, 5, 10)};
  auto secondReceipt = captureCommit(*world, second);
  ASSERT_TRUE(secondReceipt.has_value());
  ASSERT_EQ(sink.bytes_.size(), 15U);
  // The five new bytes are exactly source[10..15) = "klmno".
  EXPECT_EQ(textOf({sink.bytes_.data() + 10, 5}), "klmno");

  // The translation query follows the overlap: [5,10) maps to the address
  // the first capture appended it at, [10,15) to the second capture's.
  const auto reused =
      world->coordinator->translationFor(rangeOf(*origin, 5, 5));
  ASSERT_TRUE(reused.has_value());
  EXPECT_EQ(reused->start, std::uint64_t{5});
  EXPECT_EQ(reused->length, std::uint64_t{5});
  const auto fresh =
      world->coordinator->translationFor(rangeOf(*origin, 10, 5));
  ASSERT_TRUE(fresh.has_value());
  EXPECT_EQ(fresh->start, std::uint64_t{10});
  EXPECT_FALSE(world->coordinator->translationFor(rangeOf(*origin, 0, 16)));

  // A third capture of the fully-covered [0,15) appends nothing at all:
  // prepare still mints a cell but the append payload is empty.
  const PayloadRange third[]{rangeOf(*origin, 0, 15)};
  auto prepared = world->coordinator->prepare(third);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  auto appended      = world->coordinator->appendRequest(capture);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  world->scheduler->runNext();
  static_cast<void>(world->service->pump(8));
  const auto request = std::get<Admitted>(appended.admission.value).request;
  const auto outcome = world->service->poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());
  auto committed = world->coordinator->commit(capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Committed>(committed));
  // No new bytes: the whole selection was already translated.
  EXPECT_EQ(sink.bytes_.size(), 15U);
}

TEST_F(IoCaptureTest, DiscontiguousSelectionSplitsAndLaterOverlapReuses) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("0123456789abcdef");
  auto origin            = registerPinned(*world, transientRef(3), sourceBytes);
  ASSERT_TRUE(origin.has_value());

  // One capture over two disjoint ranges: [0,5) and [10,15).
  const PayloadRange selection[]{rangeOf(*origin, 0, 5),
                                 rangeOf(*origin, 10, 5)};
  auto receipt = captureCommit(*world, selection);
  ASSERT_TRUE(receipt.has_value());
  ASSERT_EQ(sink.bytes_.size(), 10U);
  EXPECT_EQ(textOf(sink.bytes_), "01234abcde");

  // The journal's Appended record carries two intervals whose dstOffsets
  // tile the single append payload.
  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  const auto &appended = view.records[1];
  ASSERT_EQ(appended.kind, CaptureJournalRecordKind::Appended);
  ASSERT_EQ(appended.intervals.size(), 2U);
  EXPECT_EQ(appended.intervals[0].srcOffset, std::uint64_t{0});
  EXPECT_EQ(appended.intervals[0].dstOffset, std::uint64_t{0});
  EXPECT_EQ(appended.intervals[1].srcOffset, std::uint64_t{10});
  EXPECT_EQ(appended.intervals[1].dstOffset, std::uint64_t{5});

  // [3,12) now overlaps both committed pieces: only the uncovered
  // [5,10) gap is appended -- five bytes, not nine.
  const PayloadRange overlap[]{rangeOf(*origin, 3, 9)};
  auto second = captureCommit(*world, overlap);
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(sink.bytes_.size(), 15U);
  EXPECT_EQ(textOf({sink.bytes_.data() + 10, 5}), "56789");
}

// -- commit validation --------------------------------------------------------

TEST_F(IoCaptureTest, CommitRejectsAnotherCapturesOutcomeAndNothingPublishes) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("abcdefghijkl");
  auto originA = registerPinned(*world, transientRef(10), sourceBytes);
  auto originB = registerPinned(*world, transientRef(11), sourceBytes);
  ASSERT_TRUE(originA && originB);

  const PayloadRange selA[]{rangeOf(*originA, 0, 4)};
  const PayloadRange selB[]{rangeOf(*originB, 0, 4)};
  auto preparedA = world->coordinator->prepare(selA);
  auto preparedB = world->coordinator->prepare(selB);
  ASSERT_TRUE(std::holds_alternative<Prepared>(preparedA));
  ASSERT_TRUE(std::holds_alternative<Prepared>(preparedB));
  const auto captureA = std::get<Prepared>(preparedA).capture;
  const auto captureB = std::get<Prepared>(preparedB).capture;

  // Both appends admitted and executed; only A's outcome is published so
  // far is irrelevant -- commit validates against the request's own
  // published outcome.
  auto appendA = world->coordinator->appendRequest(captureA);
  auto appendB = world->coordinator->appendRequest(captureB);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appendA.admission.value));
  ASSERT_TRUE(std::holds_alternative<Admitted>(appendB.admission.value));
  world->scheduler->runNext();
  world->scheduler->runNext();
  static_cast<void>(world->service->pump(8));
  const auto outcomeA =
      world->service->poll(std::get<Admitted>(appendA.admission.value).request)
          .outcome;
  const auto outcomeB =
      world->service->poll(std::get<Admitted>(appendB.admission.value).request)
          .outcome;
  ASSERT_TRUE(outcomeA && outcomeB);

  // B's outcome is not A's append outcome: typed refusal, nothing minted.
  auto wrong = world->coordinator->commit(captureA, *outcomeB);
  EXPECT_TRUE(std::holds_alternative<Refused>(wrong));

  // A commit with no published outcome yet is equally refused: retire A's
  // request is not needed -- poll simply has no outcome for a fresh request.
  // (Covered by the mismatch above; the second-commit check is next.)
  auto right = world->coordinator->commit(captureA, *outcomeA);
  EXPECT_TRUE(std::holds_alternative<Committed>(right));

  // A second commit on the same capture is refused: the phase moved on.
  auto again = world->coordinator->commit(captureA, *outcomeA);
  EXPECT_TRUE(std::holds_alternative<Refused>(again));

  // B still commits cleanly with its own outcome.
  auto committedB = world->coordinator->commit(captureB, *outcomeB);
  EXPECT_TRUE(std::holds_alternative<Committed>(committedB));
}

TEST_F(IoCaptureTest, CommitBeforeOutcomeIsPublishedRefuses) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("xyz");
  auto origin = registerPinned(*world, transientRef(12), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const PayloadRange selection[]{rangeOf(*origin, 0, 3)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;

  auto appended = world->coordinator->appendRequest(capture);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  // The worker turn ran (bytes landed) but the owner never pumped: no
  // outcome cell exists yet, so commit cannot validate one. The service's
  // refuse() mints a well-formed Rejected outcome cell that is definitely
  // not this request's terminal outcome.
  world->scheduler->runNext();
  const auto request = std::get<Admitted>(appended.admission.value).request;
  const auto refusal =
      world->service->refuse(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  auto committed = world->coordinator->commit(capture, refusal);
  EXPECT_TRUE(std::holds_alternative<Refused>(committed));
  // Nothing was published: the capture can still complete once the real
  // outcome lands.
  static_cast<void>(world->service->pump(8));
  const auto outcome = world->service->poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());
  auto done = world->coordinator->commit(capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Committed>(done));
}

// -- crash injection
// ------------------------------------------------------------

TEST_F(IoCaptureTest, RestartAfterAppendIntentPinsAndReportsOrphanBytes) {
  RecordingSink sink;
  PayloadRef origin;
  CaptureCell capture{};
  std::uint64_t appendLength = 0;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("persistent source bytes");
    auto registered = registerPinned(*world, transientRef(20), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;

    const PayloadRange selection[]{rangeOf(origin, 0, 12)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    capture = std::get<Prepared>(prepared).capture;

    // The append intent is durable and the request is admitted, but the
    // "crash" lands before the worker turn: no bytes can exist.
    auto appended = world->coordinator->appendRequest(capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
    ASSERT_EQ(world->scheduler->queued(), 1U);
    appendLength = 12;
    // world destroyed here with the append still queued.
  }
  EXPECT_TRUE(sink.bytes_.empty());

  // Restart: fresh arena/service/coordinator over the same journal and
  // permascroll. The same transient origin is re-registered so identity
  // matches the journal's interval records.
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("persistent source bytes");
  auto registered = registerPinned(*world, transientRef(20), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  ASSERT_EQ(*registered, origin);

  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->orphanBytes, appendLength);

  // The intent's intervals are pinned: a new overlapping capture refuses
  // rather than guessing whether bytes landed.
  const PayloadRange overlap[]{rangeOf(origin, 0, 12)};
  auto refused = world->coordinator->prepare(overlap);
  EXPECT_TRUE(std::holds_alternative<Refused>(refused));

  // A disjoint range still captures normally.
  const PayloadRange disjoint[]{rangeOf(origin, 12, 10)};
  auto receipt = captureCommit(*world, disjoint);
  EXPECT_TRUE(receipt.has_value());
}

TEST_F(IoCaptureTest, RestartAfterAppendExecutedLeavesBytesUnaliased) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("bytes that may be orphaned");
    auto registered = registerPinned(*world, transientRef(21), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;

    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    auto appended      = world->coordinator->appendRequest(capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
    // The worker turn ran: the append bytes landed in the permascroll,
    // but no Appended/Committed record followed before the crash.
    world->scheduler->runNext();
    ASSERT_EQ(sink.bytes_.size(), 10U);
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("bytes that may be orphaned");
  auto registered = registerPinned(*world, transientRef(21), sourceBytes);
  ASSERT_TRUE(registered.has_value());

  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{10});
  // The bytes are still in the permascroll -- reported as potential
  // orphans, never guessed into a translation.
  EXPECT_EQ(sink.bytes_.size(), 10U);

  // The pinned interval refuses overlapping captures until explicit
  // reconciliation; the orphaned bytes are not reachable through any
  // committed translation.
  const PayloadRange overlap[]{rangeOf(origin, 0, 10)};
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(overlap)));
}

TEST_F(IoCaptureTest, RestartAfterAppendedRecordRebuildsTranslations) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("appended then crashed");
    auto registered = registerPinned(*world, transientRef(22), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;

    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    auto append        = world->coordinator->appendRequest(capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(append.admission.value));
    // The worker turn ran: the append bytes landed in the permascroll.
    // The "crash" lands before commit(), so the journal holds only the
    // Append intent -- replay the Appended record a commit would have
    // written, using the span the sink actually allocated, to model the
    // crash boundary exactly: Appended durable, Committed absent.
    world->scheduler->runNext();
    ASSERT_EQ(sink.bytes_.size(), 10U);
    CaptureJournalRecord appendedRecord;
    appendedRecord.appendLength = 10;
    appendedRecord.intervals.push_back(
        CaptureSourceInterval{.sourceKind = origin.kind,
                              .source     = origin.source,
                              .generation = origin.generation,
                              .occurrence = origin.occurrence,
                              .srcOffset  = 0,
                              .srcLength  = 10,
                              .dstOffset  = 0});
    appendedRecord.span = xanadu::PrimediaSpan{
        .scroll = xanadu::localScroll, .start = 0, .length = 10};
    // captureId 1 is this world's first capture; its Append record is
    // already durable, so this Appended record completes the pair.
    auto stamped = world->coordinator->journal().appendRecord(
        CaptureJournalRecordKind::Appended, 1, appendedRecord);
    ASSERT_TRUE(stamped.has_value());
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("appended then crashed");
  auto registered = registerPinned(*world, transientRef(22), sourceBytes);
  ASSERT_TRUE(registered.has_value());

  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  // Appended proves the append landed and its span is durable: the
  // translation is rebuilt and the still-open attempt is closed out.
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{0});

  // The rebuilt translation covers [0,10): capturing [5,15) appends only
  // the uncovered tail src[10,15).
  const PayloadRange tail[]{rangeOf(*registered, 5, 10)};
  auto receipt = captureCommit(*world, tail);
  ASSERT_TRUE(receipt.has_value());
  ASSERT_EQ(sink.bytes_.size(), 15U);
  EXPECT_EQ(textOf({sink.bytes_.data() + 10, 5}), "hen c" /* src[10..15) */);
}

TEST_F(IoCaptureTest, RestartAfterCommittedRecordReconcilesAndReuses) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("committed before restart");
    auto registered = registerPinned(*world, transientRef(23), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;
    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    auto receipt = captureCommit(*world, selection);
    ASSERT_TRUE(receipt.has_value());
    ASSERT_EQ(sink.bytes_.size(), 10U);
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("committed before restart");
  auto registered = registerPinned(*world, transientRef(23), sourceBytes);
  ASSERT_TRUE(registered.has_value());

  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{0});

  // The rebuilt map still serves overlap reuse: [5,15) appends only
  // [10,15).
  const PayloadRange overlap[]{rangeOf(*registered, 5, 10)};
  auto receipt = captureCommit(*world, overlap);
  ASSERT_TRUE(receipt.has_value());
  ASSERT_EQ(sink.bytes_.size(), 15U);
  EXPECT_EQ(textOf({sink.bytes_.data() + 10, 5}), "befor" /* src[10..15) */);
}

TEST_F(IoCaptureTest, CertainAbortReconcilesWithoutPinsOrOrphans) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("cancelled append");
    auto registered = registerPinned(*world, transientRef(24), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;

    const PayloadRange selection[]{rangeOf(origin, 0, 8)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    auto appended      = world->coordinator->appendRequest(capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
    const auto request = std::get<Admitted>(appended.admission.value).request;
    // Cancel before the worker turn: certain that no bytes exist.
    EXPECT_EQ(world->service->cancelRequest(request).state,
              RequestCancelState::Accepted);
    static_cast<void>(world->service->pump(8));
    const auto outcome = world->service->poll(request).outcome;
    ASSERT_TRUE(outcome.has_value());
    // commit() sees a non-Complete terminal outcome and records a certain
    // abort (wasExecuted is false: execute never ran).
    auto committed = world->coordinator->commit(capture, *outcome);
    EXPECT_TRUE(std::holds_alternative<Refused>(committed));
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("cancelled append");
  auto registered = registerPinned(*world, transientRef(24), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{0});
  EXPECT_TRUE(sink.bytes_.empty());
  // No pins: the same range captures cleanly after restart.
  const PayloadRange again[]{rangeOf(*registered, 0, 8)};
  auto receipt = captureCommit(*world, again);
  EXPECT_TRUE(receipt.has_value());
}

TEST_F(IoCaptureTest, UncertainAbortPinsIntervalsAndReportsPossibleOrphans) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("sink fails mid-append");
    auto registered = registerPinned(*world, transientRef(25), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;

    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    auto appended      = world->coordinator->appendRequest(capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
    // The sink writes the bytes and THEN throws: the outcome is Failed but
    // bytes provably landed -- the uncertain-abort shape.
    sink.throwAfterAppend = true;
    world->scheduler->runNext();
    static_cast<void>(world->service->pump(8));
    const auto request = std::get<Admitted>(appended.admission.value).request;
    const auto outcome = world->service->poll(request).outcome;
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(ioOutcomeTag(world->arena, *outcome), IoOutcomeTag::Failed);
    auto committed = world->coordinator->commit(capture, *outcome);
    EXPECT_TRUE(std::holds_alternative<Refused>(committed));
    ASSERT_EQ(sink.bytes_.size(), 10U);
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("sink fails mid-append");
  auto registered = registerPinned(*world, transientRef(25), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{10});
  // Bytes landed but no committed translation names them.
  const PayloadRange overlap[]{rangeOf(*registered, 0, 10)};
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(overlap)));
}

// -- identity -----------------------------------------------------------------

TEST_F(IoCaptureTest, IdenticalBytesNeverShareCaptureIdentity) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("same bytes, different captures");
  auto originA = registerPinned(*world, transientRef(30), sourceBytes);
  auto originB = registerPinned(*world, transientRef(31), sourceBytes);
  ASSERT_TRUE(originA && originB);

  const PayloadRange selA[]{rangeOf(*originA, 0, 8)};
  const PayloadRange selB[]{rangeOf(*originB, 0, 8)};
  auto receiptA = captureCommit(*world, selA);
  auto receiptB = captureCommit(*world, selB);
  ASSERT_TRUE(receiptA && receiptB);

  // Hashes validate integrity only: equal bytes digest equal yet the two
  // captures landed at distinct permanent spans -- identity is the span,
  // never the digest.
  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  // Records: A{Append,Appended,Committed} then B{Append,Appended,Committed}.
  ASSERT_EQ(view.records.size(), 6U);
  EXPECT_EQ(view.records[1].appendDigest, view.records[4].appendDigest);
  EXPECT_NE(view.records[1].span.start, view.records[4].span.start);
  EXPECT_EQ(sink.bytes_.size(), 16U);
}

// -- journal durability
// ---------------------------------------------------------

TEST_F(IoCaptureTest, TornTailIsRepairedAndCorruptionIsTypedRefusal) {
  // A crash mid-record leaves a torn tail: the constructor cuts it and
  // recovery sees only the proven prefix. Two records go down; the file
  // is then truncated inside the second.
  std::uint64_t firstRecordEnd = 0;
  {
    CaptureJournal journal(journalPath_, 0);
    CaptureJournalRecord payload;
    payload.appendLength = 3;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 7, payload)
            .has_value());
    firstRecordEnd = journal.sizeBytes();
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 8, payload)
            .has_value());
    // Truncate inside the second record: header + record 1 + a partial
    // record 2 is exactly what a crash between write and fsync leaves.
    std::filesystem::resize_file(journalPath_, firstRecordEnd + 5);
  }
  {
    CaptureJournal journal(journalPath_, 0);
    const auto view = journal.recover();
    EXPECT_EQ(view.status, CaptureJournalStatus::Clean);
    ASSERT_EQ(view.records.size(), 1U);
    EXPECT_EQ(view.records[0].kind, CaptureJournalRecordKind::Append);
    EXPECT_EQ(view.records[0].captureId, std::uint64_t{7});
  }

  // A bad signature is a typed refusal, surfaced through the coordinator
  // as PersistenceRefusal.
  const auto badPath = dir_ / "bad.journal";
  {
    std::ofstream file(badPath, std::ios::binary);
    file << "NOTAJOURNAL";
    file.flush();
  }
  RecordingSink sink;
  auto world = boot(sink, badPath);
  ASSERT_TRUE(world);
  auto report = world->coordinator->recover();
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().code, IoErrorCode::PersistenceRefusal);
}

TEST_F(IoCaptureTest, JournalCrashPointsRollBackBeforeFlushAndKeepAfterFlush) {
  CaptureJournalRecord payload;
  payload.appendLength = 1;
  {
    CaptureJournal journal(journalPath_, 0);
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, payload)
            .has_value());
    // Death before fsync: the record never counts durable.
    EXPECT_THROW(static_cast<void>(journal.appendRecord(
                     CaptureJournalRecordKind::Appended, 1, payload,
                     {.beforeFlush = [] { throw std::runtime_error("die"); }})),
                 std::runtime_error);
  }
  {
    CaptureJournal journal(journalPath_, 0);
    const auto view = journal.recover();
    ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
    ASSERT_EQ(view.records.size(), 1U);
    // Death after fsync: the record is durable and the sequence advanced.
    EXPECT_THROW(static_cast<void>(journal.appendRecord(
                     CaptureJournalRecordKind::Committed, 1, {},
                     {.afterFlush = [] { throw std::runtime_error("die"); }})),
                 std::runtime_error);
  }
  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  ASSERT_EQ(view.records.size(), 2U);
  EXPECT_EQ(view.records[1].kind, CaptureJournalRecordKind::Committed);
  EXPECT_EQ(view.nextSequence, std::uint64_t{3});
}

TEST_F(IoCaptureTest, JournalChecksumFailureIsTypedRefusalNamingTheRecord) {
  {
    CaptureJournal journal(journalPath_, 0);
    CaptureJournalRecord payload;
    for (CaptureId id = 1; id <= 2; ++id) {
      ASSERT_TRUE(
          journal.appendRecord(CaptureJournalRecordKind::Append, id, payload)
              .has_value());
    }
  }
  {
    std::fstream file(journalPath_,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-1, std::ios::end);
    const char flipped = '\x5a';
    file.write(&flipped, 1);
  }
  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  EXPECT_EQ(view.status, CaptureJournalStatus::Corrupt);
  ASSERT_TRUE(view.refusal.has_value());
  EXPECT_EQ(view.refusal->code, IoErrorCode::PersistenceRefusal);
  EXPECT_EQ(view.refusal->stableDetail, 2U);
}

// -- review fixes (M6 review)
// --------------------------------------------------

TEST_F(IoCaptureTest, CommitSyncsPermascrollBeforeAppendedRecordIsWritten) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("durable before promised");
  auto origin = registerPinned(*world, transientRef(40), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const PayloadRange selection[]{rangeOf(*origin, 0, 9)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  const auto outcome = driveAppendToOutcome(*world, capture);
  ASSERT_TRUE(outcome.has_value());

  // The sink cannot make the bytes durable: commit refuses and no Appended
  // record exists to promise them.
  sink.failSync = true;
  EXPECT_TRUE(std::holds_alternative<Refused>(
      world->coordinator->commit(capture, *outcome)));
  {
    CaptureJournal journal(journalPath_, 0);
    const auto view = journal.recover();
    ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
    ASSERT_EQ(view.records.size(), 1U);
    EXPECT_EQ(view.records[0].kind, CaptureJournalRecordKind::Append);
  }
  // The bytes were never durable: a crash now loses them.
  sink.loseUnsynced();
  EXPECT_TRUE(sink.bytes_.empty());
}

TEST_F(IoCaptureTest, CommittedBytesSurviveSimulatedCrash) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("survives the crash");
    auto registered = registerPinned(*world, transientRef(41), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;
    const PayloadRange selection[]{rangeOf(origin, 0, 8)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
    EXPECT_GE(sink.syncCalls, 1);
  }
  sink.loseUnsynced();
  ASSERT_EQ(sink.bytes_.size(), 8U);
  auto world  = boot(sink, journalPath_);
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  const auto span = world->coordinator->translationFor(rangeOf(origin, 2, 4));
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->start, std::uint64_t{2});
}

TEST_F(IoCaptureTest, RecoveryDistrustsSpanBeyondPermascrollHighWater) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("bytes later lost");
    auto registered = registerPinned(*world, transientRef(42), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;
    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  // The permascroll lost its tail after the journal said it was there.
  sink.bytes_.resize(4);
  auto world  = boot(sink, journalPath_);
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->entries[0].reason,
            CaptureRecoveryReason::SpanBeyondPermascroll);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{10});
  // Never a valid translation; overlap is pinned.
  EXPECT_FALSE(world->coordinator->translationFor(rangeOf(origin, 0, 10)));
  const auto sourceBytes = bytesOf("bytes later lost");
  auto registered = registerPinned(*world, transientRef(42), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  const PayloadRange overlap[]{rangeOf(*registered, 0, 10)};
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(overlap)));
}

TEST_F(IoCaptureTest, PrepareRefusesUntilRecoveredAndIdsSeedFromJournal) {
  {
    CaptureJournal journal(journalPath_, 0);
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 41, record)
            .has_value());
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Aborted, 41, record)
            .has_value());
  }
  RecordingSink sink;
  auto world = boot(sink, journalPath_, 4u << 20, 1u << 20, false);
  ASSERT_TRUE(world);
  EXPECT_FALSE(world->coordinator->recovered());
  const auto sourceBytes = bytesOf("recover first");
  auto origin = registerPinned(*world, transientRef(43), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const PayloadRange selection[]{rangeOf(*origin, 0, 5)};
  auto early = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Refused>(early));

  ASSERT_TRUE(world->coordinator->recover().has_value());
  EXPECT_TRUE(world->coordinator->recovered());
  ASSERT_TRUE(captureCommit(*world, selection).has_value());

  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  // The new capture's id is past the journal's maximum, never reusing 41.
  EXPECT_EQ(view.records.back().captureId, std::uint64_t{42});
}

TEST_F(IoCaptureTest,
       PrepareHoldsJournalBudgetSoPendingCapturesCannotExhaustIt) {
  const auto worst =
      captureJournalFramedBytes(CaptureJournalRecordKind::Append, 1) +
      captureJournalFramedBytes(CaptureJournalRecordKind::Appended, 1) +
      captureJournalFramedBytes(CaptureJournalRecordKind::Committed) +
      captureJournalFramedBytes(CaptureJournalRecordKind::Aborted);
  // Room for exactly one capture's full record set, not two.
  const auto budget = kCaptureJournalHeaderBytes + worst + 8;
  RecordingSink sink;
  auto world = boot(sink, journalPath_, budget);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("held budget");
  auto originA = registerPinned(*world, transientRef(44), sourceBytes);
  auto originB = registerPinned(*world, transientRef(45), sourceBytes);
  ASSERT_TRUE(originA && originB);

  const PayloadRange selA[]{rangeOf(*originA, 0, 4)};
  const PayloadRange selB[]{rangeOf(*originB, 0, 4)};
  auto first  = world->coordinator->prepare(selA);
  auto second = world->coordinator->prepare(selB);
  ASSERT_TRUE(std::holds_alternative<Prepared>(first));
  // Refused before any append, not after bytes landed.
  ASSERT_TRUE(std::holds_alternative<Refused>(second));
  EXPECT_TRUE(sink.bytes_.empty());

  // The first capture still has every record it needs.
  const auto capture = std::get<Prepared>(first).capture;
  const auto outcome = driveAppendToOutcome(*world, capture);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_TRUE(std::holds_alternative<Committed>(
      world->coordinator->commit(capture, *outcome)));
  EXPECT_EQ(world->coordinator->journal().heldBytes(), 0U);
}

TEST_F(IoCaptureTest, TornHeaderRemnantIsAFreshJournalButForeignBytesPoison) {
  {
    // A crash while the very first write was landing: a strict prefix of
    // our own header, nothing after it.
    std::ofstream file(journalPath_, std::ios::binary);
    file.write(kCaptureJournalSignature, 5);
  }
  {
    CaptureJournal journal(journalPath_, 0);
    EXPECT_EQ(journal.sizeBytes(), 0U);
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
            .has_value());
  }
  {
    CaptureJournal journal(journalPath_, 0);
    const auto view = journal.recover();
    EXPECT_EQ(view.status, CaptureJournalStatus::Clean);
    EXPECT_EQ(view.records.size(), 1U);
  }
  const auto foreign = dir_ / "foreign.journal";
  {
    std::ofstream file(foreign, std::ios::binary);
    file << "XYZ";
  }
  CaptureJournal journal(foreign, 0);
  CaptureJournalRecord record;
  EXPECT_FALSE(journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
                   .has_value());
  EXPECT_EQ(journal.recover().status, CaptureJournalStatus::Corrupt);
}

TEST_F(IoCaptureTest, CorruptLengthMidFileRefusesInsteadOfTruncating) {
  std::uintmax_t fullSize       = 0;
  std::size_t secondRecordStart = 0;
  {
    CaptureJournal journal(journalPath_, 0);
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
            .has_value());
    secondRecordStart = journal.sizeBytes();
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 2, record)
            .has_value());
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Committed, 2, record)
            .has_value());
    fullSize = std::filesystem::file_size(journalPath_);
  }
  {
    // Record 2's length now claims far more bytes than remain, but its
    // body and the record after it are intact: valid records follow.
    std::fstream file(journalPath_,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(static_cast<std::streamoff>(secondRecordStart));
    const std::uint32_t huge = 1u << 20;
    file.write(reinterpret_cast<const char *>(&huge), sizeof(huge));
  }
  CaptureJournal journal(journalPath_, 0);
  EXPECT_EQ(std::filesystem::file_size(journalPath_), fullSize);
  const auto view = journal.recover();
  EXPECT_EQ(view.status, CaptureJournalStatus::Corrupt);
  ASSERT_TRUE(view.refusal.has_value());
  EXPECT_EQ(view.refusal->stableDetail, 2U);
  CaptureJournalRecord record;
  EXPECT_FALSE(journal.appendRecord(CaptureJournalRecordKind::Append, 3, record)
                   .has_value());
}

TEST_F(IoCaptureTest, GenuineTornTailOfAnIntervalRecordIsStillCut) {
  PayloadRef origin    = transientRef(50);
  std::size_t firstEnd = 0;
  {
    CaptureJournal journal(journalPath_, 0);
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
            .has_value());
    firstEnd            = journal.sizeBytes();
    record.appendLength = 6;
    record.intervals    = {intervalOf(origin, 0, 3, 0),
                           intervalOf(origin, 9, 3, 3)};
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 2, record)
            .has_value());
  }
  // Cut well inside the second record's interval array.
  std::filesystem::resize_file(journalPath_, firstEnd + 4 + 90);
  CaptureJournal journal(journalPath_, 0);
  EXPECT_EQ(journal.sizeBytes(), firstEnd);
  EXPECT_EQ(std::filesystem::file_size(journalPath_), firstEnd);
  EXPECT_EQ(journal.recover().records.size(), 1U);
}

TEST_F(IoCaptureTest, ZeroLengthSelectionRefusedAndTouchingIntervalsCoalesce) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("0123456789abcdefghijklmnop");
  auto origin = registerPinned(*world, transientRef(46), sourceBytes);
  ASSERT_TRUE(origin.has_value());

  const PayloadRange empty[]{rangeOf(*origin, 4, 0)};
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(empty)));
  const PayloadRange mixed[]{rangeOf(*origin, 0, 5), rangeOf(*origin, 9, 0)};
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(mixed)));
  EXPECT_EQ(world->coordinator->journal().sizeBytes(), 0U);

  // [0,10) and [10,20) touch: two pieces, no zero-length slot between.
  const PayloadRange touching[]{rangeOf(*origin, 0, 10),
                                rangeOf(*origin, 10, 10)};
  auto receipt = captureCommit(*world, touching);
  ASSERT_TRUE(receipt.has_value());
  EXPECT_EQ(sink.bytes_.size(), 20U);
  const auto span = world->coordinator->translationFor(rangeOf(*origin, 5, 10));
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->start, std::uint64_t{5});
  EXPECT_EQ(span->length, std::uint64_t{10});
}

TEST_F(IoCaptureTest, RecoverIsIdempotentAndSafeAfterLiveCommits) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world             = boot(sink, journalPath_);
    const auto sourceBytes = bytesOf("0123456789abcdefghijklmnop");
    auto registered = registerPinned(*world, transientRef(47), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;
    const PayloadRange first[]{rangeOf(origin, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, first).has_value());
  }
  auto world = boot(sink, journalPath_);
  auto first = world->coordinator->recover();
  ASSERT_TRUE(first.has_value());
  const PayloadRange second[]{rangeOf(origin, 10, 10)};
  const auto sourceBytes = bytesOf("0123456789abcdefghijklmnop");
  auto registered = registerPinned(*world, transientRef(47), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  ASSERT_TRUE(captureCommit(*world, second).has_value()); // coalesces [0,20)

  auto again = world->coordinator->recover();
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->entries.size(), first->entries.size());
  // The map is exactly as the live commits left it: one coalesced slot.
  const auto span = world->coordinator->translationFor(rangeOf(origin, 0, 20));
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->length, std::uint64_t{20});
  const PayloadRange all[]{rangeOf(origin, 0, 20)};
  const auto before = sink.bytes_.size();
  ASSERT_TRUE(captureCommit(*world, all).has_value());
  EXPECT_EQ(sink.bytes_.size(), before); // fully covered: nothing appended
}

TEST_F(IoCaptureTest, RecoveryDetectsPartialOverlapAndInconsistentSpans) {
  const auto origin = transientRef(48);
  RecordingSink sink;
  sink.bytes_.resize(64);
  {
    CaptureJournal journal(journalPath_, 0);
    // Capture 1: [0,10) at span [0,10). Capture 2 overlaps it partially.
    writeAttempt(journal, 1, {intervalOf(origin, 0, 10, 0)}, 10,
                 localSpan(0, 10));
    writeAttempt(journal, 2, {intervalOf(origin, 5, 10, 0)}, 10,
                 localSpan(10, 10));
    // Capture 3 claims 10 bytes from a 5-byte span.
    writeAttempt(journal, 3, {intervalOf(origin, 40, 10, 0)}, 5,
                 localSpan(30, 5));
  }
  auto world  = boot(sink, journalPath_, 4u << 20, 1u << 20, false);
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 3U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_EQ(report->entries[1].reason, CaptureRecoveryReason::MapConflict);
  EXPECT_EQ(report->entries[2].reason,
            CaptureRecoveryReason::InvalidTranslation);
  // Capture 1's slot is untouched by the rejected neighbours.
  const auto span = world->coordinator->translationFor(rangeOf(origin, 2, 6));
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->start, std::uint64_t{2});
}

TEST_F(IoCaptureTest, AbortedAfterAppendedIsAmbiguousAndPinned) {
  const auto origin = transientRef(49);
  RecordingSink sink;
  sink.bytes_.resize(16);
  {
    CaptureJournal journal(journalPath_, 0);
    writeAttempt(journal, 1, {intervalOf(origin, 0, 8, 0)}, 8, localSpan(0, 8));
    CaptureJournalRecord aborted;
    aborted.abortUncertain = false;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Aborted, 1, aborted)
            .has_value());
  }
  auto world  = boot(sink, journalPath_);
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->entries[0].reason, CaptureRecoveryReason::AmbiguousJournal);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{8});
  EXPECT_FALSE(world->coordinator->translationFor(rangeOf(origin, 0, 8)));
}

TEST_F(IoCaptureTest, CertainAbortReleasesSlotsAndLeasesSoTheRangeCanRetry) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("cancel then retry");
  auto origin = registerPinned(*world, transientRef(51), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const auto leasesBefore = world->payloads.liveLeases();

  const PayloadRange selection[]{rangeOf(*origin, 0, 8)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  auto appended      = world->coordinator->appendRequest(capture);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  const auto request = std::get<Admitted>(appended.admission.value).request;
  EXPECT_EQ(world->service->cancelRequest(request).state,
            RequestCancelState::Accepted);
  static_cast<void>(world->service->pump(8));
  const auto outcome = world->service->poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());
  EXPECT_TRUE(std::holds_alternative<Refused>(
      world->coordinator->commit(capture, *outcome)));

  // Certain abort: the leases are back, the reservation is gone, and the
  // same range captures now without waiting for the dead cell to be swept.
  EXPECT_EQ(world->payloads.liveLeases(), leasesBefore);
  EXPECT_EQ(world->coordinator->journal().heldBytes(), 0U);
  auto retried = captureCommit(*world, selection);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(sink.bytes_.size(), 8U);

  CaptureJournal journal(journalPath_, 0);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  ASSERT_GE(view.records.size(), 2U);
  EXPECT_EQ(view.records[1].kind, CaptureJournalRecordKind::Aborted);
  EXPECT_FALSE(view.records[1].abortUncertain);
}

TEST_F(IoCaptureTest, UncertainAbortKeepsPinsInProcessButReleasesLeases) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("sink fails after writing");
  auto origin = registerPinned(*world, transientRef(52), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const auto leasesBefore = world->payloads.liveLeases();

  const PayloadRange selection[]{rangeOf(*origin, 0, 10)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  auto appended      = world->coordinator->appendRequest(capture);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  sink.throwAfterAppend = true;
  world->scheduler->runNext();
  static_cast<void>(world->service->pump(8));
  const auto outcome =
      world->service->poll(std::get<Admitted>(appended.admission.value).request)
          .outcome;
  ASSERT_TRUE(outcome.has_value());
  EXPECT_TRUE(std::holds_alternative<Refused>(
      world->coordinator->commit(capture, *outcome)));

  EXPECT_EQ(world->payloads.liveLeases(), leasesBefore);
  // Bytes may exist: the interval stays pinned in this process too.
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(selection)));
  const PayloadRange disjoint[]{rangeOf(*origin, 12, 6)};
  EXPECT_TRUE(captureCommit(*world, disjoint).has_value());
}

TEST_F(IoCaptureTest, CommittedCaptureFreesItsPendingCaptureSlot) {
  RecordingSink sink;
  CaptureCoordinatorLimits limits;
  limits.maxPendingCaptures = 1;
  auto world = boot(sink, journalPath_, 4u << 20, 1u << 20, true, limits);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("one slot at a time");
  auto origin = registerPinned(*world, transientRef(53), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const PayloadRange a[]{rangeOf(*origin, 0, 4)};
  const PayloadRange b[]{rangeOf(*origin, 6, 4)};
  ASSERT_TRUE(captureCommit(*world, a).has_value());
  // The first capture's cell is still alive; it must not hold the slot.
  EXPECT_TRUE(captureCommit(*world, b).has_value());
}

TEST_F(IoCaptureTest, CoordinatorJournalCrashPointsBracketTheAppendedFsync) {
  RecordingSink sink;
  PayloadRef origin;
  const auto crashOnSecondWrite = [](const bool before) {
    auto counter = std::make_shared<int>(0);
    CaptureJournalCrashPoints points;
    auto hook = [counter] {
      if (++*counter == 2) throw std::runtime_error("die");
    };
    (before ? points.beforeFlush : points.afterFlush) = hook;
    return points;
  };
  const auto runUntilCrash = [&](const std::uint64_t source,
                                 const bool before) {
    auto world             = boot(sink, journalPath_);
    const auto sourceBytes = bytesOf("crash around fsync");
    auto registered = registerPinned(*world, transientRef(source), sourceBytes);
    origin          = *registered;
    world->coordinator->setJournalCrashPoints(crashOnSecondWrite(before));
    const PayloadRange selection[]{rangeOf(origin, 0, 8)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_THROW(
        static_cast<void>(world->coordinator->commit(capture, *outcome)),
        std::runtime_error);
  };

  // Death BEFORE the Appended fsync: the record is rolled back, the bytes
  // landed -- an open intent, reported as potential orphans.
  runUntilCrash(54, true);
  {
    auto world  = boot(sink, journalPath_);
    auto report = world->coordinator->recover();
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->entries.size(), 1U);
    EXPECT_EQ(report->entries[0].reason, CaptureRecoveryReason::OpenIntent);
    EXPECT_EQ(report->orphanBytes, std::uint64_t{8});
    EXPECT_FALSE(world->coordinator->translationFor(rangeOf(origin, 0, 8)));
  }

  // Death AFTER the Appended fsync: the record is durable, recovery
  // reconciles the translation and closes the attempt.
  std::filesystem::remove(journalPath_);
  sink.bytes_.clear();
  sink.durable_ = 0;
  runUntilCrash(55, false);
  {
    auto world  = boot(sink, journalPath_);
    auto report = world->coordinator->recover();
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->entries.size(), 1U);
    EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
    const auto span = world->coordinator->translationFor(rangeOf(origin, 0, 8));
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(span->start, std::uint64_t{0});
  }
}

} // namespace
