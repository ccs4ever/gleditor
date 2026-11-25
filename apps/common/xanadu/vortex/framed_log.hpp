#ifndef XANADU_VORTEX_FRAMED_LOG_HPP
#define XANADU_VORTEX_FRAMED_LOG_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vortex/io_error.hpp"

// The one copy of the machinery the vortex I/O journals share: a signature
// and version header, then u32-length-framed records, appended with a single
// fsynced write and rolled back to the last proven boundary on failure. The
// capture journal (§8.2) and the actor journal (§9) differ only in what a
// record body holds and what makes a tail plausibly torn, so those two
// questions are callbacks and everything about bytes, descriptors and
// crash ordering lives here. A fix to torn-tail repair or directory
// durability is therefore made once.

namespace xanadu::vortex {

inline constexpr std::size_t kFramedLogSignatureBytes = 8;
inline constexpr std::size_t kFramedLogFrameBytes     = sizeof(std::uint32_t);
inline constexpr std::size_t kFramedLogHeaderBytes =
    kFramedLogSignatureBytes + sizeof(std::uint32_t);

/// What identifies a journal file and the smallest body a record can have.
struct FramedLogFormat {
  const char *signature{}; // kFramedLogSignatureBytes bytes
  std::uint32_t version{};
  std::size_t minBodyBytes{};
};

void appendBytes(std::vector<std::byte> &out, const void *data,
                 std::size_t size);

template <typename T>
void appendValue(std::vector<std::byte> &out, const T &value) {
  appendBytes(out, &value, sizeof(T));
}

/// FNV-1a-64 over the concatenation of two spans: a record checksum covers
/// every body byte except the checksum field, so the two sides are hashed
/// separately instead of copying the body to verify it. Integrity against
/// bit rot and torn writes only, never identity.
[[nodiscard]] auto fnv1a64(std::span<const std::byte> first,
                           std::span<const std::byte> second) noexcept
    -> std::uint64_t;

/// `bodyLen(u32) + body`. Callers bound the body to u32 first (`fits`).
[[nodiscard]] auto frameBody(std::span<const std::byte> body)
    -> std::vector<std::byte>;

[[nodiscard]] auto writeAll(int fd, const void *data, std::size_t size) -> bool;

/// Closes the descriptor on scope exit so a throwing crash hook cannot leak it.
struct FdGuard {
  int fd;
  FdGuard(const FdGuard &)                     = delete;
  auto operator=(const FdGuard &) -> FdGuard & = delete;
  explicit FdGuard(const int descriptor) noexcept : fd(descriptor) {}
  ~FdGuard();
};

/// fsync the directory holding @p path. A created file whose directory entry
/// was never synced can vanish whole after a crash, so a failure here is a
/// failure to make the journal durable and is reported, not swallowed.
[[nodiscard]] auto syncParentDirectory(const std::filesystem::path &path)
    -> bool;

/// Outcome of scanning a journal image.
struct FramedLogScan {
  /// Offset just past the last complete, valid record: where a crash-torn
  /// tail starts. Meaningful only when !corrupt.
  std::size_t boundary{};
  bool corrupt{};
  /// 1-based position of the failing record; 0 when the header failed.
  std::size_t errorRecord{};
};

/// Whether @p partial (the bytes present after a length prefix announcing
/// @p bodyLen, for the record with 0-based @p ordinal) can be the start of
/// one genuine, interrupted record. A checksum cannot be checked on a
/// prefix, so this is the strongest honest test; what it rejects is refused
/// as corruption rather than cut, because cutting at a corrupt mid-file
/// length silently deletes every valid record after it.
using TornTailCheck = gleditor::cpp26::function_ref<bool(
    std::span<const std::byte> partial, std::size_t bodyLen,
    std::size_t ordinal)>;
/// Whether the complete body of the 0-based @p ordinal record is valid.
using RecordCheck = gleditor::cpp26::function_ref<bool(
    const std::byte *body, std::size_t bodyLen, std::size_t ordinal)>;

/// Verify the header, then walk records: a body shorter than the format's
/// minimum, a failed @p record check or a mismatched header is corruption; a
/// trailing incomplete record is a tear only if @p torn accepts what is
/// present. Shared by the constructor-time repair and recover() so the two
/// can never disagree about what counts as corruption.
[[nodiscard]] auto scanFramedLog(const FramedLogFormat &format,
                                 std::span<const std::byte> bytes,
                                 TornTailCheck torn, RecordCheck record)
    -> FramedLogScan;

using FramedLogScanFn =
    gleditor::cpp26::function_ref<FramedLogScan(std::span<const std::byte>)>;

/// The file half of a journal: classification of what is on disk at
/// construction, torn header/tail repair, bounded fsynced appends with
/// rollback, atomic whole-file replacement, and the poisoned state in which
/// the append boundary cannot be proven and every append refuses.
class FramedLog {
public:
  /// @p maxBytes bounds the file; std::nullopt is the explicit unbounded
  /// choice and 0 holds nothing.
  FramedLog(std::filesystem::path path, FramedLogFormat format,
            std::optional<std::size_t> maxBytes);

  /// Constructor-time pass: classify the file, cut a header remnant that is
  /// the start of our own header, and cut a tail @p scan proves torn. Poisons
  /// when the boundary cannot be proven or repaired. Also removes a stale
  /// replacement temp file left by a crash before its rename.
  auto open(FramedLogScanFn scan) -> FramedLog *;

  [[nodiscard]] auto path() const noexcept -> const std::filesystem::path & {
    return path_;
  }
  [[nodiscard]] auto sizeBytes() const noexcept -> std::size_t {
    return writtenBytes_;
  }
  [[nodiscard]] auto wroteHeader() const noexcept -> bool {
    return wroteHeader_;
  }
  [[nodiscard]] auto poisoned() const noexcept -> bool { return poisoned_; }
  /// The failing record's 1-based position when poisoning came from a corrupt
  /// scan; 0 otherwise.
  [[nodiscard]] auto poisonDetail() const noexcept -> std::uint32_t {
    return poisonDetail_;
  }
  [[nodiscard]] auto maxBytes() const noexcept
      -> const std::optional<std::size_t> & {
    return maxBytes_;
  }

  /// Whether @p framedBytes more fit on top of what is written plus
  /// @p heldBytes the owner promised elsewhere. BudgetExceeded when not;
  /// InvalidRequest when the frame cannot be represented in u32.
  [[nodiscard]] auto fits(std::size_t framedBytes,
                          std::size_t heldBytes = 0) const
      -> std::expected<void, IoErrorInfo>;

  /// Append one already-framed record. The first record travels with the
  /// header in one write, so a crash leaves at worst a header prefix. Hooks
  /// are crash-injection seams: @p beforeFlush throws before fsync (the
  /// record is rolled back), @p afterFlush after the bookkeeping (the record
  /// is durable and counted). A creation whose directory entry cannot be
  /// synced is rolled back and refused.
  [[nodiscard]] auto append(std::span<const std::byte> framed,
                            const std::function<void()> &beforeFlush,
                            const std::function<void()> &afterFlush)
      -> std::expected<void, IoErrorInfo>;

  /// The whole file, or std::nullopt when it does not exist; a file that
  /// exists but cannot be read is a PersistenceRefusal, never "empty".
  [[nodiscard]] auto readImage() const
      -> std::expected<std::optional<std::vector<std::byte>>, IoErrorInfo>;

  /// What a replacement that reached its rename reports.
  struct Replacement {
    /// Whether the directory sync that makes the new name durable was
    /// acknowledged. When false the journal on disk (and in this object's
    /// bookkeeping) IS the new one, but a crash before the entry reaches
    /// stable storage may bring the previous file back: still a complete,
    /// self-consistent journal, so recovery stays sound, yet retirements
    /// and records appended since may be lost. The caller must adopt the new
    /// image's state either way and treat the retirement as not durable.
    bool directoryDurable{};
  };

  /// Atomically replace the file's records with @p framedRecords (header
  /// added): write a temp file, fsync it, rename over the journal, fsync the
  /// directory. A crash before the rename leaves the original intact. An
  /// error means nothing changed; a value means the rename happened.
  [[nodiscard]] auto replaceRecords(std::span<const std::byte> framedRecords)
      -> std::expected<Replacement, IoErrorInfo>;

private:
  void repairTornHeader();
  void repairTornTail(FramedLogScanFn scan);
  [[nodiscard]] auto replacementPath() const -> std::filesystem::path;

  std::filesystem::path path_;
  FramedLogFormat format_;
  std::optional<std::size_t> maxBytes_;
  /// Atomic because a journal's budget check reads it on the owner while a
  /// worker's append is publishing a new length; both are relaxed -- a
  /// slightly stale size only makes the budget check momentarily pessimistic.
  std::atomic<std::size_t> writtenBytes_{};
  std::atomic<bool> wroteHeader_{};
  /// Set when the append boundary cannot be proven: a failed append's
  /// rollback could not be proven, the constructor found a file it cannot
  /// classify or repair, or the file is too short to ever hold a header.
  /// Every later append refuses and recovery reports Corrupt rather than
  /// risk parsing records hidden behind a tolerated trailing remnant.
  bool poisoned_{};
  std::uint32_t poisonDetail_{};
};

} // namespace xanadu::vortex

#endif
