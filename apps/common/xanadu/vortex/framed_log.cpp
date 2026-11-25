#include "common/xanadu/vortex/framed_log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <gleditor/logging.hpp>
#include <limits>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace xanadu::vortex {

namespace {

constexpr auto kJournalLogCategory = "vortex.io.journal";
// FNV-1a-64 parameters.
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime       = 1099511628211ull;
constexpr std::size_t kReadChunkBytes   = 4096;
constexpr auto kReplacementSuffix       = ".compact";

auto readAll(const int fd) -> std::optional<std::vector<std::byte>> {
  std::vector<std::byte> bytes;
  char buffer[kReadChunkBytes];
  for (;;) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (n == 0) return bytes;
    appendBytes(bytes, buffer, static_cast<std::size_t>(n));
  }
}

auto headerBytes(const FramedLogFormat &format) -> std::vector<std::byte> {
  std::vector<std::byte> header;
  header.reserve(kFramedLogHeaderBytes);
  appendBytes(header, format.signature, kFramedLogSignatureBytes);
  appendValue(header, format.version);
  return header;
}

auto fsyncRetry(const int fd) -> bool {
  while (::fsync(fd) != 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

} // namespace

void appendBytes(std::vector<std::byte> &out, const void *data,
                 const std::size_t size) {
  const auto *bytes = static_cast<const std::byte *>(data);
  out.insert(out.end(), bytes, bytes + size);
}

auto fnv1a64(const std::span<const std::byte> first,
             const std::span<const std::byte> second) noexcept
    -> std::uint64_t {
  std::uint64_t hash = kFnvOffsetBasis;
  const auto mix     = [&hash](const std::span<const std::byte> bytes) {
    for (const auto byte : bytes) {
      hash ^= static_cast<std::uint8_t>(byte);
      hash *= kFnvPrime;
    }
  };
  mix(first);
  mix(second);
  return hash;
}

auto frameBody(const std::span<const std::byte> body)
    -> std::vector<std::byte> {
  std::vector<std::byte> framed;
  framed.reserve(body.size() + kFramedLogFrameBytes);
  appendValue(framed, static_cast<std::uint32_t>(body.size()));
  framed.insert(framed.end(), body.begin(), body.end());
  return framed;
}

auto writeAll(const int fd, const void *data, const std::size_t size) -> bool {
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

FdGuard::~FdGuard() {
  if (fd >= 0) ::close(fd);
}

auto syncParentDirectory(const std::filesystem::path &path) -> bool {
  auto dir = path.parent_path();
  if (dir.empty()) dir = ".";
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  const FdGuard guard{fd};
  return fsyncRetry(fd);
}

auto scanFramedLog(const FramedLogFormat &format,
                   const std::span<const std::byte> bytes, TornTailCheck torn,
                   RecordCheck record) -> FramedLogScan {
  FramedLogScan scan;
  if (bytes.size() < kFramedLogHeaderBytes) {
    scan.corrupt = true;
    return scan;
  }
  std::uint32_t version{};
  std::memcpy(&version, bytes.data() + kFramedLogSignatureBytes,
              sizeof(version));
  if (std::memcmp(bytes.data(), format.signature, kFramedLogSignatureBytes) !=
          0 ||
      version != format.version) {
    scan.corrupt = true;
    return scan;
  }

  std::size_t cursor  = kFramedLogHeaderBytes;
  std::size_t ordinal = 0;
  while (cursor + kFramedLogFrameBytes <= bytes.size()) {
    std::uint32_t bodyLen{};
    std::memcpy(&bodyLen, bytes.data() + cursor, sizeof(bodyLen));
    if (bodyLen < format.minBodyBytes) {
      // A filesystem that extends the size before the data leaves a zeroed
      // tail after a crash. Nothing but zeros from here to the end is that
      // tear, not a record, and cutting it loses nothing.
      if (std::ranges::all_of(bytes.subspan(cursor), [](const std::byte b) {
            return b == std::byte{0};
          })) {
        break;
      }
      scan.corrupt     = true;
      scan.errorRecord = ordinal + 1;
      return scan;
    }
    // A claimed body that runs past the file is a tear only when what is
    // present is the self-consistent start of the very record the length
    // announces. Anything else -- a corrupt length mid-file -- must refuse:
    // cutting there would silently delete the valid records after it.
    const auto available = bytes.size() - cursor - kFramedLogFrameBytes;
    if (bodyLen > available) {
      if (torn(bytes.subspan(cursor + kFramedLogFrameBytes), bodyLen,
               ordinal)) {
        break;
      }
      scan.corrupt     = true;
      scan.errorRecord = ordinal + 1;
      return scan;
    }
    if (!record(bytes.data() + cursor + kFramedLogFrameBytes, bodyLen,
                ordinal)) {
      scan.corrupt     = true;
      scan.errorRecord = ordinal + 1;
      return scan;
    }
    ++ordinal;
    cursor += kFramedLogFrameBytes + bodyLen;
  }
  scan.boundary = cursor;
  return scan;
}

FramedLog::FramedLog(std::filesystem::path path, const FramedLogFormat format,
                     const std::optional<std::size_t> maxBytes)
    : path_(std::move(path)), format_(format), maxBytes_(maxBytes) {}

auto FramedLog::replacementPath() const -> std::filesystem::path {
  auto replacement = path_;
  replacement += kReplacementSuffix;
  return replacement;
}

auto FramedLog::open(const FramedLogScanFn scan) -> FramedLog * {
  std::error_code ec;
  // A crash between writing the replacement and renaming it leaves a temp
  // file whose contents are unreferenced; the original is still the journal.
  std::filesystem::remove(replacementPath(), ec);
  ec.clear();
  if (!std::filesystem::exists(path_, ec)) {
    // Absent is the only clean "fresh" state. An exists() error means the
    // path cannot even be classified, so the append boundary cannot be
    // proven -- refuse appends rather than land them on a guess.
    poisoned_ = static_cast<bool>(ec);
    return this;
  }
  const auto size = std::filesystem::file_size(path_, ec);
  if (ec) {
    // Exists but cannot be sized (a directory, a permission wall): same
    // unprovable-boundary refusal.
    poisoned_ = true;
    return this;
  }
  writtenBytes_ = static_cast<std::size_t>(size);
  if (writtenBytes_ == 0) return this; // empty: the first append writes header
  if (writtenBytes_ < kFramedLogHeaderBytes) {
    // 1..header-1 bytes: either the start of the very header a crash
    // interrupted (the first record is written with its header in one
    // write, so nothing can follow) or foreign bytes. Only the former is a
    // fresh journal; it is cut back to empty. Anything else would have the
    // first append land its header *behind* the remnant, so poison.
    repairTornHeader();
    return this;
  }
  wroteHeader_ = true;
  repairTornTail(scan);
  return this;
}

void FramedLog::repairTornHeader() {
  const int fd = ::open(path_.c_str(), O_RDWR);
  if (fd < 0) {
    poisoned_ = true;
    return;
  }
  const FdGuard guard{fd};
  const auto remnant  = readAll(fd);
  const auto expected = headerBytes(format_);
  if (!remnant || remnant->size() != writtenBytes_ ||
      std::memcmp(remnant->data(), expected.data(), remnant->size()) != 0) {
    poisoned_ = true;
    return;
  }
  if (::ftruncate(fd, 0) != 0) {
    poisoned_ = true;
    return;
  }
  writtenBytes_ = 0;
  GLEDITOR_LOG_WARN(kJournalLogCategory,
                    "journal {}: cut torn header remnant, journal is fresh",
                    path_.string());
}

void FramedLog::repairTornTail(const FramedLogScanFn scan) {
  // A crash between write and fsync leaves a torn tail: bytes after the last
  // complete, valid record. O_APPEND would land every later record behind
  // it, hiding them from recovery's prefix parse, so cut the file back to
  // the last valid boundary before this instance appends anything. A tail
  // the scan cannot prove torn-vs-corrupt -- or one it cannot cut -- poisons
  // the journal.
  int fd = ::open(path_.c_str(), O_RDWR);
  if (fd < 0) fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    poisoned_ = true;
    return;
  }
  const FdGuard guard{fd};
  const auto bytes = readAll(fd);
  if (!bytes) {
    poisoned_ = true;
    return;
  }
  const auto result = scan(*bytes);
  if (result.corrupt) {
    // A fully-present invalid record is corruption, not a tear: cutting to
    // its start would silently delete the valid records after it.
    poisoned_     = true;
    poisonDetail_ = static_cast<std::uint32_t>(result.errorRecord);
    GLEDITOR_LOG_ERROR(kJournalLogCategory,
                       "journal {} corrupt at record {}: refusing to truncate",
                       path_.string(), result.errorRecord);
    return;
  }
  writtenBytes_ = result.boundary;
  wroteHeader_  = true; // the scan proved the header bytes are ours
  if (result.boundary == bytes->size()) return; // no tear to cut
  if (::ftruncate(fd, static_cast<off_t>(result.boundary)) != 0) {
    poisoned_ = true;
    return;
  }
  GLEDITOR_LOG_WARN(kJournalLogCategory,
                    "journal {}: cut {} torn tail bytes after boundary {}",
                    path_.string(), bytes->size() - result.boundary,
                    result.boundary);
}

auto FramedLog::fits(const std::size_t framedBytes,
                     const std::size_t heldBytes) const
    -> std::expected<void, IoErrorInfo> {
  // The on-disk body length is u32: a payload that cannot be represented is
  // refused here, before anything is encoded.
  if (framedBytes > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 0});
  }
  const std::size_t header = wroteHeader_ ? 0 : kFramedLogHeaderBytes;
  if (maxBytes_ &&
      writtenBytes_ + header + heldBytes + framedBytes > *maxBytes_) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 0});
  }
  return {};
}

auto FramedLog::append(const std::span<const std::byte> framed,
                       const std::function<void()> &beforeFlush,
                       const std::function<void()> &afterFlush)
    -> std::expected<void, IoErrorInfo> {
  const auto backendFailure = [] {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  };
  if (poisoned_) {
    // A previous append could not prove the file still ends on a record
    // boundary; bytes written now would land behind a tail recovery reads
    // as the tolerated trailing remnant while hiding every record after it.
    // Refuse rather than deepen the tear.
    return backendFailure();
  }
  // The first record travels with the header in one write: a crash can then
  // leave only a header prefix (repairTornHeader's case), never a header
  // followed by a torn first record that repair could not tell from
  // corruption.
  const bool creating = !wroteHeader_;
  std::vector<std::byte> withHeader;
  if (creating) {
    withHeader = headerBytes(format_);
    withHeader.insert(withHeader.end(), framed.begin(), framed.end());
  }
  const auto bytes = creating ? std::span<const std::byte>(withHeader) : framed;

  // writtenBytes_ is the offset of the last byte this journal knows is a
  // complete, valid record (the header counts once flushed): the boundary a
  // failed append truncates back to.
  const auto recordStart = static_cast<off_t>(writtenBytes_);

  const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) return backendFailure();
  const FdGuard guard{fd};

  const auto rollbackTo = [&](const off_t target) {
    // Recovery tolerates exactly one trailing, incomplete record, so a torn
    // tail followed by later appends would hide those records. Roll the
    // file back to the last known boundary; when even that cannot be proven
    // the journal is poisoned and every later append refuses.
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
  const auto failAppend = [&] {
    rollbackTo(recordStart);
    return backendFailure();
  };

  if (!writeAll(fd, bytes.data(), bytes.size())) return failAppend();

  if (beforeFlush) {
    try {
      beforeFlush();
    } catch (...) {
      // Simulated death before fsync: the record was written but never
      // durably flushed, so roll it back to the last known boundary exactly
      // like a failed write.
      rollbackTo(recordStart);
      throw;
    }
  }
  if (!fsyncRetry(fd)) return failAppend();
  // A created file's directory entry is part of the journal's durability: a
  // file whose entry was never fsynced can vanish whole, and recovery would
  // then report Clean over an admission the caller already acted on. The
  // record is not durable until the name is, so a failure rolls it back.
  if (creating && !syncParentDirectory(path_)) return failAppend();
  // fsync acknowledged: the record IS durable. Bookkeeping must reflect that
  // BEFORE afterFlush can throw -- otherwise a throwing afterFlush leaves
  // writtenBytes_ behind a record recovery already counts, and a later
  // failAppend ftruncate could erase durable bytes.
  writtenBytes_ += bytes.size();
  wroteHeader_ = true;
  if (afterFlush) afterFlush();
  return {};
}

auto FramedLog::readImage() const
    -> std::expected<std::optional<std::vector<std::byte>>, IoErrorInfo> {
  const int fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return std::optional<std::vector<std::byte>>{};
    // Any other open failure means the contents cannot be proven:
    // EACCES/EMFILE/EIO/ENOTDIR all read as "cannot examine", never "empty".
    return std::unexpected(IoErrorInfo{IoErrorCode::PersistenceRefusal, 0});
  }
  const FdGuard guard{fd};
  auto bytes = readAll(fd);
  if (!bytes) {
    return std::unexpected(IoErrorInfo{IoErrorCode::PersistenceRefusal, 0});
  }
  return std::optional<std::vector<std::byte>>{std::move(*bytes)};
}

auto FramedLog::replaceRecords(const std::span<const std::byte> framedRecords)
    -> std::expected<void, IoErrorInfo> {
  const auto failure = [] {
    return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
  };
  if (poisoned_) return failure();
  auto image = headerBytes(format_);
  image.insert(image.end(), framedRecords.begin(), framedRecords.end());

  const auto replacement = replacementPath();
  const int fd           = ::open(replacement.c_str(),
                                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return failure();
  bool durable = false;
  {
    const FdGuard guard{fd};
    durable = writeAll(fd, image.data(), image.size()) && fsyncRetry(fd);
  }
  std::error_code ec;
  if (!durable) {
    std::filesystem::remove(replacement, ec);
    return failure();
  }
  std::filesystem::rename(replacement, path_, ec);
  if (ec) {
    std::filesystem::remove(replacement, ec);
    return failure();
  }
  // The rename has happened: the journal on disk is the new one whether or
  // not the directory sync below is acknowledged, so bookkeeping follows the
  // rename. An unacknowledged directory sync is still a refusal -- the caller
  // must not treat the retirement as durable.
  writtenBytes_ = image.size();
  wroteHeader_  = true;
  if (!syncParentDirectory(path_)) return failure();
  return {};
}

} // namespace xanadu::vortex
