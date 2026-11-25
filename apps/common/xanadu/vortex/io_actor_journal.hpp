#ifndef XANADU_VORTEX_IO_ACTOR_JOURNAL_HPP
#define XANADU_VORTEX_IO_ACTOR_JOURNAL_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vortex/framed_log.hpp"
#include "common/xanadu/vortex/io_error.hpp"

// design/vortex-io-implementation-plan.md §9, actor ledger format:
// `actor-io.journal`, signature \x89XUDACT, version 2, append-only
// bounded length-delimited records for stable intent identity, endpoint
// generation, request identity, admission, receipt/unknown result and
// checkpoint causal floor. Every record carries a gapless sequence and an
// FNV-1a-64 checksum (version 1 had neither and is refused by number).
// Recovery never resends an attempt. Missing, unknown or unreconciled
// proof refuses actor restore/replay by typed outcome. The file mechanics
// -- header, torn-tail repair, fsynced append, directory durability,
// atomic replacement -- are `FramedLog`'s, shared with the capture journal.

namespace xanadu::vortex {

inline constexpr char kActorJournalSignature[8]     = {'\x89', 'X', 'U', 'D',
                                                       'A',    'C', 'T', '\n'};
inline constexpr std::uint32_t kActorJournalVersion = 2;

/// kind(1) + sequence(8) + checksum(8) + six u64 fields + argsLen(4): the
/// smallest body a record can have, and the offset arithmetic the tests and
/// the plausibility check share with the encoder.
inline constexpr std::size_t kActorJournalFixedBodyBytes =
    1 + 8 + 8 + 6 * 8 + 4;
inline constexpr std::size_t kActorJournalArgsLenOffset =
    kActorJournalFixedBodyBytes - sizeof(std::uint32_t);

enum class ActorIntentRecordKind : std::uint8_t {
  Intent,     // reversible effect intent committed by a turn
  Admission,  // dispatcher accepted the intent for IoService submission
  Receipt,    // a conclusive (known) terminal outcome was recorded
  Unresolved, // the attempt's outcome could not be confirmed (crash/stop)
  Checkpoint, // causal floor: every intent at or below it may retire
};

/// One durable ledger entry. `intentId` is the stable identity across an
/// intent's whole lifecycle (stage -> admit -> receipt/unresolved); it is
/// never reused. `floor` is meaningful only for `Checkpoint`.
struct ActorIntentRecord {
  ActorIntentRecordKind kind{ActorIntentRecordKind::Intent};
  std::uint64_t actorGeneration{};
  std::uint64_t turnOrdinal{};
  std::uint64_t intentId{};
  std::uint64_t endpointGeneration{};
  std::uint64_t requestId{};
  std::uint64_t floor{};
  std::vector<std::byte> immutableArgs; // Intent only: bytes/order proof
  friend bool operator==(const ActorIntentRecord &,
                         const ActorIntentRecord &) = default;
};

enum class ActorRestoreStatus : std::uint8_t {
  /// Every staged intent is covered by a checkpoint floor; safe to
  /// resume turns from the next ordinal. No attempt is replayed.
  Clean,
  /// The journal is still open for admission in this process (restore
  /// was attempted without first quiescing the writer). The caller must
  /// stop admitting before restoring; this is not a crash-recovery
  /// finding and nothing here is a result of lost evidence.
  NotQuiescent,
  /// At least one intent was durably admitted with no conclusive
  /// receipt and no checkpoint covering it: the real-world effect of
  /// that admission is unknown. Restore refuses; nothing is resent.
  Unknown,
  /// Signature/version mismatch, or the file could not be parsed as a
  /// sequence of complete records at all (not merely a truncated final
  /// record, which a crash mid-flush produces and recovery tolerates).
  Corrupt,
};

struct ActorRestoreResult {
  ActorRestoreStatus status{ActorRestoreStatus::Corrupt};
  std::vector<ActorIntentRecord> records; // only well-formed, complete ones
  std::vector<std::uint64_t> unknownIntents;
};

/// Crash-injection seams. Each is invoked, if set, at the named point and
/// may throw to simulate the process dying mid-operation; the journal
/// guarantees that whatever was durably flushed before the throw is what
/// `recover()` on a fresh instance over the same file will see. A throwing
/// hook ends this instance's usable life in-process — model the restart
/// with a fresh `ActorIoJournal` over the same path, whose constructor
/// scans and repairs any torn tail before appending. `beforeFlush` throws
/// before fsync, so its record is rolled back and never counts durable;
/// `afterFlush` throws after fsync and bookkeeping, so its record is
/// durable and counted.
struct ActorJournalCrashPoints {
  std::function<void()> beforeFlush;
  std::function<void()> afterFlush;
  std::function<void()> beforeDispatch;
  std::function<void()> afterReceipt;
  std::function<void()> beforeCheckpoint;
};

/// Per-user append-only durable ledger backing VAgent effect intents
/// (§9/§3.7's outbox drain, wired up in M7-VAgent). `IoHost`/dispatchers
/// own one instance; it never runs on a worker thread.
class ActorIoJournal {
public:
  /// @p maxBytes bounds the file; std::nullopt is the explicit unbounded
  /// choice. Zero holds nothing: every reservation refuses.
  ActorIoJournal(std::filesystem::path path,
                 std::optional<std::size_t> maxBytes);
  ~ActorIoJournal();
  ActorIoJournal(const ActorIoJournal &)                     = delete;
  auto operator=(const ActorIoJournal &) -> ActorIoJournal & = delete;

  /// Reserve capacity before any work is staged. §9/§13: "durable journal
  /// full/unavailable refuses before external work" and "a full journal
  /// refuses before service admission" — callers check this before
  /// calling IoService at all, not after a failed append.
  ///
  /// A journal that would overflow first retires what the latest
  /// checkpoint covers (`compact()`), so "full" means full of records no
  /// checkpoint has released: `BudgetExceeded` is then recoverable by
  /// advancing the checkpoint floor, and only that.
  [[nodiscard]] auto reserve(std::size_t recordBytes)
      -> std::expected<void, IoErrorInfo>;

  /// Retire every record the latest checkpoint covers (§9: "ledger records
  /// retire only after checkpoint causal floor ... permit retirement").
  /// Rewrites the file as the latest checkpoint followed by the records
  /// above its floor, through a temp file, fsync, rename and directory
  /// sync, so a crash leaves the old or the new journal and never a mix.
  /// Recovery of the compacted file classifies every intent exactly as the
  /// original did. Returns the bytes reclaimed; 0 when no checkpoint
  /// exists or nothing is retirable.
  [[nodiscard]] auto compact() -> std::expected<std::size_t, IoErrorInfo>;

  /// Commit the reversible effect intent. Must be called, and durably
  /// flushed and acknowledged, before `admit`. A failed/rewound turn
  /// never calls this: "Failed/rewound turn stages nothing."
  [[nodiscard]] auto stageIntent(ActorIntentRecord intent,
                                 const ActorJournalCrashPoints & = {})
      -> std::expected<void, IoErrorInfo>;

  /// Record that the dispatcher is about to hand the intent's bytes to
  /// IoService. Only after this record's flush is acknowledged may
  /// external work begin; `beforeDispatch`/`afterFlush` let a test crash
  /// exactly at that boundary.
  [[nodiscard]] auto
  recordAdmission(std::uint64_t intentId, std::uint64_t endpointGeneration,
                  std::uint64_t requestId, const ActorJournalCrashPoints & = {})
      -> std::expected<void, IoErrorInfo>;

  /// A conclusive terminal outcome arrived. `recordUnresolved` is used
  /// instead when the result could not be confirmed (worker died,
  /// process restarted before a receipt) — recovery treats that intent
  /// as `Unknown` and never resends it.
  [[nodiscard]] auto recordReceipt(std::uint64_t intentId,
                                   const ActorJournalCrashPoints & = {})
      -> std::expected<void, IoErrorInfo>;
  [[nodiscard]] auto recordUnresolved(std::uint64_t intentId)
      -> std::expected<void, IoErrorInfo>;

  /// Advance the causal floor. Records at or below `floor` retire at the
  /// next compaction (`reserve()` runs one when the journal would
  /// overflow); `recover()` treats everything at or below the latest
  /// checkpoint as resolved.
  [[nodiscard]] auto checkpoint(std::uint64_t floor,
                                const ActorJournalCrashPoints & = {})
      -> std::expected<void, IoErrorInfo>;

  /// Parses every complete record currently on disk and classifies
  /// overall journal status. Never resubmits or acknowledges anything;
  /// it only reports. Call before resuming admission from a fresh
  /// process; call `markQuiescent` first in-process (tests use a second
  /// `ActorIoJournal` instance over the same path to model "fresh
  /// process" honestly, matching a real restart).
  [[nodiscard]] auto recover() const -> ActorRestoreResult;

  /// Marks this instance as no longer admitting; `recover()` on an
  /// instance that has not been quiesced (and is not reading a file
  /// written by a different, since-exited instance) reports
  /// `NotQuiescent` instead of attempting to classify intents.
  void markQuiescent() noexcept;

  [[nodiscard]] auto path() const -> const std::filesystem::path &;
  [[nodiscard]] auto sizeBytes() const -> std::size_t;

private:
  auto appendRecord(const ActorIntentRecord &, const ActorJournalCrashPoints &)
      -> std::expected<void, IoErrorInfo>;
  /// compact() with @p pending, when set, counted as the newest checkpoint
  /// and written into the replacement.
  auto compactTo(const ActorIntentRecord *pending)
      -> std::expected<std::size_t, IoErrorInfo>;

  FramedLog log_;
  std::uint64_t nextSequence_{1};
  bool quiescent_{false};
};

} // namespace xanadu::vortex

#endif
