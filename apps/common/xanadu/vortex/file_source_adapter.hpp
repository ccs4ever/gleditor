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

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"

namespace xanadu::vortex {

/// Declared capability a source endpoint offers for a read's consistency
/// requirement. `Live` is the only thing a plain mutable file can promise:
/// bytes observed are exactly what `pread` returned for one generation, with
/// no claim that the file did not change around that call. `Stable` requires
/// a backend-proven immutable snapshot (e.g. a content-addressed object);
/// `FileSourceAdapter` never claims it for an ordinary path.
enum class SourceConsistency : std::uint8_t { Live, Stable };

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
struct ReadObservation {
  std::uint64_t observationId{};
  std::uint64_t sourceGeneration{};
  MediaRange range;
  SourceConsistency consistency{SourceConsistency::Live};
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
  std::uint64_t sourceGeneration{};
};

/// Declared endpoint capabilities returned by `open` (plan §7.2: "a locator
/// plus declared capabilities, never an immutable identity").
struct FileOpenCapabilities {
  std::string locator;
  SourceConsistency consistency{SourceConsistency::Live};
  std::uint64_t initialGeneration{};
  std::uint64_t sizeAtOpen{};
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
  /// expected at staging time (0 disables the check, matching
  /// `StreamOpSpec::expectedGeneration`'s convention). Returns the key to
  /// place in the request's `specification` cell.
  [[nodiscard]] std::uint32_t stageRead(MediaRange range,
                                        std::uint64_t expectedGeneration);
  /// Stage a bounded write at a checked 64-bit offset. `Stable` sources never
  /// reach here (open already refused); writable `Live` files accept it.
  [[nodiscard]] std::uint32_t stageWrite(std::uint64_t offset,
                                         std::vector<std::byte> bytes,
                                         std::uint64_t expectedGeneration);
  /// Stage a strict-snapshot request. Always refused at `prepare` time with
  /// `Unsupported` -- a plain POSIX file is never a backend-proven immutable
  /// source -- but staged like any other operation so the refusal flows
  /// through the normal admission path instead of being special-cased by
  /// callers.
  [[nodiscard]] std::uint32_t stageStableRead(MediaRange range);

  /// Owner-thread: drop a staged op that will never reach `execute` (the
  /// submission was refused, or the caller abandoned it). The staging
  /// table is bounded at 4096 live ops; `stage*` returns 0 when full and a
  /// request staged under key 0 then fails `prepare` as InvalidRequest.
  void abandon(std::uint32_t key);

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

  /// Current `stat`-derived generation; bumps whenever the adapter observes
  /// (via `stat` taken around a transfer) that size or mtime moved, whether
  /// by this adapter's own write or an external writer.
  [[nodiscard]] std::uint64_t currentGeneration() const noexcept;

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

  void setMaxDecodeLeases(std::size_t value) noexcept;

private:
  struct StagedFileOp {
    enum class Kind : std::uint8_t { Read, Write, StableRead } kind{};
    MediaRange range;
    std::vector<std::byte> writeBytes;
    std::uint64_t expectedGeneration{};
  };

  [[nodiscard]] std::uint64_t statGeneration() const noexcept;
  /// Next free auto stage key; caller holds mutex_.
  std::uint32_t nextStageKeyLocked();

  int fd_{-1};
  std::string path_;
  bool writable_{false};

  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, StagedFileOp> staged_;
  std::uint32_t nextStageKey_{1};
  /// Sum of writeBytes held across staged_ (guarded by mutex_); the byte
  /// half of the staging bound the entry count alone cannot express.
  std::size_t stagedWriteBytes_{};

  /// Ordered by observationId so the retention cap evicts the oldest
  /// (smallest id) first -- ids mint sequentially from nextObservationId_.
  std::map<std::uint64_t, ReadObservation> observations_;
  std::uint64_t nextObservationId_{1};
  static constexpr std::size_t maxRetainedObservations_ = 4096;

  std::unordered_map<LeaseId, DecodeLease> decodeLeases_;
  LeaseId nextDecodeLease_{1};
  std::size_t maxDecodeLeases_{256};
};

} // namespace xanadu::vortex

#endif
