#include "common/xanadu/vortex/file_source_adapter.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace xanadu::vortex {

namespace {

/// Live staged ops per adapter; a full table refuses new stages (key 0)
/// rather than evicting an entry an in-flight request still names.
constexpr std::size_t maxStagedOps = 4096;

/// Total frozen write bytes the staging table may hold across all live
/// ops. The entry cap alone lets 4096 staged writes pin unbounded memory
/// (each carries its own byte vector); the bound sits well above the
/// scheduler's default 16 MiB per-op backend budget so legitimate
/// multi-op staging still fits, while a stage past it fails as key 0
/// exactly like a full table.
constexpr std::size_t maxStagedWriteBytes = 64U * 1024U * 1024U;

/// Largest offset+pwrite length `off_t` can address; staged ranges are
/// checked against it before any pread/pwrite offset arithmetic.
constexpr std::uint64_t maxOffT =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

IoProgress terminalProgress(std::uint64_t transferred) {
  return IoProgress{.acceptedBytes    = transferred,
                    .transferredBytes = transferred,
                    .consumedBytes    = transferred,
                    .phase            = IoPhase::Terminal};
}

BackendResult failedResult(IoErrorInfo reason, IoProgress progress = {}) {
  if (progress.phase == IoPhase::Preparing) progress.phase = IoPhase::Terminal;
  return BackendResult{Failed{reason, {}, progress}, {}};
}

/// `stat` combines size and mtime into one opaque generation value. This
/// detects an *uncertain* write (plan §7.2); it is not proof that no write
/// happened (a same-size, same-timestamp in-place rewrite is invisible to
/// it), which is exactly why a `Stable`/CAS request is refused rather than
/// synthesized from this value.
std::uint64_t generationFromStat(const struct stat &st) noexcept {
  std::uint64_t mix = static_cast<std::uint64_t>(st.st_size);
  mix ^= static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000007ULL;
  mix ^= static_cast<std::uint64_t>(st.st_mtim.tv_nsec) * 1000000009ULL;
  mix ^= static_cast<std::uint64_t>(st.st_ino) * 2654435761ULL;
  return mix == 0 ? 1 : mix;
}

} // namespace

FileSourceAdapter::FileSourceAdapter(std::string path, bool writable)
    : path_(std::move(path)), writable_(writable) {
  const int flags = writable_ ? O_RDWR : O_RDONLY;
  fd_             = ::open(path_.c_str(), flags);
}

FileSourceAdapter::~FileSourceAdapter() {
  if (fd_ >= 0) ::close(fd_);
}

std::uint64_t FileSourceAdapter::statGeneration() const noexcept {
  if (fd_ < 0) return 0;
  struct stat st{};
  if (::fstat(fd_, &st) != 0) return 0;
  return generationFromStat(st);
}

std::uint64_t FileSourceAdapter::currentGeneration() const noexcept {
  return statGeneration();
}

auto FileSourceAdapter::probeCapabilities() const
    -> std::optional<FileOpenCapabilities> {
  if (fd_ < 0) return std::nullopt;
  struct stat st{};
  if (::fstat(fd_, &st) != 0) return std::nullopt;
  return FileOpenCapabilities{.locator           = path_,
                              .consistency       = SourceConsistency::Live,
                              .initialGeneration = generationFromStat(st),
                              .sizeAtOpen =
                                  static_cast<std::uint64_t>(st.st_size)};
}

std::uint32_t FileSourceAdapter::stageRead(MediaRange range,
                                           std::uint64_t expectedGeneration) {
  const std::lock_guard lock(mutex_);
  if (staged_.size() >= maxStagedOps) return 0;
  const auto key = nextStageKeyLocked();
  staged_[key] =
      StagedFileOp{StagedFileOp::Kind::Read, range, {}, expectedGeneration};
  return key;
}

std::uint32_t FileSourceAdapter::stageWrite(std::uint64_t offset,
                                            std::vector<std::byte> bytes,
                                            std::uint64_t expectedGeneration) {
  const std::lock_guard lock(mutex_);
  if (staged_.size() >= maxStagedOps ||
      bytes.size() > maxStagedWriteBytes - stagedWriteBytes_)
    return 0;
  const auto key       = nextStageKeyLocked();
  const auto byteCount = bytes.size();
  staged_[key] =
      StagedFileOp{StagedFileOp::Kind::Write,
                   MediaRange{offset, static_cast<std::uint64_t>(byteCount)},
                   std::move(bytes), expectedGeneration};
  // Count only once the op is actually staged: a throwing map insert must
  // not leave the byte ledger ahead of the table.
  stagedWriteBytes_ += byteCount;
  return key;
}

std::uint32_t FileSourceAdapter::stageStableRead(MediaRange range) {
  const std::lock_guard lock(mutex_);
  if (staged_.size() >= maxStagedOps) return 0;
  const auto key = nextStageKeyLocked();
  staged_[key]   = StagedFileOp{StagedFileOp::Kind::StableRead, range, {}, 0};
  return key;
}

std::uint32_t FileSourceAdapter::nextStageKeyLocked() {
  // Skip 0 (the table-full sentinel) and any key a wrapped counter could
  // revisit while still live; the table bound keeps the skip short.
  auto key = nextStageKey_++;
  while (key == 0 || staged_.contains(key)) key = nextStageKey_++;
  return key;
}

void FileSourceAdapter::abandon(const std::uint32_t key) {
  const std::lock_guard lock(mutex_);
  const auto it = staged_.find(key);
  if (it != staged_.end()) stagedWriteBytes_ -= it->second.writeBytes.size();
  staged_.erase(key);
}

void FileSourceAdapter::abandonStaged(const OwnerCell specification) noexcept {
  abandon(static_cast<std::uint32_t>(specification));
}

auto FileSourceAdapter::prepare(const CellRequest &request, OwnerContext &)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  if (fd_ < 0) return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});

  const auto key = static_cast<std::uint32_t>(request.specification);
  StagedFileOp op;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it == staged_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    op = it->second;
  }

  // Plan §7.2: "A strict snapshot/CAS request is refused if backend cannot
  // meet it." A plain POSIX regular file never proves atomicity via `stat`
  // or watches, so every stable-read request is refused here, before
  // admission, regardless of content.
  if (op.kind == StagedFileOp::Kind::StableRead)
    return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});

  if (op.kind == StagedFileOp::Kind::Write && !writable_)
    return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});

  const auto current = statGeneration();
  if (op.expectedGeneration != 0) {
    if (current == 0)
      // stat failed, so no generation can be verified; refuse rather than
      // silently accept the staged expectation.
      return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});
    if (op.expectedGeneration != current)
      return std::unexpected(IoErrorInfo{IoErrorCode::Stale});
  }

  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  return PreparedOperation{request.key, std::move(bytes)};
}

void FileSourceAdapter::execute(const BackendCommand &command,
                                BackendContext &context,
                                CompletionSink &sink) noexcept {
  std::uint32_t key{};
  if (command.operation.bytes.size() >= sizeof(key))
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));

  // Everything that can throw -- the staged-op copy, payload registration --
  // stays inside this try: execute is noexcept, so an escaped exception
  // would terminate the worker.
  BackendResult result;
  std::optional<PayloadLease> payload;
  // A PayloadLease dropped on any path -- the catch below, an early exit --
  // must still release its resolver lease; terminalCandidate's own release
  // of the same id on the success path makes the guard's a no-op there.
  const PayloadLeaseGuard payloadGuard(context.payloads, payload);
  try {
    std::optional<StagedFileOp> op;
    {
      const std::lock_guard lock(mutex_);
      const auto it = staged_.find(key);
      if (it != staged_.end()) {
        stagedWriteBytes_ -= it->second.writeBytes.size();
        op = std::move(it->second);
        staged_.erase(it);
      }
    }

    if (!op) {
      result = failedResult({IoErrorCode::InvalidRequest, 0});
    } else {
      // Re-check the generation at transfer time: the file may have moved
      // between a successful `prepare` and this worker turn (plan §7.2's
      // Conflict path, mirrored from `StagedByteSourceAdapter`). A failed
      // `stat` cannot verify the staged expectation either -- prepare()
      // refuses that same unverifiable case as Unsupported, and bypassing
      // the CAS here would silently accept a request the caller expected
      // to be checked.
      const auto observedBefore = statGeneration();
      if (op->expectedGeneration != 0 && observedBefore == 0) {
        result = failedResult({IoErrorCode::Unsupported, 0});
      } else if (op->expectedGeneration != 0 &&
                 op->expectedGeneration != observedBefore) {
        result = BackendResult{Conflict{Generation{op->expectedGeneration},
                                        Generation{observedBefore},
                                        IoProgress{.phase = IoPhase::Terminal}},
                               {}};
      } else {
        result = BackendResult{};
        if (op->kind == StagedFileOp::Kind::Write) {
          if (context.stop.stop_requested()) {
            result = BackendResult{
                Cancelled{{}, IoProgress{.phase = IoPhase::Terminal}, false},
                {}};
          } else if (context.budget.maxBytes != 0 &&
                     op->writeBytes.size() > context.budget.maxBytes) {
            result = failedResult({IoErrorCode::BudgetExceeded, 0},
                                  terminalProgress(0));
          } else {
            const auto offset = static_cast<off_t>(op->range.offset);
            if (offset < 0 ||
                static_cast<std::uint64_t>(offset) != op->range.offset ||
                op->writeBytes.size() > maxOffT ||
                static_cast<std::uint64_t>(offset) >
                    maxOffT - op->writeBytes.size()) {
              result = failedResult({IoErrorCode::InvalidRequest, 0});
            } else {
              std::size_t written{};
              bool stopped = false;
              while (written < op->writeBytes.size()) {
                if (context.stop.stop_requested()) {
                  stopped = true;
                  break;
                }
                const auto rc = ::pwrite(fd_, op->writeBytes.data() + written,
                                         op->writeBytes.size() - written,
                                         offset + static_cast<off_t>(written));
                if (rc < 0) {
                  if (errno == EINTR) continue;
                  break;
                }
                if (rc == 0) {
                  errno = EIO;
                  break;
                }
                written += static_cast<std::size_t>(rc);
              }
              if (stopped) {
                // A partial pwrite may have landed bytes; report the
                // transfer as uncertain rather than claiming completion.
                result = BackendResult{
                    Cancelled{{},
                              IoProgress{.transferredBytes = written,
                                         .phase            = IoPhase::Terminal},
                              true},
                    {}};
              } else if (written != op->writeBytes.size()) {
                result = failedResult({IoErrorCode::BackendFailure,
                                       static_cast<std::uint32_t>(errno)},
                                      terminalProgress(written));
              } else {
                result = BackendResult{
                    Complete{{}, std::monostate{}, terminalProgress(written)},
                    {}};
              }
            }
          }
        } else if (context.stop.stop_requested()) {
          result = BackendResult{
              Cancelled{{}, IoProgress{.phase = IoPhase::Terminal}, false}, {}};
        } else {
          // Read, 64-bit range (plan §7.3).
          const auto offset = static_cast<off_t>(op->range.offset);
          if (offset < 0 ||
              static_cast<std::uint64_t>(offset) != op->range.offset) {
            result = failedResult({IoErrorCode::InvalidRequest, 0});
          } else {
            struct stat st{};
            const bool statOk = ::fstat(fd_, &st) == 0;
            const auto size =
                statOk ? static_cast<std::uint64_t>(st.st_size) : 0;
            if (statOk && op->range.offset >= size) {
              result = BackendResult{EndOfStream{terminalProgress(0)}, {}};
            } else {
              const auto maxBytes = static_cast<std::uint64_t>(
                  context.budget.maxBytes > 0 ? context.budget.maxBytes
                                              : op->range.length);
              auto toRead = op->range.length == 0
                                ? 0
                                : std::min(op->range.length, maxBytes);
              // A stat-able file bounds the read by what is actually
              // there: range.length or an unbounded budget can name far
              // more than the file holds past offset, and the vector
              // below would allocate the whole request before the first
              // pread discovers the EOF (a 100GB range on a 1KB file).
              // offset < size is already proven by the EndOfStream branch.
              if (statOk) {
                toRead = std::min(toRead, size - op->range.offset);
              }
              // offset+got must stay inside off_t for every pread call.
              if (toRead > maxOffT ||
                  static_cast<std::uint64_t>(offset) > maxOffT - toRead) {
                result = failedResult({IoErrorCode::InvalidRequest, 0});
              } else {
                std::vector<std::byte> out(toRead);
                std::size_t got{};
                bool readFailed         = false;
                std::uint32_t readErrno = 0;
                while (got < out.size()) {
                  if (context.stop.stop_requested()) break;
                  const auto rc =
                      ::pread(fd_, out.data() + got, out.size() - got,
                              offset + static_cast<off_t>(got));
                  if (rc < 0) {
                    if (errno == EINTR) continue;
                    readFailed = true;
                    readErrno  = static_cast<std::uint32_t>(errno);
                    break;
                  }
                  if (rc == 0) break; // EOF: a short read publishes partial.
                  got += static_cast<std::size_t>(rc);
                }
                out.resize(got);
                const auto progress = terminalProgress(got);
                if (context.stop.stop_requested()) {
                  // Reads never "land" bytes the way pwrite can, so a
                  // stopped read reports its range with uncertain=false.
                  if (out.empty()) {
                    result = BackendResult{Cancelled{{}, progress, false}, {}};
                  } else {
                    auto lease = publishTransientBytes(context.payloads, out);
                    if (!lease) {
                      result = failedResult({IoErrorCode::BackendFailure, 0});
                    } else {
                      payload = *lease;
                      result  = BackendResult{
                          Cancelled{lease->range, progress, false}, {}};
                    }
                  }
                } else if (readFailed) {
                  result = failedResult(
                      {IoErrorCode::BackendFailure, readErrno}, progress);
                } else if (out.empty() && op->range.length != 0) {
                  result = BackendResult{EndOfStream{progress}, {}};
                } else {
                  if (out.empty()) {
                    result = BackendResult{
                        Complete{{}, std::monostate{}, progress}, {}};
                  } else {
                    // Read bytes publish through the payload channel: the
                    // outcome names the retained range and the lease reaches
                    // terminalCandidate below.
                    auto lease = publishTransientBytes(context.payloads, out);
                    if (!lease) {
                      result = failedResult({IoErrorCode::BackendFailure, 0});
                    } else {
                      payload = *lease;
                      result  = BackendResult{
                          Complete{lease->range, std::monostate{}, progress},
                           {}};
                    }
                  }
                  // Record a distinct observation for every successful live
                  // read (plan §7.2). Keyed by the generation observed around
                  // this transfer, not the (possibly stale) one requested.
                  const std::lock_guard lock(mutex_);
                  const auto observationId = nextObservationId_++;
                  // observations_ is a std::map keyed by the sequential id:
                  // begin() is the oldest retained observation, so the cap
                  // evicts in insertion (FIFO) order rather than whatever
                  // hash bucket unordered_map happened to pick first.
                  if (observations_.size() >= maxRetainedObservations_)
                    observations_.erase(observations_.begin());
                  observations_[observationId] =
                      ReadObservation{observationId, statGeneration(),
                                      op->range, SourceConsistency::Live};
                }
              }
            }
          }
        }
      }
    }
  } catch (...) {
    result = failedResult({IoErrorCode::BackendFailure, 0});
  }

  sink.terminalCandidate(command.token, std::move(result), payload);
  sink.releaseAcknowledged({PrivateReleaseToken{command.token}});
}

auto FileSourceAdapter::publish(const NativeCompletion &completion,
                                OwnerContext &)
    -> std::expected<OwnerPublication, IoErrorInfo> {
  return OwnerPublication{completion.result.outcome};
}

auto FileSourceAdapter::observation(std::uint64_t observationId) const
    -> std::optional<ReadObservation> {
  const std::lock_guard lock(mutex_);
  const auto it = observations_.find(observationId);
  if (it == observations_.end()) return std::nullopt;
  return it->second;
}

auto FileSourceAdapter::acquireDecodeLease(MediaRange range)
    -> std::expected<DecodeLease, IoErrorInfo> {
  const std::lock_guard lock(mutex_);
  if (decodeLeases_.size() >= maxDecodeLeases_)
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded});
  const auto id = nextDecodeLease_++;
  DecodeLease lease{id, range, statGeneration()};
  decodeLeases_[id] = lease;
  return lease;
}

void FileSourceAdapter::releaseDecodeLease(LeaseId lease) noexcept {
  const std::lock_guard lock(mutex_);
  decodeLeases_.erase(lease);
}

std::size_t FileSourceAdapter::liveDecodeLeases() const noexcept {
  const std::lock_guard lock(mutex_);
  return decodeLeases_.size();
}

void FileSourceAdapter::setMaxDecodeLeases(std::size_t value) noexcept {
  const std::lock_guard lock(mutex_);
  maxDecodeLeases_ = value;
}

} // namespace xanadu::vortex
