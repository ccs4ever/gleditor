#ifndef XANADU_VORTEX_CAPTURE_JOURNAL_HPP
#define XANADU_VORTEX_CAPTURE_JOURNAL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/vortex/framed_log.hpp"
#include "common/xanadu/vortex/io_error.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"

// design/vortex-io-implementation-plan.md §8.2, capture journal format:
// `capture.journal`, signature \x89XUDCAP, version 1, append-only
// length-delimited records carrying record kind, sequence, CaptureId,
// source interval, permascroll span, append digest and checksum. Records
// are metadata only -- never transient bytes. A journal entry is fsynced
// before Structure publication; the completion record follows publication.
// Unknown signature/version, checksum failure, a truncated record or a
// sequence gap is a typed refusal naming the record/error. This journal is
// distinct from `actor-io.journal` (\x89XUDACT): capture durability is a
// per-user transaction ledger, not an actor effect outbox.

namespace xanadu::vortex {

inline constexpr char kCaptureJournalSignature[8]     = {'\x89', 'X', 'U', 'D',
                                                         'C',    'A', 'P', '\n'};
inline constexpr std::uint32_t kCaptureJournalVersion = 2;

// On-disk frame sizes, exported so reservations are computed from the format
// and cannot drift from it. Every record is u32 bodyLen + body.
inline constexpr std::size_t kCaptureJournalHeaderBytes =
    sizeof(kCaptureJournalSignature) + sizeof(std::uint32_t);
inline constexpr std::size_t kCaptureJournalFrameBytes = sizeof(std::uint32_t);
/// kind(1) + sequence(8) + captureId(8) + checksum(8)
inline constexpr std::size_t kCaptureJournalFixedBodyBytes = 1 + 8 + 8 + 8;
/// sourceKind(1) + source, generation, occurrence, srcOffset, srcLength,
/// dstOffset (8 each)
inline constexpr std::size_t kCaptureJournalIntervalBytes = 1 + 6 * 8;
/// appendDigest(32) + appendLength(8) + intervalCount(4)
inline constexpr std::size_t kCaptureJournalAppendFixedBytes = 32 + 8 + 4;
/// scroll(4) + start(8) + length(8)
inline constexpr std::size_t kCaptureJournalSpanBytes = 4 + 8 + 8;
/// abortReason(1) + abortUncertain(1)
inline constexpr std::size_t kCaptureJournalAbortedBytes = 1 + 1;
/// promoted(1) + span(scroll, start, length)
inline constexpr std::size_t kCaptureJournalReconciledBytes =
    1 + kCaptureJournalSpanBytes;

/// Private opaque capture identity, minted monotonically by the owning
/// CaptureCoordinator and never reused (including across restarts: recovery
/// seeds the counter past the journal's maximum).
using CaptureId = std::uint64_t;

enum class CaptureJournalRecordKind : std::uint8_t {
  /// Append intent: the CaptureAppend request was admitted and the
  /// translation intent (source intervals + append digest) is durable.
  /// Written before the append is submitted so a crash can never leave
  /// permascroll bytes with no journal trace.
  Append,
  /// Append completion: the terminal append outcome was a validated
  /// success and the allocated permascroll span plus the full translation
  /// intent is durable. Fsynced before Structure publication.
  Appended,
  /// Completion: Structure publication finished; the capture's
  /// translations are final. Follows publication.
  Committed,
  /// The append resolved without publication: admission was refused
  /// (certain: no bytes can exist) or the terminal outcome was not a
  /// successful Complete (uncertain flag reports whether bytes may have
  /// landed anyway).
  Aborted,
  /// The owner's explicit decision about a capture whose bytes could not be
  /// proven (plan §8.2 "explicit reconciliation"): either Promoted -- the
  /// bytes were verified at the carried span, so the capture proceeds as if
  /// an Appended record had named it -- or Released -- the capture's
  /// intervals are unpinned and its bytes, if any, stay unreachable. Version
  /// 2 of the journal; version 1 had no such record and is refused.
  Reconciled,
};

/// Bytes one record of @p kind carrying @p intervalCount intervals occupies
/// on disk, frame included (the file header is separate).
[[nodiscard]] constexpr auto
captureJournalFramedBytes(const CaptureJournalRecordKind kind,
                          const std::size_t intervalCount = 0) noexcept
    -> std::size_t {
  std::size_t payload = 0;
  switch (kind) {
  case CaptureJournalRecordKind::Append:
    payload = kCaptureJournalAppendFixedBytes +
              intervalCount * kCaptureJournalIntervalBytes;
    break;
  case CaptureJournalRecordKind::Appended:
    payload = kCaptureJournalAppendFixedBytes +
              intervalCount * kCaptureJournalIntervalBytes +
              kCaptureJournalSpanBytes;
    break;
  case CaptureJournalRecordKind::Committed:
    break;
  case CaptureJournalRecordKind::Aborted:
    payload = kCaptureJournalAbortedBytes;
    break;
  case CaptureJournalRecordKind::Reconciled:
    payload = kCaptureJournalReconciledBytes;
    break;
  }
  return kCaptureJournalFrameBytes + kCaptureJournalFixedBodyBytes + payload;
}

/// One source interval's translation intent: the logical source address
/// (kind/source/generation/occurrence + offset/length, plan §8.1) and the
/// offset its bytes occupy inside the single appended byte string. At
/// Appended time the destination permascroll span is `span.start +
/// dstOffset` for `srcLength` bytes.
struct CaptureSourceInterval {
  PayloadSourceKind sourceKind{};
  std::uint64_t source{};
  std::uint64_t generation{};
  std::uint64_t occurrence{};
  std::uint64_t srcOffset{};
  std::uint64_t srcLength{};
  std::uint64_t dstOffset{};
  friend bool operator==(const CaptureSourceInterval &,
                         const CaptureSourceInterval &) = default;
};

/// One durable ledger entry. `sequence` and `captureId` are stamped by the
/// journal/coordinator; `sequence` is assigned by the journal at append
/// time and verified gapless on recovery. Fields meaningful per kind:
/// Append/Appended carry appendDigest, appendLength and intervals;
/// Appended additionally carries span; Committed carries neither;
/// Aborted carries abortReason/abortUncertain; Reconciled carries
/// reconciledPromoted and, when promoted, span.
struct CaptureJournalRecord {
  CaptureJournalRecordKind kind{CaptureJournalRecordKind::Append};
  CaptureId captureId{};
  std::uint64_t sequence{};
  /// BLAKE3-256 of the appended byte string; integrity only, never
  /// identity (plan §1.6/§9.1: hashes validate, they do not select).
  std::array<std::byte, 32> appendDigest{};
  std::uint64_t appendLength{};
  std::vector<CaptureSourceInterval> intervals;
  xanadu::PrimediaSpan span{};
  /// Aborted only: IoErrorCode of the refusal/failure, for diagnostics.
  std::uint8_t abortReason{};
  /// Aborted only: true when the append may have written bytes anyway
  /// (a non-Complete terminal outcome cannot prove the permascroll
  /// append did not partially land).
  bool abortUncertain{};
  /// Reconciled only: true when the owner promoted the capture at `span`,
  /// false when it released the pinned intervals (span is then unused).
  bool reconciledPromoted{};
  friend bool operator==(const CaptureJournalRecord &,
                         const CaptureJournalRecord &) = default;
};

enum class CaptureJournalStatus : std::uint8_t {
  /// Every complete record parsed; sequences are gapless from 1.
  Clean,
  /// Signature/version mismatch, checksum failure, malformed record or
  /// sequence gap. `refusal` names the record index and detail.
  Corrupt,
};

struct CaptureJournalRecovery {
  CaptureJournalStatus status{CaptureJournalStatus::Corrupt};
  /// Well-formed complete records only, in journal order.
  std::vector<CaptureJournalRecord> records;
  /// The sequence the next appended record must carry.
  std::uint64_t nextSequence{1};
  /// Highest CaptureId seen; the coordinator seeds its counter past it.
  CaptureId maxCaptureId{0};
  /// Set when status is Corrupt: PersistenceRefusal with stableDetail
  /// naming the failing record's 1-based position (0 = header/scan).
  std::optional<IoErrorInfo> refusal;
};

/// Crash-injection seams, mirroring ActorJournalCrashPoints: each hook,
/// when set, is invoked at the named point and may throw to simulate the
/// process dying mid-operation. `beforeFlush` throws before fsync so its
/// record is rolled back and never counts durable; `afterFlush` throws
/// after fsync and bookkeeping so its record is durable and counted. A
/// throwing hook ends this instance's usable life in-process; model the
/// restart with a fresh CaptureJournal over the same path.
struct CaptureJournalCrashPoints {
  std::function<void()> beforeFlush;
  std::function<void()> afterFlush;
};

/// Per-user append-only durable ledger backing capture transactions
/// (§8.2). The owning CaptureCoordinator holds one instance; it never runs
/// on a worker thread.
class CaptureJournal {
public:
  CaptureJournal(std::filesystem::path path, std::size_t maxBytes);
  ~CaptureJournal();
  CaptureJournal(const CaptureJournal &)                     = delete;
  auto operator=(const CaptureJournal &) -> CaptureJournal & = delete;

  /// Hold @p framedBytes of the file budget for records a capture has yet
  /// to write (§8.1/§8.2: "prepare reserves bounded interval-map/journal
  /// capacity ... otherwise refuses without append"). Holds sum: the check
  /// is against written + held + @p framedBytes, so N pending captures can
  /// never jointly exhaust a budget each saw as free. The owner returns the
  /// hold with unhold() as its records land or the capture dies. Holds never
  /// block appendRecord itself -- they are the owner's promise, already
  /// checked.
  [[nodiscard]] auto hold(std::size_t framedBytes)
      -> std::expected<void, IoErrorInfo>;
  /// Return up to @p framedBytes of previously held budget.
  auto unhold(std::size_t framedBytes) noexcept -> CaptureJournal *;
  [[nodiscard]] auto heldBytes() const noexcept -> std::size_t;

  /// Append one record of @p kind for @p captureId. The journal stamps
  /// `sequence` (returned in the stamped record on success). Kind-specific
  /// payload fields must already be filled; fields that do not apply to
  /// @p kind are ignored on encode. A failed append rolls the file back
  /// to the last proven record boundary; when even that cannot be proven
  /// the journal poisons and every later append refuses.
  [[nodiscard]] auto appendRecord(CaptureJournalRecordKind kind,
                                  CaptureId captureId,
                                  const CaptureJournalRecord &payload,
                                  const CaptureJournalCrashPoints & = {})
      -> std::expected<CaptureJournalRecord, IoErrorInfo>;

  /// Parses every complete record currently on disk, verifies header,
  /// per-record checksums and sequence gaplessness, and reports the
  /// recovery view. Never resubmits, acknowledges or mutates anything;
  /// classification is the coordinator's job. A file left by a crashed
  /// writer keeps its proven prefix: the constructor already cut a
  /// crash-torn tail, and a trailing incomplete record is tolerated here
  /// exactly as the constructor's repair tolerates it.
  [[nodiscard]] auto recover() const -> CaptureJournalRecovery;

  /// Highest CaptureId the journal knew at construction or has appended
  /// since; the coordinator seeds its counter past it so ids are never
  /// reused across restarts.
  [[nodiscard]] auto maxCaptureId() const noexcept -> CaptureId;

  [[nodiscard]] auto path() const -> const std::filesystem::path &;
  [[nodiscard]] auto sizeBytes() const -> std::size_t;

private:
  /// Whether @p framedBytes more fit in the budget on top of what is
  /// written, optionally counting holds.
  [[nodiscard]] auto fits(std::size_t framedBytes, bool countHolds) const
      -> std::expected<void, IoErrorInfo>;
  /// Scan callbacks for FramedLog: validate one body (checksum, per-kind
  /// length, gapless sequence) and judge whether a partial tail is the start
  /// of the very record its length prefix announces.
  static auto scanImage(std::span<const std::byte> bytes, CaptureId &maxId,
                        std::uint64_t &nextSequence) -> FramedLogScan;

  /// File mechanics shared with ActorIoJournal: header, torn-tail repair,
  /// fsynced appends with rollback, directory durability.
  FramedLog log_;
  std::size_t heldBytes_{};
  std::uint64_t nextSequence_{1};
  CaptureId maxCaptureId_{};
};

} // namespace xanadu::vortex

#endif
