#ifndef XANADU_VORTEX_FILE_SOURCE_ADAPTER_HPP
#define XANADU_VORTEX_FILE_SOURCE_ADAPTER_HPP

/**
 * @file file_source_adapter.hpp
 * @brief `FileSourceAdapter` for Vortex I/O M5
 *        (design/vortex-io-implementation-plan.md §7.2, §7.3, §13 M5 row;
 *        source contract design/vortex-io-redesign.md).
 *
 * A mutable-file source: `open` returns a locator plus declared live/stable
 * capability, never an immutable identity (plan §7.2). Every successful live
 * read is recorded as a `ReadObservation` keyed by the generation the adapter
 * observed at transfer time; the owner may read a prior observation's bytes
 * back out while it stays leased, but a fresh read always gets a distinct
 * observation even when the content happens not to have changed. `stat` (used
 * here for the mtime/size generation check) and the caller's own watches do
 * not prove atomicity: a `Stable` consistency request is refused up front
 * because a plain POSIX regular file offers no backend-proven snapshot/CAS.
 *
 * Media access (plan §7.3) is `ReadBytes` with checked 64-bit offsets and
 * bounded lengths -- `requestRead` takes `MediaRange` directly rather than
 * the narrower in-memory `StreamOpSpec` length field. Decoded/derived work
 * (a "DecodeRange" product: frame, sample, index, ...) gets its own bounded
 * lease pool here, separate from the raw byte read path and separate from
 * `PayloadResolver` leases over the transferred bytes themselves.
 */

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"

namespace xanadu::vortex {

/// A checked 64-bit byte range (plan §7.3). Never aliases `PayloadRange`,
/// whose `offset`/`length` describe a slice of a resolved chunk rather than a
/// raw source byte range.
struct MediaRange {
  std::uint64_t offset{};
  std::uint64_t length{};
  friend bool operator==(const MediaRange &, const MediaRange &) = default;
};

/// Recorded once per successful live read (plan §7.2). Two observations with
/// the same `sourceGeneration` and overlapping `range` still have distinct
/// `observationId`s unless they are literally the same read; nothing proves
/// the bytes are the backend's one immutable snapshot for `Live` sources, so
/// callers must not collapse them by generation/range alone -- only an
/// explicit `Stable` consistency (refused here) would license that.
/// `range` records the ACTUAL bytes-read extent -- it may be shorter than
/// the requested range at EOF -- and `payload` binds the observation to the
/// exact retained bytes the read published.
struct ReadObservation {
  std::uint64_t observationId{};
  std::uint64_t sourceGeneration{};
  /// The actual bytes-read extent [offset, offset+length) of the source.
  MediaRange range;
  std::string locator;
  SourceConsistency consistency{SourceConsistency::Live};
  /// The retained bytes bound to this observation (the outcome payload's
  /// range); absent for a zero-byte read.
  std::optional<PayloadRange> payload;
};

/// A bounded lease over adapter-side decode/cache state (plan §7.3: "Encoded
/// source, decoded frame/sample, index and GPU surface have separate leases
/// and budget classes."). `FileSourceAdapter` does not itself decode media;
/// it hands out and accounts for the lease slots a decoding adapter layered
/// on top would occupy, so the raw byte path enforces the same bound a real
/// decoder must respect. `sourceGeneration` lets a holder detect that the
/// range it decoded is no longer backed by the generation it was leased
/// against; the lease remains valid to read until released regardless.
struct DecodeLease {
  LeaseId lease{};
  MediaRange sourceRange;
  /// nullopt when the file could not be stat'd at acquisition.
  std::optional<std::uint64_t> sourceGeneration;
};

/// Whether a file source's operations address the inode opened at open
/// time or re-resolve the path on every operation (plan §7.2's "a locator
/// plus declared capabilities, never an immutable identity" -- the identity
/// question is which of the two the locator actually pins).
enum class FileSourceIdentity : std::uint8_t {
  /// Reads and writes stay on the inode the open() call returned: renaming
  /// or replacing the path afterwards does not move them, and the
  /// `stat`-derived generation is unaffected by the path's new inode.
  /// `FileSourceAdapter` always reports this -- it holds one fd.
  OpenedInode,
  /// Each operation re-resolves the path, so a rename redirects later ops
  /// to the new inode. Not offered by `FileSourceAdapter`; declared so a
  /// future adapter can offer the distinction explicitly.
  PathFollowing,
};

/// Declared endpoint capabilities returned by `open` (plan §7.2: "a locator
/// plus declared capabilities, never an immutable identity").
struct FileOpenCapabilities {
  std::string locator;
  SourceConsistency consistency{SourceConsistency::Live};
  /// Which identity semantics the opened source has (see
  /// FileSourceIdentity); FileSourceAdapter always opens the inode.
  FileSourceIdentity identity{FileSourceIdentity::OpenedInode};
  std::uint64_t initialGeneration{};
  std::uint64_t sizeAtOpen{};
};

/// The exact `fstat` fields a generation is made of. Equality is
/// field-for-field, so two generations compare equal only when every field
/// is equal -- no hashing or mixing that could make distinct states
/// collide. The public `uint64_t` generation the protocol carries
/// (`ReadObservationResult::sourceGeneration`, `Conflict`) is an id the
/// adapter interns for one of these values.
struct FileGeneration {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint64_t size{};
  std::int64_t mtimeSeconds{};
  std::int64_t mtimeNanoseconds{};
  friend auto operator<=>(const FileGeneration &,
                          const FileGeneration &) = default;
  friend bool operator==(const FileGeneration &,
                         const FileGeneration &)  = default;
};

/// The key a staged op is stored under: the minted spec cell's ref, so its
/// width follows `OwnerCell` rather than a hand-picked integer.
using StageKey = OwnerCell;

/// Rights bits for file endpoint `OpenSpec::requestedRights` /
/// `OpenGrant::rights` (adapter-defined per plan §3.3): bit 0 read, bit 1
/// positional write, bit 2 append, bit 3 create. An open always carries
/// read. Write opens the descriptor O_RDWR; append opens it O_RDWR|O_APPEND
/// (the kernel, not the request, then decides where bytes land, so a
/// positional write on it is refused rather than silently relocated);
/// create adds O_CREAT|O_EXCL and needs write or append beside it. Write and
/// append are exclusive: one capability has one write discipline.
/// (design/vortex-io-m7-vql-c.md §4.1-§4.3.)
inline constexpr std::uint64_t fileRightRead   = 1ULL << 0;
inline constexpr std::uint64_t fileRightWrite  = 1ULL << 1;
inline constexpr std::uint64_t fileRightAppend = 1ULL << 2;
inline constexpr std::uint64_t fileRightCreate = 1ULL << 3;

/// Every right that lets a capability change the file; what a host's write
/// grant has to approve (a read right never needs one).
inline constexpr std::uint64_t fileRightsMutating =
    fileRightWrite | fileRightAppend | fileRightCreate;

/// `IoErrorInfo::detail` for the typed refusals a write path raises, so a
/// script can tell them apart without parsing text (design note §7).
inline constexpr std::uint32_t fileDetailNoWriteGrant = 1;
inline constexpr std::uint32_t fileDetailWrongMode    = 2;

/// A bounded file operation for `stageAt`: the same frozen arguments
/// `stageRead`/`stageWrite`/`stageStableRead` take, under a caller-chosen
/// key (the minted spec cell's ref) for callers crossing an `IoCellStore`.
struct FileOpSpec {
  /// `Append` writes at the end through an O_APPEND descriptor (no offset);
  /// `Sync` is `fdatasync`, a request of its own because a write is not
  /// durable until one says so (plan §7.1). New kinds go last: a stage key's
  /// meaning must not shift under a rebuilt test.
  enum class Kind : std::uint8_t {
    Read,
    Write,
    StableRead,
    Append,
    Sync
  } kind{Kind::Read};
  /// Read/StableRead: the checked 64-bit source range. Write: `offset` is
  /// the write offset (`length` is ignored; the byte count comes from
  /// `writeBytes`). Append and Sync ignore it.
  MediaRange range{};
  /// Write/Append only: the frozen bytes. They sit in the staging table only
  /// until `prepare`, which moves them into the prepared command so the
  /// service charges them to its command budget.
  std::vector<std::byte> writeBytes;
  /// Expected generation id at staging time (see `currentGeneration`);
  /// nullopt performs no check.
  std::optional<std::uint64_t> expectedGeneration;
};

/// A `d.io.kind` "file" source over one already-resolved filesystem path.
/// `open()` performs no admission of its own (that is `IoService`'s job via
/// `IoCellStore::mintOpenedEndpoint`); it is the synchronous, owner-thread
/// capability probe a registering caller runs once to learn what the backend
/// can promise before wiring up `OpenSpec`.
///
/// Reads are staged through `stageRead`/`stageWrite` (bounded 64-bit
/// `MediaRange`, mirroring `StagedByteSourceAdapter::stage` but keyed on
/// `MediaRange` rather than the narrower `StreamOpSpec`), then admitted
/// through the ordinary `IoAdapter::prepare`/`execute`/`publish` path. Every
/// successful execute(): (1) revalidates the generation it observed at
/// `stageRead` time against the file's current `stat`, refusing `Conflict`
/// on mismatch exactly like `StagedByteSourceAdapter`; (2) performs a
/// `pread`/`pwrite` at the checked 64-bit offset; (3) records a
/// `ReadObservation` for a successful read. `stat`-based generation tracking
/// only detects an *uncertain* write (plan §7.2: "After uncertain write,
/// invalidate live generation/cache and require revalidation; prior
/// observations remain readable."); it is not proof of atomicity, so a
/// `Stable` request is refused before admission.
class FileSourceAdapter final : public IoAdapter {
public:
  /// Opens @p path read-write if @p writable, else read-only. Throws nothing;
  /// failure to open is reported by `probeCapabilities` returning nullopt and
  /// every subsequent `prepare` refusing `Unsupported`.
  explicit FileSourceAdapter(std::string path, bool writable = false);
  /// Adopt an already-open descriptor: the file endpoint adapter performs
  /// `open()` inside `execute` (off the owner thread, plan §7.2) and hands
  /// the descriptor here so every later read/write stays on the inode that
  /// was opened then -- never a path re-resolved later
  /// (`FileSourceIdentity::OpenedInode`). A negative @p fd behaves like a
  /// failed open: `probeCapabilities` reports nullopt and `prepare` refuses
  /// `Unsupported`.
  /// @p append says the descriptor was opened O_APPEND: `Append` ops are then
  /// the only writes it accepts and `Write` is refused, because `pwrite` to
  /// an O_APPEND descriptor ignores its offset on Linux.
  FileSourceAdapter(int fd, std::string path, bool writable,
                    bool append = false);
  ~FileSourceAdapter() override;
  FileSourceAdapter(const FileSourceAdapter &)                     = delete;
  auto operator=(const FileSourceAdapter &) -> FileSourceAdapter & = delete;

  /// Owner-thread capability probe (not an `IoAdapter` method: this is the
  /// synchronous pre-open step a caller runs to populate `OpenSpec`, not a
  /// request the scheduler admits). Returns nullopt if the file could not be
  /// opened/stat'd at construction time.
  [[nodiscard]] auto probeCapabilities() const
      -> std::optional<FileOpenCapabilities>;

  /// Stage a bounded read at a checked 64-bit range, keyed by the generation
  /// expected at staging time (nullopt performs no check). Returns the key
  /// to place in the request's `specification` cell, or nullopt when the
  /// staging table is full.
  [[nodiscard]] std::optional<StageKey>
  stageRead(MediaRange range,
            std::optional<std::uint64_t> expectedGeneration = std::nullopt);
  /// Stage a bounded write at a checked 64-bit offset. `Stable` sources never
  /// reach here (open already refused); writable `Live` files accept it.
  /// nullopt when the table or its byte bound is full.
  [[nodiscard]] std::optional<StageKey>
  stageWrite(std::uint64_t offset, std::vector<std::byte> bytes,
             std::optional<std::uint64_t> expectedGeneration = std::nullopt);
  /// Stage a strict-snapshot request. Always refused at `prepare` time with
  /// `Unsupported` -- a plain POSIX file is never a backend-proven immutable
  /// source -- but staged like any other operation so the refusal flows
  /// through the normal admission path instead of being special-cased by
  /// callers. nullopt when the table is full.
  [[nodiscard]] std::optional<StageKey> stageStableRead(MediaRange range);

  /// Stage under a caller-chosen @p key (the minted spec cell's ref) -- the
  /// `StagedLensSourceAdapter::stageAt` pattern for callers whose request
  /// `specification` must name a live arena cell:
  /// `ArenaIoCellStore::freezeRequest` refuses any OwnerCell with no
  /// incarnation, so the key IS that cell and the adapter owns what it
  /// means. Refuses an occupied key or a full table; a write spec is
  /// charged against the same staged-byte bound `stageWrite` enforces.
  [[nodiscard]] std::expected<StageKey, std::string> stageAt(StageKey key,
                                                             FileOpSpec spec);

  /// Owner-thread: drop a staged op that will never reach `execute` (the
  /// submission was refused, or the caller abandoned it). The staging
  /// table is bounded at 4096 live ops; `stage*` returns nullopt when full.
  void abandon(StageKey key);

  /// `IoAdapter` hook: the service calls this with the request's
  /// `specification` when a submission is refused or its request reaches a
  /// terminal state without `execute` consuming the staged entry.
  void abandonStaged(OwnerCell specification) noexcept override;

  auto prepare(const CellRequest &request, OwnerContext &context)
      -> std::expected<PreparedOperation, IoErrorInfo> override;
  void execute(const BackendCommand &command, BackendContext &context,
               CompletionSink &sink) noexcept override;
  auto publish(const NativeCompletion &completion, OwnerContext &context)
      -> std::expected<OwnerPublication, IoErrorInfo> override;

  /// Id of the file's current generation (an exact `FileGeneration`
  /// interned by this adapter); changes whenever size, mtime, device or
  /// inode moved, whether by this adapter's own write or an external
  /// writer. nullopt when the descriptor is absent or `fstat` fails.
  [[nodiscard]] auto currentGeneration() const -> std::optional<std::uint64_t>;

  /// All recorded observations' bytes remain nominally retrievable while the
  /// caller holds the observation; the adapter keeps the metadata (not the
  /// bytes, which live in the resolved `PayloadLease`) so a test or owner can
  /// confirm distinct origin. Bounded by `maxRetainedObservations`.
  [[nodiscard]] auto observation(std::uint64_t observationId) const
      -> std::optional<ReadObservation>;

  /// Bounded decode/cache lease pool (plan §7.3). Refuses `BudgetExceeded`
  /// once `maxDecodeLeases` are outstanding. A lease taken against a stale
  /// generation is still returned (the caller asked for exactly that
  /// combination); it is the caller's job to compare `sourceGeneration`
  /// against `currentGeneration()` before trusting cached decode output.
  [[nodiscard]] auto acquireDecodeLease(MediaRange range)
      -> std::expected<DecodeLease, IoErrorInfo>;
  void releaseDecodeLease(LeaseId lease) noexcept;
  [[nodiscard]] std::size_t liveDecodeLeases() const noexcept;

  auto setMaxDecodeLeases(std::size_t value) -> FileSourceAdapter *;

  /// Test seam: @p hook runs on the executing thread after the transfer and
  /// before the post-transfer generation sample, so a test can mutate the
  /// file inside the window the double sample exists to detect.
  auto setAfterTransferHook(std::function<void()> hook) -> FileSourceAdapter *;

private:
  using StagedFileOp = FileOpSpec;

  [[nodiscard]] auto statFileGeneration() const noexcept
      -> std::optional<FileGeneration>;
  /// Stat and intern the current generation; takes `mutex_`.
  [[nodiscard]] auto observeGeneration() const -> std::optional<std::uint64_t>;
  /// Id of @p generation, minting one on first sight; caller holds mutex_.
  [[nodiscard]] auto
  internGenerationLocked(const FileGeneration &generation) const
      -> std::uint64_t;
  /// Next free auto stage key; caller holds mutex_.
  StageKey nextStageKeyLocked();
  /// Insert under a free @p key, charging write bytes; caller holds mutex_.
  [[nodiscard]] auto stageLocked(StageKey key, StagedFileOp op)
      -> std::expected<StageKey, std::string>;

  int fd_{-1};
  std::string path_;
  bool writable_{false};
  bool append_{false};

  mutable std::mutex mutex_;
  std::unordered_map<StageKey, StagedFileOp> staged_;
  StageKey nextStageKey_{};
  /// Sum of writeBytes held across staged_ (guarded by mutex_); the byte
  /// half of the staging bound the entry count alone cannot express.
  std::size_t stagedWriteBytes_{};

  /// Ordered by observationId so the retention cap evicts the oldest
  /// (smallest id) first -- ids mint sequentially from nextObservationId_.
  std::map<std::uint64_t, ReadObservation> observations_;
  std::uint64_t nextObservationId_{1};
  static constexpr std::size_t maxRetainedObservations_ = 4096;

  /// Interned generations (guarded by mutex_): exact value -> public id,
  /// with insertion order so the cap evicts the oldest. An evicted value
  /// that reappears gets a fresh id, which only ever makes a stale
  /// expectation fail, never pass.
  mutable std::map<FileGeneration, std::uint64_t> generationIds_;
  mutable std::deque<FileGeneration> generationOrder_;
  mutable std::uint64_t nextGenerationId_{1};
  static constexpr std::size_t maxInternedGenerations_ = 4096;

  std::function<void()> afterTransferHook_;

  std::unordered_map<LeaseId, DecodeLease> decodeLeases_;
  LeaseId nextDecodeLease_{1};
  std::size_t maxDecodeLeases_{256};
};

} // namespace xanadu::vortex

#endif
