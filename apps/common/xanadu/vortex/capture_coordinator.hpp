#ifndef XANADU_VORTEX_CAPTURE_COORDINATOR_HPP
#define XANADU_VORTEX_CAPTURE_COORDINATOR_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/vortex/capture_constants.hpp"
#include "common/xanadu/vortex/capture_journal.hpp"
#include "common/xanadu/vortex/capture_publication.hpp"
#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/vortex/io_service.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

// design/vortex-io-implementation-plan.md §8.2 +
// design/vortex-io-redesign.md §9.1-9.3: capture is a two-resource
// transaction -- append the selected transient bytes to the author's one
// permascroll, then commit the bounded interval translation plus the
// Structure references under one owner mark. This header is the
// file-agnostic machinery: it consumes PayloadRanges and produces
// permascroll spans, journal records and owner cells. File/media capture
// sources feed the same path through their materialized read observations
// (M5); nothing here knows what a file is.
//
// Threading: every public CaptureCoordinator method runs on the owner
// thread (the same thread that owns the arena and drives IoHost). The
// CaptureAppendAdapter's execute() runs on scheduler worker threads and
// synchronizes internally; the byte sink must tolerate serialized
// appends, which the adapter's perform mutex guarantees. A commit's
// blocking work -- the sink sync, the journal fsyncs and the Store snapshot
// write -- is staged as CaptureDurable steps and runs there too, never on
// the owner (plan §14); the owner only validates, publishes into the Store
// in memory and copies the snapshot. The one blocking write left on the
// owner is the Append intent in appendRequest(): see that method.

namespace xanadu {
class UserPermascroll;
class SegmentedPrimediaSpool;
} // namespace xanadu

namespace xanadu::vortex {

/// The durable append target a capture writes into: the author's one
/// permascroll (design §9.1 "registers content in the author's one
/// permascroll"). Implementations return the canonical PrimediaSpan the
/// bytes landed at; appends are serialized by CaptureAppendAdapter, so a
/// sink need only be internally consistent, not internally locked.
class CaptureByteSink {
public:
  virtual xanadu::PrimediaSpan append(std::span<const std::byte> bytes) = 0;
  /// Make every byte appended so far survive a crash (for the permascroll:
  /// write the tail to the active segment file and fsync it). The
  /// coordinator calls this before the Appended journal record is written:
  /// that record promises the span exists, so the bytes must be durable
  /// first or a crash leaves a durable record naming bytes that are gone.
  /// Returns false when durability could not be established.
  [[nodiscard]] virtual bool sync() = 0;
  /// Bytes the target holds. Read on restart, it is the durable high-water
  /// mark recovery reconciles journal spans against (§8.2): a span that
  /// ends past it names bytes that were lost and must not be trusted.
  [[nodiscard]] virtual std::uint64_t highWaterBytes() const = 0;
  /// The bytes at @p span, or std::nullopt when the target does not hold
  /// them. Reconciliation verifies an owner-named span against the append
  /// digest with this; it never searches for bytes (a hash validates, it
  /// does not select identity).
  [[nodiscard]] virtual std::optional<std::vector<std::byte>>
  read(const xanadu::PrimediaSpan &span) const = 0;
  virtual ~CaptureByteSink()                   = default;
};

/// CaptureByteSink over the author's UserPermascroll (the production
/// target). UserPermascroll::append takes its own lock, so concurrent
/// capture appends and ordinary typing interleave safely.
class UserPermascrollCaptureSink final : public CaptureByteSink {
public:
  explicit UserPermascrollCaptureSink(xanadu::UserPermascroll &permascroll);
  auto append(std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override;
  [[nodiscard]] auto sync() -> bool override;
  [[nodiscard]] auto highWaterBytes() const -> std::uint64_t override;
  [[nodiscard]] auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override;

private:
  xanadu::UserPermascroll &permascroll_;
};

/// CaptureByteSink over a SegmentedPrimediaSpool (tests and spool-backed
/// stores). SegmentedPrimediaSpool::append is single-appender; the adapter
/// mutex supplies that serialization.
class SegmentedPrimediaCaptureSink final : public CaptureByteSink {
public:
  explicit SegmentedPrimediaCaptureSink(xanadu::SegmentedPrimediaSpool &spool);
  auto append(std::span<const std::byte> bytes)
      -> xanadu::PrimediaSpan override;
  [[nodiscard]] auto sync() -> bool override;
  [[nodiscard]] auto highWaterBytes() const -> std::uint64_t override;
  [[nodiscard]] auto read(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>> override;

private:
  xanadu::SegmentedPrimediaSpool &spool_;
};

/// What one durable step of a commit reports back to the owner. A step runs on
/// a worker thread (a sink sync, a journal record, a snapshot write: blocking
/// I/O the owner must not do), so it cannot return into the commit that asked
/// for it; the owner's continuation reads this once the step's request is
/// terminal.
struct CaptureDurableReport {
  /// A typed refusal: the step did not complete. Nothing it was to make
  /// durable is claimed, and the commit may be retried.
  std::optional<IoErrorInfo> failure;
  /// An exception that escaped the step -- in tests, a crash point firing.
  /// The continuation rethrows it, which is what the same hook did when the
  /// step ran inside commit(), and the coordinator is not used afterwards.
  std::exception_ptr thrown;
  /// The journal sequence of the record the step wrote, when it wrote one.
  std::optional<std::uint64_t> journalSequence;
};
/// A durable step. Called once, on a scheduler worker, never on the owner.
using CaptureDurableStep = std::function<CaptureDurableReport()>;

inline constexpr std::uint32_t captureSourceKind    = 5;
inline constexpr AdapterKey captureAppendAdapterKey = {captureSourceKind, 1};

/// The frozen payload one CaptureAppend request carries: the uncovered
/// bytes to append, plus the capture identity for diagnostics. Staged
/// under the spec cell's CellRef, the StagedLensSourceAdapter pattern --
/// ArenaIoCellStore::freezeRequest requires a live spec cell, so the
/// staging key IS that cell's ref.
struct CaptureAppendSpec {
  std::vector<std::byte> bytes;
  CaptureId captureId{};
};

/// IoAdapter for the CaptureAppend operation: appends one frozen byte
/// string to the sink and records the allocated PrimediaSpan as the
/// request's receipt, which CaptureCoordinator::commit collects through
/// receiptFor(). The receipt never rides the outcome's payload channel --
/// it is permascroll metadata, not transient bytes.
///
/// The same adapter runs a commit's durable steps (stageDurableAt): one
/// adapter key, so a host registers one thing, and one set of rules for what
/// a retired capture's still-running turn may erase.
class CaptureAppendAdapter final : public IoAdapter {
public:
  explicit CaptureAppendAdapter(CaptureByteSink &sink);

  /// Stage append bytes under @p key (the spec cell's CellRef). Refuses
  /// an occupied key or a full table rather than silently redirecting an
  /// in-flight request's worker turn onto the new spec.
  [[nodiscard]] auto stageAt(std::uint32_t key, CaptureAppendSpec spec)
      -> std::expected<std::uint32_t, IoErrorInfo>;
  /// Stage a durable step under @p key (a spec cell's CellRef), with the same
  /// refusals as stageAt. The step runs on the worker that executes the
  /// request; its report is collected with takeDurableReport().
  [[nodiscard]] auto stageDurableAt(std::uint32_t key, CaptureDurableStep step)
      -> std::expected<std::uint32_t, IoErrorInfo>;
  /// The report the step staged under @p key produced, once; std::nullopt
  /// when its turn never ran (cancelled before start). Owner thread.
  [[nodiscard]] auto takeDurableReport(std::uint32_t key)
      -> std::optional<CaptureDurableReport>;
  /// Drop @p key's staged spec only. The service calls this through
  /// abandonStaged at EVERY terminal -- including requests whose worker
  /// turn already consumed the spec and stored its receipt -- so it must
  /// never touch receipts_/executed_, the evidence commit() validates.
  void abandon(std::uint32_t key) noexcept;
  /// Drop all of @p key's append state -- staged spec, receipt and the
  /// executed mark. The coordinator calls this once the append's outcome
  /// is journaled (Committed or Aborted) or the capture is swept dead;
  /// until then the entries are what commit() and wasExecuted() prove
  /// against.
  void complete(std::uint32_t key) noexcept;
  void abandonStaged(OwnerCell specification) noexcept override;

  /// The permascroll span execute() allocated for @p key, when the append
  /// completed. Owner thread; the entry stays until complete() or
  /// destruction -- commit() correlates it to the request's terminal
  /// Complete outcome before trusting it.
  [[nodiscard]] auto receiptFor(std::uint32_t key) const
      -> std::optional<xanadu::PrimediaSpan>;
  /// Whether execute() ever attempted the sink append for @p key. A
  /// request cancelled before its worker turn never reaches execute, so
  /// wasExecuted()==false proves no bytes can exist; true means the
  /// append was attempted and bytes may have landed even on a non-Complete
  /// outcome.
  [[nodiscard]] auto wasExecuted(std::uint32_t key) const noexcept -> bool;
  /// CaptureByteSink::sync() under the append serialization, so a flush
  /// never interleaves with a worker's append on a single-appender sink.
  [[nodiscard]] auto syncSink() noexcept -> bool;
  [[nodiscard]] auto sinkHighWater() const -> std::uint64_t;
  /// CaptureByteSink::read() under the append serialization.
  [[nodiscard]] auto sinkRead(const xanadu::PrimediaSpan &span) const
      -> std::optional<std::vector<std::byte>>;

  auto prepare(const CellRequest &, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> override;
  void execute(const BackendCommand &, BackendContext &,
               CompletionSink &) noexcept override;
  auto publish(const NativeCompletion &, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> override;

private:
  CaptureByteSink &sink_;
  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, CaptureAppendSpec> staged_;
  std::unordered_map<std::uint32_t, CaptureDurableStep> durableStaged_;
  std::unordered_map<std::uint32_t, CaptureDurableReport> reports_;
  std::unordered_map<std::uint32_t, xanadu::PrimediaSpan> receipts_;
  std::unordered_set<std::uint32_t> executed_;
  /// Keys whose worker turn is inside execute(): complete() must not erase
  /// what that turn is about to write, so it defers to the turn's end.
  std::unordered_set<std::uint32_t> running_;
  std::unordered_set<std::uint32_t> completeAfterTurn_;
  /// Run a staged durable step as @p key's worker turn.
  void executeDurable(const BackendCommand &, BackendContext &,
                      CompletionSink &, std::uint32_t key,
                      CaptureDurableStep step) noexcept;
  /// Ends @p key's worker turn: a complete() that arrived meanwhile takes
  /// effect now.
  void endTurn(std::uint32_t key) noexcept;
  /// Serializes sink_.append across worker turns: CaptureByteSink
  /// implementations are not required to be internally locked.
  mutable std::mutex performMutex_;
  static constexpr std::size_t maxStagedAppends = 4096;
};

/// Bounds prepare() applies to one selection, on top of the coordinator's
/// map/journal limits.
struct CapturePolicy {
  /// Selected PayloadRanges accepted per capture.
  std::size_t maxIntervals{capture_defaults::maxIntervals};
  /// Uncovered bytes accepted per capture (the append payload bound).
  std::size_t maxAppendBytes{capture_defaults::maxAppendBytes};
  /// Uncovered pieces accepted per capture (each becomes a translation
  /// entry and a journal interval record).
  std::size_t maxPieces{capture_defaults::maxPieces};
};

/// Bounds the coordinator's own state: live pending captures, live
/// translation-map slots (committed + pending + pinned) and the journal
/// file's byte budget.
struct CaptureCoordinatorLimits {
  std::size_t maxPendingCaptures{capture_defaults::maxPendingCaptures};
  std::size_t maxMapEntries{capture_defaults::maxMapEntries};
  std::size_t journalMaxBytes{capture_defaults::journalMaxBytes};
};

/// prepare() success: the owner-only capture cell naming this transaction.
struct Prepared {
  CaptureCell capture;
};
/// appendRequest() result: the service's own admission verdict. Refusals
/// -- pre-submit journal/cell failures and service refusals alike -- ride
/// the same SubmitResult::Refused outcome channel.
struct CaptureAppend {
  SubmitResult admission;
};
/// commit() success: the minted CaptureReceipt cell.
struct Committed {
  CaptureReceiptCell receipt;
};
/// commit() or continueCommit() has submitted a durable step and returned
/// without waiting for it: @p request is that step's CaptureDurable request.
/// Drive it like the append (pump the service, poll it to its terminal
/// outcome) and hand the outcome to continueCommit().
struct CommitPending {
  RequestCell request;
};
// Refused{OutcomeCell} from io_protocol.hpp is the shared refusal shape
// for prepare() and commit().

using CapturePrepareResult = std::variant<Prepared, Refused>;
using CaptureCommitResult  = std::variant<Committed, Refused, CommitPending>;

enum class CaptureRecoveryState : std::uint8_t {
  /// The journal proved the capture's final state: translations were
  /// rebuilt into the interval map (or the capture resolved without
  /// bytes). Nothing is pinned.
  Reconciled,
  /// Ordering or completeness could not be proven: the capture's source
  /// intervals are pinned against new overlapping captures until explicit
  /// reconciliation (a later milestone) resolves them, and any append
  /// bytes it may have written are reported as potential orphans --
  /// unreachable, never guessed into identity.
  NeedsReconciliation,
};

/// Why a capture needs reconciliation (None when Reconciled); diagnostics
/// and tests, not control flow.
enum class CaptureRecoveryReason : std::uint8_t {
  None,
  /// An Append intent with no outcome record: the append may have run.
  OpenIntent,
  /// A non-Complete outcome after the append was attempted.
  UncertainAbort,
  /// Record order the attempt state machine does not allow.
  AmbiguousJournal,
  /// An Appended span ends past the permascroll's durable size: its bytes
  /// were lost (or never became durable) and must not be trusted.
  SpanBeyondPermascroll,
  /// Append bytes are proven (Appended span within the permascroll) but the
  /// Structure publication could not be completed -- the Store or its save
  /// refused. Translations are rebuilt; no Committed record is written
  /// until resumePublications() succeeds. Nothing is pinned.
  PublicationIncomplete,
  /// An Appended record's intervals are inconsistent with its span or
  /// with each other.
  InvalidTranslation,
  /// The rebuilt translation overlaps an existing slot differently.
  MapConflict,
};

struct CaptureRecoveryEntry {
  CaptureId capture;
  CaptureRecoveryState state;
  CaptureRecoveryReason reason{CaptureRecoveryReason::None};
};

struct CaptureRecoveryReport {
  std::vector<CaptureRecoveryEntry> entries;
  /// Bytes appends may have written without a proven commit: the sum of
  /// appendLength over captures whose intent is open or whose abort is
  /// uncertain. Reported, never aliased.
  std::uint64_t orphanBytes{};
};

/// reconcile() decisions for a pinned capture (plan §8.2, explicit
/// reconciliation). Release declares the capture's bytes unreachable: its
/// intervals are unpinned and may be captured afresh; any bytes that did land
/// stay orphaned, never aliased. Promote asserts the bytes are at a span:
/// the coordinator verifies length, high-water and the append digest against
/// the permascroll, and only then turns the pinned intervals into committed
/// translations and publishes them. @c span defaults to the span a journal
/// Appended record already names (SpanBeyondPermascroll after the bytes
/// returned); an open intent or uncertain abort has none, so the owner must
/// name it.
struct ReconcileRelease {};
struct ReconcilePromote {
  std::optional<xanadu::PrimediaSpan> span;
};
using CaptureReconcileDecision =
    std::variant<ReconcileRelease, ReconcilePromote>;

struct CaptureReconcileResult {
  enum class Resolution : std::uint8_t { Released, Promoted };
  Resolution resolution{Resolution::Released};
  /// Promoted only: the decision and translations are durable but the
  /// Structure publication (or its completion record) is not yet; call
  /// resumePublications().
  bool publicationPending{};
};

/// The per-user capture owner (plan §8.2): owns the interval translation
/// map, the versioned capture.journal and the CaptureAppendAdapter, and
/// drives the two-resource transaction through IoService. One instance
/// per user/permascroll pair; all methods run on the owner thread.
class CaptureCoordinator {
public:
  /// @p journalPath names the user's capture.journal; @p journalMaxBytes
  /// bounds it (std::nullopt is the explicit unbounded choice, 0 holds
  /// nothing).
  /// The host must register appendAdapter() with the IoService under
  /// appendKey() and route its RequestDecoder through
  /// decodeAppendRequest() for capture spec cells.
  CaptureCoordinator(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                     PayloadResolver &payloads, IoService &service,
                     CaptureByteSink &sink, std::filesystem::path journalPath,
                     std::optional<std::size_t> journalMaxBytes =
                         capture_defaults::journalMaxBytes,
                     CaptureCoordinatorLimits limits = {});
  ~CaptureCoordinator()                                              = default;
  CaptureCoordinator(const CaptureCoordinator &)                     = delete;
  auto operator=(const CaptureCoordinator &) -> CaptureCoordinator & = delete;

  [[nodiscard]] auto appendAdapter() noexcept -> CaptureAppendAdapter &;
  [[nodiscard]] auto appendKey() const noexcept -> AdapterKey;
  [[nodiscard]] auto journal() noexcept -> CaptureJournal &;
  /// Crash-injection seam: @p points is handed to every journal write this
  /// coordinator makes. A hook that throws propagates out of the public
  /// method that wrote; model the restart with a fresh coordinator.
  auto setJournalCrashPoints(CaptureJournalCrashPoints points)
      -> CaptureCoordinator *;
  /// Whether recover() has succeeded: prepare() refuses until it has.
  [[nodiscard]] auto recovered() const noexcept -> bool;

  /// The permanent span @p range maps to, when the whole range lies inside
  /// one committed translation (offsets inside a coalesced slot map
  /// linearly). std::nullopt when any part is uncovered, pending, pinned or
  /// spread over translations that are not contiguous in the permascroll.
  [[nodiscard]] auto translationFor(const PayloadRange &range) const
      -> std::optional<xanadu::PrimediaSpan>;

  /// What the coordinator is holding and what it has let go, so a refusal
  /// that names a bound can be told apart from another.
  struct StateCounts {
    /// Committed + pending + pinned translation slots now.
    std::size_t translationSlots{};
    /// Distinct captures whose records the journal kept at its last
    /// compaction.
    std::size_t retainedCaptures{};
    /// Committed translations evicted (least recently hit first) because a
    /// bound would otherwise have refused; each costs at most a repeated
    /// append.
    std::size_t evictedTranslations{};
    /// Durable-origin captures whose journal records compaction dropped
    /// because every translation they backed had been evicted.
    std::size_t evictedCaptures{};
  };
  [[nodiscard]] auto stateCounts() const noexcept -> StateCounts;

  /// RequestDecoder hook: when @p specification names the live spec cell
  /// of a capture append awaiting admission, returns the frozen
  /// CellRequest for captureAppendAdapterKey; std::nullopt otherwise so
  /// the host's decoder falls through to its own operations.
  [[nodiscard]] auto decodeAppendRequest(CapabilityCell capability,
                                         OwnerCell specification)
      -> std::optional<CellRequest>;

  /// Refuses with Stale until recover() has run: ids and the translation
  /// map are only trustworthy once the journal is reconciled. Zero-length
  /// ranges are refused (InvalidRequest). Freeze @p selection against the
  /// translation map and reserve bounded map/journal capacity, then mint the
  /// CaptureCell. Covered intervals reuse their committed or
  /// same-capture-pending translations; only uncovered bytes are appended.
  /// Refuses -- minting nothing durable -- on an unresolvable range, a pinned
  /// (needs-reconciliation) overlap, another capture's pending overlap, or any
  /// bound exceeded.
  [[nodiscard]] auto prepare(std::span<const PayloadRange> selection,
                             const CapturePolicy &policy = {})
      -> CapturePrepareResult;

  /// Journal the append intent, stage the frozen bytes under a fresh spec
  /// cell and submit the CaptureAppend through IoService.
  ///
  /// The intent's fsync stays on the owner, deliberately (P3 moved the
  /// commit's, not this): the Append record is the only evidence that bytes
  /// may exist, so recovery's state machine, the certain-abort path (an
  /// Aborted record closing an intent the admission refusal proved never
  /// ran) and every restart test read it as "durable before submit". Running
  /// it in the append's worker turn would make an admission refusal an Aborted
  /// record with no intent, and would still leave the Aborted fsyncs of the
  /// failure paths on the owner. The returned
  /// CaptureAppend wraps the service's SubmitResult: Admitted carries the
  /// RequestCell commit() later validates; Refused carries the typed
  /// outcome (journal failure, cell mint failure or service refusal).
  /// A capture whose append was refused at admission stays Prepared and
  /// may be retried.
  [[nodiscard]] auto appendRequest(CaptureCell capture) -> CaptureAppend;

  /// Begin publishing the capture: validate that @p successfulAppend is the
  /// terminal outcome of THIS capture's append request (followed through the
  /// capture cell's request occurrence, incarnation-checked, and confirmed
  /// Complete), then move the commit's durable work -- the permascroll sync
  /// and the fsynced Appended record, then the Store snapshot write and the
  /// fsynced Committed record -- onto the scheduler as CaptureDurable
  /// requests, so the owner thread never waits on a disk (plan §14).
  ///
  /// Returns CommitPending for the first durable step the capture still
  /// needs. Any mismatch -- outcome of another capture, stale incarnation,
  /// nonterminal request, non-Complete outcome, missing receipt, a durable
  /// step already in flight -- refuses with a Rejected outcome and publishes
  /// nothing. May be called again after a refusal: every step is idempotent.
  [[nodiscard]] auto commit(CaptureCell capture, OutcomeCell successfulAppend)
      -> CaptureCommitResult;

  /// The owner-side continuation: @p durableOutcome is the terminal outcome
  /// of the CommitPending request commit() or an earlier continueCommit()
  /// returned. Collects what the worker's step did and takes the commit to
  /// its next owner step.
  ///
  /// Crash order, one capture, unchanged from the synchronous commit:
  /// permascroll sync -> Appended (fsynced) [worker] -> publish into the
  /// Store and take its snapshot [owner] -> snapshot written and fsynced ->
  /// Committed (fsynced) [worker] -> translations committed [owner]. A crash
  /// between any two leaves what recovery completes without re-appending or
  /// double-publishing.
  ///
  /// Returns Committed when the capture is done, CommitPending for the
  /// second step, or Refused when a step failed (the capture stays
  /// retriable through commit()). A step that threw -- a crash point -- is
  /// rethrown here. Refuses with InvalidRequest, leaving the step in flight,
  /// when @p durableOutcome is not that request's terminal outcome.
  [[nodiscard]] auto continueCommit(CaptureCell capture,
                                    OutcomeCell durableOutcome)
      -> CaptureCommitResult;

  /// Reconcile capture.journal against the interval map after a restart
  /// (or on demand): committed and provably-appended captures rebuild
  /// their translations; open intents, uncertain aborts and anomalies pin
  /// their source intervals against overlapping captures and report
  /// potential orphan bytes. A journal that fails its format checks is a
  /// typed PersistenceRefusal. Never resubmits an append and never
  /// guesses ambiguous overlap into identity.
  /// Idempotent: the first successful call reconciles and caches its
  /// report, later calls return that report without touching the map or the
  /// journal, so a second call can never re-insert inside slots that live
  /// commits have since coalesced.
  [[nodiscard]] auto recover()
      -> std::expected<CaptureRecoveryReport, IoErrorInfo>;

  /// Where commit() publishes Structure (plan §8.2). Must be set before
  /// recover() so recovery can complete half-finished publications; a
  /// coordinator with no publisher still journals and maps but a Committed
  /// record then proves only the journal, not a published reference.
  ///
  /// Ordering, one capture: Appended (fsynced, after the permascroll sync)
  /// -> publish into the Store -> Store snapshot written and fsynced ->
  /// Committed (the two durable steps run on a worker; see commit()). A crash
  /// between any two leaves a state recovery completes without re-appending or
  /// double-publishing: publication is idempotent keyed by (journalId,
  /// CaptureId), found by walking the Store's receipt rank.
  auto setPublisher(CapturePublisher *publisher) -> CaptureCoordinator *;
  /// Complete publications recovery or reconcile() found proven but not
  /// finished (idempotent by (journalId, CaptureId)): publish what the Store
  /// lacks, save once, then write the Committed records. Returns how many
  /// captures were completed. Any refusal leaves the remainder queued for a
  /// retry.
  [[nodiscard]] auto resumePublications()
      -> std::expected<std::size_t, IoErrorInfo>;

  /// CaptureIds whose intervals are pinned awaiting reconcile().
  [[nodiscard]] auto pinnedCaptures() const -> std::vector<CaptureId>;
  /// Resolve one pinned capture explicitly, journaling the decision before
  /// acting on it so a restart replays it. Refuses (and keeps the pin) when
  /// verification of a Promote fails: nothing is ever guessed into identity.
  [[nodiscard]] auto reconcile(CaptureId capture,
                               const CaptureReconcileDecision &decision)
      -> std::expected<CaptureReconcileResult, IoErrorInfo>;

private:
  enum class PendingPhase : std::uint8_t {
    Prepared,
    AppendRequested,
    Committed,
    Aborted,
  };

  /// One uncovered source interval: the bytes at [srcOffset, srcOffset +
  /// srcLength) of `origin` were appended at dstOffset within the single
  /// append payload, so the permanent span is span.start + dstOffset.
  struct CapturePiece {
    PayloadRef origin;
    std::uint64_t srcOffset{};
    std::uint64_t srcLength{};
    std::uint64_t dstOffset{};
    std::size_t leaseIndex{};
  };

  struct PendingCapture {
    CaptureId captureId{};
    CaptureCell cell{};
    PendingPhase phase{PendingPhase::Prepared};
    /// Leases pinning the selected source bytes until commit/abandon;
    /// translation entries never pin bytes, only this record does.
    std::vector<PayloadLease> leases;
    std::vector<CapturePiece> pieces;
    std::vector<std::byte> appendBytes;
    std::uint64_t appendLength{};
    std::array<std::byte, 32> appendDigest{};
    // Append-request linkage, set by appendRequest():
    struct SpecLink {
      zigzag::CellRef cell;
      AllocationIncarnation incarnation;
    };
    std::optional<SpecLink> spec;
    std::optional<RequestCell> appendRequest;
    /// Journal sequence of the durable Appended record, once written.
    std::optional<std::uint64_t> appendedSequence;
    std::optional<CaptureReceiptCell> receiptCell;
    bool committedRecorded{};
    /// The Structure publication is in the Store (in memory).
    bool publishedInStore{};
    /// ... and a snapshot of it was written durably.
    bool published{};
    /// The durable step in flight, from submit until its continuation.
    struct DurableLink {
      zigzag::CellRef spec;
      AllocationIncarnation incarnation;
      /// Set once admitted; the spec is decodable only until then.
      std::optional<RequestCell> request;
      /// What the step makes durable, to account its record when it reports.
      CaptureJournalRecordKind record{CaptureJournalRecordKind::Appended};
    };
    std::optional<DurableLink> durable;
    /// Journal file budget still held for this capture's records.
    std::size_t heldJournalBytes{};
  };

  /// One interval-map slot. Committed slots name the permanent span;
  /// Pending slots reserve an interval for an in-flight capture (its dst
  /// is unknown until the append lands); Pinned slots mark intervals a
  /// NeedsReconciliation capture may have appended -- new overlapping
  /// captures are refused until explicit reconciliation.
  struct TranslationSlot {
    enum class Kind : std::uint8_t { Committed, Pending, Pinned };
    Kind kind{Kind::Committed};
    std::uint64_t srcOffset{};
    std::uint64_t srcLength{};
    xanadu::PrimediaSpan dst{};
    CaptureId pendingCapture{};
    /// Recency of the last commit or covering hit (hitClock_ tick), the
    /// order in which a full map gives committed translations up.
    std::uint64_t lastHit{};
  };
  /// Per-origin non-overlapping slots keyed by srcOffset.
  using OriginMap = std::map<std::uint64_t, TranslationSlot>;
  struct PayloadRefLess {
    bool operator()(const PayloadRef &a, const PayloadRef &b) const {
      return std::tie(a.kind, a.source, a.generation, a.occurrence) <
             std::tie(b.kind, b.source, b.generation, b.occurrence);
    }
  };
  using TranslationMap = std::map<PayloadRef, OriginMap, PayloadRefLess>;
  /// pending_ is keyed by the capture cell's (ref, incarnation) pair so a
  /// stale handle can never alias a recycled allocation.
  using PendingKey = std::pair<zigzag::CellRef, AllocationIncarnation>;

  void sweepDead();
  /// Make room when a bound is about to refuse (plan §14: no unbounded
  /// capture maps, and no ceiling a user works around by deleting the
  /// journal): forget committed translations of origins nothing can read
  /// any more, and rewrite the journal without the records of captures that
  /// are resolved and published.
  void reapUnreachableSlots();
  /// Evict at least @p wanted committed translations, least recently hit
  /// first, skipping any hit at or after tick @p protectFrom (the current
  /// prepare's own coverage). Evicts nothing, and returns 0, when fewer than
  /// @p wanted are eligible: a partial eviction would lose cache for no
  /// room. The translation map is a dedup cache over journal-proven spans
  /// (plan §8.2), so what is evicted costs a repeated append, never a wrong
  /// reference, and the Store's published receipts do not depend on it.
  auto evictColdTranslations(std::size_t wanted, std::uint64_t protectFrom)
      -> std::size_t;
  [[nodiscard]] auto compactJournal() -> std::expected<void, IoErrorInfo>;
  /// Whether a committed translation still covers any part of @p interval's
  /// origin range, which is what keeps a durable capture's records.
  [[nodiscard]] auto
  hasCommittedSlotOver(const CaptureSourceInterval &interval) const -> bool;
  void releaseLeases(PendingCapture &pending);
  void erasePendingSlots(CaptureId captureId);
  [[nodiscard]] auto pendingFor(CaptureCell capture)
      -> std::map<PendingKey, PendingCapture>::iterator;
  [[nodiscard]] auto pendingFor(CaptureCell capture) const
      -> std::map<PendingKey, PendingCapture>::const_iterator;

  /// Compute the uncovered gaps of [offset, offset+length) on @p origin
  /// against the translation map, appending pieces (with running
  /// dstOffset) for each gap. Refuses on pinned overlap or another
  /// capture's pending overlap; same-capture pending slots (inserted by
  /// earlier ranges of this prepare) count as covered so overlapping
  /// selections coalesce.
  [[nodiscard]] auto
  uncoveredPieces(const PayloadRef &origin, std::uint64_t offset,
                  std::uint64_t length, CaptureId selfCapture,
                  std::vector<CapturePiece> &pieces, std::uint64_t &appendBytes,
                  std::size_t leaseIndex) -> std::expected<void, IoErrorInfo>;
  [[nodiscard]] auto insertPendingSlots(CaptureId captureId,
                                        const std::vector<CapturePiece> &pieces,
                                        std::size_t begin, std::size_t end)
      -> std::expected<void, IoErrorInfo>;
  /// Turn @p captureId's Pending slots into Pinned ones: its append may
  /// have landed, so overlapping capture must keep refusing in this process
  /// exactly as recovery would after a restart.
  void pinPendingSlots(CaptureId captureId);
  /// A capture reached a terminal state: drop its slots (pinned when
  /// @p uncertain, erased otherwise), leases and journal hold, free its
  /// pending-capture slot, and drop the adapter's spent evidence.
  void retire(std::map<PendingKey, PendingCapture>::iterator it, bool uncertain,
              CaptureRecoveryReason reason = CaptureRecoveryReason::OpenIntent);
  /// Write one journal record for @p pending, logging a refusal and
  /// consuming the held budget the record accounts for.
  auto appendJournal(PendingCapture &pending, CaptureJournalRecordKind kind,
                     const CaptureJournalRecord &payload)
      -> std::expected<CaptureJournalRecord, IoErrorInfo>;
  /// Hold enough journal budget for every record @p pending may still
  /// write in one attempt.
  [[nodiscard]] auto topUpHold(PendingCapture &pending)
      -> std::expected<void, IoErrorInfo>;
  [[nodiscard]] static auto worstCaseJournalBytes(std::size_t pieces)
      -> std::size_t;
  void finalizeTranslations(PendingCapture &pending,
                            const xanadu::PrimediaSpan &span);
  void coalesceOrigin(OriginMap &slots);
  /// All-or-nothing: inserts nothing and returns the reason when the
  /// record's intervals are inconsistent or overlap an existing slot.
  [[nodiscard]] auto rebuildFromAppended(const CaptureJournalRecord &record)
      -> std::optional<CaptureRecoveryReason>;
  void pinFromRecord(const CaptureJournalRecord &record);
  /// Pieces -> journal intervals: the translation intent each Append and
  /// Appended record carries (private CapturePiece needs member access).
  [[nodiscard]] static auto
  journalIntervals(const std::vector<CapturePiece> &pieces)
      -> std::vector<CaptureSourceInterval>;

  /// A capture whose intervals are pinned until reconcile(): what its
  /// journal records proved, enough to promote or release without the
  /// (long gone) CaptureCell.
  struct PinnedCapture {
    CaptureRecoveryReason reason{CaptureRecoveryReason::OpenIntent};
    /// The latest Append/Appended record, captureId stamped.
    CaptureJournalRecord record;
    /// record.span is a span the journal named.
    bool hasSpan{};
  };
  /// Proven-appended captures whose Structure publication or Committed
  /// record is outstanding.
  struct UnpublishedCapture {
    CaptureJournalRecord appended;
    bool committedRecorded{};
  };
  /// Pieces a publication carries: each interval at its permanent span.
  [[nodiscard]] static auto
  publishedPieces(const CaptureJournalRecord &appended)
      -> std::vector<PublishedPiece>;
  /// Publish (idempotently) and save @p appended, then record Committed
  /// unless already recorded. The one completion path for recovery,
  /// reconcile() and resumePublications().
  [[nodiscard]] auto completePublications()
      -> std::expected<std::size_t, IoErrorInfo>;
  void erasePinnedSlots(CaptureId captureId);

  /// The owner step that follows validation or a finished durable step: the
  /// next of {Appended step, publish + snapshot + Committed step, finish}.
  [[nodiscard]] auto
  advanceCommit(std::map<PendingKey, PendingCapture>::iterator it,
                const xanadu::PrimediaSpan &receipt) -> CaptureCommitResult;
  /// Stage @p step, submit it as a CaptureDurable request through the
  /// service and return CommitPending, or Refused when it was not admitted.
  [[nodiscard]] auto
  submitDurable(std::map<PendingKey, PendingCapture>::iterator it,
                CaptureJournalRecordKind record, CaptureDurableStep step)
      -> CaptureCommitResult;
  /// Account a journal record a worker step wrote against the capture's held
  /// budget, as appendJournal does for one the owner wrote.
  void consumeHold(PendingCapture &pending, CaptureJournalRecordKind kind,
                   std::size_t intervals);
  /// Finish a commit whose Committed record is durable.
  [[nodiscard]] auto
  finishCommit(std::map<PendingKey, PendingCapture>::iterator it,
               const xanadu::PrimediaSpan &receipt) -> CaptureCommitResult;

  zigzag::ArenaManifold &arena_;
  IoCellSchema schema_;
  PayloadResolver &payloads_;
  IoService &service_;
  CaptureJournal journal_;
  CaptureAppendAdapter adapter_;
  CaptureJournalCrashPoints crashPoints_;
  std::optional<CaptureRecoveryReport> recoveryReport_;
  AdapterKey appendKey_{captureAppendAdapterKey};
  CaptureCoordinatorLimits limits_;
  std::map<PendingKey, PendingCapture> pending_;
  CapturePublisher *publisher_{};
  std::map<CaptureId, PinnedCapture> pinned_;
  std::map<CaptureId, UnpublishedCapture> unpublished_;
  TranslationMap translations_;
  std::size_t translationSlots_{};
  std::uint64_t hitClock_{};
  std::size_t retainedCaptures_{};
  std::size_t evictedTranslations_{};
  std::size_t evictedCaptures_{};
  CaptureId nextCaptureId_{1};
};

} // namespace xanadu::vortex

#endif
