#include <gtest/gtest.h>

#include <algorithm>
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

#include "common/xanadu/segmented_primedia_spool.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/vortex/capture_coordinator.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"
#include "common/xanadu/vortex/capture_publication.hpp"
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
  auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override {
    if (span.scroll != xanadu::localScroll ||
        span.start + span.length > bytes_.size()) {
      return std::nullopt;
    }
    return std::vector<std::byte>(
        bytes_.begin() + static_cast<std::ptrdiff_t>(span.start),
        bytes_.begin() + static_cast<std::ptrdiff_t>(span.start + span.length));
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

std::unique_ptr<World> boot(CaptureByteSink &sink,
                            const std::filesystem::path &journalPath,
                            const std::size_t journalMaxBytes     = 4u << 20,
                            const std::size_t backendMaxBytes     = 1u << 20,
                            const bool recoverNow                 = true,
                            const CaptureCoordinatorLimits limits = {},
                            CapturePublisher *publisher           = nullptr) {
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
  // Before recover(): recovery completes half-finished publications.
  world->coordinator->setPublisher(publisher);
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

std::optional<zigzag::CellRef> requestOf(const zigzag::ArenaManifold &arena,
                                         const IoCellSchema &schema,
                                         const zigzag::CellRef owner,
                                         const IoDimension field) {
  const auto canonical = canonicalOfField(arena, schema, owner, field);
  if (!canonical) return std::nullopt;
  return *canonical;
}

/// A live arena cell standing in for an await's cursor.
OwnerWaiterRegistration waiterOn(World &world, const std::uint64_t label) {
  const auto cursor      = world.arena.makeCell("await cursor");
  const auto incarnation = world.arena.incarnationOf(cursor);
  EXPECT_TRUE(incarnation.has_value());
  return OwnerWaiterRegistration{static_cast<OwnerCell>(cursor),
                                 incarnation.value_or(0), label};
}

TEST_F(IoCaptureTest, AwaitingTheAppendRequestDoesNotEvictTheCaptureLink) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  auto origin = registerPinned(*world, transientRef(1), bytesOf("abcdefgh"));
  ASSERT_TRUE(origin.has_value());
  const PayloadRange selection[]{rangeOf(*origin, 0, 4)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  auto appended      = world->coordinator->appendRequest(capture);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  const auto request = std::get<Admitted>(appended.admission.value).request;

  const auto schema = makeIoCellSchema(world->arena).value();
  // Two waiters on one request, both before the outcome: each must keep
  // its own request link and the capture's must survive both.
  const auto first  = world->service->await(request, waiterOn(*world, 1));
  const auto second = world->service->await(request, waiterOn(*world, 2));
  ASSERT_TRUE(std::holds_alternative<Pending>(first.value));
  ASSERT_TRUE(std::holds_alternative<Pending>(second.value));
  const auto requestDim = schema.dim(IoDimension::Request);
  for (const auto &waiter : {std::get<Pending>(first.value).waiter,
                             std::get<Pending>(second.value).waiter}) {
    EXPECT_EQ(
        requestOf(world->arena, schema, waiter.cell, IoDimension::Request),
        std::optional{request.cell});
  }
  EXPECT_EQ(requestOf(world->arena, schema, capture.cell, IoDimension::Request),
            std::optional{request.cell});
  // The request is never a direct target on d.io.request.
  EXPECT_FALSE(
      world->arena.linked(request.cell, requestDim, zigzag::DimVector::NEG));

  // Retiring one waiter leaves the other's and the capture's chains whole.
  world->service->retireWaiter(std::get<Pending>(first.value).waiter);
  EXPECT_EQ(requestOf(world->arena, schema,
                      std::get<Pending>(second.value).waiter.cell,
                      IoDimension::Request),
            std::optional{request.cell});

  world->scheduler->runNext();
  static_cast<void>(world->service->pump(8));
  const auto poll = world->service->poll(request);
  ASSERT_TRUE(poll.outcome.has_value());
  EXPECT_TRUE(std::holds_alternative<Committed>(
      world->coordinator->commit(capture, *poll.outcome)));
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

// -- Structure publication (M6, plan §8.2)
// -------------------------------------

/// The production stack for publication tests: a real UserPermascroll in a
/// directory, the sink over it, and a Store (loaded when one was saved) with
/// its publisher. Destroyed and rebuilt over the same directory to model a
/// restart.
struct Durable {
  std::filesystem::path root;
  std::shared_ptr<xanadu::UserPermascroll> perma;
  std::unique_ptr<UserPermascrollCaptureSink> sink;
  std::unique_ptr<xanadu::Store> store;
  std::unique_ptr<CapturePublisher> publisher;
  [[nodiscard]] std::filesystem::path storeDir() const {
    return root / "store";
  }
};

std::unique_ptr<Durable> openDurable(const std::filesystem::path &root,
                                     std::filesystem::path storeDir = {}) {
  auto durable  = std::make_unique<Durable>();
  durable->root = root;
  xanadu::UserPermascroll::Config config;
  config.storageDir = root / "permascroll";
  durable->perma    = std::make_shared<xanadu::UserPermascroll>(config);
  durable->sink = std::make_unique<UserPermascrollCaptureSink>(*durable->perma);
  if (storeDir.empty()) storeDir = durable->storeDir();
  if (std::filesystem::exists(storeDir / "ops.nodes")) {
    durable->store = xanadu::loadStore(storeDir, durable->perma);
  } else {
    durable->store = std::make_unique<xanadu::Store>(durable->perma);
  }
  durable->publisher =
      std::make_unique<CapturePublisher>(*durable->store, storeDir);
  return durable;
}

std::string permascrollText(const Durable &durable,
                            const xanadu::PrimediaSpan &span) {
  return durable.perma->read(span);
}

std::vector<CaptureJournalRecordKind>
journalKinds(const std::filesystem::path &path) {
  CaptureJournal journal(path, 0);
  std::vector<CaptureJournalRecordKind> kinds;
  for (const auto &record : journal.recover().records) {
    kinds.push_back(record.kind);
  }
  return kinds;
}

std::size_t countKind(const std::filesystem::path &path,
                      const CaptureJournalRecordKind kind) {
  const auto kinds = journalKinds(path);
  return static_cast<std::size_t>(std::ranges::count(kinds, kind));
}

TEST_F(IoCaptureTest, CommitPublishesTranslatedSpansAndReceiptIntoTheStore) {
  auto durable = openDurable(dir_);
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("hello world");
  auto registered = registerPinned(*world, transientRef(70), sourceBytes);
  ASSERT_TRUE(registered.has_value());

  const PayloadRange first[]{rangeOf(*registered, 0, 5)};
  ASSERT_TRUE(captureCommit(*world, first).has_value());
  // [3,8) overlaps: only the uncovered suffix [5,8) is appended and published.
  const PayloadRange second[]{rangeOf(*registered, 3, 5)};
  ASSERT_TRUE(captureCommit(*world, second).has_value());
  // A fully covered selection appends nothing: a receipt with no pieces.
  const PayloadRange third[]{rangeOf(*registered, 0, 5)};
  ASSERT_TRUE(captureCommit(*world, third).has_value());

  const auto one = durable->publisher->published(1);
  ASSERT_TRUE(one.has_value());
  ASSERT_EQ(one->pieces.size(), 1U);
  EXPECT_EQ(permascrollText(*durable, one->pieces[0].span), "hello");
  EXPECT_EQ(one->pieces[0].origin, *registered);
  EXPECT_EQ(one->pieces[0].srcOffset, std::uint64_t{0});

  const auto two = durable->publisher->published(2);
  ASSERT_TRUE(two.has_value());
  ASSERT_EQ(two->pieces.size(), 1U);
  EXPECT_EQ(permascrollText(*durable, two->pieces[0].span), " wo");
  EXPECT_EQ(two->pieces[0].srcOffset, std::uint64_t{5});

  const auto three = durable->publisher->published(3);
  ASSERT_TRUE(three.has_value());
  EXPECT_TRUE(three->pieces.empty());

  // The receipt cell is a scalar whose canonical bits are the CaptureId,
  // readable without parsing text, and the three form one rank in order.
  auto manifold = durable->store->rebuildManifold(durable->store->latest());
  manifold.setStore(durable->store.get());
  EXPECT_EQ(manifold.asInt64(one->receipt), std::optional<std::int64_t>{1});
  EXPECT_EQ(durable->publisher->publishedIds().size(), 3U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 3U);
}

TEST_F(IoCaptureTest, PublicationRoundTripsThroughStoreSaveAndReload) {
  xanadu::PrimediaSpan span;
  const auto sourceBytes = bytesOf("round trip bytes");
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, transientRef(71), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    const PayloadRange selection[]{rangeOf(*registered, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
    span = durable->publisher->published(1).value().pieces.at(0).span;
    world.reset();
  }
  // Commit already saved: nothing else is written before the reload.
  auto durable        = openDurable(dir_);
  const auto reloaded = durable->publisher->published(1);
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_EQ(reloaded->pieces.size(), 1U);
  EXPECT_EQ(reloaded->pieces[0].span, span);
  EXPECT_EQ(permascrollText(*durable, reloaded->pieces[0].span), "round trip");
  EXPECT_EQ(reloaded->pieces[0].origin.source, std::uint64_t{71});
}

/// Throws on the Nth journal write of the coordinator it is installed in,
/// before or after the fsync: the process dying at that durable step.
CaptureJournalCrashPoints crashOnWrite(const int nth, const bool before) {
  auto counter = std::make_shared<int>(0);
  CaptureJournalCrashPoints points;
  auto hook = [counter, nth] {
    if (++*counter == nth) throw std::runtime_error("die");
  };
  (before ? points.beforeFlush : points.afterFlush) = hook;
  return points;
}

TEST_F(IoCaptureTest,
       CrashBetweenStoreSaveAndCommittedRecoversWithoutRepublish) {
  const auto sourceBytes = bytesOf("published but not committed");
  PayloadRef origin;
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, transientRef(72), sourceBytes);
    origin          = *registered;
    // Writes this coordinator makes: 1 Append, 2 Appended, 3 Committed.
    world->coordinator->setJournalCrashPoints(crashOnWrite(3, true));
    const PayloadRange selection[]{rangeOf(origin, 0, 9)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_THROW(
        static_cast<void>(world->coordinator->commit(capture, *outcome)),
        std::runtime_error);
  }
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);

  auto durable = openDurable(dir_);
  // The Store was saved before the crash: the receipt is already there.
  ASSERT_TRUE(durable->publisher->published(1).has_value());
  const auto before = durable->store->latest();
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  auto registered = registerPinned(*world, transientRef(72), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  // No second publication: the Store did not move, one receipt, and the
  // missing Committed record was written.
  EXPECT_EQ(durable->store->latest(), before);
  EXPECT_EQ(durable->publisher->publishedIds().size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  const auto translated =
      world->coordinator->translationFor(rangeOf(*registered, 0, 9));
  ASSERT_TRUE(translated.has_value());
  EXPECT_EQ(translated, durable->publisher->published(1)->pieces[0].span);
}

TEST_F(IoCaptureTest, CrashBetweenAppendedAndPublicationPublishesExactlyOnce) {
  const auto sourceBytes = bytesOf("appended but not published");
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, transientRef(73), sourceBytes);
    // Die right after the Appended record is durable, before publication.
    world->coordinator->setJournalCrashPoints(crashOnWrite(2, false));
    const PayloadRange selection[]{rangeOf(*registered, 0, 8)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_THROW(
        static_cast<void>(world->coordinator->commit(capture, *outcome)),
        std::runtime_error);
    EXPECT_FALSE(durable->publisher->published(1).has_value());
  }
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);

  {
    auto durable = openDurable(dir_);
    EXPECT_FALSE(durable->publisher->published(1).has_value());
    auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                      {}, durable->publisher.get());
    ASSERT_TRUE(world);
    const auto published = durable->publisher->published(1);
    ASSERT_TRUE(published.has_value());
    ASSERT_EQ(published->pieces.size(), 1U);
    EXPECT_EQ(permascrollText(*durable, published->pieces[0].span), "appended");
    EXPECT_EQ(durable->publisher->publishedIds().size(), 1U);
    EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  }
  // A third boot finds everything finished and mints nothing.
  auto durable      = openDurable(dir_);
  const auto before = durable->store->latest();
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  ASSERT_TRUE(world);
  EXPECT_EQ(durable->store->latest(), before);
  EXPECT_EQ(durable->publisher->publishedIds().size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
}

TEST_F(IoCaptureTest, CommitRetryAfterRefusedStoreSaveNeverPublishesTwice) {
  const auto blocker = dir_ / "blocker";
  {
    std::ofstream out(blocker);
    out << "a file where the store directory must go";
  }
  auto durable =
      openDurable(dir_, blocker / "store"); // create_directories must fail
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  const auto sourceBytes = bytesOf("retry me");
  auto registered = registerPinned(*world, transientRef(74), sourceBytes);
  const PayloadRange selection[]{rangeOf(*registered, 0, 8)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  const auto outcome = driveAppendToOutcome(*world, capture);
  ASSERT_TRUE(outcome.has_value());

  // Publication reaches the in-memory Store but the save is refused: the
  // commit refuses and no Committed record exists.
  auto refused = world->coordinator->commit(capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Refused>(refused));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);
  EXPECT_EQ(durable->publisher->publishedIds().size(), 1U);

  std::filesystem::remove(blocker);
  auto committed = world->coordinator->commit(capture, *outcome);
  ASSERT_TRUE(std::holds_alternative<Committed>(committed));
  EXPECT_EQ(durable->publisher->publishedIds().size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  EXPECT_TRUE(std::filesystem::exists(blocker / "store" / "ops.nodes"));
}

// -- real sinks ---------------------------------------------------------------

TEST_F(IoCaptureTest, UserPermascrollSinkSyncMakesAppendsDurableAcrossReopen) {
  xanadu::PrimediaSpan span;
  const auto active = dir_ / "permascroll" / "active.primedia";
  {
    auto durable = openDurable(dir_);
    EXPECT_EQ(durable->sink->highWaterBytes(), std::uint64_t{0});
    span = durable->sink->append(bytesOf("durable capture bytes"));
    EXPECT_EQ(span.length, std::uint64_t{21});
    EXPECT_EQ(durable->sink->highWaterBytes(), std::uint64_t{21});
    // append() writes into memory; only sync() reaches the file.
    const auto beforeSync = std::filesystem::exists(active)
                                ? std::filesystem::file_size(active)
                                : std::uintmax_t{0};
    EXPECT_LT(beforeSync, std::uintmax_t{21});
    ASSERT_TRUE(durable->sink->sync());
    EXPECT_EQ(std::filesystem::file_size(active), std::uintmax_t{21});
    const auto held = durable->sink->read(span);
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(textOf(*held), "durable capture bytes");
    // A span that runs past the target is no read at all.
    EXPECT_FALSE(durable->sink->read(xanadu::PrimediaSpan{
        .scroll = xanadu::localScroll, .start = 10, .length = 50}));
  }
  auto reopened = openDurable(dir_);
  EXPECT_EQ(reopened->sink->highWaterBytes(), std::uint64_t{21});
  const auto held = reopened->sink->read(span);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(textOf(*held), "durable capture bytes");
  // The next append continues after the reopened high-water mark.
  const auto next = reopened->sink->append(bytesOf("more"));
  EXPECT_EQ(next.start, std::uint64_t{21});
}

TEST_F(IoCaptureTest, SegmentedSinkSyncAndHighWaterSurviveReopen) {
  const auto file = dir_ / "segment.primedia";
  xanadu::PrimediaSpan span;
  {
    xanadu::SegmentedPrimediaSpool spool;
    ASSERT_TRUE(spool.openActiveSegment(file));
    SegmentedPrimediaCaptureSink sink(spool);
    EXPECT_EQ(sink.highWaterBytes(), std::uint64_t{0});
    span = sink.append(bytesOf("segmented bytes"));
    EXPECT_EQ(sink.highWaterBytes(), std::uint64_t{15});
    EXPECT_LT(std::filesystem::file_size(file), std::uintmax_t{15});
    ASSERT_TRUE(sink.sync());
    EXPECT_EQ(std::filesystem::file_size(file), std::uintmax_t{15});
  }
  xanadu::SegmentedPrimediaSpool reopened;
  ASSERT_TRUE(reopened.openActiveSegment(file));
  SegmentedPrimediaCaptureSink sink(reopened);
  EXPECT_EQ(sink.highWaterBytes(), std::uint64_t{15});
  const auto held = sink.read(span);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(textOf(*held), "segmented bytes");
}

TEST_F(IoCaptureTest, CaptureThroughRealPermascrollSinkEndToEnd) {
  auto durable = openDurable(dir_);
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  const auto sourceBytes = bytesOf("end to end through the real sink");
  auto registered = registerPinned(*world, transientRef(75), sourceBytes);
  const PayloadRange selection[]{rangeOf(*registered, 4, 6)};
  ASSERT_TRUE(captureCommit(*world, selection).has_value());
  const auto span = world->coordinator->translationFor(selection[0]);
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(permascrollText(*durable, *span), "to end");
  EXPECT_LE(span->start + span->length, durable->sink->highWaterBytes());
}

// -- reconciliation of pinned intervals ---------------------------------------

/// Run one capture whose append lands bytes and then fails: an uncertain
/// abort, leaving the capture pinned. Returns the world still open.
struct AbortedCapture {
  std::unique_ptr<World> world;
  PayloadRef origin;
};

AbortedCapture uncertainAbort(RecordingSink &sink,
                              const std::filesystem::path &journalPath,
                              const std::uint64_t source,
                              const std::string_view text,
                              const std::uint64_t length) {
  AbortedCapture out;
  out.world              = boot(sink, journalPath);
  const auto sourceBytes = bytesOf(text);
  auto registered =
      registerPinned(*out.world, transientRef(source), sourceBytes);
  out.origin = *registered;
  const PayloadRange selection[]{rangeOf(out.origin, 0, length)};
  auto prepared = out.world->coordinator->prepare(selection);
  EXPECT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  auto appended      = out.world->coordinator->appendRequest(capture);
  EXPECT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  sink.throwAfterAppend = true;
  out.world->scheduler->runNext();
  static_cast<void>(out.world->service->pump(8));
  const auto request = std::get<Admitted>(appended.admission.value).request;
  const auto outcome = out.world->service->poll(request).outcome;
  EXPECT_TRUE(outcome.has_value());
  EXPECT_TRUE(std::holds_alternative<Refused>(
      out.world->coordinator->commit(capture, *outcome)));
  return out;
}

TEST_F(IoCaptureTest, ReconcilePromoteVerifiesBytesThenCommitsTheTranslation) {
  RecordingSink sink;
  auto aborted =
      uncertainAbort(sink, journalPath_, 80, "promote these bytes", 7);
  auto &coordinator = *aborted.world->coordinator;
  ASSERT_EQ(coordinator.pinnedCaptures(), std::vector<CaptureId>{1});
  const PayloadRange range[]{rangeOf(aborted.origin, 0, 7)};
  EXPECT_TRUE(std::holds_alternative<Refused>(coordinator.prepare(range)));

  // A span holding other bytes, of another length, or past the high-water
  // mark is refused and the pin stays.
  sink.bytes_.insert(sink.bytes_.end(), 7, std::byte{'x'});
  const auto wrongBytes =
      coordinator.reconcile(1, ReconcilePromote{localSpan(7, 7)});
  ASSERT_FALSE(wrongBytes.has_value());
  EXPECT_EQ(wrongBytes.error().code, IoErrorCode::Conflict);
  EXPECT_FALSE(
      coordinator.reconcile(1, ReconcilePromote{localSpan(0, 6)}).has_value());
  EXPECT_FALSE(
      coordinator.reconcile(1, ReconcilePromote{localSpan(20, 7)}).has_value());
  // An open intent names no span, so the owner must supply one.
  EXPECT_FALSE(coordinator.reconcile(1, ReconcilePromote{}).has_value());
  EXPECT_EQ(coordinator.pinnedCaptures().size(), 1U);
  EXPECT_FALSE(coordinator.translationFor(range[0]).has_value());

  const auto promoted =
      coordinator.reconcile(1, ReconcilePromote{localSpan(0, 7)});
  ASSERT_TRUE(promoted.has_value());
  EXPECT_EQ(promoted->resolution, CaptureReconcileResult::Resolution::Promoted);
  EXPECT_FALSE(promoted->publicationPending);
  EXPECT_TRUE(coordinator.pinnedCaptures().empty());
  EXPECT_EQ(coordinator.translationFor(range[0]), localSpan(0, 7));
  // The range now reuses the committed translation: nothing is appended.
  const auto size = sink.bytes_.size();
  EXPECT_TRUE(captureCommit(*aborted.world, range).has_value());
  EXPECT_EQ(sink.bytes_.size(), size);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Reconciled), 1U);
  // Not pinned any more, so a second decision has nothing to act on.
  EXPECT_FALSE(coordinator.reconcile(1, ReconcileRelease{}).has_value());
}

TEST_F(IoCaptureTest, ReconcileReleaseUnpinsAndAllowsRecapture) {
  RecordingSink sink;
  auto aborted = uncertainAbort(sink, journalPath_, 81, "release these", 7);
  auto &coordinator = *aborted.world->coordinator;
  const PayloadRange range[]{rangeOf(aborted.origin, 0, 7)};
  EXPECT_TRUE(std::holds_alternative<Refused>(coordinator.prepare(range)));

  const auto released = coordinator.reconcile(1, ReconcileRelease{});
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->resolution, CaptureReconcileResult::Resolution::Released);
  EXPECT_TRUE(coordinator.pinnedCaptures().empty());
  // The orphaned bytes stay where they are, never aliased: a recapture
  // appends the range again after them.
  const auto orphanEnd = sink.bytes_.size();
  ASSERT_TRUE(captureCommit(*aborted.world, range).has_value());
  const auto span = coordinator.translationFor(range[0]);
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(span->start, orphanEnd);
}

TEST_F(IoCaptureTest, ReconcileAfterRestartResolvesRecoveredPins) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto aborted =
        uncertainAbort(sink, journalPath_, 82, "pinned over restart", 8);
    origin = aborted.origin;
  }
  {
    auto world             = boot(sink, journalPath_);
    const auto sourceBytes = bytesOf("pinned over restart");
    auto registered = registerPinned(*world, transientRef(82), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    EXPECT_EQ(world->coordinator->pinnedCaptures(), std::vector<CaptureId>{1});
    const auto promoted =
        world->coordinator->reconcile(1, ReconcilePromote{localSpan(0, 8)});
    ASSERT_TRUE(promoted.has_value());
  }
  // The decision replays: committed, nothing pinned, nothing reported.
  auto world        = boot(sink, journalPath_);
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{0});
  EXPECT_TRUE(world->coordinator->pinnedCaptures().empty());
  EXPECT_EQ(world->coordinator->translationFor(rangeOf(origin, 0, 8)),
            localSpan(0, 8));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
}

TEST_F(IoCaptureTest,
       RestartBetweenReconciledAndCommittedFinishesThePromotion) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto aborted =
        uncertainAbort(sink, journalPath_, 83, "die mid reconcile", 9);
    origin = aborted.origin;
  }
  {
    auto world             = boot(sink, journalPath_);
    const auto sourceBytes = bytesOf("die mid reconcile");
    ASSERT_TRUE(registerPinned(*world, transientRef(83), sourceBytes));
    // Writes from here: 1 Reconciled, 2 Committed -- die before the second.
    world->coordinator->setJournalCrashPoints(crashOnWrite(2, true));
    EXPECT_THROW(static_cast<void>(world->coordinator->reconcile(
                     1, ReconcilePromote{localSpan(0, 9)})),
                 std::runtime_error);
  }
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Reconciled), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);
  auto world        = boot(sink, journalPath_);
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_TRUE(world->coordinator->pinnedCaptures().empty());
  EXPECT_EQ(world->coordinator->translationFor(rangeOf(origin, 0, 9)),
            localSpan(0, 9));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
}

TEST_F(IoCaptureTest, ReconcileReleaseSurvivesRestart) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto aborted =
        uncertainAbort(sink, journalPath_, 84, "release and restart", 7);
    origin = aborted.origin;
    ASSERT_TRUE(aborted.world->coordinator->reconcile(1, ReconcileRelease{})
                    .has_value());
  }
  auto world             = boot(sink, journalPath_);
  const auto sourceBytes = bytesOf("release and restart");
  auto registered = registerPinned(*world, transientRef(84), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  EXPECT_TRUE(world->coordinator->pinnedCaptures().empty());
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->orphanBytes, std::uint64_t{0});
  const PayloadRange range[]{rangeOf(*registered, 0, 7)};
  EXPECT_TRUE(
      std::holds_alternative<Prepared>(world->coordinator->prepare(range)));
  (void)origin;
}

TEST_F(IoCaptureTest,
       ReconcileSpanBeyondPermascrollUsesTheJournalsSpanOnceBytesReturn) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world             = boot(sink, journalPath_);
    const auto sourceBytes = bytesOf("bytes later lost");
    auto registered = registerPinned(*world, transientRef(85), sourceBytes);
    origin          = *registered;
    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  const auto saved = sink.bytes_;
  sink.bytes_.resize(4);
  auto world             = boot(sink, journalPath_);
  const auto sourceBytes = bytesOf("bytes later lost");
  ASSERT_TRUE(registerPinned(*world, transientRef(85), sourceBytes));
  auto &coordinator = *world->coordinator;
  ASSERT_EQ(coordinator.pinnedCaptures(), std::vector<CaptureId>{1});
  // The journal names the span, but the permascroll no longer holds it.
  const auto missing = coordinator.reconcile(1, ReconcilePromote{});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, IoErrorCode::Conflict);
  EXPECT_EQ(coordinator.pinnedCaptures().size(), 1U);
  // The bytes come back (a restored permascroll): the journal's span verifies.
  sink.bytes_         = saved;
  const auto promoted = coordinator.reconcile(1, ReconcilePromote{});
  ASSERT_TRUE(promoted.has_value());
  EXPECT_EQ(coordinator.translationFor(rangeOf(origin, 0, 10)),
            localSpan(0, 10));
}

TEST_F(IoCaptureTest, ReconcileReleaseOfALostSpanFreesTheRangeForRecapture) {
  RecordingSink sink;
  {
    auto world             = boot(sink, journalPath_);
    const auto sourceBytes = bytesOf("bytes truly lost");
    auto registered = registerPinned(*world, transientRef(86), sourceBytes);
    const PayloadRange selection[]{rangeOf(*registered, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  sink.bytes_.clear();
  auto world             = boot(sink, journalPath_);
  const auto sourceBytes = bytesOf("bytes truly lost");
  auto registered = registerPinned(*world, transientRef(86), sourceBytes);
  ASSERT_TRUE(world->coordinator->reconcile(1, ReconcileRelease{}).has_value());
  const PayloadRange range[]{rangeOf(*registered, 0, 10)};
  ASSERT_TRUE(captureCommit(*world, range).has_value());
  EXPECT_EQ(world->coordinator->translationFor(range[0]), localSpan(0, 10));
}

TEST_F(IoCaptureTest,
       ReconcilePromotePublishesIntoTheStoreWhenAPublisherIsAttached) {
  /// A real permascroll that lands the bytes and then throws: the uncertain
  /// abort that pins a capture.
  class FailAfterSink final : public CaptureByteSink {
  public:
    explicit FailAfterSink(CaptureByteSink &inner) : inner_(inner) {}
    auto append(const std::span<const std::byte> bytes)
        -> xanadu::PrimediaSpan override {
      const auto span = inner_.append(bytes);
      if (failOnce) {
        failOnce = false;
        throw std::runtime_error("sink boom after write");
      }
      return span;
    }
    auto sync() -> bool override { return inner_.sync(); }
    auto highWaterBytes() const -> std::uint64_t override {
      return inner_.highWaterBytes();
    }
    auto read(const xanadu::PrimediaSpan &span) const
        -> std::optional<std::vector<std::byte>> override {
      return inner_.read(span);
    }
    bool failOnce{};

  private:
    CaptureByteSink &inner_;
  };

  const auto sourceBytes = bytesOf("promote into the store");
  xanadu::PrimediaSpan landed;
  {
    auto durable = openDurable(dir_);
    FailAfterSink sink(*durable->sink);
    auto world      = boot(sink, journalPath_, 4u << 20, 1u << 20, true, {},
                           durable->publisher.get());
    auto registered = registerPinned(*world, transientRef(87), sourceBytes);
    const PayloadRange selection[]{rangeOf(*registered, 0, 7)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    auto appended      = world->coordinator->appendRequest(capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
    sink.failOnce = true;
    world->scheduler->runNext();
    static_cast<void>(world->service->pump(8));
    const auto request = std::get<Admitted>(appended.admission.value).request;
    const auto outcome = world->service->poll(request).outcome;
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(std::holds_alternative<Refused>(
        world->coordinator->commit(capture, *outcome)));
    EXPECT_FALSE(durable->publisher->published(1).has_value());
    // The bytes sit at the old high-water mark: the owner names that span.
    landed = localSpan(0, 7);
    const auto promoted =
        world->coordinator->reconcile(1, ReconcilePromote{landed});
    ASSERT_TRUE(promoted.has_value());
    EXPECT_FALSE(promoted->publicationPending);
    const auto published = durable->publisher->published(1);
    ASSERT_TRUE(published.has_value());
    ASSERT_EQ(published->pieces.size(), 1U);
    EXPECT_EQ(published->pieces[0].span, landed);
    EXPECT_EQ(permascrollText(*durable, landed), "promote");
  }
  // The promotion and its publication are durable across a restart.
  auto durable = openDurable(dir_);
  ASSERT_TRUE(durable->publisher->published(1).has_value());
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  EXPECT_TRUE(world->coordinator->pinnedCaptures().empty());
  EXPECT_EQ(durable->publisher->publishedIds().size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
}

TEST_F(IoCaptureTest, ReconciledRecordRoundTripsAndVersionOneIsRefused) {
  {
    CaptureJournal journal(journalPath_, 0);
    CaptureJournalRecord record;
    record.reconciledPromoted = true;
    record.span               = localSpan(3, 9);
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Reconciled, 7, record)
            .has_value());
    const auto view = journal.recover();
    ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
    ASSERT_EQ(view.records.size(), 1U);
    EXPECT_EQ(view.records[0].kind, CaptureJournalRecordKind::Reconciled);
    EXPECT_TRUE(view.records[0].reconciledPromoted);
    EXPECT_EQ(view.records[0].span, localSpan(3, 9));
  }
  // A version 1 file (no Reconciled record existed) is refused by number.
  {
    std::fstream file(journalPath_,
                      std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(sizeof(kCaptureJournalSignature));
    const std::uint32_t one = 1;
    file.write(reinterpret_cast<const char *>(&one), sizeof(one));
  }
  CaptureJournal reopened(journalPath_, 0);
  EXPECT_EQ(reopened.recover().status, CaptureJournalStatus::Corrupt);
}

} // namespace
