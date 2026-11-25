#include "common/xanadu/vortex/file_source_adapter.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include <gleditor/logging.hpp>

namespace xanadu::vortex {

namespace {

/// Spdlog category for every diagnostic this file's adapters emit.
constexpr const char *fileLogCategory = "vortex.io.file";

/// Live staged ops per adapter; a full table refuses new stages
/// rather than evicting an entry an in-flight request still names.
constexpr std::size_t maxStagedOps = 4096;

/// Total frozen write bytes the staging table may hold across all live
/// ops. The entry cap alone lets 4096 staged writes pin unbounded memory
/// (each carries its own byte vector); the bound sits well above the
/// scheduler's default 16 MiB per-op backend budget so legitimate
/// multi-op staging still fits, while a stage past it is refused
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

/// The exact fstat fields a generation is made of. This detects an
/// *uncertain* write (plan §7.2); it is not proof that no write happened (a
/// same-size, same-timestamp in-place rewrite is invisible to it), which is
/// exactly why a `Stable`/CAS request is refused rather than synthesized
/// from this value.
FileGeneration generationFromStat(const struct stat &st) noexcept {
  return FileGeneration{
      .device           = static_cast<std::uint64_t>(st.st_dev),
      .inode            = static_cast<std::uint64_t>(st.st_ino),
      .size             = static_cast<std::uint64_t>(st.st_size),
      .mtimeSeconds     = static_cast<std::int64_t>(st.st_mtim.tv_sec),
      .mtimeNanoseconds = static_cast<std::int64_t>(st.st_mtim.tv_nsec)};
}

} // namespace

FileSourceAdapter::FileSourceAdapter(std::string path, bool writable)
    : FileSourceAdapter(::open(path.c_str(), writable ? O_RDWR : O_RDONLY),
                        std::move(path), writable) {}

FileSourceAdapter::FileSourceAdapter(int fd, std::string path, bool writable)
    : fd_(fd), path_(std::move(path)), writable_(writable) {}

FileSourceAdapter::~FileSourceAdapter() {
  if (fd_ >= 0) ::close(fd_);
}

auto FileSourceAdapter::statFileGeneration() const noexcept
    -> std::optional<FileGeneration> {
  if (fd_ < 0) return std::nullopt;
  struct stat st{};
  if (::fstat(fd_, &st) != 0) return std::nullopt;
  return generationFromStat(st);
}

auto FileSourceAdapter::internGenerationLocked(
    const FileGeneration &generation) const -> std::uint64_t {
  if (const auto found = generationIds_.find(generation);
      found != generationIds_.end())
    return found->second;
  if (generationOrder_.size() >= maxInternedGenerations_) {
    generationIds_.erase(generationOrder_.front());
    generationOrder_.pop_front();
  }
  const auto id = nextGenerationId_++;
  generationIds_.emplace(generation, id);
  generationOrder_.push_back(generation);
  return id;
}

auto FileSourceAdapter::observeGeneration() const
    -> std::optional<std::uint64_t> {
  const auto generation = statFileGeneration();
  if (!generation) return std::nullopt;
  const std::lock_guard lock(mutex_);
  return internGenerationLocked(*generation);
}

auto FileSourceAdapter::currentGeneration() const
    -> std::optional<std::uint64_t> {
  return observeGeneration();
}

auto FileSourceAdapter::probeCapabilities() const
    -> std::optional<FileOpenCapabilities> {
  if (fd_ < 0) return std::nullopt;
  struct stat st{};
  if (::fstat(fd_, &st) != 0) return std::nullopt;
  const std::lock_guard lock(mutex_);
  return FileOpenCapabilities{
      .locator           = path_,
      .consistency       = SourceConsistency::Live,
      .identity          = FileSourceIdentity::OpenedInode,
      .initialGeneration = internGenerationLocked(generationFromStat(st)),
      .sizeAtOpen        = static_cast<std::uint64_t>(st.st_size)};
}

auto FileSourceAdapter::stageLocked(const StageKey key, StagedFileOp op)
    -> std::expected<StageKey, std::string> {
  if (staged_.contains(key))
    return std::unexpected("staging key already in use");
  if (staged_.size() >= maxStagedOps)
    return std::unexpected("staging table full");
  const auto byteCount = op.writeBytes.size();
  if (byteCount > maxStagedWriteBytes - stagedWriteBytes_)
    return std::unexpected("staged write byte bound exceeded");
  staged_.emplace(key, std::move(op));
  // Count only once the op is actually staged: a throwing map insert must
  // not leave the byte ledger ahead of the table.
  stagedWriteBytes_ += byteCount;
  return key;
}

std::optional<StageKey>
FileSourceAdapter::stageRead(MediaRange range,
                             std::optional<std::uint64_t> expectedGeneration) {
  const std::lock_guard lock(mutex_);
  const auto key    = nextStageKeyLocked();
  const auto staged = stageLocked(
      key,
      StagedFileOp{StagedFileOp::Kind::Read, range, {}, expectedGeneration});
  if (!staged) return std::nullopt;
  return *staged;
}

std::optional<StageKey>
FileSourceAdapter::stageWrite(std::uint64_t offset,
                              std::vector<std::byte> bytes,
                              std::optional<std::uint64_t> expectedGeneration) {
  const std::lock_guard lock(mutex_);
  const auto key       = nextStageKeyLocked();
  const auto byteCount = static_cast<std::uint64_t>(bytes.size());
  const auto staged =
      stageLocked(key, StagedFileOp{StagedFileOp::Kind::Write,
                                    MediaRange{offset, byteCount},
                                    std::move(bytes), expectedGeneration});
  if (!staged) return std::nullopt;
  return *staged;
}

std::optional<StageKey> FileSourceAdapter::stageStableRead(MediaRange range) {
  const std::lock_guard lock(mutex_);
  const auto key    = nextStageKeyLocked();
  const auto staged = stageLocked(
      key,
      StagedFileOp{StagedFileOp::Kind::StableRead, range, {}, std::nullopt});
  if (!staged) return std::nullopt;
  return *staged;
}

std::expected<StageKey, std::string>
FileSourceAdapter::stageAt(const StageKey key, FileOpSpec spec) {
  const std::lock_guard lock(mutex_);
  switch (spec.kind) {
  case FileOpSpec::Kind::Read:
    if (!spec.writeBytes.empty())
      return std::unexpected("read spec carries write bytes");
    return stageLocked(key, StagedFileOp{StagedFileOp::Kind::Read,
                                         spec.range,
                                         {},
                                         spec.expectedGeneration});
  case FileOpSpec::Kind::StableRead:
    if (!spec.writeBytes.empty())
      return std::unexpected("stable-read spec carries write bytes");
    return stageLocked(
        key, StagedFileOp{
                 StagedFileOp::Kind::StableRead, spec.range, {}, std::nullopt});
  case FileOpSpec::Kind::Write: {
    const auto byteCount = static_cast<std::uint64_t>(spec.writeBytes.size());
    return stageLocked(
        key, StagedFileOp{StagedFileOp::Kind::Write,
                          MediaRange{spec.range.offset, byteCount},
                          std::move(spec.writeBytes), spec.expectedGeneration});
  }
  }
  return std::unexpected("unknown file op kind");
}

StageKey FileSourceAdapter::nextStageKeyLocked() {
  // Skip any key a caller-chosen stageAt or a wrapped counter left live;
  // the table bound keeps the skip short. Every key value is a valid cell
  // ref, so nothing is reserved.
  auto key = nextStageKey_++;
  while (staged_.contains(key)) key = nextStageKey_++;
  return key;
}

void FileSourceAdapter::abandon(const StageKey key) {
  const std::lock_guard lock(mutex_);
  const auto it = staged_.find(key);
  if (it != staged_.end()) stagedWriteBytes_ -= it->second.writeBytes.size();
  staged_.erase(key);
}

void FileSourceAdapter::abandonStaged(const OwnerCell specification) noexcept {
  abandon(specification);
}

auto FileSourceAdapter::prepare(const CellRequest &request, OwnerContext &)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  if (fd_ < 0) return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});

  const StageKey key = request.specification;
  StagedFileOp op;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it == staged_.end()) {
      GLEDITOR_LOG_DEBUG(fileLogCategory,
                         "prepare refused: no staged op under key {}", key);
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    }
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

  if (op.expectedGeneration) {
    const auto current = observeGeneration();
    if (!current) {
      // fstat failed, so no generation can be verified; refuse rather than
      // silently accept the staged expectation.
      GLEDITOR_LOG_WARN(fileLogCategory,
                        "prepare refused: fstat failed (errno {})", errno);
      return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});
    }
    if (*op.expectedGeneration != *current) {
      GLEDITOR_LOG_DEBUG(fileLogCategory,
                         "prepare refused: generation {} expected, {} current",
                         *op.expectedGeneration, *current);
      return std::unexpected(IoErrorInfo{IoErrorCode::Stale});
    }
  }

  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  return PreparedOperation{request.key, std::move(bytes)};
}

void FileSourceAdapter::execute(const BackendCommand &command,
                                BackendContext &context,
                                CompletionSink &sink) noexcept {
  StageKey key{};
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
      const auto observedBefore = observeGeneration();
      if (op->expectedGeneration && !observedBefore) {
        result = failedResult({IoErrorCode::Unsupported, 0});
      } else if (op->expectedGeneration &&
                 *op->expectedGeneration != *observedBefore) {
        result = BackendResult{Conflict{Generation{*op->expectedGeneration},
                                        Generation{*observedBefore},
                                        IoProgress{.phase = IoPhase::Terminal}},
                               {}};
      } else {
        result = BackendResult{};
        if (op->kind == StagedFileOp::Kind::Write) {
          if (context.stop.stop_requested()) {
            result = BackendResult{
                Cancelled{{}, IoProgress{.phase = IoPhase::Terminal}, false},
                {}};
          } else if (context.budget.maxBytes &&
                     op->writeBytes.size() > *context.budget.maxBytes) {
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
                  context.budget.maxBytes.value_or(op->range.length));
              auto toRead = op->range.length == 0
                                ? 0
                                : std::min(op->range.length, maxBytes);
              // The service rejects a payload over the request's payload
              // bound after the read; stop at the bound instead of reading
              // and publishing bytes that are certain to be refused.
              if (context.budget.maxPayloadBytes) {
                toRead = std::min<std::uint64_t>(
                    toRead, *context.budget.maxPayloadBytes);
              }
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
                // Sample the generation again after the transfer: a pread
                // is not atomic against a concurrent writer, so a read whose
                // before/after generations differ may mix two states and
                // must not be recorded as an observation of either.
                std::function<void()> afterTransfer;
                {
                  const std::lock_guard lock(mutex_);
                  afterTransfer = afterTransferHook_;
                }
                if (afterTransfer) afterTransfer();
                const auto observedAfter = observeGeneration();
                if (context.stop.stop_requested()) {
                  // Reads never "land" bytes the way pwrite can, so a
                  // stopped read reports its range with uncertain=false.
                  if (out.empty()) {
                    result = BackendResult{Cancelled{{}, progress, false}, {}};
                  } else {
                    auto lease = publishTransientBytes(context.payloads, out);
                    if (!lease) {
                      GLEDITOR_LOG_WARN(fileLogCategory,
                                        "payload publish failed for {} bytes",
                                        out.size());
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
                } else if (!observedBefore || !observedAfter) {
                  // No generation can be claimed for bytes whose source
                  // state could not be sampled on both sides.
                  GLEDITOR_LOG_WARN(fileLogCategory,
                                    "read refused: fstat failed around the "
                                    "transfer (errno {})",
                                    errno);
                  result =
                      failedResult({IoErrorCode::BackendFailure, 0}, progress);
                } else if (*observedBefore != *observedAfter) {
                  // Conflict (not Complete-with-uncertainty): the outcome
                  // vocabulary's Conflict names exactly "the generation I
                  // saw moved", the caller can retry, and no bytes are
                  // published that belong to neither generation.
                  GLEDITOR_LOG_DEBUG(
                      fileLogCategory,
                      "read raced a change: generation {} then {}",
                      *observedBefore, *observedAfter);
                  result = BackendResult{
                      Conflict{Generation{*observedBefore},
                               Generation{*observedAfter},
                               IoProgress{.phase = IoPhase::Terminal}},
                      {}};
                } else {
                  // Read bytes publish through the payload channel: the
                  // outcome names the retained range and the lease reaches
                  // terminalCandidate below. Publish BEFORE recording the
                  // observation so the observation binds the exact retained
                  // range; a publish failure is a Failed outcome and
                  // records nothing.
                  std::optional<PayloadRange> retainedRange;
                  bool readPublished = true;
                  if (!out.empty()) {
                    auto lease = publishTransientBytes(context.payloads, out);
                    if (!lease) {
                      GLEDITOR_LOG_WARN(fileLogCategory,
                                        "payload publish failed for {} bytes",
                                        out.size());
                      result = failedResult({IoErrorCode::BackendFailure, 0});
                      readPublished = false;
                    } else {
                      payload       = *lease;
                      retainedRange = lease->range;
                    }
                  }
                  if (readPublished) {
                    // Record a distinct observation for every successful
                    // live read (plan §7.2), keyed by the generation
                    // observed around this transfer and bound to the exact
                    // retained bytes -- minted before the outcome so
                    // ReadObservationResult names the same id.
                    std::uint64_t observationId     = 0;
                    const std::uint64_t observedGen = *observedAfter;
                    {
                      const std::lock_guard lock(mutex_);
                      observationId = nextObservationId_++;
                      // observations_ is a std::map keyed by the sequential
                      // id: begin() is the oldest retained observation, so
                      // the cap evicts in insertion (FIFO) order rather
                      // than whatever hash bucket unordered_map happened
                      // to pick first.
                      if (observations_.size() >= maxRetainedObservations_)
                        observations_.erase(observations_.begin());
                      observations_[observationId] = ReadObservation{
                          observationId,
                          observedGen,
                          MediaRange{op->range.offset,
                                     static_cast<std::uint64_t>(out.size())},
                          path_,
                          SourceConsistency::Live,
                          retainedRange};
                    }
                    const ReadObservationResult details{
                        .observationId    = observationId,
                        .sourceGeneration = observedGen,
                        .readOffset       = op->range.offset,
                        .readLength  = static_cast<std::uint64_t>(out.size()),
                        .locator     = path_,
                        .consistency = SourceConsistency::Live};
                    if (retainedRange) {
                      result = BackendResult{
                          Complete{*retainedRange, details, progress}, {}};
                    } else {
                      result =
                          BackendResult{Complete{{}, details, progress}, {}};
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  } catch (...) {
    GLEDITOR_LOG_WARN(fileLogCategory,
                      "execute threw; reporting BackendFailure");
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
  const auto id         = nextDecodeLease_++;
  const auto generation = statFileGeneration();
  DecodeLease lease{id, range, std::nullopt};
  if (generation) lease.sourceGeneration = internGenerationLocked(*generation);
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

auto FileSourceAdapter::setMaxDecodeLeases(std::size_t value)
    -> FileSourceAdapter * {
  const std::lock_guard lock(mutex_);
  maxDecodeLeases_ = value;
  return this;
}

auto FileSourceAdapter::setAfterTransferHook(std::function<void()> hook)
    -> FileSourceAdapter * {
  const std::lock_guard lock(mutex_);
  afterTransferHook_ = std::move(hook);
  return this;
}

} // namespace xanadu::vortex
