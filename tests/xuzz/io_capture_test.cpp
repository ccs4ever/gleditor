#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

[[maybe_unused]] PayloadRef transientRef(const std::uint64_t source) {
  return PayloadRef{.kind       = PayloadSourceKind::Transient,
                    .source     = source,
                    .generation = 1,
                    .occurrence = 1};
}

/// The origin the recovery tests capture from. Only a durable origin has an
/// identity that outlives its process, which is what a restart test needs;
/// transient origins have their own tests below.
PayloadRef durableRef(const std::uint64_t id) {
  return PayloadRef{.kind       = PayloadSourceKind::Durable,
                    .source     = xanadu::localScroll,
                    .generation = 0,
                    .occurrence = id};
}

/// A SpanReader over a fixed byte string, whatever span is asked for.
class FixedBytesReader final : public xanadu::SpanReader {
public:
  explicit FixedBytesReader(const std::span<const std::byte> bytes)
      : text_(textOf(bytes)) {}
  [[nodiscard]] std::string read(const xanadu::PrimediaSpan &) const override {
    return text_;
  }

private:
  std::string text_;
};

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
    run(entry);
  }
  /// The queued command at @p index on a thread of its own, joined before
  /// returning: the worker the owner must not be doing blocking I/O on
  /// behalf of. Returns that thread's id.
  auto runOnWorker(const std::size_t index = 0) -> std::thread::id {
    EXPECT_LT(index, entries_.size());
    if (index >= entries_.size()) return {};
    Entry entry = std::move(entries_[index]);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    std::thread::id worker;
    std::thread([&] {
      worker = std::this_thread::get_id();
      run(entry);
    }).join();
    return worker;
  }
  /// Every queued command at once, each on its own thread.
  void runAllConcurrently() {
    std::vector<Entry> batch(std::make_move_iterator(entries_.begin()),
                             std::make_move_iterator(entries_.end()));
    entries_.clear();
    std::vector<std::thread> threads;
    for (auto &entry : batch) {
      threads.emplace_back([&entry, this] { run(entry); });
    }
    for (auto &thread : threads) thread.join();
  }
  [[nodiscard]] auto queued() const -> std::size_t { return entries_.size(); }

private:
  struct Entry {
    SchedulerReservation reservation;
    BackendCommand command;
    IoAdapter *adapter;
    CompletionSink *sink;
  };
  void run(Entry &entry) {
    BackendContext context{.stop     = {},
                           .payloads = payloads_,
                           .budget   = {.maxBytes = backendMaxBytes_}};
    entry.adapter->execute(entry.command, context, *entry.sink);
  }
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
  if (ref.kind == PayloadSourceKind::Durable) {
    const FixedBytesReader reader(bytes);
    auto pin = world.payloads.resolveDurable(
        reader, xanadu::PrimediaSpan{
                    .scroll = static_cast<xanadu::ScrollId>(ref.source),
                    .start  = ref.occurrence,
                    .length = bytes.size()});
    if (!pin) return std::unexpected(pin.error());
    world.pins.push_back(*pin);
    return pin->range.origin;
  }
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

/// commit(), then every durable step it submits, run one worker turn at a
/// time and continued on the owner: what commit() returned before the
/// blocking work left the owner. A step that threw (a crash point) rethrows
/// out of continueCommit, as the same hook did out of the synchronous commit.
CaptureCommitResult commitToEnd(World &world, const CaptureCell capture,
                                const OutcomeCell outcome) {
  auto result = world.coordinator->commit(capture, outcome);
  while (const auto *const pending = std::get_if<CommitPending>(&result)) {
    world.scheduler->runNext();
    static_cast<void>(world.service->pump(8));
    const auto poll = world.service->poll(pending->request);
    EXPECT_TRUE(poll.outcome.has_value());
    result = world.coordinator->continueCommit(
        capture, poll.outcome.value_or(OutcomeCell{}));
  }
  return result;
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
  auto committed = commitToEnd(world, capture, *outcome);
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
  auto origin            = registerPinned(*world, durableRef(1), sourceBytes);
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
  CaptureJournal journal(journalPath_, std::nullopt);
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
  auto origin = registerPinned(*world, durableRef(1), bytesOf("abcdefgh"));
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
      commitToEnd(*world, capture, *poll.outcome)));
}

TEST_F(IoCaptureTest, OverlappingLaterSelectionReusesCommittedSpan) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("abcdefghijklmnop"); // 16 bytes
  auto origin            = registerPinned(*world, durableRef(2), sourceBytes);
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
  auto committed = commitToEnd(*world, capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Committed>(committed));
  // No new bytes: the whole selection was already translated.
  EXPECT_EQ(sink.bytes_.size(), 15U);
}

TEST_F(IoCaptureTest, DiscontiguousSelectionSplitsAndLaterOverlapReuses) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("0123456789abcdef");
  auto origin            = registerPinned(*world, durableRef(3), sourceBytes);
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
  CaptureJournal journal(journalPath_, std::nullopt);
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
  auto originA           = registerPinned(*world, durableRef(10), sourceBytes);
  auto originB           = registerPinned(*world, durableRef(11), sourceBytes);
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
  auto wrong = commitToEnd(*world, captureA, *outcomeB);
  EXPECT_TRUE(std::holds_alternative<Refused>(wrong));

  // A commit with no published outcome yet is equally refused: retire A's
  // request is not needed -- poll simply has no outcome for a fresh request.
  // (Covered by the mismatch above; the second-commit check is next.)
  auto right = commitToEnd(*world, captureA, *outcomeA);
  EXPECT_TRUE(std::holds_alternative<Committed>(right));

  // A second commit on the same capture is refused: the phase moved on.
  auto again = commitToEnd(*world, captureA, *outcomeA);
  EXPECT_TRUE(std::holds_alternative<Refused>(again));

  // B still commits cleanly with its own outcome.
  auto committedB = commitToEnd(*world, captureB, *outcomeB);
  EXPECT_TRUE(std::holds_alternative<Committed>(committedB));
}

TEST_F(IoCaptureTest, CommitBeforeOutcomeIsPublishedRefuses) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);

  const auto sourceBytes = bytesOf("xyz");
  auto origin            = registerPinned(*world, durableRef(12), sourceBytes);
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
  auto committed = commitToEnd(*world, capture, refusal);
  EXPECT_TRUE(std::holds_alternative<Refused>(committed));
  // Nothing was published: the capture can still complete once the real
  // outcome lands.
  static_cast<void>(world->service->pump(8));
  const auto outcome = world->service->poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());
  auto done = commitToEnd(*world, capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Committed>(done));
}

// -- crash injection
// ------------------------------------------------------------

TEST_F(IoCaptureTest, RestartBeforeTheWorkerTurnLeavesNoIntentToReconcile) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("persistent source bytes");
    auto registered = registerPinned(*world, durableRef(21), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;
    const PayloadRange selection[]{rangeOf(origin, 0, 12)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    // Admitted and queued; the "crash" lands before the worker turn. The
    // intent is the worker's to write, so there is none, and no bytes.
    auto appended =
        world->coordinator->appendRequest(std::get<Prepared>(prepared).capture);
    ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
    ASSERT_EQ(world->scheduler->queued(), 1U);
    CaptureJournal journal(journalPath_, std::nullopt);
    EXPECT_TRUE(journal.recover().records.empty());
  }
  EXPECT_TRUE(sink.bytes_.empty());

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  auto registered = registerPinned(*world, durableRef(21),
                                   bytesOf("persistent source bytes"));
  ASSERT_TRUE(registered.has_value());
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  EXPECT_TRUE(report->entries.empty());
  EXPECT_EQ(report->orphanBytes, 0U);
  // Nothing is pinned: the same range captures at once.
  const PayloadRange again[]{rangeOf(origin, 0, 12)};
  EXPECT_TRUE(captureCommit(*world, again).has_value());
}

TEST_F(IoCaptureTest, RestartAfterAppendIntentPinsAndReportsOrphanBytes) {
  RecordingSink sink;
  PayloadRef origin;
  CaptureCell capture{};
  std::uint64_t appendLength = 0;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    const auto sourceBytes = bytesOf("persistent source bytes");
    auto registered = registerPinned(*world, durableRef(20), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    origin = *registered;

    const PayloadRange selection[]{rangeOf(origin, 0, 12)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    capture = std::get<Prepared>(prepared).capture;

    // The worker turn writes the intent, and the process dies right after
    // the fsync, before it hands a byte to the sink: the intent is durable
    // and no bytes exist.
    CaptureJournalCrashPoints points;
    points.afterFlush = [] { throw std::runtime_error("die"); };
    world->coordinator->setJournalCrashPoints(points);
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(ioOutcomeTag(world->arena, *outcome), IoOutcomeTag::Failed);
    appendLength = 12;
    // world destroyed here: the owner never got to write an Aborted.
  }
  EXPECT_TRUE(sink.bytes_.empty());

  // Restart: fresh arena/service/coordinator over the same journal and
  // permascroll. The same transient origin is re-registered so identity
  // matches the journal's interval records.
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("persistent source bytes");
  auto registered        = registerPinned(*world, durableRef(20), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(21), sourceBytes);
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
  auto registered        = registerPinned(*world, durableRef(21), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(22), sourceBytes);
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
  auto registered        = registerPinned(*world, durableRef(22), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(23), sourceBytes);
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
  auto registered        = registerPinned(*world, durableRef(23), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(24), sourceBytes);
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
    auto committed = commitToEnd(*world, capture, *outcome);
    EXPECT_TRUE(std::holds_alternative<Refused>(committed));
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("cancelled append");
  auto registered        = registerPinned(*world, durableRef(24), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(25), sourceBytes);
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
    auto committed = commitToEnd(*world, capture, *outcome);
    EXPECT_TRUE(std::holds_alternative<Refused>(committed));
    ASSERT_EQ(sink.bytes_.size(), 10U);
  }

  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("sink fails mid-append");
  auto registered        = registerPinned(*world, durableRef(25), sourceBytes);
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
  auto originA           = registerPinned(*world, durableRef(30), sourceBytes);
  auto originB           = registerPinned(*world, durableRef(31), sourceBytes);
  ASSERT_TRUE(originA && originB);

  const PayloadRange selA[]{rangeOf(*originA, 0, 8)};
  const PayloadRange selB[]{rangeOf(*originB, 0, 8)};
  auto receiptA = captureCommit(*world, selA);
  auto receiptB = captureCommit(*world, selB);
  ASSERT_TRUE(receiptA && receiptB);

  // Hashes validate integrity only: equal bytes digest equal yet the two
  // captures landed at distinct permanent spans -- identity is the span,
  // never the digest.
  CaptureJournal journal(journalPath_, std::nullopt);
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
    CaptureJournal journal(journalPath_, std::nullopt);
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
    CaptureJournal journal(journalPath_, std::nullopt);
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
    CaptureJournal journal(journalPath_, std::nullopt);
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
    CaptureJournal journal(journalPath_, std::nullopt);
    const auto view = journal.recover();
    ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
    ASSERT_EQ(view.records.size(), 1U);
    // Death after fsync: the record is durable and the sequence advanced.
    EXPECT_THROW(static_cast<void>(journal.appendRecord(
                     CaptureJournalRecordKind::Committed, 1, {},
                     {.afterFlush = [] { throw std::runtime_error("die"); }})),
                 std::runtime_error);
  }
  CaptureJournal journal(journalPath_, std::nullopt);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  ASSERT_EQ(view.records.size(), 2U);
  EXPECT_EQ(view.records[1].kind, CaptureJournalRecordKind::Committed);
  // Sequence 1 is the Identity record every journal opens with.
  EXPECT_EQ(view.nextSequence, std::uint64_t{4});
}

TEST_F(IoCaptureTest, JournalChecksumFailureIsTypedRefusalNamingTheRecord) {
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  CaptureJournal journal(journalPath_, std::nullopt);
  const auto view = journal.recover();
  EXPECT_EQ(view.status, CaptureJournalStatus::Corrupt);
  ASSERT_TRUE(view.refusal.has_value());
  EXPECT_EQ(view.refusal->code, IoErrorCode::PersistenceRefusal);
  // Record 1 is the Identity record; the flipped byte is in the third.
  EXPECT_EQ(view.refusal->stableDetail, 3U);
}

// -- review fixes (M6 review)
// --------------------------------------------------

TEST_F(IoCaptureTest, CommitSyncsPermascrollBeforeAppendedRecordIsWritten) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("durable before promised");
  auto origin            = registerPinned(*world, durableRef(40), sourceBytes);
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
  EXPECT_TRUE(
      std::holds_alternative<Refused>(commitToEnd(*world, capture, *outcome)));
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
    auto registered = registerPinned(*world, durableRef(41), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(42), sourceBytes);
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
  auto registered        = registerPinned(*world, durableRef(42), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  const PayloadRange overlap[]{rangeOf(*registered, 0, 10)};
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->prepare(overlap)));
}

TEST_F(IoCaptureTest, PrepareRefusesUntilRecoveredAndIdsSeedFromJournal) {
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  auto origin            = registerPinned(*world, durableRef(43), sourceBytes);
  ASSERT_TRUE(origin.has_value());
  const PayloadRange selection[]{rangeOf(*origin, 0, 5)};
  auto early = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Refused>(early));

  ASSERT_TRUE(world->coordinator->recover().has_value());
  EXPECT_TRUE(world->coordinator->recovered());
  ASSERT_TRUE(captureCommit(*world, selection).has_value());

  CaptureJournal journal(journalPath_, std::nullopt);
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
  const auto budget =
      kCaptureJournalHeaderBytes +
      captureJournalFramedBytes(CaptureJournalRecordKind::Identity) + worst + 8;
  RecordingSink sink;
  auto world = boot(sink, journalPath_, budget);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("held budget");
  auto originA           = registerPinned(*world, durableRef(44), sourceBytes);
  auto originB           = registerPinned(*world, durableRef(45), sourceBytes);
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
      commitToEnd(*world, capture, *outcome)));
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
    CaptureJournal journal(journalPath_, std::nullopt);
    EXPECT_EQ(journal.sizeBytes(), 0U);
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
            .has_value());
  }
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    const auto view = journal.recover();
    EXPECT_EQ(view.status, CaptureJournalStatus::Clean);
    EXPECT_EQ(view.records.size(), 1U);
  }
  const auto foreign = dir_ / "foreign.journal";
  {
    std::ofstream file(foreign, std::ios::binary);
    file << "XYZ";
  }
  CaptureJournal journal(foreign, std::nullopt);
  CaptureJournalRecord record;
  EXPECT_FALSE(journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
                   .has_value());
  EXPECT_EQ(journal.recover().status, CaptureJournalStatus::Corrupt);
}

TEST_F(IoCaptureTest, CorruptLengthMidFileRefusesInsteadOfTruncating) {
  std::uintmax_t fullSize       = 0;
  std::size_t secondRecordStart = 0;
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  CaptureJournal journal(journalPath_, std::nullopt);
  EXPECT_EQ(std::filesystem::file_size(journalPath_), fullSize);
  const auto view = journal.recover();
  EXPECT_EQ(view.status, CaptureJournalStatus::Corrupt);
  ASSERT_TRUE(view.refusal.has_value());
  EXPECT_EQ(view.refusal->stableDetail, 3U);
  CaptureJournalRecord record;
  EXPECT_FALSE(journal.appendRecord(CaptureJournalRecordKind::Append, 3, record)
                   .has_value());
}

TEST_F(IoCaptureTest, GenuineTornTailOfAnIntervalRecordIsStillCut) {
  PayloadRef origin    = durableRef(50);
  std::size_t firstEnd = 0;
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  CaptureJournal journal(journalPath_, std::nullopt);
  EXPECT_EQ(journal.sizeBytes(), firstEnd);
  EXPECT_EQ(std::filesystem::file_size(journalPath_), firstEnd);
  EXPECT_EQ(journal.recover().records.size(), 1U);
}

TEST_F(IoCaptureTest, ZeroLengthSelectionRefusedAndTouchingIntervalsCoalesce) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("0123456789abcdefghijklmnop");
  auto origin            = registerPinned(*world, durableRef(46), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(47), sourceBytes);
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
  auto registered        = registerPinned(*world, durableRef(47), sourceBytes);
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
  const auto origin = durableRef(48);
  RecordingSink sink;
  sink.bytes_.resize(64);
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  const auto origin = durableRef(49);
  RecordingSink sink;
  sink.bytes_.resize(16);
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  auto origin            = registerPinned(*world, durableRef(51), sourceBytes);
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
  EXPECT_TRUE(
      std::holds_alternative<Refused>(commitToEnd(*world, capture, *outcome)));

  // Certain abort: the leases are back, the reservation is gone, and the
  // same range captures now without waiting for the dead cell to be swept.
  EXPECT_EQ(world->payloads.liveLeases(), leasesBefore);
  EXPECT_EQ(world->coordinator->journal().heldBytes(), 0U);
  auto retried = captureCommit(*world, selection);
  ASSERT_TRUE(retried.has_value());
  EXPECT_EQ(sink.bytes_.size(), 8U);

  CaptureJournal journal(journalPath_, std::nullopt);
  const auto view = journal.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  // Cancelled before its worker turn, so there was never an intent: the
  // Aborted stands alone, and recovery reads it as nothing having landed.
  ASSERT_GE(view.records.size(), 1U);
  EXPECT_EQ(view.records[0].kind, CaptureJournalRecordKind::Aborted);
  EXPECT_FALSE(view.records[0].abortUncertain);
}

TEST_F(IoCaptureTest, UncertainAbortKeepsPinsInProcessButReleasesLeases) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("sink fails after writing");
  auto origin            = registerPinned(*world, durableRef(52), sourceBytes);
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
  EXPECT_TRUE(
      std::holds_alternative<Refused>(commitToEnd(*world, capture, *outcome)));

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
  auto origin            = registerPinned(*world, durableRef(53), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(source), sourceBytes);
    origin          = *registered;
    world->coordinator->setJournalCrashPoints(crashOnSecondWrite(before));
    const PayloadRange selection[]{rangeOf(origin, 0, 8)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_THROW(static_cast<void>(commitToEnd(*world, capture, *outcome)),
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

// -- review r1 (fixer B): identity, retention, tears -------------------------

/// Forget a source for good: its pin goes and nothing can retain it again.
void dropSource(World &world, const PayloadRef &origin) {
  for (auto &pin : world.pins) {
    if (pin.range.origin == origin) world.payloads.release(pin.lease);
  }
  std::erase_if(world.pins, [&](const PayloadLease &pin) {
    return pin.range.origin == origin;
  });
  world.payloads.retireSource(origin);
}

TEST_F(IoCaptureTest,
       ARestartedProcessReusingATransientIdIsNotAliasedToTheOld) {
  RecordingSink sink;
  PayloadRef first;
  {
    auto world = boot(sink, journalPath_);
    ASSERT_TRUE(world);
    auto registered = registerPinned(*world, transientRef(1), bytesOf("AAAA"));
    ASSERT_TRUE(registered.has_value());
    first = *registered;
    const PayloadRange selection[]{rangeOf(first, 0, 4)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  ASSERT_EQ(textOf(sink.bytes_), "AAAA");
  // A new process hands out source 1 again, for different bytes.
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world);
  auto registered = registerPinned(*world, transientRef(1), bytesOf("BBBB"));
  ASSERT_TRUE(registered.has_value());
  ASSERT_EQ(*registered, first);
  const PayloadRange selection[]{rangeOf(*registered, 0, 4)};
  // Nothing about the old session's source 1 is known: the bytes are new.
  EXPECT_FALSE(world->coordinator->translationFor(selection[0]).has_value());
  ASSERT_TRUE(captureCommit(*world, selection).has_value());
  EXPECT_EQ(textOf(sink.bytes_), "AAAABBBB");
  EXPECT_EQ(world->coordinator->translationFor(selection[0]),
            std::optional{localSpan(4, 4)});
}

TEST_F(IoCaptureTest, AnOpenIntentOnATransientSourceDoesNotPinTheNextSession) {
  RecordingSink sink;
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    writeAttempt(journal, 1, {intervalOf(transientRef(9), 0, 4, 0)}, 4,
                 std::nullopt);
  }
  auto world        = boot(sink, journalPath_);
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  // Still reported and reconcilable by id, so the possible orphan bytes are
  // not lost ...
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{4});
  // ... but an unrelated source that reuses id 9 is not refused for it.
  auto registered = registerPinned(*world, transientRef(9), bytesOf("WXYZ"));
  ASSERT_TRUE(registered.has_value());
  const PayloadRange selection[]{rangeOf(*registered, 0, 4)};
  EXPECT_TRUE(captureCommit(*world, selection).has_value());
}

TEST_F(IoCaptureTest, AJournalKeepsItsIdentityAndIdsAcrossAReopen) {
  std::uint64_t id = 0;
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    id = journal.journalId();
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 7, record)
            .has_value());
  }
  CaptureJournal reopened(journalPath_, std::nullopt);
  EXPECT_EQ(reopened.journalId(), id);
  EXPECT_EQ(reopened.recover().journalId, id);
  EXPECT_EQ(reopened.maxCaptureId(), CaptureId{7});
  // A file without the version-3 Identity record is refused by number.
  CaptureJournal fresh(dir_ / "other.journal", std::nullopt);
  EXPECT_NE(fresh.journalId(), id);
}

TEST_F(IoCaptureTest, CaptureStateIsReapedSoALongRunPastTheOldCeilingsWorks) {
  // Bounds a few captures deep: both the translation map and the journal
  // would have refused for good after a handful of origins before.
  CaptureCoordinatorLimits limits;
  limits.maxMapEntries = 4;
  const auto roomForFour =
      kCaptureJournalHeaderBytes +
      captureJournalFramedBytes(CaptureJournalRecordKind::Identity) +
      3 * (captureJournalFramedBytes(CaptureJournalRecordKind::Append, 1) +
           captureJournalFramedBytes(CaptureJournalRecordKind::Appended, 1) +
           captureJournalFramedBytes(CaptureJournalRecordKind::Committed));
  RecordingSink sink;
  auto world = boot(sink, journalPath_, roomForFour, 1u << 20, true, limits);
  ASSERT_TRUE(world);
  constexpr std::uint64_t captures = 60;
  for (std::uint64_t i = 1; i <= captures; ++i) {
    auto registered = registerPinned(*world, transientRef(i), bytesOf("data"));
    ASSERT_TRUE(registered.has_value());
    const PayloadRange selection[]{rangeOf(*registered, 0, 4)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value())
        << "capture " << i << " refused";
    // The source dies with its last reader: its translation is dead weight.
    dropSource(*world, *registered);
  }
  EXPECT_EQ(sink.bytes_.size(), captures * 4);
  // Compaction kept the journal inside its bound and a restart reads it.
  EXPECT_LE(std::filesystem::file_size(journalPath_), roomForFour);
  CaptureJournal reopened(journalPath_, roomForFour);
  EXPECT_EQ(reopened.recover().status, CaptureJournalStatus::Clean);
  EXPECT_EQ(reopened.maxCaptureId(), captures);
  // Ids never repeat across a compaction.
  auto restarted = boot(sink, journalPath_, roomForFour);
  auto next      = registerPinned(*restarted, transientRef(1), bytesOf("more"));
  const PayloadRange selection[]{rangeOf(*next, 0, 4)};
  ASSERT_TRUE(captureCommit(*restarted, selection).has_value());
  CaptureJournal after(journalPath_, std::nullopt);
  EXPECT_EQ(after.maxCaptureId(), captures + 1);
}

TEST_F(IoCaptureTest,
       DurableCaptureStateIsEvictedSoALongRunPastBothBoundsWorks) {
  // A durable origin is reachable forever, so nothing resolves its captures;
  // only evicting the least recently hit translations (and their records)
  // keeps a long run of distinct ranges from refusing for good.
  CaptureCoordinatorLimits limits;
  limits.maxMapEntries = 4;
  const auto roomForFour =
      kCaptureJournalHeaderBytes +
      captureJournalFramedBytes(CaptureJournalRecordKind::Identity) +
      3 * (captureJournalFramedBytes(CaptureJournalRecordKind::Append, 1) +
           captureJournalFramedBytes(CaptureJournalRecordKind::Appended, 1) +
           captureJournalFramedBytes(CaptureJournalRecordKind::Committed));
  constexpr std::uint64_t captures = 60;
  constexpr std::uint64_t stride   = 8; // a gap between ranges: no coalescing
  constexpr std::uint64_t width    = 4;
  const std::vector<std::byte> source((captures + 30) * stride, std::byte{'x'});
  RecordingSink sink;
  {
    auto world = boot(sink, journalPath_, roomForFour, 1u << 20, true, limits);
    ASSERT_TRUE(world);
    auto registered = registerPinned(*world, durableRef(1), source);
    ASSERT_TRUE(registered.has_value());
    for (std::uint64_t i = 0; i < captures; ++i) {
      const PayloadRange selection[]{rangeOf(*registered, i * stride, width)};
      ASSERT_TRUE(captureCommit(*world, selection).has_value())
          << "capture " << i << " refused";
    }
    EXPECT_EQ(sink.bytes_.size(), captures * width);
    const auto counts = world->coordinator->stateCounts();
    EXPECT_LE(counts.translationSlots, limits.maxMapEntries);
    EXPECT_GT(counts.evictedTranslations, 0U);
    EXPECT_GT(counts.evictedCaptures, 0U)
        << "compaction must drop the records of evicted durable captures";
    EXPECT_LE(std::filesystem::file_size(journalPath_), roomForFour);
    // The newest range is still a translation; the oldest was evicted.
    EXPECT_TRUE(world->coordinator->translationFor(
        rangeOf(*registered, (captures - 1) * stride, width)));
    EXPECT_FALSE(
        world->coordinator->translationFor(rangeOf(*registered, 0, width)));
  }
  // The restart reads the compacted journal and keeps going past the bounds.
  auto restarted =
      boot(sink, journalPath_, roomForFour, 1u << 20, true, limits);
  ASSERT_TRUE(restarted);
  auto registered = registerPinned(*restarted, durableRef(1), source);
  ASSERT_TRUE(registered.has_value());
  EXPECT_TRUE(restarted->coordinator->translationFor(
      rangeOf(*registered, (captures - 1) * stride, width)))
      << "a retained durable translation is rebuilt from the journal";
  EXPECT_FALSE(
      restarted->coordinator->translationFor(rangeOf(*registered, 0, width)));
  for (std::uint64_t i = captures; i < captures + 30; ++i) {
    const PayloadRange selection[]{rangeOf(*registered, i * stride, width)};
    ASSERT_TRUE(captureCommit(*restarted, selection).has_value())
        << "capture " << i << " refused after a restart";
  }
  CaptureJournal after(journalPath_, std::nullopt);
  EXPECT_EQ(after.recover().status, CaptureJournalStatus::Clean);
  EXPECT_EQ(after.maxCaptureId(), captures + 30);
}

TEST_F(IoCaptureTest, EvictionNeverTakesCoverageThePrepareRelies) {
  CaptureCoordinatorLimits limits;
  limits.maxMapEntries = 3;
  RecordingSink sink;
  auto world = boot(sink, journalPath_, 4u << 20, 1u << 20, true, limits);
  ASSERT_TRUE(world);
  const std::vector<std::byte> source(256, std::byte{'y'});
  auto registered = registerPinned(*world, durableRef(1), source);
  ASSERT_TRUE(registered.has_value());
  for (std::uint64_t i = 0; i < 3; ++i) {
    const PayloadRange selection[]{rangeOf(*registered, i * 8, 4)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  // Range 0 is the coldest committed slot but this prepare covers it, so the
  // one new piece must evict another: offset 8, not offset 0.
  const PayloadRange selection[]{rangeOf(*registered, 0, 4),
                                 rangeOf(*registered, 100, 4)};
  ASSERT_TRUE(captureCommit(*world, selection).has_value());
  EXPECT_TRUE(world->coordinator->translationFor(rangeOf(*registered, 0, 4)));
  EXPECT_FALSE(world->coordinator->translationFor(rangeOf(*registered, 8, 4)));
  EXPECT_EQ(world->coordinator->stateCounts().evictedTranslations, 1U);
}

TEST_F(IoCaptureTest, CompactionKeepsWhatIsUnresolvedAndWhatSurvivesARestart) {
  RecordingSink sink;
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    // 1: committed on a process-local source (droppable). 2: committed on a
    // durable one (its translation outlives the process). 3: an open intent.
    writeAttempt(journal, 1, {intervalOf(transientRef(5), 0, 3, 0)}, 3,
                 localSpan(0, 3));
    ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Committed, 1, {})
                    .has_value());
    writeAttempt(journal, 2, {intervalOf(durableRef(6), 0, 3, 0)}, 3,
                 localSpan(3, 3));
    ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Committed, 2, {})
                    .has_value());
    writeAttempt(journal, 3, {intervalOf(durableRef(7), 0, 3, 0)}, 3,
                 std::nullopt);
    sink.bytes_.resize(6);
  }
  auto world = boot(sink, journalPath_);
  ASSERT_TRUE(world->coordinator->recover().has_value());
  // So little room that a new capture cannot be held until the resolved
  // process-local capture's records are rewritten away.
  const auto used = std::filesystem::file_size(journalPath_);
  auto tight      = boot(sink, journalPath_, used + 40, 1u << 20, false);
  ASSERT_TRUE(tight->coordinator->recover().has_value());
  auto registered = registerPinned(*tight, transientRef(8), bytesOf("abc"));
  const PayloadRange selection[]{rangeOf(*registered, 0, 3)};
  ASSERT_TRUE(captureCommit(*tight, selection).has_value());

  CaptureJournal after(journalPath_, std::nullopt);
  const auto view = after.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  std::set<CaptureId> ids;
  for (const auto &record : view.records) ids.insert(record.captureId);
  EXPECT_FALSE(ids.contains(1)) << "a resolved process-local capture retires";
  EXPECT_TRUE(ids.contains(2)) << "a durable-origin translation survives";
  EXPECT_TRUE(ids.contains(3)) << "an open intent is never dropped";
  EXPECT_EQ(view.maxCaptureId, CaptureId{4});
}

TEST_F(IoCaptureTest, UnsyncedDirectoryAfterCompactionRenameKeepsSequence) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  CaptureJournal journal(journalPath_, std::nullopt);
  CaptureJournalRecord record;
  for (CaptureId id = 1; id <= 3; ++id) {
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, id, record)
            .has_value());
  }
  // Write+search without read: rename works, the directory cannot be opened
  // to fsync, so the replacement is in place but unacknowledged.
  std::filesystem::permissions(dir_, std::filesystem::perms::owner_write |
                                         std::filesystem::perms::owner_exec);
  const auto keepNothing = [](std::span<const CaptureJournalRecord>)
      -> std::expected<std::vector<CaptureJournalRecord>, IoErrorInfo> {
    return std::vector<CaptureJournalRecord>{};
  };
  const auto compacted = journal.compactWhere(keepNothing);
  std::filesystem::permissions(dir_, std::filesystem::perms::owner_all);
  ASSERT_FALSE(compacted.has_value());
  EXPECT_EQ(compacted.error().code, IoErrorCode::BackendFailure);

  // The next append continues the compacted file's sequence, and a reopen
  // reads the whole journal instead of refusing a gap.
  ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Append, 4, record)
                  .has_value());
  CaptureJournal reopened(journalPath_, std::nullopt);
  EXPECT_EQ(reopened.recover().status, CaptureJournalStatus::Clean);
  EXPECT_EQ(reopened.maxCaptureId(), CaptureId{4});
}

TEST_F(IoCaptureTest, ACompactionNeverErasesARecordAppendedWhileItChooses) {
  CaptureJournal journal(journalPath_, std::nullopt);
  CaptureJournalRecord record;
  ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
                  .has_value());
  ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Append, 2, record)
                  .has_value());

  // A worker's step appends capture 2's Appended record at the very moment
  // the owner has read the journal and is choosing what to keep. Capture 2 is
  // live, so the choice keeps what it saw of it. The append can only run
  // before the read or after the rewrite; a separate read and rewrite let it
  // land between them, where the rename erased it.
  std::atomic<bool> started{false};
  std::thread worker;
  std::size_t seen  = 0;
  const auto select = [&](const std::span<const CaptureJournalRecord> view)
      -> std::expected<std::vector<CaptureJournalRecord>, IoErrorInfo> {
    seen   = view.size();
    worker = std::thread([&] {
      started.store(true);
      EXPECT_TRUE(
          journal.appendRecord(CaptureJournalRecordKind::Appended, 2, record)
              .has_value());
    });
    while (!started.load()) std::this_thread::yield();
    std::vector<CaptureJournalRecord> keep;
    for (const auto &kept : view) {
      if (kept.captureId == 2) keep.push_back(kept);
    }
    return keep;
  };
  ASSERT_TRUE(journal.compactWhere(select).has_value());
  worker.join();

  EXPECT_EQ(seen, 2U);
  CaptureJournal reopened(journalPath_, std::nullopt);
  const auto view = reopened.recover();
  ASSERT_EQ(view.status, CaptureJournalStatus::Clean);
  ASSERT_EQ(view.records.size(), 2U);
  EXPECT_EQ(view.records[0].kind, CaptureJournalRecordKind::Append);
  EXPECT_EQ(view.records[1].kind, CaptureJournalRecordKind::Appended)
      << "the worker's record survives, after the compaction";
  EXPECT_EQ(view.records[0].captureId, CaptureId{2});
}

TEST_F(IoCaptureTest, ACompactionRefusedByItsChoiceLeavesTheFileAlone) {
  CaptureJournal journal(journalPath_, std::nullopt);
  CaptureJournalRecord record;
  ASSERT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
                  .has_value());
  const auto before = std::filesystem::file_size(journalPath_);
  const auto refuse = [](std::span<const CaptureJournalRecord>)
      -> std::expected<std::vector<CaptureJournalRecord>, IoErrorInfo> {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 77});
  };
  const auto refused = journal.compactWhere(refuse);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().stableDetail, 77U);
  EXPECT_EQ(std::filesystem::file_size(journalPath_), before);
}

TEST_F(IoCaptureTest, ABoundOfZeroHoldsNothingAndNulloptIsUnbounded) {
  {
    CaptureJournal none(dir_ / "zero.journal", std::size_t{0});
    CaptureJournalRecord record;
    const auto refused =
        none.appendRecord(CaptureJournalRecordKind::Append, 1, record);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, IoErrorCode::BudgetExceeded);
    EXPECT_FALSE(none.hold(1).has_value());
  }
  CaptureJournal unbounded(dir_ / "all.journal", std::nullopt);
  CaptureJournalRecord record;
  EXPECT_TRUE(
      unbounded.appendRecord(CaptureJournalRecordKind::Append, 1, record)
          .has_value());
}

TEST_F(IoCaptureTest, AZeroExtendedCrashTailIsCutNotRefused) {
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    CaptureJournalRecord record;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, record)
            .has_value());
  }
  const auto boundary = std::filesystem::file_size(journalPath_);
  {
    // A size update that reached the disk ahead of the data.
    std::ofstream file(journalPath_, std::ios::binary | std::ios::app);
    const std::string zeros(97, '\0');
    file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  }
  CaptureJournal journal(journalPath_, std::nullopt);
  EXPECT_EQ(std::filesystem::file_size(journalPath_), boundary);
  const auto view = journal.recover();
  EXPECT_EQ(view.status, CaptureJournalStatus::Clean);
  EXPECT_EQ(view.records.size(), 1U);
  CaptureJournalRecord record;
  EXPECT_TRUE(journal.appendRecord(CaptureJournalRecordKind::Append, 2, record)
                  .has_value());
}

/// A sink whose append blocks until released, to hold a worker turn open.
class GatedSink final : public CaptureByteSink {
public:
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    entered.store(true);
    while (!open.load()) std::this_thread::yield();
    return xanadu::PrimediaSpan{
        .scroll = xanadu::localScroll, .start = 0, .length = bytes.size()};
  }
  auto sync() -> bool override { return true; }
  auto highWaterBytes() const -> std::uint64_t override { return 0; }
  auto read(const xanadu::PrimediaSpan &) const
      -> std::optional<std::vector<std::byte>> override {
    return std::nullopt;
  }
  std::atomic<bool> entered{false};
  std::atomic<bool> open{false};
};

class StubCompletionSink final : public CompletionSink {
public:
  void progress(const PrivateRequestToken, const IoProgress) noexcept override {
  }
  void terminalCandidate(const PrivateRequestToken, BackendResult,
                         std::optional<PayloadLease>) noexcept override {}
  void releaseAcknowledged(const BackendReleaseAck) noexcept override {}
};

TEST_F(IoCaptureTest, CompleteDuringAWorkerTurnLeavesNoStaleReceipt) {
  GatedSink sink;
  CaptureAppendAdapter adapter(sink);
  constexpr std::uint32_t key = 77;
  ASSERT_TRUE(adapter.stageAt(key, CaptureAppendSpec{bytesOf("late"), 1}));
  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  BackendCommand command{};
  command.operation.bytes = std::move(bytes);
  PayloadResolver payloads;
  BackendContext context{.stop = {}, .payloads = payloads, .budget = {}};
  StubCompletionSink completion;
  std::thread worker([&] { adapter.execute(command, context, completion); });
  while (!sink.entered.load()) std::this_thread::yield();
  // The capture is swept dead while the append is still running.
  adapter.complete(key);
  sink.open.store(true);
  worker.join();
  EXPECT_FALSE(adapter.receiptFor(key).has_value());
  EXPECT_FALSE(adapter.wasExecuted(key));
}

TEST_F(IoCaptureTest, ReusedKeyIsRefusedWhileTheRetiredTurnStillRuns) {
  GatedSink sink;
  CaptureAppendAdapter adapter(sink);
  constexpr std::uint32_t key = 78;
  ASSERT_TRUE(adapter.stageAt(key, CaptureAppendSpec{bytesOf("old"), 1}));
  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  BackendCommand command{};
  command.operation.bytes = std::move(bytes);
  PayloadResolver payloads;
  BackendContext context{.stop = {}, .payloads = payloads, .budget = {}};
  StubCompletionSink completion;
  std::thread worker([&] { adapter.execute(command, context, completion); });
  while (!sink.entered.load()) std::this_thread::yield();
  adapter.complete(key); // the capture is retired; its turn is still running
  // The cell key is reusable by a new capture, but its receipt must not be
  // erased by the old turn's deferred cleanup: staging is refused instead.
  const auto reused =
      adapter.stageAt(key, CaptureAppendSpec{bytesOf("new"), 2});
  ASSERT_FALSE(reused.has_value());
  EXPECT_EQ(reused.error().code, IoErrorCode::Conflict);
  sink.open.store(true);
  worker.join();
  EXPECT_TRUE(adapter.stageAt(key, CaptureAppendSpec{bytesOf("new"), 2}))
      << "the key is usable again once the old turn has ended";
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
  CaptureJournal journal(path, std::nullopt);
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

/// The first sealed publication carrying @p id under any journal: a test
/// convenience for stores one journal wrote. The library API is keyed by
/// (journal, id), so a lookup that ignores the journal cannot be misused
/// outside a test.
std::optional<PublishedCapture> publishedById(const CapturePublisher &publisher,
                                              const CaptureId id) {
  const auto keys = publisher.publishedKeys();
  if (!keys) return std::nullopt;
  for (const auto &key : *keys) {
    if (key.captureId != id) continue;
    if (const auto found = publisher.published(key); found && *found) {
      return **found;
    }
  }
  return std::nullopt;
}

std::set<CaptureId> publishedIdsOf(const CapturePublisher &publisher) {
  std::set<CaptureId> ids;
  if (const auto keys = publisher.publishedKeys()) {
    for (const auto &key : *keys) ids.insert(key.captureId);
  }
  return ids;
}

TEST_F(IoCaptureTest, TwoJournalsOverOneStoreNeverTakeEachOthersReceipt) {
  auto durable               = openDurable(dir_);
  const auto bytes           = bytesOf("shared store");
  std::uint64_t firstJournal = 0;
  {
    auto world = boot(*durable->sink, dir_ / "a.journal", 4u << 20, 1u << 20,
                      true, {}, durable->publisher.get());
    auto registered = registerPinned(*world, transientRef(1), bytes);
    const PayloadRange selection[]{rangeOf(*registered, 0, 6)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
    firstJournal = world->coordinator->journal().journalId();
  }
  {
    // A fresh journal restarts CaptureId at 1 over the same Store.
    auto world = boot(*durable->sink, dir_ / "b.journal", 4u << 20, 1u << 20,
                      true, {}, durable->publisher.get());
    EXPECT_NE(world->coordinator->journal().journalId(), firstJournal);
    auto registered = registerPinned(*world, transientRef(2), bytes);
    const PayloadRange selection[]{rangeOf(*registered, 7, 5)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
    const auto key = PublicationKey{
        .journalId = world->coordinator->journal().journalId(), .captureId = 1};
    const auto published = durable->publisher->published(key);
    ASSERT_TRUE(published.has_value());
    ASSERT_TRUE(published->has_value());
    ASSERT_EQ((*published)->pieces.size(), 1U);
    EXPECT_EQ(permascrollText(*durable, (*published)->pieces[0].span), "store");
  }
  const auto keys = durable->publisher->publishedKeys();
  ASSERT_TRUE(keys.has_value());
  EXPECT_EQ(keys->size(), 2U);
}

TEST_F(IoCaptureTest, PublishingOneKeyWithOtherPiecesRefusesInsteadOfSkipping) {
  auto durable = openDurable(dir_);
  const std::array first{PublishedPiece{
      .origin = transientRef(1), .srcOffset = 0, .span = localSpan(0, 4)}};
  const std::array other{PublishedPiece{
      .origin = transientRef(1), .srcOffset = 0, .span = localSpan(9, 4)}};
  const CapturePublication one{.journalId = 5, .captureId = 1, .pieces = first};
  ASSERT_TRUE(durable->publisher->publish(one).has_value());
  // The same publication again is the idempotent no-op ...
  EXPECT_TRUE(durable->publisher->publish(one).has_value());
  // ... another journal's id 1 is a different publication ...
  const CapturePublication elsewhere{
      .journalId = 6, .captureId = 1, .pieces = other};
  EXPECT_TRUE(durable->publisher->publish(elsewhere).has_value());
  // ... and the same key naming other content is refused, not skipped.
  const CapturePublication clash{
      .journalId = 5, .captureId = 1, .pieces = other};
  const auto refused = durable->publisher->publish(clash);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, IoErrorCode::PersistenceRefusal);
}

TEST_F(IoCaptureTest, CommitPublishesTranslatedSpansAndReceiptIntoTheStore) {
  auto durable = openDurable(dir_);
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  ASSERT_TRUE(world);
  const auto sourceBytes = bytesOf("hello world");
  auto registered        = registerPinned(*world, durableRef(70), sourceBytes);
  ASSERT_TRUE(registered.has_value());

  const PayloadRange first[]{rangeOf(*registered, 0, 5)};
  ASSERT_TRUE(captureCommit(*world, first).has_value());
  // [3,8) overlaps: only the uncovered suffix [5,8) is appended and published.
  const PayloadRange second[]{rangeOf(*registered, 3, 5)};
  ASSERT_TRUE(captureCommit(*world, second).has_value());
  // A fully covered selection appends nothing: a receipt with no pieces.
  const PayloadRange third[]{rangeOf(*registered, 0, 5)};
  ASSERT_TRUE(captureCommit(*world, third).has_value());

  const auto one = publishedById(*durable->publisher, 1);
  ASSERT_TRUE(one.has_value());
  ASSERT_EQ(one->pieces.size(), 1U);
  EXPECT_EQ(permascrollText(*durable, one->pieces[0].span), "hello");
  EXPECT_EQ(one->pieces[0].origin, *registered);
  EXPECT_EQ(one->pieces[0].srcOffset, std::uint64_t{0});

  const auto two = publishedById(*durable->publisher, 2);
  ASSERT_TRUE(two.has_value());
  ASSERT_EQ(two->pieces.size(), 1U);
  EXPECT_EQ(permascrollText(*durable, two->pieces[0].span), " wo");
  EXPECT_EQ(two->pieces[0].srcOffset, std::uint64_t{5});

  const auto three = publishedById(*durable->publisher, 3);
  ASSERT_TRUE(three.has_value());
  EXPECT_TRUE(three->pieces.empty());

  // The receipt cell is a scalar whose canonical bits are the CaptureId,
  // readable without parsing text, and the three form one rank in order.
  auto manifold = durable->store->rebuildManifold(durable->store->latest());
  manifold.setStore(durable->store.get());
  EXPECT_EQ(manifold.asInt64(one->receipt), std::optional<std::int64_t>{1});
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 3U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 3U);
}

TEST_F(IoCaptureTest, PublicationRoundTripsThroughStoreSaveAndReload) {
  xanadu::PrimediaSpan span;
  std::uint64_t journalId = 0;
  const auto sourceBytes  = bytesOf("round trip bytes");
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, durableRef(71), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    const PayloadRange selection[]{rangeOf(*registered, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
    span      = publishedById(*durable->publisher, 1).value().pieces.at(0).span;
    journalId = world->coordinator->journal().journalId();
    world.reset();
  }
  // Commit already saved: nothing else is written before the reload.
  auto durable        = openDurable(dir_);
  const auto reloaded = publishedById(*durable->publisher, 1);
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_EQ(reloaded->pieces.size(), 1U);
  EXPECT_EQ(reloaded->pieces[0].span, span);
  EXPECT_EQ(permascrollText(*durable, reloaded->pieces[0].span), "round trip");
  EXPECT_EQ(reloaded->pieces[0].origin.occurrence, std::uint64_t{71});
  // The journal identity rides in the Store, so the reloaded receipt is
  // found by (journal, id) and by no other journal's key.
  const auto keyed = durable->publisher->published(
      PublicationKey{.journalId = journalId, .captureId = 1});
  ASSERT_TRUE(keyed.has_value());
  ASSERT_TRUE(keyed->has_value());
  EXPECT_EQ((*keyed)->pieces[0].span, span);
  const auto foreign = durable->publisher->published(
      PublicationKey{.journalId = journalId + 1, .captureId = 1});
  ASSERT_TRUE(foreign.has_value());
  EXPECT_FALSE(foreign->has_value());
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
    auto registered = registerPinned(*world, durableRef(72), sourceBytes);
    origin          = *registered;
    // Writes this coordinator makes: 1 Append, 2 Appended, 3 Committed.
    world->coordinator->setJournalCrashPoints(crashOnWrite(3, true));
    const PayloadRange selection[]{rangeOf(origin, 0, 9)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_THROW(static_cast<void>(commitToEnd(*world, capture, *outcome)),
                 std::runtime_error);
  }
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);

  auto durable = openDurable(dir_);
  // The Store was saved before the crash: the receipt is already there.
  ASSERT_TRUE(publishedById(*durable->publisher, 1).has_value());
  const auto before = durable->store->latest();
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  auto registered = registerPinned(*world, durableRef(72), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  const auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  // No second publication: the Store did not move, one receipt, and the
  // missing Committed record was written.
  EXPECT_EQ(durable->store->latest(), before);
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  const auto translated =
      world->coordinator->translationFor(rangeOf(*registered, 0, 9));
  ASSERT_TRUE(translated.has_value());
  EXPECT_EQ(translated, publishedById(*durable->publisher, 1)->pieces[0].span);
}

TEST_F(IoCaptureTest, CrashBetweenAppendedAndPublicationPublishesExactlyOnce) {
  const auto sourceBytes = bytesOf("appended but not published");
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, durableRef(73), sourceBytes);
    // Die right after the Appended record is durable, before publication.
    world->coordinator->setJournalCrashPoints(crashOnWrite(2, false));
    const PayloadRange selection[]{rangeOf(*registered, 0, 8)};
    auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_THROW(static_cast<void>(commitToEnd(*world, capture, *outcome)),
                 std::runtime_error);
    EXPECT_FALSE(publishedById(*durable->publisher, 1).has_value());
  }
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);

  {
    auto durable = openDurable(dir_);
    EXPECT_FALSE(publishedById(*durable->publisher, 1).has_value());
    auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                      {}, durable->publisher.get());
    ASSERT_TRUE(world);
    const auto published = publishedById(*durable->publisher, 1);
    ASSERT_TRUE(published.has_value());
    ASSERT_EQ(published->pieces.size(), 1U);
    EXPECT_EQ(permascrollText(*durable, published->pieces[0].span), "appended");
    EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
    EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  }
  // A third boot finds everything finished and mints nothing.
  auto durable      = openDurable(dir_);
  const auto before = durable->store->latest();
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  ASSERT_TRUE(world);
  EXPECT_EQ(durable->store->latest(), before);
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
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
  auto registered        = registerPinned(*world, durableRef(74), sourceBytes);
  const PayloadRange selection[]{rangeOf(*registered, 0, 8)};
  auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  const auto outcome = driveAppendToOutcome(*world, capture);
  ASSERT_TRUE(outcome.has_value());

  // Publication reaches the in-memory Store but the save is refused: the
  // commit refuses and no Committed record exists.
  auto refused = commitToEnd(*world, capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Refused>(refused));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);

  std::filesystem::remove(blocker);
  auto committed = commitToEnd(*world, capture, *outcome);
  ASSERT_TRUE(std::holds_alternative<Committed>(committed));
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  EXPECT_TRUE(std::filesystem::exists(blocker / "store" / "ops.nodes"));
}

// -- the commit's durable work is off the owner (P3) --------------------------

/// A sink that notes the thread each sync() ran on, over a real one.
class ThreadNotingSink final : public CaptureByteSink {
public:
  explicit ThreadNotingSink(CaptureByteSink &inner) : inner_(inner) {}
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    return inner_.append(bytes);
  }
  auto sync() -> bool override {
    syncThreads.push_back(std::this_thread::get_id());
    return inner_.sync();
  }
  auto highWaterBytes() const -> std::uint64_t override {
    return inner_.highWaterBytes();
  }
  auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override {
    return inner_.read(span);
  }
  std::vector<std::thread::id> syncThreads;

private:
  CaptureByteSink &inner_;
};

/// Journal hooks that note the thread each record's fsync ran on.
CaptureJournalCrashPoints
noteJournalThreads(std::shared_ptr<std::vector<std::thread::id>> threads) {
  CaptureJournalCrashPoints points;
  points.beforeFlush = [threads] {
    threads->push_back(std::this_thread::get_id());
  };
  return points;
}

/// One CommitPending request's terminal outcome, once its worker turn ran.
OutcomeCell outcomeOf(World &world, const CommitPending &pending) {
  static_cast<void>(world.service->pump(8));
  const auto poll = world.service->poll(pending.request);
  EXPECT_TRUE(poll.outcome.has_value());
  return poll.outcome.value_or(OutcomeCell{});
}

struct PreparedAppend {
  CaptureCell capture;
  OutcomeCell outcome;
};

std::optional<PreparedAppend> prepareAndAppend(World &world,
                                               const PayloadRange range) {
  const PayloadRange selection[]{range};
  auto prepared = world.coordinator->prepare(selection);
  if (!std::holds_alternative<Prepared>(prepared)) return std::nullopt;
  const auto capture = std::get<Prepared>(prepared).capture;
  const auto outcome = driveAppendToOutcome(world, capture);
  if (!outcome) return std::nullopt;
  return PreparedAppend{capture, *outcome};
}

TEST_F(IoCaptureTest, CommitDoesNoBlockingIoOnTheOwnerThread) {
  const auto owner       = std::this_thread::get_id();
  const auto sourceBytes = bytesOf("blocking work belongs to workers");
  auto durable           = openDurable(dir_);
  ThreadNotingSink sink(*durable->sink);
  auto writeThreads = std::make_shared<std::vector<std::thread::id>>();
  durable->publisher->setSnapshotWriter(
      [writeThreads](const xanadu::StoreSnapshot &snapshot) {
        writeThreads->push_back(std::this_thread::get_id());
        return xanadu::SnapshotWriter::write(snapshot);
      });
  auto world      = boot(sink, journalPath_, 4u << 20, 1u << 20, true, {},
                         durable->publisher.get());
  auto registered = registerPinned(*world, durableRef(80), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  auto journalThreads = std::make_shared<std::vector<std::thread::id>>();
  world->coordinator->setJournalCrashPoints(noteJournalThreads(journalThreads));
  const auto prepared = prepareAndAppend(*world, rangeOf(*registered, 0, 8));
  ASSERT_TRUE(prepared.has_value());
  // The intent is written in the append's worker turn, which this harness
  // runs on the calling thread; AppendRequestDoesNoJournalFsyncOnTheOwnerThread
  // runs it on a thread of its own. Everything after it must not be the
  // owner's.
  ASSERT_EQ(journalThreads->size(), 1U);
  EXPECT_EQ(journalThreads->front(), owner);

  // Step 1: submitted, nothing durable yet. The owner returned without a
  // sync or an Appended record, so it never waited for either.
  auto first = world->coordinator->commit(prepared->capture, prepared->outcome);
  const auto *pendingFirst = std::get_if<CommitPending>(&first);
  ASSERT_NE(pendingFirst, nullptr);
  EXPECT_TRUE(sink.syncThreads.empty());
  EXPECT_EQ(journalThreads->size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 0U);

  const auto workerOne = world->scheduler->runOnWorker();
  EXPECT_NE(workerOne, owner);
  auto second = world->coordinator->continueCommit(
      prepared->capture, outcomeOf(*world, *pendingFirst));
  const auto *pendingSecond = std::get_if<CommitPending>(&second);
  ASSERT_NE(pendingSecond, nullptr);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);

  // Step 2: the owner published into the Store and took the snapshot, in
  // memory; the directory has no file and no Committed record until the
  // worker has run.
  EXPECT_TRUE(publishedById(*durable->publisher, 1).has_value());
  EXPECT_FALSE(std::filesystem::exists(durable->storeDir() / "ops.nodes"));
  EXPECT_TRUE(writeThreads->empty());
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);

  const auto workerTwo = world->scheduler->runOnWorker();
  EXPECT_NE(workerTwo, owner);
  const auto done = world->coordinator->continueCommit(
      prepared->capture, outcomeOf(*world, *pendingSecond));
  EXPECT_TRUE(std::holds_alternative<Committed>(done));
  EXPECT_TRUE(std::filesystem::exists(durable->storeDir() / "ops.nodes"));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);

  // Every blocking step ran on a worker and none on the owner.
  ASSERT_EQ(sink.syncThreads.size(), 1U);
  EXPECT_EQ(sink.syncThreads.front(), workerOne);
  ASSERT_EQ(writeThreads->size(), 1U);
  EXPECT_EQ(writeThreads->front(), workerTwo);
  ASSERT_EQ(journalThreads->size(), 3U);
  EXPECT_EQ((*journalThreads)[1], workerOne);
  EXPECT_EQ((*journalThreads)[2], workerTwo);
}

/// A sink over a recording one that notes, at the moment of each append,
/// whether the journal already held an Append intent for the capture and on
/// which thread the append ran.
class IntentOrderingSink final : public CaptureByteSink {
public:
  IntentOrderingSink(CaptureByteSink &inner, std::filesystem::path journal)
      : inner_(inner), journal_(std::move(journal)) {}
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    intentsSeenAtAppend.push_back(
        countKind(journal_, CaptureJournalRecordKind::Append));
    return inner_.append(bytes);
  }
  auto sync() -> bool override { return inner_.sync(); }
  auto highWaterBytes() const -> std::uint64_t override {
    return inner_.highWaterBytes();
  }
  auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override {
    return inner_.read(span);
  }
  std::vector<std::size_t> intentsSeenAtAppend;

private:
  CaptureByteSink &inner_;
  std::filesystem::path journal_;
};

TEST_F(IoCaptureTest, AppendRequestDoesNoJournalFsyncOnTheOwnerThread) {
  const auto owner = std::this_thread::get_id();
  RecordingSink recording;
  IntentOrderingSink sink(recording, journalPath_);
  auto world      = boot(sink, journalPath_);
  auto registered = registerPinned(*world, durableRef(82),
                                   bytesOf("the intent fsync is a worker's"));
  ASSERT_TRUE(registered.has_value());
  auto journalThreads = std::make_shared<std::vector<std::thread::id>>();
  world->coordinator->setJournalCrashPoints(noteJournalThreads(journalThreads));

  const PayloadRange selection[]{rangeOf(*registered, 0, 8)};
  const auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture    = std::get<Prepared>(prepared).capture;
  const auto heldBefore = world->coordinator->journal().heldBytes();
  const auto appended   = world->coordinator->appendRequest(capture);
  ASSERT_TRUE(std::holds_alternative<Admitted>(appended.admission.value));
  const auto request = std::get<Admitted>(appended.admission.value).request;

  // Admitted and queued: the owner returned without an fsync, a record, or a
  // byte, and the intent's budget is still promised.
  EXPECT_TRUE(journalThreads->empty());
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Append), 0U);
  EXPECT_TRUE(recording.bytes_.empty());
  EXPECT_EQ(world->coordinator->journal().heldBytes(), heldBefore);

  const auto worker = world->scheduler->runOnWorker();
  ASSERT_NE(worker, owner);
  ASSERT_EQ(journalThreads->size(), 1U);
  EXPECT_EQ(journalThreads->front(), worker);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Append), 1U);
  // The intent was durable when the sink saw its first byte.
  EXPECT_EQ(sink.intentsSeenAtAppend, std::vector<std::size_t>{1U});

  static_cast<void>(world->service->pump(8));
  const auto outcome = world->service->poll(request).outcome;
  ASSERT_TRUE(outcome.has_value());
  const auto done = commitToEnd(*world, capture, *outcome);
  EXPECT_TRUE(std::holds_alternative<Committed>(done));
  // The commit gave the intent's bytes back; nothing is left promised.
  EXPECT_EQ(world->coordinator->journal().heldBytes(), 0U);
}

TEST_F(IoCaptureTest, AnAdmissionRefusalWritesNoRecordAndTheCaptureRetries) {
  RecordingSink sink;
  auto world = boot(sink, journalPath_);
  auto registered =
      registerPinned(*world, durableRef(83), bytesOf("refused, then admitted"));
  ASSERT_TRUE(registered.has_value());
  const PayloadRange selection[]{rangeOf(*registered, 0, 8)};
  const auto prepared = world->coordinator->prepare(selection);
  ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
  const auto capture = std::get<Prepared>(prepared).capture;
  const auto held    = world->coordinator->journal().heldBytes();

  // No worker turn ever runs, so no intent exists and none needs closing.
  static_cast<void>(world->service->requestShutdown(ShutdownMode::Drain));
  const auto refused = world->coordinator->appendRequest(capture);
  EXPECT_TRUE(std::holds_alternative<Refused>(refused.admission.value));
  EXPECT_TRUE(journalKinds(journalPath_).empty());
  EXPECT_EQ(world->coordinator->journal().heldBytes(), held)
      << "a refusal spends none of the capture's budget";
  EXPECT_TRUE(sink.bytes_.empty());

  // Restart: nothing to reconcile, nothing pinned.
  auto restarted    = boot(sink, journalPath_);
  const auto report = restarted->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  EXPECT_TRUE(report->entries.empty());
}

TEST_F(IoCaptureTest, ARefusedIntentWriteAppendsNothingAndAbortsCertainly) {
  RecordingSink sink;
  PayloadRef origin;
  {
    auto world      = boot(sink, journalPath_);
    auto registered = registerPinned(*world, durableRef(84),
                                     bytesOf("the intent could not land"));
    ASSERT_TRUE(registered.has_value());
    origin = *registered;
    // Death before the intent's fsync: the record is rolled back.
    CaptureJournalCrashPoints points;
    points.beforeFlush = [] { throw std::runtime_error("die"); };
    world->coordinator->setJournalCrashPoints(points);
    const PayloadRange selection[]{rangeOf(origin, 0, 8)};
    const auto prepared = world->coordinator->prepare(selection);
    ASSERT_TRUE(std::holds_alternative<Prepared>(prepared));
    const auto capture = std::get<Prepared>(prepared).capture;
    const auto outcome = driveAppendToOutcome(*world, capture);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(ioOutcomeTag(world->arena, *outcome), IoOutcomeTag::Failed);
    // The sink never saw a byte, which is what makes the abort certain.
    EXPECT_TRUE(sink.bytes_.empty());
    world->coordinator->setJournalCrashPoints({});
    EXPECT_TRUE(std::holds_alternative<Refused>(
        world->coordinator->commit(capture, *outcome)));
    // An Aborted with no intent before it: certain, so the range is free.
    const auto kinds = journalKinds(journalPath_);
    ASSERT_EQ(kinds.size(), 1U);
    EXPECT_EQ(kinds[0], CaptureJournalRecordKind::Aborted);
    EXPECT_TRUE(captureCommit(*world, selection).has_value());
  }

  // And a restart over that journal pins nothing for the aborted attempt.
  auto restarted    = boot(sink, journalPath_);
  const auto report = restarted->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->orphanBytes, 0U);
  for (const auto &entry : report->entries) {
    EXPECT_EQ(entry.state, CaptureRecoveryState::Reconciled);
  }
}

TEST_F(IoCaptureTest, AnAbortedWithNoIntentIsACertainAbortNotAnAmbiguity) {
  RecordingSink sink;
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    CaptureJournalRecord aborted;
    aborted.abortUncertain = true;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Aborted, 1, aborted)
            .has_value());
  }
  auto world  = boot(sink, journalPath_);
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state, CaptureRecoveryState::Reconciled);
  EXPECT_EQ(report->orphanBytes, 0U);
}

TEST_F(IoCaptureTest, AnIntentThatLandsAfterTheOwnersAbortedIsAnOpenAttempt) {
  // The owner gave an admitted request up and wrote an uncertain Aborted
  // while its worker turn was still to come; the turn then wrote its intent
  // and may have appended. Either order pins, never reads as nothing landed.
  const auto origin = durableRef(85);
  RecordingSink sink;
  sink.bytes_.resize(8);
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    CaptureJournalRecord aborted;
    aborted.abortUncertain = true;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Aborted, 1, aborted)
            .has_value());
    CaptureJournalRecord intent;
    intent.appendLength = 8;
    intent.intervals    = {intervalOf(origin, 0, 8, 0)};
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, intent)
            .has_value());
  }
  auto world  = boot(sink, journalPath_);
  auto report = world->coordinator->recover();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->entries.size(), 1U);
  EXPECT_EQ(report->entries[0].state,
            CaptureRecoveryState::NeedsReconciliation);
  EXPECT_EQ(report->entries[0].reason, CaptureRecoveryReason::OpenIntent);
  EXPECT_EQ(report->orphanBytes, std::uint64_t{8});
}

TEST_F(IoCaptureTest, AVersionThreeJournalIsRefusedByNumber) {
  EXPECT_EQ(kCaptureJournalVersion, 4U);
  {
    CaptureJournal journal(journalPath_, std::nullopt);
    CaptureJournalRecord intent;
    intent.appendLength = 1;
    ASSERT_TRUE(
        journal.appendRecord(CaptureJournalRecordKind::Append, 1, intent)
            .has_value());
  }
  {
    std::fstream file(journalPath_,
                      std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(sizeof(kCaptureJournalSignature));
    const std::uint32_t three = 3;
    file.write(reinterpret_cast<const char *>(&three), sizeof(three));
  }
  CaptureJournal reopened(journalPath_, std::nullopt);
  const auto view = reopened.recover();
  EXPECT_EQ(view.status, CaptureJournalStatus::Corrupt);
  EXPECT_TRUE(view.records.empty());
}

TEST_F(IoCaptureTest, AStoreEditedWhileItsSnapshotIsInFlightIsNotTornIntoIt) {
  const auto sourceBytes = bytesOf("snapshot then edit");
  auto durable           = openDurable(dir_);
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  auto registered = registerPinned(*world, durableRef(81), sourceBytes);
  ASSERT_TRUE(registered.has_value());
  const auto prepared = prepareAndAppend(*world, rangeOf(*registered, 0, 8));
  ASSERT_TRUE(prepared.has_value());

  auto first = world->coordinator->commit(prepared->capture, prepared->outcome);
  ASSERT_TRUE(std::holds_alternative<CommitPending>(first));
  world->scheduler->runOnWorker();
  auto second = world->coordinator->continueCommit(
      prepared->capture, outcomeOf(*world, std::get<CommitPending>(first)));
  ASSERT_TRUE(std::holds_alternative<CommitPending>(second));

  // The snapshot is taken. The owner goes on editing the very Store while
  // the worker writes it.
  const auto opsAtSnapshot = durable->store->opCount();
  durable->store->insert(durable->store->latest(), 0, "edited after");
  EXPECT_GT(durable->store->opCount(), opsAtSnapshot);
  world->scheduler->runOnWorker();
  const auto done = world->coordinator->continueCommit(
      prepared->capture, outcomeOf(*world, std::get<CommitPending>(second)));
  ASSERT_TRUE(std::holds_alternative<Committed>(done));

  xanadu::Store reloaded(durable->perma);
  reloaded.load(durable->storeDir().string());
  EXPECT_EQ(reloaded.opCount(), opsAtSnapshot);
  // The edit is not lost, only not yet saved: the next save adds it.
  durable->store->save(durable->storeDir().string());
  xanadu::Store later(durable->perma);
  later.load(durable->storeDir().string());
  EXPECT_EQ(later.opCount(), durable->store->opCount());
}

TEST_F(IoCaptureTest, TwoCommitsInFlightTogetherLeaveBothDurable) {
  const auto sourceBytes = bytesOf("first capture second capture");
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, durableRef(82), sourceBytes);
    ASSERT_TRUE(registered.has_value());
    const auto one = prepareAndAppend(*world, rangeOf(*registered, 0, 13));
    const auto two = prepareAndAppend(*world, rangeOf(*registered, 14, 14));
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());

    auto a1 = world->coordinator->commit(one->capture, one->outcome);
    auto b1 = world->coordinator->commit(two->capture, two->outcome);
    ASSERT_TRUE(std::holds_alternative<CommitPending>(a1));
    ASSERT_TRUE(std::holds_alternative<CommitPending>(b1));
    world->scheduler->runAllConcurrently();
    auto a2 = world->coordinator->continueCommit(
        one->capture, outcomeOf(*world, std::get<CommitPending>(a1)));
    auto b2 = world->coordinator->continueCommit(
        two->capture, outcomeOf(*world, std::get<CommitPending>(b1)));
    ASSERT_TRUE(std::holds_alternative<CommitPending>(a2));
    ASSERT_TRUE(std::holds_alternative<CommitPending>(b2));

    // Both snapshots were taken before either was written, so the second
    // carries the first's operations too; they are written at the same
    // time, in whichever order the threads win.
    world->scheduler->runAllConcurrently();
    EXPECT_TRUE(
        std::holds_alternative<Committed>(world->coordinator->continueCommit(
            two->capture, outcomeOf(*world, std::get<CommitPending>(b2)))));
    EXPECT_TRUE(
        std::holds_alternative<Committed>(world->coordinator->continueCommit(
            one->capture, outcomeOf(*world, std::get<CommitPending>(a2)))));
  }
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 2U);
  auto durable = openDurable(dir_);
  EXPECT_EQ(publishedIdsOf(*durable->publisher), (std::set<CaptureId>{1, 2}));
  EXPECT_EQ(
      permascrollText(*durable,
                      publishedById(*durable->publisher, 1)->pieces[0].span),
      "first capture");
}

TEST_F(IoCaptureTest, TheSecondSnapshotWrittenFirstStillLeavesBothDurable) {
  const auto sourceBytes = bytesOf("alpha capture beta capture");
  {
    auto durable = openDurable(dir_);
    auto world   = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                        {}, durable->publisher.get());
    auto registered = registerPinned(*world, durableRef(83), sourceBytes);
    const auto one  = prepareAndAppend(*world, rangeOf(*registered, 0, 13));
    const auto two  = prepareAndAppend(*world, rangeOf(*registered, 14, 12));
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());
    auto a1 = world->coordinator->commit(one->capture, one->outcome);
    auto b1 = world->coordinator->commit(two->capture, two->outcome);
    world->scheduler->runAllConcurrently();
    auto a2 = world->coordinator->continueCommit(
        one->capture, outcomeOf(*world, std::get<CommitPending>(a1)));
    auto b2 = world->coordinator->continueCommit(
        two->capture, outcomeOf(*world, std::get<CommitPending>(b1)));
    ASSERT_TRUE(std::holds_alternative<CommitPending>(a2));
    ASSERT_TRUE(std::holds_alternative<CommitPending>(b2));
    // The later snapshot is written first, then the earlier one arrives with
    // nothing left to add and must not move the directory back.
    world->scheduler->runOnWorker(1);
    world->scheduler->runOnWorker(0);
    EXPECT_TRUE(
        std::holds_alternative<Committed>(world->coordinator->continueCommit(
            one->capture, outcomeOf(*world, std::get<CommitPending>(a2)))));
    EXPECT_TRUE(
        std::holds_alternative<Committed>(world->coordinator->continueCommit(
            two->capture, outcomeOf(*world, std::get<CommitPending>(b2)))));
  }
  auto durable = openDurable(dir_);
  EXPECT_EQ(publishedIdsOf(*durable->publisher), (std::set<CaptureId>{1, 2}));
}

struct CrashInjected {};

TEST_F(IoCaptureTest,
       CrashDuringTheSnapshotWriteRecoversByPublishingAndSaving) {
  const auto sourceBytes = bytesOf("snapshot never lands");
  {
    auto durable = openDurable(dir_);
    durable->publisher->setSnapshotWriter(
        [](const xanadu::StoreSnapshot &) -> bool { throw CrashInjected{}; });
    auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                      {}, durable->publisher.get());
    auto registered     = registerPinned(*world, durableRef(84), sourceBytes);
    const auto prepared = prepareAndAppend(*world, rangeOf(*registered, 0, 8));
    ASSERT_TRUE(prepared.has_value());
    EXPECT_THROW(static_cast<void>(
                     commitToEnd(*world, prepared->capture, prepared->outcome)),
                 CrashInjected);
    EXPECT_FALSE(std::filesystem::exists(durable->storeDir() / "ops.nodes"));
  }
  // Appended is durable, no receipt is: recovery publishes once, saves, and
  // writes the Committed record.
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);
  {
    auto durable = openDurable(dir_);
    EXPECT_FALSE(publishedById(*durable->publisher, 1).has_value());
    auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true,
                      {}, durable->publisher.get());
    ASSERT_TRUE(world);
    EXPECT_TRUE(publishedById(*durable->publisher, 1).has_value());
    EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  }
  auto durable = openDurable(dir_);
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
}

TEST_F(IoCaptureTest, ARefusedSnapshotWriteLeavesTheCommitRetriable) {
  const auto sourceBytes = bytesOf("write refused once");
  auto durable           = openDurable(dir_);
  auto refusals          = std::make_shared<int>(1);
  durable->publisher->setSnapshotWriter(
      [refusals](const xanadu::StoreSnapshot &snapshot) {
        if (*refusals > 0) {
          --*refusals;
          throw std::runtime_error("disk full");
        }
        return xanadu::SnapshotWriter::write(snapshot);
      });
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  auto registered     = registerPinned(*world, durableRef(85), sourceBytes);
  const auto prepared = prepareAndAppend(*world, rangeOf(*registered, 0, 5));
  ASSERT_TRUE(prepared.has_value());

  auto refused = commitToEnd(*world, prepared->capture, prepared->outcome);
  EXPECT_TRUE(std::holds_alternative<Refused>(refused));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 0U);

  // The retry skips the Appended step (it is durable), publishes nothing
  // twice and takes a fresh snapshot from where the directory really is.
  auto committed = commitToEnd(*world, prepared->capture, prepared->outcome);
  ASSERT_TRUE(std::holds_alternative<Committed>(committed));
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
}

TEST_F(IoCaptureTest, ACommitWhileAStepIsInFlightIsRefusedNotDoubled) {
  const auto sourceBytes = bytesOf("one step at a time");
  auto durable           = openDurable(dir_);
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  auto registered     = registerPinned(*world, durableRef(86), sourceBytes);
  const auto prepared = prepareAndAppend(*world, rangeOf(*registered, 0, 5));
  ASSERT_TRUE(prepared.has_value());
  auto first = world->coordinator->commit(prepared->capture, prepared->outcome);
  ASSERT_TRUE(std::holds_alternative<CommitPending>(first));
  EXPECT_TRUE(std::holds_alternative<Refused>(
      world->coordinator->commit(prepared->capture, prepared->outcome)));
  // A continuation with the wrong outcome leaves the step in flight.
  EXPECT_TRUE(
      std::holds_alternative<Refused>(world->coordinator->continueCommit(
          prepared->capture, prepared->outcome)));
  world->scheduler->runNext();
  auto second = world->coordinator->continueCommit(
      prepared->capture, outcomeOf(*world, std::get<CommitPending>(first)));
  EXPECT_TRUE(std::holds_alternative<CommitPending>(second));
}

/// A sink whose sync() parks the worker until the test lets it go.
class SyncGatedSink final : public CaptureByteSink {
public:
  explicit SyncGatedSink(CaptureByteSink &inner) : inner_(inner) {}
  auto append(const std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override {
    return inner_.append(bytes);
  }
  auto sync() -> bool override {
    entered.store(true);
    while (!open.load()) std::this_thread::yield();
    return inner_.sync();
  }
  auto highWaterBytes() const -> std::uint64_t override {
    return inner_.highWaterBytes();
  }
  auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override {
    return inner_.read(span);
  }
  std::atomic<bool> entered{false};
  std::atomic<bool> open{false};

private:
  CaptureByteSink &inner_;
};

TEST_F(IoCaptureTest, ARetiredCapturesJournalHoldOutlivesItsRunningStep) {
  RecordingSink recording;
  SyncGatedSink sink(recording);
  auto world      = boot(sink, journalPath_);
  auto registered = registerPinned(*world, transientRef(90),
                                   bytesOf("first capture and second capture"));
  ASSERT_TRUE(registered.has_value());
  const auto mark = world->arena.mark();
  const auto one  = prepareAndAppend(*world, rangeOf(*registered, 0, 13));
  ASSERT_TRUE(one.has_value());
  // What a capture of one piece holds at prepare: every record it may write.
  const auto worstCase =
      captureJournalFramedBytes(CaptureJournalRecordKind::Append, 1) +
      captureJournalFramedBytes(CaptureJournalRecordKind::Appended, 1) +
      captureJournalFramedBytes(CaptureJournalRecordKind::Committed) +
      captureJournalFramedBytes(CaptureJournalRecordKind::Aborted);

  auto first = world->coordinator->commit(one->capture, one->outcome);
  ASSERT_TRUE(std::holds_alternative<CommitPending>(first));
  // The commit gave back the Append intent's bytes, which the worker's append
  // turn had already written.
  const auto held = world->coordinator->journal().heldBytes();
  ASSERT_GT(held, 0U);
  std::thread worker([&] { world->scheduler->runOnWorker(); });
  while (!sink.entered.load()) std::this_thread::yield();

  // The capture's cell dies while its step is parked before the Appended
  // record. The owner notices on its next call and retires the capture.
  ASSERT_TRUE(world->arena.release(mark));
  // Only prepared: the parked step holds the sink, so a second append's
  // worker turn would wait behind it.
  const PayloadRange secondRange[]{rangeOf(*registered, 14, 14)};
  ASSERT_TRUE(std::holds_alternative<Prepared>(
      world->coordinator->prepare(secondRange)));
  const auto heldWithTwo = world->coordinator->journal().heldBytes();
  EXPECT_EQ(heldWithTwo, held + worstCase)
      << "the dead capture's bytes are still promised to the step that is "
         "about to append under them";

  sink.open.store(true);
  worker.join();
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Appended), 1U)
      << "the record the hold was for was not refused";

  // The turn has ended: the next owner call hands the bytes back.
  const PayloadRange thirdRange[]{rangeOf(*registered, 28, 4)};
  ASSERT_TRUE(std::holds_alternative<Prepared>(
      world->coordinator->prepare(thirdRange)));
  EXPECT_EQ(world->coordinator->journal().heldBytes(), worstCase * 2)
      << "the second and third captures; the dead one's bytes came back";
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
  auto registered        = registerPinned(*world, durableRef(75), sourceBytes);
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
  auto registered = registerPinned(*out.world, durableRef(source), sourceBytes);
  out.origin      = *registered;
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
      commitToEnd(*out.world, capture, *outcome)));
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
    auto registered = registerPinned(*world, durableRef(82), sourceBytes);
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
    ASSERT_TRUE(registerPinned(*world, durableRef(83), sourceBytes));
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
  auto registered        = registerPinned(*world, durableRef(84), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(85), sourceBytes);
    origin          = *registered;
    const PayloadRange selection[]{rangeOf(origin, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  const auto saved = sink.bytes_;
  sink.bytes_.resize(4);
  auto world             = boot(sink, journalPath_);
  const auto sourceBytes = bytesOf("bytes later lost");
  ASSERT_TRUE(registerPinned(*world, durableRef(85), sourceBytes));
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
    auto registered = registerPinned(*world, durableRef(86), sourceBytes);
    const PayloadRange selection[]{rangeOf(*registered, 0, 10)};
    ASSERT_TRUE(captureCommit(*world, selection).has_value());
  }
  sink.bytes_.clear();
  auto world             = boot(sink, journalPath_);
  const auto sourceBytes = bytesOf("bytes truly lost");
  auto registered        = registerPinned(*world, durableRef(86), sourceBytes);
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
    auto registered = registerPinned(*world, durableRef(87), sourceBytes);
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
        commitToEnd(*world, capture, *outcome)));
    EXPECT_FALSE(publishedById(*durable->publisher, 1).has_value());
    // The bytes sit at the old high-water mark: the owner names that span.
    landed = localSpan(0, 7);
    const auto promoted =
        world->coordinator->reconcile(1, ReconcilePromote{landed});
    ASSERT_TRUE(promoted.has_value());
    EXPECT_FALSE(promoted->publicationPending);
    const auto published = publishedById(*durable->publisher, 1);
    ASSERT_TRUE(published.has_value());
    ASSERT_EQ(published->pieces.size(), 1U);
    EXPECT_EQ(published->pieces[0].span, landed);
    EXPECT_EQ(permascrollText(*durable, landed), "promote");
  }
  // The promotion and its publication are durable across a restart.
  auto durable = openDurable(dir_);
  ASSERT_TRUE(publishedById(*durable->publisher, 1).has_value());
  auto world = boot(*durable->sink, journalPath_, 4u << 20, 1u << 20, true, {},
                    durable->publisher.get());
  EXPECT_TRUE(world->coordinator->pinnedCaptures().empty());
  EXPECT_EQ(publishedIdsOf(*durable->publisher).size(), 1U);
  EXPECT_EQ(countKind(journalPath_, CaptureJournalRecordKind::Committed), 1U);
}

TEST_F(IoCaptureTest, ReconciledRecordRoundTripsAndVersionOneIsRefused) {
  {
    CaptureJournal journal(journalPath_, std::nullopt);
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
  CaptureJournal reopened(journalPath_, std::nullopt);
  EXPECT_EQ(reopened.recover().status, CaptureJournalStatus::Corrupt);
}

} // namespace
