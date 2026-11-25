#include "common/xanadu/vortex/io_actor_journal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
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

} // namespace

ActorIoJournal::ActorIoJournal(std::filesystem::path path,
                               const std::size_t maxBytes)
    : path_(std::move(path)), maxBytes_(maxBytes) {
  if (std::filesystem::exists(path_)) {
    writtenBytes_ = static_cast<std::size_t>(std::filesystem::file_size(path_));
    wroteHeader_  = writtenBytes_ >= kHeaderBytes;
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
  const auto framed = encode(record);
  if (const auto ok = reserve(record.immutableArgs.size()); !ok) {
    return std::unexpected(ok.error());
  }

  // writtenBytes_ is the offset of the last byte this journal knows is a
  // complete record (the header counts once flushed): the boundary a
  // failed append truncates back to.
  const auto recordStart = static_cast<off_t>(writtenBytes_);

  const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }

  const auto failAppend = [&]() -> std::expected<void, IoErrorInfo> {
    // recover() tolerates exactly one trailing, incomplete record, so a
    // torn tail followed by later appends would hide those records and
    // could report Clean over a durable admission. Roll the file back to
    // the last known boundary; when even that cannot be proven the
    // journal is poisoned and every later append refuses.
    if (::ftruncate(fd, recordStart) == 0) {
      writtenBytes_ = static_cast<std::size_t>(recordStart);
    } else {
      poisoned_ = true;
      struct stat st{};
      if (::fstat(fd, &st) == 0 && st.st_size >= 0) {
        writtenBytes_ = static_cast<std::size_t>(st.st_size);
      }
    }
    ::close(fd);
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

  if (crash.beforeFlush) crash.beforeFlush();
  while (::fsync(fd) != 0) {
    if (errno != EINTR) return failAppend();
  }
  if (crash.afterFlush) crash.afterFlush();
  ::close(fd);

  if (!wroteHeader_) {
    writtenBytes_ += kHeaderBytes;
    wroteHeader_ = true;
  }
  writtenBytes_ += framed.size();
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
      // Nothing was ever written: a quiesced, never-used journal is
      // vacuously clean.
      return {ActorRestoreStatus::Clean, {}, {}};
    }
    char buffer[4096];
    for (;;) {
      const auto n = ::read(fd, buffer, sizeof(buffer));
      if (n < 0) {
        if (errno == EINTR) continue;
        ::close(fd);
        return {ActorRestoreStatus::Corrupt, {}, {}};
      }
      if (n == 0) break;
      appendBytes(bytes, buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
  }

  if (bytes.empty()) return {ActorRestoreStatus::Clean, {}, {}};
  if (bytes.size() < kHeaderBytes) {
    return {ActorRestoreStatus::Corrupt, {}, {}};
  }
  if (std::memcmp(bytes.data(), kActorJournalSignature,
                  sizeof(kActorJournalSignature)) != 0) {
    return {ActorRestoreStatus::Corrupt, {}, {}};
  }
  std::uint32_t version{};
  std::memcpy(&version, bytes.data() + sizeof(kActorJournalSignature),
              sizeof(version));
  if (version != kActorJournalVersion) {
    return {ActorRestoreStatus::Corrupt, {}, {}};
  }

  std::vector<ActorIntentRecord> records;
  std::size_t cursor = kHeaderBytes;
  while (cursor + 4 <= bytes.size()) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    if (bodyLen < kFixedRecordBytes) {
      // The encoder never emits a body this small, and a crash mid-flush
      // can only truncate -- it cannot invent a short length. A fully
      // present "record" this malformed is foreign bytes mid-stream:
      // corruption, not the tolerated trailing remnant.
      return {ActorRestoreStatus::Corrupt, {}, {}};
    }
    const std::size_t recordEnd = cursor + 4 + bodyLen;
    if (recordEnd > bytes.size()) {
      // A crash mid-flush leaves exactly one trailing, incomplete
      // record: tolerate it as the last thing on disk, not corruption.
      break;
    }
    const std::byte *body = bytes.data() + cursor + 4;
    ActorIntentRecord record;
    std::size_t offset = 0;
    std::uint8_t kind{};
    std::memcpy(&kind, body + offset, 1);
    offset += 1;
    if (kind > static_cast<std::uint8_t>(ActorIntentRecordKind::Checkpoint)) {
      // Every record this format writes carries a kind in range; anything
      // else means the "record" is not one the journal produced.
      return {ActorRestoreStatus::Corrupt, {}, {}};
    }
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
    if (offset + argsLen != static_cast<std::size_t>(bodyLen)) {
      // A complete-looking record whose args length disagrees with its
      // body length is not a journal record; a real torn write truncates
      // instead of producing this shape, so this is corruption.
      return {ActorRestoreStatus::Corrupt, {}, {}};
    }
    record.immutableArgs.assign(body + offset, body + offset + argsLen);
    records.push_back(std::move(record));
    cursor = recordEnd;
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
