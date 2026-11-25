#include "common/xanadu/vortex/io_actor_journal.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <span>

namespace xanadu::vortex {

namespace {

constexpr std::size_t kFixedBodyBytes = kActorJournalFixedBodyBytes;
constexpr std::size_t kSequenceOffset = 1;
constexpr std::size_t kChecksumOffset = kSequenceOffset + 8;
// The checksum covers every body byte except its own field.
constexpr std::size_t kFieldsOffset  = kChecksumOffset + 8;
constexpr std::size_t kArgsLenOffset = kActorJournalArgsLenOffset;
constexpr std::size_t kMaxKindValue =
    static_cast<std::size_t>(ActorIntentRecordKind::Checkpoint);

constexpr FramedLogFormat kFormat{kActorJournalSignature, kActorJournalVersion,
                                  kFixedBodyBytes};

struct DecodedRecord {
  ActorIntentRecord record;
  std::uint64_t sequence{};
};

/// Only an Intent carries immutable args; every other kind's body is
/// exactly the fixed fields.
bool carriesArgs(const std::uint8_t kind) {
  return kind == static_cast<std::uint8_t>(ActorIntentRecordKind::Intent);
}

std::vector<std::byte> encodeBody(const ActorIntentRecord &record,
                                  const std::uint64_t sequence) {
  std::vector<std::byte> body;
  body.reserve(kFixedBodyBytes + record.immutableArgs.size());
  appendValue(body, static_cast<std::uint8_t>(record.kind));
  appendValue(body, sequence);
  appendValue(body, std::uint64_t{0}); // checksum placeholder
  appendValue(body, record.actorGeneration);
  appendValue(body, record.turnOrdinal);
  appendValue(body, record.intentId);
  appendValue(body, record.endpointGeneration);
  appendValue(body, record.requestId);
  appendValue(body, record.floor);
  appendValue(body, static_cast<std::uint32_t>(record.immutableArgs.size()));
  body.insert(body.end(), record.immutableArgs.begin(),
              record.immutableArgs.end());
  const auto sum =
      fnv1a64(std::span<const std::byte>(body.data(), kChecksumOffset),
              std::span<const std::byte>(body.data() + kFieldsOffset,
                                         body.size() - kFieldsOffset));
  std::memcpy(body.data() + kChecksumOffset, &sum, sizeof(sum));
  return body;
}

/// Fully validate one record body and decode it: fixed minimum, known
/// kind, checksum, args length that matches the body exactly, and no args
/// on a kind that cannot carry them.
std::optional<DecodedRecord> decodeBody(const std::byte *body,
                                        const std::size_t bodyLen) {
  if (bodyLen < kFixedBodyBytes) return std::nullopt;
  std::uint8_t kind{};
  std::memcpy(&kind, body, 1);
  if (kind > kMaxKindValue) return std::nullopt;

  std::uint64_t storedChecksum{};
  std::memcpy(&storedChecksum, body + kChecksumOffset, sizeof(storedChecksum));
  const auto computed =
      fnv1a64(std::span<const std::byte>(body, kChecksumOffset),
              std::span<const std::byte>(body + kFieldsOffset,
                                         bodyLen - kFieldsOffset));
  if (computed != storedChecksum) return std::nullopt;

  std::uint32_t argsLen{};
  std::memcpy(&argsLen, body + kArgsLenOffset, sizeof(argsLen));
  if (kFixedBodyBytes + static_cast<std::size_t>(argsLen) != bodyLen)
    return std::nullopt;
  if (argsLen != 0 && !carriesArgs(kind)) return std::nullopt;

  DecodedRecord out;
  out.record.kind = static_cast<ActorIntentRecordKind>(kind);
  std::memcpy(&out.sequence, body + kSequenceOffset, sizeof(out.sequence));
  std::size_t offset = kFieldsOffset;
  const auto read    = [&](std::uint64_t &field) {
    std::memcpy(&field, body + offset, sizeof(field));
    offset += sizeof(field);
  };
  read(out.record.actorGeneration);
  read(out.record.turnOrdinal);
  read(out.record.intentId);
  read(out.record.endpointGeneration);
  read(out.record.requestId);
  read(out.record.floor);
  out.record.immutableArgs.assign(body + kFixedBodyBytes,
                                  body + kFixedBodyBytes + argsLen);
  return out;
}

/// Whether @p partial (the bytes present after a length prefix announcing
/// @p bodyLen) can be the start of one genuine, interrupted record: its
/// kind is known, the announced length is one that kind can have, its
/// sequence (once present) is the next expected, and an args length (once
/// present) agrees with the announced length. A checksum cannot be checked
/// on a prefix, so this is the strongest honest test; anything it rejects
/// is refused as corruption rather than cut.
bool tornTailPlausible(const std::span<const std::byte> partial,
                       const std::size_t bodyLen,
                       const std::uint64_t expectedSequence) {
  if (partial.empty()) return true;
  const auto kind = static_cast<std::uint8_t>(partial[0]);
  if (kind > kMaxKindValue) return false;
  if (!carriesArgs(kind) && bodyLen != kFixedBodyBytes) return false;
  if (partial.size() >= kSequenceOffset + sizeof(std::uint64_t)) {
    std::uint64_t sequence{};
    std::memcpy(&sequence, partial.data() + kSequenceOffset, sizeof(sequence));
    if (sequence != expectedSequence) return false;
  }
  if (carriesArgs(kind) &&
      partial.size() >= kArgsLenOffset + sizeof(std::uint32_t)) {
    std::uint32_t argsLen{};
    std::memcpy(&argsLen, partial.data() + kArgsLenOffset, sizeof(argsLen));
    if (kFixedBodyBytes + static_cast<std::size_t>(argsLen) != bodyLen)
      return false;
  }
  return true;
}

/// Scan an image with the shared framed-log walk; when @p out is set every
/// valid record is decoded into it. recover(), compact() and the
/// constructor's tail repair all come through here so they can never
/// disagree about what counts as corruption.
FramedLogScan scanImage(const std::span<const std::byte> bytes,
                        std::vector<ActorIntentRecord> *out,
                        std::uint64_t &nextSequence) {
  nextSequence = 1;
  return scanFramedLog(
      kFormat, bytes,
      [](const std::span<const std::byte> partial, const std::size_t bodyLen,
         const std::size_t ordinal) {
        return tornTailPlausible(partial, bodyLen, ordinal + 1);
      },
      [&](const std::byte *body, const std::size_t bodyLen,
          const std::size_t ordinal) {
        auto decoded = decodeBody(body, bodyLen);
        if (!decoded || decoded->sequence != ordinal + 1) return false;
        nextSequence = decoded->sequence + 1;
        if (out != nullptr) out->push_back(std::move(decoded->record));
        return true;
      });
}

constexpr std::size_t framedBytesFor(const std::size_t argsBytes) {
  return kFramedLogFrameBytes + kFixedBodyBytes + argsBytes;
}

} // namespace

ActorIoJournal::ActorIoJournal(std::filesystem::path path,
                               const std::optional<std::size_t> maxBytes)
    : log_(std::move(path), kFormat, maxBytes) {
  log_.open([this](const std::span<const std::byte> bytes) {
    return scanImage(bytes, nullptr, nextSequence_);
  });
}

ActorIoJournal::~ActorIoJournal() = default;

void ActorIoJournal::markQuiescent() noexcept { quiescent_ = true; }

auto ActorIoJournal::path() const -> const std::filesystem::path & {
  return log_.path();
}

auto ActorIoJournal::sizeBytes() const -> std::size_t {
  return log_.sizeBytes();
}

auto ActorIoJournal::reserve(const std::size_t recordBytes)
    -> std::expected<void, IoErrorInfo> {
  // Both on-disk length fields are u32: argsLen names immutableArgs and
  // bodyLen names the fixed record plus args. encodeBody() would truncate
  // either silently, so a record that cannot be represented is refused
  // here -- before appendRecord ever encodes.
  if (recordBytes >
      std::numeric_limits<std::uint32_t>::max() - kFixedBodyBytes) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  }
  const auto framed = framedBytesFor(recordBytes);
  auto fit          = log_.fits(framed);
  if (fit || fit.error().code != IoErrorCode::BudgetExceeded) return fit;
  // Full is recoverable exactly when a checkpoint has released records:
  // retire them and look again. A failed compaction leaves the journal
  // as it was, so the caller still gets the typed refusal.
  if (const auto reclaimed = compact(); !reclaimed || *reclaimed == 0) {
    return fit;
  }
  return log_.fits(framed);
}

auto ActorIoJournal::appendRecord(const ActorIntentRecord &record,
                                  const ActorJournalCrashPoints &crash)
    -> std::expected<void, IoErrorInfo> {
  if (log_.poisoned()) {
    // A previous append could not prove the file still ends on a record
    // boundary; refuse rather than deepen the tear.
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  // reserve() runs before encode: it refuses a record whose args cannot fit
  // the u32 length fields, so encodeBody never truncates a length.
  if (const auto ok = reserve(record.immutableArgs.size()); !ok) {
    return std::unexpected(ok.error());
  }
  const auto framed = frameBody(encodeBody(record, nextSequence_));
  // Sequence bookkeeping follows durability, ahead of the caller's own hook,
  // so a throwing afterFlush still leaves a durable record counted.
  const auto counted = [&] {
    ++nextSequence_;
    if (crash.afterFlush) crash.afterFlush();
  };
  return log_.append(framed, crash.beforeFlush, counted);
}

auto ActorIoJournal::compact() -> std::expected<std::size_t, IoErrorInfo> {
  return compactTo(nullptr);
}

auto ActorIoJournal::compactTo(const ActorIntentRecord *pending)
    -> std::expected<std::size_t, IoErrorInfo> {
  if (log_.poisoned()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  const auto image = log_.readImage();
  if (!image) return std::unexpected(image.error());
  if (!*image) return std::size_t{0};

  std::vector<ActorIntentRecord> records;
  std::uint64_t next{};
  const auto scan = scanImage(**image, &records, next);
  if (scan.corrupt) {
    return std::unexpected(
        IoErrorInfo{IoErrorCode::PersistenceRefusal,
                    static_cast<std::uint32_t>(scan.errorRecord)});
  }

  // A pending checkpoint is the floor being advanced right now; it joins
  // the candidates so the rewrite is also what makes it durable.
  const ActorIntentRecord *latest = pending;
  for (const auto &record : records) {
    if (record.kind == ActorIntentRecordKind::Checkpoint &&
        (latest == nullptr || record.floor > latest->floor)) {
      latest = &record;
    }
  }
  if (latest == nullptr) return std::size_t{0}; // nothing is retirable

  std::vector<const ActorIntentRecord *> kept{latest};
  for (const auto &record : records) {
    if (record.kind != ActorIntentRecordKind::Checkpoint &&
        record.intentId > latest->floor) {
      kept.push_back(&record);
    }
  }
  if (pending == nullptr && kept.size() == records.size()) {
    return std::size_t{0};
  }

  std::vector<std::byte> rewritten;
  std::uint64_t sequence = 1;
  for (const auto *record : kept) {
    const auto framed = frameBody(encodeBody(*record, sequence++));
    rewritten.insert(rewritten.end(), framed.begin(), framed.end());
  }
  const auto before = log_.sizeBytes();
  if (const auto ok = log_.replaceRecords(rewritten); !ok) {
    return std::unexpected(ok.error());
  }
  nextSequence_ = sequence;
  return before > log_.sizeBytes() ? before - log_.sizeBytes() : std::size_t{0};
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
  // The checkpoint is what releases records, so a journal full of unreleased
  // ones must not refuse it for want of room it is about to free: the
  // rewrite carries the new floor itself.
  if (const auto room = log_.fits(framedBytesFor(0));
      !room && room.error().code == IoErrorCode::BudgetExceeded) {
    if (const auto done = compactTo(&record); !done) {
      return std::unexpected(done.error());
    }
    if (crash.afterFlush) crash.afterFlush();
    return {};
  }
  return appendRecord(record, crash);
}

auto ActorIoJournal::recover() const -> ActorRestoreResult {
  if (!quiescent_) {
    return {ActorRestoreStatus::NotQuiescent, {}, {}};
  }
  if (log_.poisoned()) {
    // This instance already knows the file may not end on a record boundary:
    // bytes on disk may hide records behind what would parse as the
    // tolerated trailing remnant. Refuse rather than classify a prefix.
    return {ActorRestoreStatus::Corrupt, {}, {}};
  }
  const auto image = log_.readImage();
  // Any open or read failure other than "no file" means the contents cannot
  // be proven, which is Corrupt, never Clean over missing evidence.
  if (!image) return {ActorRestoreStatus::Corrupt, {}, {}};
  // Nothing was ever written: a quiesced, never-used journal is vacuously
  // clean.
  if (!*image || (*image)->empty()) {
    return {ActorRestoreStatus::Clean, {}, {}};
  }

  std::vector<ActorIntentRecord> records;
  std::uint64_t next{};
  const auto scan = scanImage(**image, &records, next);
  if (scan.corrupt) return {ActorRestoreStatus::Corrupt, {}, {}};

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
