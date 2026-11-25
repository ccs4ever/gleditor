#include "common/xanadu/vortex/io_actor_journal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <span>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace xanadu::vortex {

namespace {

constexpr std::size_t kHeaderBytes =
    sizeof(kActorJournalSignature) + sizeof(std::uint32_t);
// kind(1) + 6 uint64 fields + argsLen(4)
constexpr std::size_t kFixedRecordBytes = 1 + 6 * 8 + 4;

void appendBytes(std::vector<std::byte> &out, const void *data,
                 const std::size_t size) {
  const auto *bytes = static_cast<const std::byte *>(data);
  out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
void appendValue(std::vector<std::byte> &out, const T &value) {
  appendBytes(out, &value, sizeof(T));
}

std::vector<std::byte> encode(const ActorIntentRecord &record) {
  std::vector<std::byte> body;
  body.reserve(kFixedRecordBytes + record.immutableArgs.size());
  appendValue(body, static_cast<std::uint8_t>(record.kind));
  appendValue(body, record.actorGeneration);
  appendValue(body, record.turnOrdinal);
  appendValue(body, record.intentId);
  appendValue(body, record.endpointGeneration);
  appendValue(body, record.requestId);
  appendValue(body, record.floor);
  appendValue(body, static_cast<std::uint32_t>(record.immutableArgs.size()));
  body.insert(body.end(), record.immutableArgs.begin(),
              record.immutableArgs.end());
  std::vector<std::byte> framed;
  framed.reserve(body.size() + 4);
  appendValue(framed, static_cast<std::uint32_t>(body.size()));
  framed.insert(framed.end(), body.begin(), body.end());
  return framed;
}

bool writeAll(const int fd, const void *data, const std::size_t size) {
  const auto *cursor    = static_cast<const char *>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const auto written = ::write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

/// Scan a journal image for the offset just past its last complete,
/// well-formed record -- the boundary a crash-torn tail starts at.
/// Returns std::nullopt when the bytes cannot be proven to be a journal
/// prefix: a short file, a signature/version mismatch, or a fully-present
/// malformed record (a body too small for the fixed fields, a kind the
/// format never writes, or an args length that disagrees with the body
/// length). A trailing *incomplete* record is a tear, not corruption: the
/// scan stops at its start and reports that offset. recover() and the
/// constructor's tail repair share this so the two can never disagree
/// about what counts as corruption.
std::optional<std::size_t> journalPrefixEnd(std::span<const std::byte> bytes) {
  if (bytes.size() < kHeaderBytes) return std::nullopt;
  if (std::memcmp(bytes.data(), kActorJournalSignature,
                  sizeof(kActorJournalSignature)) != 0)
    return std::nullopt;
  std::uint32_t version{};
  std::memcpy(&version, bytes.data() + sizeof(kActorJournalSignature),
              sizeof(version));
  if (version != kActorJournalVersion) return std::nullopt;

  std::size_t cursor = kHeaderBytes;
  while (cursor + 4 <= bytes.size()) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    if (bodyLen < kFixedRecordBytes) return std::nullopt;
    // recordEnd may run past the file: that is the tolerated tear. The
    // subtraction form avoids any overflow on the sum.
    if (bodyLen > bytes.size() - cursor - 4) break;
    const std::byte *body = bytes.data() + cursor + 4;
    std::uint8_t kind{};
    std::memcpy(&kind, body, 1);
    if (kind > static_cast<std::uint8_t>(ActorIntentRecordKind::Checkpoint))
      return std::nullopt;
    // argsLen is the last u32 of the fixed record body.
    std::uint32_t argsLen{};
    std::memcpy(&argsLen, body + kFixedRecordBytes - sizeof(argsLen),
                sizeof(argsLen));
    if (static_cast<std::size_t>(kFixedRecordBytes) + argsLen !=
        static_cast<std::size_t>(bodyLen))
      return std::nullopt;
    cursor += 4 + bodyLen;
  }
  return cursor;
}

/// Closes the descriptor on scope exit. appendRecord's crash hooks may
/// throw to simulate the process dying mid-operation; without this guard
/// a throwing hook leaks the fd.
struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

} // namespace

ActorIoJournal::ActorIoJournal(std::filesystem::path path,
                               const std::size_t maxBytes)
    : path_(std::move(path)), maxBytes_(maxBytes) {
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    // Absent is the only clean "fresh" state. An exists() error means the
    // path cannot even be classified, so the append boundary cannot be
    // proven -- refuse appends rather than land them on a guess.
    poisoned_ = static_cast<bool>(ec);
    return;
  }
  const auto size = std::filesystem::file_size(path_, ec);
  if (ec) {
    // Exists but cannot be sized (a directory, a permission wall): same
    // unprovable-boundary refusal.
    poisoned_ = true;
    return;
  }
  writtenBytes_ = static_cast<std::size_t>(size);
  if (writtenBytes_ == 0) return; // empty: the first append writes header
  wroteHeader_ = writtenBytes_ >= kHeaderBytes;
  if (writtenBytes_ < kHeaderBytes) {
    // 1..kHeaderBytes-1 bytes can never become a journal: the first
    // append would O_APPEND its header *behind* them and every later
    // recover() would read a signature mismatch while appends reported
    // success. Poison now; recover() reports the same Corrupt on its own
    // read of the same bytes.
    poisoned_ = true;
    return;
  }
  repairTornTail();
}

void ActorIoJournal::repairTornTail() {
  // A crash between writeAll and fsync leaves a torn tail: bytes after
  // the last complete record that recover() tolerates as the trailing
  // remnant. O_APPEND would land every later record behind it, hiding
  // them from recover()'s prefix parse, so cut the file back to the last
  // valid boundary before this instance appends anything. A tail the
  // scan cannot prove torn-vs-corrupt -- or one it cannot cut -- poisons
  // the journal: appends refuse and recover() reports Corrupt.
  int fd = ::open(path_.c_str(), O_RDWR);
  if (fd < 0) fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    poisoned_ = true;
    return;
  }
  // Every exit -- a read error, an unprovable boundary, a refused
  // ftruncate, or a throwing appendBytes allocation -- closes the
  // descriptor exactly once through this guard, the same discipline
  // appendRecord keeps.
  const FdGuard guard{fd};

  std::vector<std::byte> bytes;
  char buffer[4096];
  bool readOk = true;
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) continue;
      readOk = false;
      break;
    }
    if (n == 0) break;
    appendBytes(bytes, buffer, static_cast<std::size_t>(n));
  }
  if (!readOk) {
    poisoned_ = true;
    return;
  }

  const auto boundary = journalPrefixEnd(bytes);
  if (!boundary) {
    poisoned_ = true;
    return;
  }
  writtenBytes_ = *boundary;
  wroteHeader_  = true; // the scan proved the header bytes are ours
  if (*boundary == bytes.size()) {
    return; // no tear to cut
  }
  // ftruncate needs a writable fd; an O_RDONLY scan fd fails here and
  // poisons, which is correct: a proven tear that cannot be cut still
  // cannot accept appends.
  if (::ftruncate(fd, static_cast<off_t>(*boundary)) != 0) {
    poisoned_ = true;
    return;
  }
}

ActorIoJournal::~ActorIoJournal() = default;

void ActorIoJournal::markQuiescent() noexcept { quiescent_ = true; }

auto ActorIoJournal::path() const -> const std::filesystem::path & {
  return path_;
}

auto ActorIoJournal::sizeBytes() const -> std::size_t { return writtenBytes_; }

auto ActorIoJournal::reserve(const std::size_t recordBytes) const
    -> std::expected<void, IoErrorInfo> {
  // Both on-disk length fields are u32: argsLen names immutableArgs and
  // bodyLen names the fixed record plus args. encode() would truncate
  // either silently, so a record that cannot be represented is refused
  // here -- before appendRecord ever calls encode().
  if (recordBytes >
      std::numeric_limits<std::uint32_t>::max() - kFixedRecordBytes) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  }
  const std::size_t header = wroteHeader_ ? 0 : kHeaderBytes;
  const std::size_t framed = 4 + kFixedRecordBytes + recordBytes;
  if (maxBytes_ != 0 && writtenBytes_ + header + framed > maxBytes_) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 0});
  }
  return {};
}

auto ActorIoJournal::appendRecord(const ActorIntentRecord &record,
                                  const ActorJournalCrashPoints &crash)
    -> std::expected<void, IoErrorInfo> {
  if (poisoned_) {
    // A previous append could not prove the file still ends on a record
    // boundary; bytes written now would land behind a tail recover()
    // reads as the tolerated trailing remnant while hiding every record
    // after it. Refuse rather than deepen the tear.
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  // reserve() runs before encode(): it refuses a record whose args cannot
  // fit the u32 length fields, so encode() never truncates a length.
  if (const auto ok = reserve(record.immutableArgs.size()); !ok) {
    return std::unexpected(ok.error());
  }
  const auto framed = encode(record);

  // writtenBytes_ is the offset of the last byte this journal knows is a
  // complete record (the header counts once flushed): the boundary a
  // failed append truncates back to.
  const auto recordStart = static_cast<off_t>(writtenBytes_);

  const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  // Every exit -- write failure, fsync failure, a throwing crash hook --
  // closes the descriptor exactly once through this guard.
  const FdGuard guard{fd};

  const auto rollbackTo = [&](const off_t target) {
    // recover() tolerates exactly one trailing, incomplete record, so a
    // torn tail followed by later appends would hide those records and
    // could report Clean over a durable admission. Roll the file back to
    // the last known boundary; when even that cannot be proven the
    // journal is poisoned and every later append refuses.
    if (::ftruncate(fd, target) == 0) {
      writtenBytes_ = static_cast<std::size_t>(target);
    } else {
      poisoned_ = true;
      struct stat st{};
      if (::fstat(fd, &st) == 0 && st.st_size >= 0) {
        writtenBytes_ = static_cast<std::size_t>(st.st_size);
      }
    }
  };
  const auto failAppend = [&]() -> std::expected<void, IoErrorInfo> {
    rollbackTo(recordStart);
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  };

  bool ok = true;
  if (!wroteHeader_) {
    ok = ok &&
         writeAll(fd, kActorJournalSignature, sizeof(kActorJournalSignature));
    ok =
        ok && writeAll(fd, &kActorJournalVersion, sizeof(kActorJournalVersion));
  }
  ok = ok && writeAll(fd, framed.data(), framed.size());

  if (!ok) return failAppend();

  if (crash.beforeFlush) {
    try {
      crash.beforeFlush();
    } catch (...) {
      // Simulated death before fsync: the record was written but never
      // durably flushed, so roll it back to the last known boundary
      // exactly like a failed write. Left in place, the un-fsynced bytes
      // would sit mid-file and hide every later append behind what
      // recover() reads as the tolerated trailing remnant.
      rollbackTo(recordStart);
      throw;
    }
  }
  while (::fsync(fd) != 0) {
    if (errno != EINTR) return failAppend();
  }
  // fsync acknowledged: the record IS durable. Bookkeeping must reflect
  // that BEFORE afterFlush can throw -- otherwise a throwing afterFlush
  // leaves writtenBytes_ behind a record recover() already counts, and a
  // later failAppend ftruncate could erase durable bytes.
  if (!wroteHeader_) {
    writtenBytes_ += kHeaderBytes;
    wroteHeader_ = true;
  }
  writtenBytes_ += framed.size();
  // A throwing afterFlush simulates death after durability: the record
  // stays counted and the guard closes the fd during unwind.
  if (crash.afterFlush) crash.afterFlush();
  return {};
}

auto ActorIoJournal::stageIntent(ActorIntentRecord intent,
                                 const ActorJournalCrashPoints &crash)
    -> std::expected<void, IoErrorInfo> {
  intent.kind = ActorIntentRecordKind::Intent;
  return appendRecord(intent, crash);
}

auto ActorIoJournal::recordAdmission(const std::uint64_t intentId,
                                     const std::uint64_t endpointGeneration,
                                     const std::uint64_t requestId,
                                     const ActorJournalCrashPoints &crash)
    -> std::expected<void, IoErrorInfo> {
  if (crash.beforeDispatch) crash.beforeDispatch();
  ActorIntentRecord record{.kind     = ActorIntentRecordKind::Admission,
                           .intentId = intentId,
                           .endpointGeneration = endpointGeneration,
                           .requestId          = requestId};
  return appendRecord(record, crash);
}

auto ActorIoJournal::recordReceipt(const std::uint64_t intentId,
                                   const ActorJournalCrashPoints &crash)
    -> std::expected<void, IoErrorInfo> {
  ActorIntentRecord record{.kind     = ActorIntentRecordKind::Receipt,
                           .intentId = intentId};
  auto result = appendRecord(record, crash);
  if (result && crash.afterReceipt) crash.afterReceipt();
  return result;
}

auto ActorIoJournal::recordUnresolved(const std::uint64_t intentId)
    -> std::expected<void, IoErrorInfo> {
  ActorIntentRecord record{.kind     = ActorIntentRecordKind::Unresolved,
                           .intentId = intentId};
  return appendRecord(record, {});
}

auto ActorIoJournal::checkpoint(const std::uint64_t floor,
                                const ActorJournalCrashPoints &crash)
    -> std::expected<void, IoErrorInfo> {
  if (crash.beforeCheckpoint) crash.beforeCheckpoint();
  ActorIntentRecord record{.kind  = ActorIntentRecordKind::Checkpoint,
                           .floor = floor};
  return appendRecord(record, crash);
}

auto ActorIoJournal::recover() const -> ActorRestoreResult {
  if (!quiescent_) {
    return {ActorRestoreStatus::NotQuiescent, {}, {}};
  }
  if (poisoned_) {
    // This instance already knows the file may not end on a record
    // boundary: a failed append's rollback could not be proven, so bytes
    // on disk may hide records behind what would parse as the tolerated
    // trailing remnant. Refuse rather than classify a prefix.
    return {ActorRestoreStatus::Corrupt, {}, {}};
  }

  std::vector<std::byte> bytes;
  {
    const int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT) {
        // Nothing was ever written: a quiesced, never-used journal is
        // vacuously clean.
        return {ActorRestoreStatus::Clean, {}, {}};
      }
      // Any other open failure means the contents cannot be proven --
      // EACCES/EMFILE/EIO/ENOTDIR all read as "cannot examine", never as
      // "empty". Report Corrupt rather than Clean over missing evidence.
      return {ActorRestoreStatus::Corrupt, {}, {}};
    }
    // A throwing appendBytes allocation mid-scan must not leak the
    // descriptor; the guard closes it on every exit path.
    const FdGuard guard{fd};
    char buffer[4096];
    for (;;) {
      const auto n = ::read(fd, buffer, sizeof(buffer));
      if (n < 0) {
        if (errno == EINTR) continue;
        return {ActorRestoreStatus::Corrupt, {}, {}};
      }
      if (n == 0) break;
      appendBytes(bytes, buffer, static_cast<std::size_t>(n));
    }
  }

  if (bytes.empty()) return {ActorRestoreStatus::Clean, {}, {}};
  // journalPrefixEnd proves the header and walks the record section:
  // std::nullopt is a signature/version mismatch or a fully-present
  // malformed record (corruption); a returned boundary stops at the
  // tolerated trailing tear, so bytes past it are never parsed. This is
  // the same scan the constructor's tail repair uses, so recovery and
  // append-time repair can never disagree about what counts as a tear.
  const auto boundary = journalPrefixEnd(bytes);
  if (!boundary) {
    return {ActorRestoreStatus::Corrupt, {}, {}};
  }

  // Every record in [kHeaderBytes, *boundary) is complete and
  // well-formed (the scan proved it), so decode is a straight field read.
  std::vector<ActorIntentRecord> records;
  std::size_t cursor = kHeaderBytes;
  while (cursor < *boundary) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    const std::byte *body = bytes.data() + cursor + 4;
    ActorIntentRecord record;
    std::size_t offset = 0;
    std::uint8_t kind{};
    std::memcpy(&kind, body + offset, 1);
    offset += 1;
    record.kind = static_cast<ActorIntentRecordKind>(kind);
    std::memcpy(&record.actorGeneration, body + offset, 8);
    offset += 8;
    std::memcpy(&record.turnOrdinal, body + offset, 8);
    offset += 8;
    std::memcpy(&record.intentId, body + offset, 8);
    offset += 8;
    std::memcpy(&record.endpointGeneration, body + offset, 8);
    offset += 8;
    std::memcpy(&record.requestId, body + offset, 8);
    offset += 8;
    std::memcpy(&record.floor, body + offset, 8);
    offset += 8;
    std::uint32_t argsLen{};
    std::memcpy(&argsLen, body + offset, 4);
    offset += 4;
    record.immutableArgs.assign(body + offset, body + offset + argsLen);
    records.push_back(std::move(record));
    cursor += 4 + bodyLen;
  }

  std::uint64_t checkpointFloor = 0;
  bool haveCheckpoint           = false;
  std::map<std::uint64_t, ActorIntentRecordKind> lastStage;
  for (const auto &record : records) {
    if (record.kind == ActorIntentRecordKind::Checkpoint) {
      checkpointFloor = std::max(checkpointFloor, record.floor);
      haveCheckpoint  = true;
      continue;
    }
    lastStage[record.intentId] = record.kind;
  }

  std::vector<std::uint64_t> unknown;
  for (const auto &[intentId, stage] : lastStage) {
    const bool coveredByCheckpoint =
        haveCheckpoint && intentId <= checkpointFloor;
    if (coveredByCheckpoint) continue;
    if (stage == ActorIntentRecordKind::Admission ||
        stage == ActorIntentRecordKind::Unresolved) {
      // §9: "Crash after durable admission with no conclusive receipt
      // returns Unknown." An explicit Unresolved record is the same
      // finding recorded without waiting for a crash.
      unknown.push_back(intentId);
    }
  }

  if (!unknown.empty()) {
    return {ActorRestoreStatus::Unknown, std::move(records),
            std::move(unknown)};
  }
  return {ActorRestoreStatus::Clean, std::move(records), {}};
}

} // namespace xanadu::vortex
