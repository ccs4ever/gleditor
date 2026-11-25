#include "common/xanadu/vortex/capture_journal.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <random>
#include <span>

namespace xanadu::vortex {

namespace {

constexpr std::size_t kHeaderBytes      = kCaptureJournalHeaderBytes;
constexpr std::size_t kFixedBodyBytes   = kCaptureJournalFixedBodyBytes;
constexpr std::size_t kChecksumOffset   = 1 + 8 + 8;
constexpr std::size_t kIntervalBytes    = kCaptureJournalIntervalBytes;
constexpr std::size_t kAppendFixedBytes = kCaptureJournalAppendFixedBytes;
constexpr std::size_t kSpanBytes        = kCaptureJournalSpanBytes;
constexpr std::size_t kAbortedBytes     = kCaptureJournalAbortedBytes;
constexpr std::size_t kReconciledBytes  = kCaptureJournalReconciledBytes;
constexpr std::size_t kIdentityBytes    = kCaptureJournalIdentityBytes;
// Where intervalCount sits in an Append/Appended body: after the fixed
// header, the digest and the append length.
constexpr std::size_t kIntervalCountOffset = kFixedBodyBytes + 32 + 8;
constexpr std::size_t kMaxKindValue =
    static_cast<std::size_t>(CaptureJournalRecordKind::Identity);

/// The kind-specific payload size an honest encode produces, or
/// std::nullopt when the record cannot be represented (interval count
/// overflow). encode/decode/scan share this so the three can never
/// disagree about a record's length.
std::optional<std::size_t> payloadBytesFor(const CaptureJournalRecordKind kind,
                                           const std::size_t intervalCount) {
  switch (kind) {
  case CaptureJournalRecordKind::Append:
    if (intervalCount >
        (std::numeric_limits<std::uint32_t>::max() - kAppendFixedBytes) /
            kIntervalBytes)
      return std::nullopt;
    return kAppendFixedBytes + intervalCount * kIntervalBytes;
  case CaptureJournalRecordKind::Appended:
    if (intervalCount > (std::numeric_limits<std::uint32_t>::max() -
                         kAppendFixedBytes - kSpanBytes) /
                            kIntervalBytes)
      return std::nullopt;
    return kAppendFixedBytes + intervalCount * kIntervalBytes + kSpanBytes;
  case CaptureJournalRecordKind::Committed:
    return std::size_t{0};
  case CaptureJournalRecordKind::Aborted:
    return kAbortedBytes;
  case CaptureJournalRecordKind::Reconciled:
    return kReconciledBytes;
  case CaptureJournalRecordKind::Identity:
    return kIdentityBytes;
  }
  return std::nullopt;
}

void appendInterval(std::vector<std::byte> &out,
                    const CaptureSourceInterval &interval) {
  appendValue(out, static_cast<std::uint8_t>(interval.sourceKind));
  appendValue(out, interval.source);
  appendValue(out, interval.generation);
  appendValue(out, interval.occurrence);
  appendValue(out, interval.srcOffset);
  appendValue(out, interval.srcLength);
  appendValue(out, interval.dstOffset);
}

/// Encode one record body (no frame). The checksum field is computed
/// over the finished body and patched in place.
std::vector<std::byte> encodeBody(const CaptureJournalRecord &record) {
  const auto payloadBytes =
      payloadBytesFor(record.kind, record.intervals.size());
  std::vector<std::byte> body;
  body.reserve(kFixedBodyBytes + payloadBytes.value_or(0));
  appendValue(body, static_cast<std::uint8_t>(record.kind));
  appendValue(body, record.sequence);
  appendValue(body, record.captureId);
  appendValue(body, std::uint64_t{0}); // checksum placeholder

  switch (record.kind) {
  case CaptureJournalRecordKind::Append:
  case CaptureJournalRecordKind::Appended: {
    appendBytes(body, record.appendDigest.data(), record.appendDigest.size());
    appendValue(body, record.appendLength);
    appendValue(body, static_cast<std::uint32_t>(record.intervals.size()));
    for (const auto &interval : record.intervals)
      appendInterval(body, interval);
    if (record.kind == CaptureJournalRecordKind::Appended) {
      appendValue(body, record.span.scroll);
      appendValue(body, record.span.start);
      appendValue(body, record.span.length);
    }
    break;
  }
  case CaptureJournalRecordKind::Committed:
    break;
  case CaptureJournalRecordKind::Aborted:
    appendValue(body, record.abortReason);
    appendValue(body, static_cast<std::uint8_t>(record.abortUncertain ? 1 : 0));
    break;
  case CaptureJournalRecordKind::Reconciled:
    appendValue(body,
                static_cast<std::uint8_t>(record.reconciledPromoted ? 1 : 0));
    appendValue(body, record.span.scroll);
    appendValue(body, record.span.start);
    appendValue(body, record.span.length);
    break;
  case CaptureJournalRecordKind::Identity:
    appendValue(body, record.journalId);
    break;
  }

  const auto sum =
      fnv1a64(std::span<const std::byte>(body.data(), kChecksumOffset),
              std::span<const std::byte>(body.data() + kFixedBodyBytes,
                                         body.size() - kFixedBodyBytes));
  std::memcpy(body.data() + kChecksumOffset, &sum, sizeof(sum));
  return body;
}

/// Fully validate one record body and decode it. Every check the durable
/// format promises lives here: fixed minimum, known kind, checksum,
/// per-kind exact length, interval count consistency and sequence-free
/// field sanity (sequence itself is checked by the image scan, which
/// owns gaplessness). Returns std::nullopt on any violation.
std::optional<CaptureJournalRecord> decodeBody(const std::byte *body,
                                               const std::size_t bodyLen) {
  if (bodyLen < kFixedBodyBytes) return std::nullopt;
  std::size_t offset = 0;
  std::uint8_t kind{};
  std::memcpy(&kind, body + offset, 1);
  offset += 1;
  if (kind > kMaxKindValue) return std::nullopt;
  const auto recordKind = static_cast<CaptureJournalRecordKind>(kind);

  std::uint64_t storedChecksum{};
  std::memcpy(&storedChecksum, body + kChecksumOffset, sizeof(storedChecksum));
  const auto computed =
      fnv1a64(std::span<const std::byte>(body, kChecksumOffset),
              std::span<const std::byte>(body + kFixedBodyBytes,
                                         bodyLen - kFixedBodyBytes));
  if (computed != storedChecksum) return std::nullopt;

  CaptureJournalRecord record;
  record.kind = recordKind;
  offset      = kChecksumOffset + sizeof(storedChecksum);
  std::memcpy(&record.sequence, body + 1, sizeof(record.sequence));
  std::memcpy(&record.captureId, body + 9, sizeof(record.captureId));

  switch (recordKind) {
  case CaptureJournalRecordKind::Append:
  case CaptureJournalRecordKind::Appended: {
    const std::size_t tailBytes =
        recordKind == CaptureJournalRecordKind::Appended ? kSpanBytes : 0;
    if (bodyLen < offset + kAppendFixedBytes + tailBytes) return std::nullopt;
    std::memcpy(record.appendDigest.data(), body + offset,
                record.appendDigest.size());
    offset += record.appendDigest.size();
    std::memcpy(&record.appendLength, body + offset,
                sizeof(record.appendLength));
    offset += sizeof(record.appendLength);
    std::uint32_t intervalCount{};
    std::memcpy(&intervalCount, body + offset, sizeof(intervalCount));
    offset += sizeof(intervalCount);
    if (intervalCount > (bodyLen - offset - tailBytes) / kIntervalBytes)
      return std::nullopt;
    if (offset + static_cast<std::size_t>(intervalCount) * kIntervalBytes +
            tailBytes !=
        bodyLen)
      return std::nullopt;
    record.intervals.reserve(intervalCount);
    for (std::uint32_t i = 0; i < intervalCount; ++i) {
      CaptureSourceInterval interval;
      std::uint8_t sourceKind{};
      std::memcpy(&sourceKind, body + offset, 1);
      offset += 1;
      if (sourceKind > static_cast<std::uint8_t>(PayloadSourceKind::Proxy))
        return std::nullopt;
      interval.sourceKind = static_cast<PayloadSourceKind>(sourceKind);
      std::memcpy(&interval.source, body + offset, sizeof(interval.source));
      offset += sizeof(interval.source);
      std::memcpy(&interval.generation, body + offset,
                  sizeof(interval.generation));
      offset += sizeof(interval.generation);
      std::memcpy(&interval.occurrence, body + offset,
                  sizeof(interval.occurrence));
      offset += sizeof(interval.occurrence);
      std::memcpy(&interval.srcOffset, body + offset,
                  sizeof(interval.srcOffset));
      offset += sizeof(interval.srcOffset);
      std::memcpy(&interval.srcLength, body + offset,
                  sizeof(interval.srcLength));
      offset += sizeof(interval.srcLength);
      std::memcpy(&interval.dstOffset, body + offset,
                  sizeof(interval.dstOffset));
      offset += sizeof(interval.dstOffset);
      record.intervals.push_back(interval);
    }
    if (recordKind == CaptureJournalRecordKind::Appended) {
      std::memcpy(&record.span.scroll, body + offset,
                  sizeof(record.span.scroll));
      offset += sizeof(record.span.scroll);
      std::memcpy(&record.span.start, body + offset, sizeof(record.span.start));
      offset += sizeof(record.span.start);
      std::memcpy(&record.span.length, body + offset,
                  sizeof(record.span.length));
      offset += sizeof(record.span.length);
    }
    break;
  }
  case CaptureJournalRecordKind::Committed:
    if (bodyLen != kFixedBodyBytes) return std::nullopt;
    break;
  case CaptureJournalRecordKind::Aborted:
    if (bodyLen != kFixedBodyBytes + kAbortedBytes) return std::nullopt;
    std::memcpy(&record.abortReason, body + offset, 1);
    offset += 1;
    {
      std::uint8_t uncertain{};
      std::memcpy(&uncertain, body + offset, 1);
      if (uncertain > 1) return std::nullopt;
      record.abortUncertain = uncertain != 0;
    }
    break;
  case CaptureJournalRecordKind::Reconciled: {
    if (bodyLen != kFixedBodyBytes + kReconciledBytes) return std::nullopt;
    std::uint8_t promoted{};
    std::memcpy(&promoted, body + offset, 1);
    offset += 1;
    if (promoted > 1) return std::nullopt;
    record.reconciledPromoted = promoted != 0;
    std::memcpy(&record.span.scroll, body + offset, sizeof(record.span.scroll));
    offset += sizeof(record.span.scroll);
    std::memcpy(&record.span.start, body + offset, sizeof(record.span.start));
    offset += sizeof(record.span.start);
    std::memcpy(&record.span.length, body + offset, sizeof(record.span.length));
    offset += sizeof(record.span.length);
    break;
  }
  case CaptureJournalRecordKind::Identity:
    if (bodyLen != kFixedBodyBytes + kIdentityBytes) return std::nullopt;
    std::memcpy(&record.journalId, body + offset, sizeof(record.journalId));
    break;
  }
  return record;
}

/// Whether @p partial (the bytes present after a length prefix announcing
/// @p bodyLen) can be the start of one genuine, interrupted record: its kind
/// is known, the announced length is one that kind can have, its sequence (once
/// present) is the next expected, and an interval count (once present)
/// agrees with the announced length. A checksum cannot be checked on a
/// prefix, so this is the strongest honest test; anything it rejects is
/// refused as corruption rather than cut.
bool tornTailPlausible(const std::span<const std::byte> partial,
                       const std::size_t bodyLen,
                       const std::uint64_t expectedSeq) {
  if (partial.empty()) return true;
  const auto kind = static_cast<std::uint8_t>(partial[0]);
  if (kind > kMaxKindValue) return false;
  const auto recordKind       = static_cast<CaptureJournalRecordKind>(kind);
  const auto matchesIntervals = [&](const std::size_t tail,
                                    const std::size_t count) {
    return bodyLen ==
           kFixedBodyBytes + kAppendFixedBytes + tail + count * kIntervalBytes;
  };
  switch (recordKind) {
  case CaptureJournalRecordKind::Committed:
    if (bodyLen != kFixedBodyBytes) return false;
    break;
  case CaptureJournalRecordKind::Aborted:
    if (bodyLen != kFixedBodyBytes + kAbortedBytes) return false;
    break;
  case CaptureJournalRecordKind::Reconciled:
    if (bodyLen != kFixedBodyBytes + kReconciledBytes) return false;
    break;
  case CaptureJournalRecordKind::Identity:
    if (bodyLen != kFixedBodyBytes + kIdentityBytes) return false;
    break;
  case CaptureJournalRecordKind::Append:
  case CaptureJournalRecordKind::Appended: {
    const std::size_t tail =
        recordKind == CaptureJournalRecordKind::Appended ? kSpanBytes : 0;
    const auto base = kFixedBodyBytes + kAppendFixedBytes + tail;
    if (bodyLen < base || (bodyLen - base) % kIntervalBytes != 0) return false;
    if (partial.size() >= kIntervalCountOffset + sizeof(std::uint32_t)) {
      std::uint32_t count{};
      std::memcpy(&count, partial.data() + kIntervalCountOffset, sizeof(count));
      if (!matchesIntervals(tail, count)) return false;
    }
    break;
  }
  }
  if (partial.size() >= 1 + sizeof(std::uint64_t)) {
    std::uint64_t sequence{};
    std::memcpy(&sequence, partial.data() + 1, sizeof(sequence));
    if (sequence != expectedSeq) return false;
  }
  return true;
}

constexpr FramedLogFormat kFormat{kCaptureJournalSignature,
                                  kCaptureJournalVersion, kFixedBodyBytes};

} // namespace

auto CaptureJournal::scanImage(const std::span<const std::byte> bytes,
                               CaptureId &maxId, std::uint64_t &nextSequence,
                               std::uint64_t &journalId) -> FramedLogScan {
  CaptureId seenMax{};
  std::uint64_t seenNext = 1;
  std::uint64_t seenId{};
  const auto scan = scanFramedLog(
      kFormat, bytes,
      [](const std::span<const std::byte> partial, const std::size_t bodyLen,
         const std::size_t ordinal) {
        return tornTailPlausible(partial, bodyLen, ordinal + 1);
      },
      [&](const std::byte *body, const std::size_t bodyLen,
          const std::size_t ordinal) {
        const auto record = decodeBody(body, bodyLen);
        if (!record || record->sequence != ordinal + 1) return false;
        // The Identity record is the first record and only the first.
        if ((record->kind == CaptureJournalRecordKind::Identity) !=
            (ordinal == 0)) {
          return false;
        }
        if (ordinal == 0) seenId = record->journalId;
        seenMax  = std::max(seenMax, record->captureId);
        seenNext = record->sequence + 1;
        return true;
      });
  maxId        = seenMax;
  nextSequence = seenNext;
  journalId    = seenId;
  return scan;
}

CaptureJournal::CaptureJournal(std::filesystem::path path,
                               const std::optional<std::size_t> maxBytes)
    : log_(std::move(path), kFormat, maxBytes) {
  CaptureId scannedMax = 0;
  log_.open([&](const std::span<const std::byte> bytes) {
    return scanImage(bytes, scannedMax, nextSequence_, journalId_);
  });
  maxCaptureId_    = scannedMax;
  identityWritten_ = log_.wroteHeader() && nextSequence_ > 1;
  if (!identityWritten_.load()) {
    // A journal not yet written (or just repaired to nothing) names itself
    // on its first record. The identity only has to differ between journals
    // sharing one Store, so a random 64 bits suffices; it is never an
    // address and never derived from content.
    std::random_device entropy;
    journalId_    = (static_cast<std::uint64_t>(entropy()) << 32U) ^ entropy();
    nextSequence_ = 1;
  }
}

auto CaptureJournal::journalId() const noexcept -> std::uint64_t {
  return journalId_;
}

auto CaptureJournal::identityRecord() const -> CaptureJournalRecord {
  CaptureJournalRecord record;
  record.kind      = CaptureJournalRecordKind::Identity;
  record.captureId = maxCaptureId_;
  record.journalId = journalId_;
  return record;
}

auto CaptureJournal::pendingIdentityBytes() const noexcept -> std::size_t {
  return identityWritten_
             ? std::size_t{0}
             : captureJournalFramedBytes(CaptureJournalRecordKind::Identity);
}

CaptureJournal::~CaptureJournal() = default;

auto CaptureJournal::path() const -> const std::filesystem::path & {
  return log_.path();
}

auto CaptureJournal::sizeBytes() const -> std::size_t {
  return log_.sizeBytes();
}

auto CaptureJournal::fits(const std::size_t framedBytes,
                          const bool countHolds) const
    -> std::expected<void, IoErrorInfo> {
  return log_.fits(framedBytes + pendingIdentityBytes(),
                   countHolds ? heldBytes_ : 0);
}

auto CaptureJournal::hold(const std::size_t framedBytes)
    -> std::expected<void, IoErrorInfo> {
  if (const auto ok = fits(framedBytes, true); !ok) return ok;
  heldBytes_ += framedBytes;
  return {};
}

auto CaptureJournal::unhold(const std::size_t framedBytes) noexcept
    -> CaptureJournal * {
  heldBytes_ -= std::min(heldBytes_, framedBytes);
  return this;
}

auto CaptureJournal::heldBytes() const noexcept -> std::size_t {
  return heldBytes_;
}

auto CaptureJournal::maxCaptureId() const noexcept -> CaptureId {
  return maxCaptureId_;
}

auto CaptureJournal::appendRecord(const CaptureJournalRecordKind kind,
                                  const CaptureId captureId,
                                  const CaptureJournalRecord &payload,
                                  const CaptureJournalCrashPoints &crash)
    -> std::expected<CaptureJournalRecord, IoErrorInfo> {
  const std::lock_guard lock(mutex_);
  if (log_.poisoned()) {
    // A previous append could not prove the file still ends on a record
    // boundary; refuse rather than deepen the tear.
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  if (kind == CaptureJournalRecordKind::Identity) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 1});
  }
  CaptureJournalRecord record = payload;
  record.kind                 = kind;
  record.captureId            = captureId;
  const auto payloadBytes     = payloadBytesFor(kind, record.intervals.size());
  if (!payloadBytes) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  }
  if (const auto ok =
          fits(captureJournalFramedBytes(kind, record.intervals.size()), false);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (!identityWritten_) {
    // The first record of a journal names the journal. It is its own durable
    // write: a crash between it and the record leaves a valid journal that
    // holds only its identity.
    auto identity     = identityRecord();
    identity.sequence = nextSequence_;
    if (const auto ok = log_.append(frameBody(encodeBody(identity)), {}, [] {});
        !ok) {
      return std::unexpected(ok.error());
    }
    ++nextSequence_;
    identityWritten_ = true;
  }
  record.sequence   = nextSequence_;
  const auto framed = frameBody(encodeBody(record));
  // Bookkeeping that follows durability runs inside the afterFlush slot of
  // the shared append, ahead of the caller's own hook, so a throwing hook
  // still leaves a durable record counted.
  const auto counted = [&] {
    ++nextSequence_;
    maxCaptureId_ = std::max(maxCaptureId_.load(), captureId);
    if (crash.afterFlush) crash.afterFlush();
  };
  if (const auto ok = log_.append(framed, crash.beforeFlush, counted); !ok) {
    return std::unexpected(ok.error());
  }
  return record;
}

auto CaptureJournal::recover() const -> CaptureJournalRecovery {
  const std::lock_guard lock(mutex_);
  CaptureJournalRecovery recovery;
  if (log_.poisoned()) {
    // This instance already knows the file may not end on a record
    // boundary: a failed append's rollback could not be proven, so bytes on
    // disk may hide records behind what would parse as the tolerated
    // trailing remnant. Refuse rather than classify a prefix.
    recovery.status = CaptureJournalStatus::Corrupt;
    recovery.refusal =
        IoErrorInfo{IoErrorCode::PersistenceRefusal, log_.poisonDetail()};
    return recovery;
  }

  recovery.journalId = journalId_;
  const auto image   = log_.readImage();
  if (!image) {
    recovery.status  = CaptureJournalStatus::Corrupt;
    recovery.refusal = image.error();
    return recovery;
  }
  // Nothing was ever written: a never-used journal is vacuously clean.
  if (!*image || (*image)->empty()) {
    recovery.status = CaptureJournalStatus::Clean;
    return recovery;
  }
  const auto &bytes = **image;
  // A trailing incomplete record is the tolerated crash tear; the
  // constructor already cut it when this instance appended. Here the same
  // scan proves the boundary so recover() and repair can never disagree.
  CaptureId maxId{};
  std::uint64_t nextSequence{1};
  std::uint64_t scannedId{};
  const auto scan = scanImage(bytes, maxId, nextSequence, scannedId);
  if (scan.corrupt) {
    recovery.status = CaptureJournalStatus::Corrupt;
    recovery.refusal =
        IoErrorInfo{IoErrorCode::PersistenceRefusal,
                    static_cast<std::uint32_t>(scan.errorRecord)};
    return recovery;
  }

  // Every record in [header, scan.boundary) is complete and fully valid
  // (the scan proved it), so decode cannot fail; decodeBody runs again only
  // to materialize the fields.
  std::size_t cursor = kHeaderBytes;
  while (cursor < scan.boundary) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    const std::byte *body = bytes.data() + cursor + sizeof(std::uint32_t);
    if (const auto record = decodeBody(body, bodyLen);
        record && record->kind != CaptureJournalRecordKind::Identity) {
      recovery.records.push_back(*record);
    }
    cursor += sizeof(std::uint32_t) + bodyLen;
  }
  recovery.journalId    = scannedId;
  recovery.nextSequence = nextSequence;
  recovery.maxCaptureId = maxId;
  recovery.status       = CaptureJournalStatus::Clean;
  return recovery;
}

auto CaptureJournal::compact(const std::span<const CaptureJournalRecord> keep)
    -> std::expected<void, IoErrorInfo> {
  const std::lock_guard lock(mutex_);
  if (log_.poisoned()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  std::vector<std::byte> image;
  std::uint64_t sequence = 1;
  const auto add         = [&](CaptureJournalRecord record) {
    record.sequence   = sequence++;
    const auto framed = frameBody(encodeBody(record));
    image.insert(image.end(), framed.begin(), framed.end());
  };
  add(identityRecord());
  for (const auto &record : keep) {
    if (record.kind != CaptureJournalRecordKind::Identity) add(record);
  }
  if (log_.maxBytes() &&
      kCaptureJournalHeaderBytes + image.size() > *log_.maxBytes()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 4});
  }
  const auto replaced = log_.replaceRecords(image);
  if (!replaced) return std::unexpected(replaced.error());
  // The file is the new image from the rename on, so the sequence follows it
  // even when the directory sync was not acknowledged; otherwise the next
  // append would leave a gap the next open refuses as corruption.
  identityWritten_ = true;
  nextSequence_    = sequence;
  if (!replaced->directoryDurable) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  return {};
}

} // namespace xanadu::vortex
