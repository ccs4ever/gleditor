#include "common/xanadu/vortex/capture_journal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <gleditor/logging.hpp>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace xanadu::vortex {

namespace {

constexpr std::size_t kHeaderBytes      = kCaptureJournalHeaderBytes;
constexpr std::size_t kFixedBodyBytes   = kCaptureJournalFixedBodyBytes;
constexpr std::size_t kChecksumOffset   = 1 + 8 + 8;
constexpr std::size_t kIntervalBytes    = kCaptureJournalIntervalBytes;
constexpr std::size_t kAppendFixedBytes = kCaptureJournalAppendFixedBytes;
constexpr std::size_t kSpanBytes        = kCaptureJournalSpanBytes;
constexpr std::size_t kAbortedBytes     = kCaptureJournalAbortedBytes;
// Where intervalCount sits in an Append/Appended body: after the fixed
// header, the digest and the append length.
constexpr std::size_t kIntervalCountOffset = kFixedBodyBytes + 32 + 8;
constexpr std::size_t kMaxKindValue =
    static_cast<std::size_t>(CaptureJournalRecordKind::Aborted);

void appendBytes(std::vector<std::byte> &out, const void *data,
                 const std::size_t size) {
  const auto *bytes = static_cast<const std::byte *>(data);
  out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
void appendValue(std::vector<std::byte> &out, const T &value) {
  appendBytes(out, &value, sizeof(T));
}

/// FNV-1a-64 over the concatenation of two spans. The record checksum
/// covers every body byte except the checksum field itself; hashing the
/// two sides separately avoids copying the body for verification.
std::uint64_t checksumOf(const std::span<const std::byte> first,
                         const std::span<const std::byte> second) {
  std::uint64_t hash = 14695981039346656037ull;
  const auto mix     = [&hash](const std::span<const std::byte> bytes) {
    for (const auto byte : bytes) {
      hash ^= static_cast<std::uint8_t>(byte);
      hash *= 1099511628211ull;
    }
  };
  mix(first);
  mix(second);
  return hash;
}

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
  }

  const auto sum =
      checksumOf(std::span<const std::byte>(body.data(), kChecksumOffset),
                 std::span<const std::byte>(body.data() + kFixedBodyBytes,
                                            body.size() - kFixedBodyBytes));
  std::memcpy(body.data() + kChecksumOffset, &sum, sizeof(sum));
  return body;
}

/// u32-framed record: bodyLen(4) + body. encodeBody output is trusted to
/// be non-empty; callers run payloadBytesFor first so no length truncates.
std::vector<std::byte> frameBody(const std::vector<std::byte> &body) {
  std::vector<std::byte> framed;
  framed.reserve(body.size() + sizeof(std::uint32_t));
  appendValue(framed, static_cast<std::uint32_t>(body.size()));
  framed.insert(framed.end(), body.begin(), body.end());
  return framed;
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
      checksumOf(std::span<const std::byte>(body, kChecksumOffset),
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
  }
  return record;
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

/// Closes the descriptor on scope exit so a throwing crash hook cannot
/// leak the fd (same discipline as ActorIoJournal).
struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

struct CaptureJournalScan {
  /// Offset just past the last complete, fully-valid record -- the
  /// boundary a crash-torn tail starts at. Meaningful only when !corrupt.
  std::size_t boundary{};
  bool corrupt{};
  /// 1-based position of the failing record; 0 when the header or the
  /// scan itself failed before any record was proven.
  std::size_t errorRecord{};
  /// Next sequence a valid append must carry (last valid + 1, from 1).
  std::uint64_t nextSequence{1};
  CaptureId maxCaptureId{};
};

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

/// Scan a journal image: verify header, then walk records validating
/// kind, exact per-kind length, checksum, interval count consistency and
/// gapless sequence. A trailing *incomplete* record is a tolerated tear
/// (the scan stops at its start); a fully-present invalid record,
/// signature/version mismatch or sequence gap is corruption. Shared by
/// the constructor's tail repair and recover() so the two can never
/// disagree about what counts as corruption.
CaptureJournalScan scanJournalImage(const std::span<const std::byte> bytes) {
  CaptureJournalScan scan;
  if (bytes.size() < kHeaderBytes) {
    scan.corrupt     = true;
    scan.errorRecord = 0;
    return scan;
  }
  if (std::memcmp(bytes.data(), kCaptureJournalSignature,
                  sizeof(kCaptureJournalSignature)) != 0 ||
      [&] {
        std::uint32_t version{};
        std::memcpy(&version, bytes.data() + sizeof(kCaptureJournalSignature),
                    sizeof(version));
        return version != kCaptureJournalVersion;
      }()) {
    scan.corrupt     = true;
    scan.errorRecord = 0;
    return scan;
  }

  std::size_t cursor        = kHeaderBytes;
  std::uint64_t expectedSeq = 1;
  std::size_t recordOrdinal = 0;
  while (cursor + sizeof(std::uint32_t) <= bytes.size()) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    if (bodyLen < kFixedBodyBytes) {
      scan.corrupt     = true;
      scan.errorRecord = recordOrdinal + 1;
      return scan;
    }
    // A claimed body that runs past the file is a tear only when what
    // is present is the self-consistent start of the very record the
    // length announces. Anything else -- a corrupt length mid-file -- must
    // refuse: cutting there would silently delete the valid records after
    // it.
    const auto available = bytes.size() - cursor - sizeof(std::uint32_t);
    if (bodyLen > available) {
      if (tornTailPlausible(bytes.subspan(cursor + sizeof(std::uint32_t)),
                            bodyLen, expectedSeq)) {
        break;
      }
      scan.corrupt     = true;
      scan.errorRecord = recordOrdinal + 1;
      return scan;
    }
    const std::byte *body = bytes.data() + cursor + sizeof(std::uint32_t);
    ++recordOrdinal;
    const auto record = decodeBody(body, bodyLen);
    if (!record || record->sequence != expectedSeq) {
      scan.corrupt     = true;
      scan.errorRecord = recordOrdinal;
      return scan;
    }
    scan.maxCaptureId = std::max(scan.maxCaptureId, record->captureId);
    scan.nextSequence = ++expectedSeq;
    cursor += sizeof(std::uint32_t) + bodyLen;
  }
  scan.boundary = cursor;
  return scan;
}

} // namespace

CaptureJournal::CaptureJournal(std::filesystem::path path,
                               const std::size_t maxBytes)
    : path_(std::move(path)), maxBytes_(maxBytes) {
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    // Absent is the only clean "fresh" state; an exists() error means the
    // boundary cannot be proven, so appends refuse rather than land on a
    // guess.
    poisoned_ = static_cast<bool>(ec);
    return;
  }
  const auto size = std::filesystem::file_size(path_, ec);
  if (ec) {
    poisoned_ = true;
    return;
  }
  writtenBytes_ = static_cast<std::size_t>(size);
  if (writtenBytes_ == 0) return; // empty: the first append writes header
  if (writtenBytes_ < kHeaderBytes) {
    // 1..kHeaderBytes-1 bytes: either the start of the very header a crash
    // interrupted (the first record is written with its header in one
    // write, so nothing can follow) or foreign bytes. Only the former is a
    // fresh journal; it is cut back to empty. Anything else would have the
    // first append land its header *behind* the remnant, so poison.
    repairTornHeader();
    return;
  }
  wroteHeader_ = true;
  repairTornTail();
}

void CaptureJournal::repairTornHeader() {
  const int fd = ::open(path_.c_str(), O_RDWR);
  if (fd < 0) {
    poisoned_ = true;
    return;
  }
  const FdGuard guard{fd};
  std::byte remnant[kHeaderBytes]{};
  std::size_t have = 0;
  while (have < writtenBytes_) {
    const auto n = ::read(fd, remnant + have, writtenBytes_ - have);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      poisoned_ = true;
      return;
    }
    have += static_cast<std::size_t>(n);
  }
  std::byte expected[kHeaderBytes]{};
  std::memcpy(expected, kCaptureJournalSignature,
              sizeof(kCaptureJournalSignature));
  std::memcpy(expected + sizeof(kCaptureJournalSignature),
              &kCaptureJournalVersion, sizeof(kCaptureJournalVersion));
  if (std::memcmp(remnant, expected, writtenBytes_) != 0) {
    poisoned_ = true;
    return;
  }
  if (::ftruncate(fd, 0) != 0) {
    poisoned_ = true;
    return;
  }
  writtenBytes_ = 0;
  GLEDITOR_LOG_WARN("vortex.io.capture",
                    "capture journal: cut torn header remnant, journal is "
                    "fresh");
}

void CaptureJournal::repairTornTail() {
  // A crash between writeAll and fsync leaves a torn tail: bytes after
  // the last complete, fully-valid record. O_APPEND would land every
  // later record behind it, hiding them from recover()'s prefix parse,
  // so cut the file back to the last valid boundary before this instance
  // appends anything. A tail the scan cannot prove torn-vs-corrupt -- or
  // one it cannot cut -- poisons the journal.
  int fd = ::open(path_.c_str(), O_RDWR);
  if (fd < 0) fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    poisoned_ = true;
    return;
  }
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

  const auto scan = scanJournalImage(bytes);
  if (scan.corrupt) {
    // A fully-present invalid record is corruption, not a tear: cutting
    // to its start would silently delete the valid records after it.
    // Poison so appends refuse and recover() reports the same Corrupt.
    poisoned_     = true;
    poisonDetail_ = static_cast<std::uint32_t>(scan.errorRecord);
    GLEDITOR_LOG_ERROR("vortex.io.capture",
                       "capture journal corrupt at record {}: refusing to "
                       "truncate",
                       scan.errorRecord);
    return;
  }
  writtenBytes_ = scan.boundary;
  wroteHeader_  = true; // the scan proved the header bytes are ours
  nextSequence_ = scan.nextSequence;
  maxCaptureId_ = scan.maxCaptureId;
  if (scan.boundary == bytes.size()) return; // no tear to cut
  if (::ftruncate(fd, static_cast<off_t>(scan.boundary)) != 0) {
    poisoned_ = true;
    return;
  }
  GLEDITOR_LOG_WARN("vortex.io.capture",
                    "capture journal: cut {} torn tail bytes after record "
                    "boundary {}",
                    bytes.size() - scan.boundary, scan.boundary);
}

CaptureJournal::~CaptureJournal() = default;

void CaptureJournal::syncParentDirectory() const noexcept {
  auto dir = path_.parent_path();
  if (dir.empty()) dir = ".";
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return;
  const FdGuard guard{fd};
  while (::fsync(fd) != 0 && errno == EINTR) {
  }
}

auto CaptureJournal::path() const -> const std::filesystem::path & {
  return path_;
}

auto CaptureJournal::sizeBytes() const -> std::size_t { return writtenBytes_; }

auto CaptureJournal::fits(const std::size_t framedBytes,
                          const bool countHolds) const
    -> std::expected<void, IoErrorInfo> {
  // The on-disk body length is u32: a payload that cannot be represented
  // is refused here, before appendRecord ever encodes.
  if (framedBytes > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  }
  const std::size_t header = wroteHeader_ ? 0 : kHeaderBytes;
  const std::size_t held   = countHolds ? heldBytes_ : 0;
  if (maxBytes_ != 0 &&
      writtenBytes_ + header + held + framedBytes > maxBytes_) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 0});
  }
  return {};
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
  if (poisoned_) {
    // A previous append could not prove the file still ends on a record
    // boundary; bytes written now would land behind a tail recover()
    // reads as the tolerated trailing remnant while hiding every record
    // after it. Refuse rather than deepen the tear.
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  CaptureJournalRecord record = payload;
  record.kind                 = kind;
  record.captureId            = captureId;
  record.sequence             = nextSequence_;
  const auto payloadBytes     = payloadBytesFor(kind, record.intervals.size());
  if (!payloadBytes) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  }
  const auto body = encodeBody(record);
  auto framed     = frameBody(body);
  if (const auto ok = fits(framed.size(), false); !ok) {
    return std::unexpected(ok.error());
  }
  // The first record travels with the header in one write: a crash can
  // then leave only a header prefix (repairTornHeader's case), never a
  // header followed by a torn first record that repair could not tell
  // from corruption.
  const bool creating = !wroteHeader_;
  if (creating) {
    std::vector<std::byte> withHeader;
    withHeader.reserve(kHeaderBytes + framed.size());
    appendBytes(withHeader, kCaptureJournalSignature,
                sizeof(kCaptureJournalSignature));
    appendValue(withHeader, kCaptureJournalVersion);
    withHeader.insert(withHeader.end(), framed.begin(), framed.end());
    framed = std::move(withHeader);
  }

  // writtenBytes_ is the offset of the last byte this journal knows is a
  // complete, valid record (the header counts once flushed): the
  // boundary a failed append truncates back to.
  const auto recordStart = static_cast<off_t>(writtenBytes_);

  const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  }
  const FdGuard guard{fd};

  const auto rollbackTo = [&](const off_t target) {
    // recover() tolerates exactly one trailing, incomplete record, so a
    // torn tail followed by later appends would hide those records.
    // Roll the file back to the last known boundary; when even that
    // cannot be proven the journal is poisoned and every later append
    // refuses.
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
  const auto failAppend =
      [&]() -> std::expected<CaptureJournalRecord, IoErrorInfo> {
    rollbackTo(recordStart);
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  };

  const bool ok = writeAll(fd, framed.data(), framed.size());

  if (!ok) return failAppend();

  if (crash.beforeFlush) {
    try {
      crash.beforeFlush();
    } catch (...) {
      // Simulated death before fsync: the record was written but never
      // durably flushed, so roll it back to the last known boundary
      // exactly like a failed write.
      rollbackTo(recordStart);
      throw;
    }
  }
  while (::fsync(fd) != 0) {
    if (errno != EINTR) return failAppend();
  }
  // fsync acknowledged: the record IS durable. Bookkeeping must reflect
  // that BEFORE afterFlush can throw -- otherwise a throwing afterFlush
  // leaves writtenBytes_/nextSequence_ behind a record recover() already
  // counts, and a later failAppend ftruncate could erase durable bytes.
  if (creating) {
    // The file's directory entry is part of the journal's durability: a
    // created file whose entry was never fsynced can vanish whole.
    syncParentDirectory();
    wroteHeader_ = true;
  }
  writtenBytes_ += framed.size();
  ++nextSequence_;
  maxCaptureId_ = std::max(maxCaptureId_, captureId);
  if (crash.afterFlush) crash.afterFlush();
  return record;
}

auto CaptureJournal::recover() const -> CaptureJournalRecovery {
  CaptureJournalRecovery recovery;
  if (poisoned_) {
    // This instance already knows the file may not end on a record
    // boundary: a failed append's rollback could not be proven, so bytes
    // on disk may hide records behind what would parse as the tolerated
    // trailing remnant. Refuse rather than classify a prefix.
    recovery.status = CaptureJournalStatus::Corrupt;
    recovery.refusal =
        IoErrorInfo{IoErrorCode::PersistenceRefusal, poisonDetail_};
    return recovery;
  }

  std::vector<std::byte> bytes;
  {
    const int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT) {
        // Nothing was ever written: a never-used journal is vacuously
        // clean.
        recovery.status = CaptureJournalStatus::Clean;
        return recovery;
      }
      // Any other open failure means the contents cannot be proven.
      recovery.status  = CaptureJournalStatus::Corrupt;
      recovery.refusal = IoErrorInfo{IoErrorCode::PersistenceRefusal, 0};
      return recovery;
    }
    const FdGuard guard{fd};
    char buffer[4096];
    for (;;) {
      const auto n = ::read(fd, buffer, sizeof(buffer));
      if (n < 0) {
        if (errno == EINTR) continue;
        recovery.status  = CaptureJournalStatus::Corrupt;
        recovery.refusal = IoErrorInfo{IoErrorCode::PersistenceRefusal, 0};
        return recovery;
      }
      if (n == 0) break;
      appendBytes(bytes, buffer, static_cast<std::size_t>(n));
    }
  }

  if (bytes.empty()) {
    recovery.status = CaptureJournalStatus::Clean;
    return recovery;
  }
  // A trailing incomplete record is the tolerated crash tear; the scan
  // already cut it in the constructor when this instance appended, and
  // a fresh instance's constructor cut it before recover() could run.
  // Here the same scan proves the boundary so recover() and repair can
  // never disagree.
  const auto scan = scanJournalImage(bytes);
  if (scan.corrupt) {
    recovery.status = CaptureJournalStatus::Corrupt;
    recovery.refusal =
        IoErrorInfo{IoErrorCode::PersistenceRefusal,
                    static_cast<std::uint32_t>(scan.errorRecord)};
    return recovery;
  }

  // Every record in [kHeaderBytes, scan.boundary) is complete and
  // fully valid (the scan proved it), so decode cannot fail; decodeBody
  // runs again only to materialize the fields.
  std::size_t cursor = kHeaderBytes;
  while (cursor < scan.boundary) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    const std::byte *body = bytes.data() + cursor + sizeof(std::uint32_t);
    if (const auto record = decodeBody(body, bodyLen)) {
      recovery.records.push_back(*record);
    }
    cursor += sizeof(std::uint32_t) + bodyLen;
  }
  recovery.nextSequence = scan.nextSequence;
  recovery.maxCaptureId = scan.maxCaptureId;
  recovery.status       = CaptureJournalStatus::Clean;
  return recovery;
}

} // namespace xanadu::vortex
