#include "common/xanadu/vortex/source_adapters.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xanadu::vortex {

namespace {

/// Live staged specs per adapter; a full table refuses new stages (key 0)
/// rather than evicting an entry an in-flight request still names.
constexpr std::size_t maxStagedSpecs = 4096;

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

BackendResult conflictResult(std::uint64_t expected, std::uint64_t observed,
                             IoProgress progress) {
  return BackendResult{
      Conflict{Generation{expected}, Generation{observed}, progress}, {}};
}

/// Publish @p bytes through the payload channel: the retained range goes
/// into the outcome the @p makeOutcome lambda builds, and the lease comes
/// back through @p payload for terminalCandidate. A resolver refusal
/// becomes a typed Failed outcome instead of silent byte loss.
inline auto payloadOutcome(PayloadResolver &payloads,
                           std::vector<std::byte> bytes, IoProgress progress,
                           std::optional<PayloadLease> &payload,
                           auto makeOutcome) -> BackendResult {
  auto lease = publishTransientBytes(payloads, bytes);
  if (!lease) return failedResult({IoErrorCode::BackendFailure, 0});
  payload = *lease;
  return BackendResult{makeOutcome(lease->range, progress), {}};
}

} // namespace

std::uint32_t StagedByteSourceAdapter::stage(StreamOpSpec spec) {
  const std::lock_guard lock(mutex_);
  if (staged_.size() >= maxStagedSpecs) return 0;
  // Skip 0 (the table-full sentinel a request reports as InvalidRequest)
  // and any still-live key a wrapped nextKey_ could revisit.
  auto key = nextKey_++;
  while (key == 0 || staged_.contains(key)) key = nextKey_++;
  staged_[key] = std::move(spec);
  return key;
}

void StagedByteSourceAdapter::abandon(const std::uint32_t key) {
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
}

void StagedByteSourceAdapter::abandonStaged(
    const OwnerCell specification) noexcept {
  abandon(static_cast<std::uint32_t>(specification));
}

auto StagedByteSourceAdapter::prepare(const CellRequest &request,
                                      OwnerContext &)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  const auto key = static_cast<std::uint32_t>(request.specification);
  StreamOpSpec spec;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it == staged_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    spec = it->second;
  }
  const auto current = currentGeneration();
  if (spec.expectedGeneration != 0) {
    if (current == 0)
      // This adapter tracks no generation, so it cannot honour a nonzero
      // expectation; refuse rather than silently accept it.
      return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});
    if (spec.expectedGeneration != current)
      return std::unexpected(IoErrorInfo{IoErrorCode::Stale});
  }
  if (const auto refused = validateSpec(spec)) return std::unexpected(*refused);

  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  return PreparedOperation{request.key, std::move(bytes)};
}

void StagedByteSourceAdapter::execute(const BackendCommand &command,
                                      BackendContext &context,
                                      CompletionSink &sink) noexcept {
  std::uint32_t key{};
  if (command.operation.bytes.size() >= sizeof(key))
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));

  // Everything that can throw -- the spec copy, perform, and the payload
  // registration inside it -- stays inside this try: execute is noexcept,
  // so an escaped exception would terminate the worker.
  BackendResult result;
  std::optional<PayloadLease> payload;
  // A PayloadLease dropped on any path -- the catch below, an early exit --
  // must still release its resolver lease; terminalCandidate's own release
  // of the same id on the success path makes the guard's a no-op there.
  const PayloadLeaseGuard payloadGuard(context.payloads, payload);
  try {
    std::optional<StreamOpSpec> spec;
    {
      const std::lock_guard lock(mutex_);
      const auto it = staged_.find(key);
      if (it != staged_.end()) {
        spec = it->second;
        staged_.erase(it);
      }
    }
    if (!spec) {
      result = failedResult({IoErrorCode::InvalidRequest, 0});
    } else {
      result = perform(*spec, context, payload);
    }
  } catch (...) {
    result = failedResult({IoErrorCode::BackendFailure, 0});
  }
  sink.terminalCandidate(command.token, std::move(result), payload);
  sink.releaseAcknowledged({PrivateReleaseToken{command.token}});
}

auto StagedByteSourceAdapter::publish(const NativeCompletion &completion,
                                      OwnerContext &)
    -> std::expected<OwnerPublication, IoErrorInfo> {
  return OwnerPublication{completion.result.outcome};
}

// -- InMemoryStreamAdapter ---------------------------------------------------

void InMemoryStreamAdapter::setContent(std::vector<std::byte> bytes) {
  const std::lock_guard lock(stateMutex_);
  content_ = std::move(bytes);
  ++generation_;
}

std::uint64_t InMemoryStreamAdapter::generation() const noexcept {
  const std::lock_guard lock(stateMutex_);
  return generation_;
}

std::vector<std::byte> InMemoryStreamAdapter::content() const {
  const std::lock_guard lock(stateMutex_);
  return content_;
}

std::uint64_t InMemoryStreamAdapter::currentGeneration() const noexcept {
  const std::lock_guard lock(stateMutex_);
  return generation_;
}

auto InMemoryStreamAdapter::perform(const StreamOpSpec &spec,
                                    BackendContext &context,
                                    std::optional<PayloadLease> &payload)
    -> BackendResult {
  const std::lock_guard lock(stateMutex_);

  if (spec.expectedGeneration != 0 && spec.expectedGeneration != generation_)
    return conflictResult(spec.expectedGeneration, generation_,
                          IoProgress{.phase = IoPhase::Terminal});

  if (spec.kind == StreamOpKind::Write) {
    // Checked arithmetic and bounded extension: a sparse write must not
    // resize the buffer to an attacker-chosen offset, and the declared
    // completion/payload budget bounds the write itself.
    if (spec.writeBytes.size() > context.budget.maxBytes &&
        context.budget.maxBytes != 0) {
      return failedResult({IoErrorCode::BudgetExceeded, 0},
                          terminalProgress(0));
    }
    if (spec.offset > content_.size() &&
        spec.offset - content_.size() > context.budget.maxBytes &&
        context.budget.maxBytes != 0) {
      return failedResult({IoErrorCode::InvalidRequest, 0},
                          terminalProgress(0));
    }
    const auto end = spec.offset + spec.writeBytes.size();
    if (end < spec.offset) // wrapped
      return failedResult({IoErrorCode::InvalidRequest, 0},
                          terminalProgress(0));
    if (end > content_.size()) content_.resize(end);
    std::ranges::copy(
        spec.writeBytes,
        content_.begin() +
            static_cast<std::vector<std::byte>::difference_type>(spec.offset));
    ++generation_;
    return BackendResult{Complete{{},
                                  std::monostate{},
                                  terminalProgress(spec.writeBytes.size())},
                         {}};
  }

  if (spec.offset >= content_.size())
    return BackendResult{EndOfStream{terminalProgress(0)}, {}};

  const auto available =
      static_cast<std::uint64_t>(content_.size()) - spec.offset;
  auto toRead = std::min(spec.length, available);
  if (context.budget.maxBytes != 0)
    toRead = std::min<std::uint64_t>(toRead, context.budget.maxBytes);
  // length == 0 asks for zero bytes: complete trivially exactly as the
  // file adapter does -- EndOfStream would claim the source is exhausted
  // when the request simply named no bytes.
  if (toRead == 0)
    return BackendResult{Complete{{}, std::monostate{}, terminalProgress(0)},
                         {}};
  std::vector<std::byte> out(
      content_.begin() +
          static_cast<std::vector<std::byte>::difference_type>(spec.offset),
      content_.begin() + static_cast<std::vector<std::byte>::difference_type>(
                             spec.offset + toRead));
  const auto progress = terminalProgress(toRead);
  return payloadOutcome(context.payloads, std::move(out), progress, payload,
                        [progress](const PayloadRange &range, IoProgress) {
                          return Complete{range, std::monostate{}, progress};
                        });
}

// -- ProcessIoAdapter ---------------------------------------------------------

ProcessIoAdapter::ProcessIoAdapter(std::string command)
    : command_(std::move(command)) {}

auto ProcessIoAdapter::validateSpec(const StreamOpSpec &spec) const
    -> std::optional<IoErrorInfo> {
  // A spawned process has no seekable offset; refuse before admission
  // rather than silently ignoring it. Writes have no stdin channel at all.
  if (spec.kind != StreamOpKind::Read)
    return IoErrorInfo{IoErrorCode::Unsupported, 0};
  if (spec.offset != 0) return IoErrorInfo{IoErrorCode::InvalidRequest, 0};
  return std::nullopt;
}

auto ProcessIoAdapter::perform(const StreamOpSpec &spec,
                               BackendContext &context,
                               std::optional<PayloadLease> &payload)
    -> BackendResult {
  if (spec.kind != StreamOpKind::Read)
    return failedResult({IoErrorCode::Unsupported, 0});
  if (spec.offset != 0) return failedResult({IoErrorCode::InvalidRequest, 0});
  // length == 0 asks for zero bytes: complete trivially rather than
  // spawning a process to fill a default chunk the spec never named.
  if (spec.length == 0) {
    return BackendResult{Complete{{}, std::monostate{}, terminalProgress(0)},
                         {}};
  }

  FILE *pipe = ::popen(command_.c_str(), "r");
  if (pipe == nullptr) return failedResult({IoErrorCode::BackendFailure, 0});

  std::vector<std::byte> out;
  auto limit =
      spec.length > 0 ? spec.length : static_cast<std::uint64_t>(1 << 20);
  if (context.budget.maxBytes != 0)
    limit = std::min<std::uint64_t>(limit, context.budget.maxBytes);
  std::array<char, 4096> buffer{};
  bool cancelled = false;
  while (out.size() < limit) {
    // popen cannot be killed mid-read (see the header note), so stop is
    // honored between chunks: the child may run to completion, but the
    // captured prefix still publishes as retained bytes.
    if (context.stop.stop_requested()) {
      cancelled = true;
      break;
    }
    const auto toRead =
        std::min<std::size_t>(buffer.size(), limit - out.size());
    const auto read = std::fread(buffer.data(), 1, toRead, pipe);
    if (read == 0) break;
    const auto *bytes = reinterpret_cast<const std::byte *>(buffer.data());
    out.insert(out.end(), bytes, bytes + read);
  }
  const auto exitStatus = ::pclose(pipe);
  const auto progress   = terminalProgress(out.size());
  if (cancelled) {
    if (out.empty()) return BackendResult{Cancelled{{}, progress, false}, {}};
    return payloadOutcome(context.payloads, std::move(out), progress, payload,
                          [progress](const PayloadRange &range, IoProgress) {
                            return Cancelled{range, progress, false};
                          });
  }
  if (exitStatus != 0) {
    // A nonzero exit is a backend failure; retained output still publishes
    // so the caller sees how much the child produced before failing.
    if (out.empty())
      return failedResult(
          {IoErrorCode::BackendFailure, static_cast<std::uint32_t>(exitStatus)},
          progress);
    return payloadOutcome(context.payloads, std::move(out), progress, payload,
                          [progress](const PayloadRange &range, IoProgress) {
                            return Partial{range, PartialCause::BackendFailure,
                                           progress};
                          });
  }
  if (out.empty()) return BackendResult{EndOfStream{progress}, {}};
  return payloadOutcome(context.payloads, std::move(out), progress, payload,
                        [progress](const PayloadRange &range, IoProgress) {
                          return Complete{range, std::monostate{}, progress};
                        });
}

// -- NetIoAdapter
// --------------------------------------------------------------

auto NetIoAdapter::perform(const StreamOpSpec &spec, BackendContext &context,
                           std::optional<PayloadLease> &payload)
    -> BackendResult {
  // One descriptor, one flight: concurrent ops on this adapter would
  // interleave reads and writes on the same stream, so they serialize.
  const std::lock_guard ioLock(ioMutex_);

  if (spec.kind == StreamOpKind::Write) {
    // The same bound the read clamp and the in-memory/file adapters
    // enforce: maxBytes == 0 reads as unbounded (BackendBudget's
    // convention), a nonzero cap must cover the whole frozen write -- a
    // partial send would misreport progress and leave a half-written
    // stream the caller cannot distinguish from a completed one.
    if (context.budget.maxBytes != 0 &&
        spec.writeBytes.size() > context.budget.maxBytes) {
      return failedResult({IoErrorCode::BudgetExceeded, 0},
                          terminalProgress(0));
    }
    std::size_t sent{};
    while (sent < spec.writeBytes.size()) {
      if (context.stop.stop_requested())
        return BackendResult{Cancelled{{},
                                       IoProgress{.transferredBytes = sent,
                                                  .phase = IoPhase::Terminal},
                                       true},
                             {}};
      // Poll for writability first: a blocking ::write on a full socket
      // would ignore context.stop and the request's deadline entirely.
      pollfd pfd{fd_, POLLOUT, 0};
      const auto ready = ::poll(&pfd, 1, 200);
      if (ready < 0) {
        if (errno == EINTR) continue;
        return failedResult(
            {IoErrorCode::BackendFailure, static_cast<std::uint32_t>(errno)},
            terminalProgress(sent));
      }
      if (ready == 0) {
        // Write window expired; bytes already sent may have landed, so the
        // partial transfer is uncertain.
        return BackendResult{TimedOut{{},
                                      IoProgress{.transferredBytes = sent,
                                                 .phase = IoPhase::Terminal},
                                      sent != 0},
                             {}};
      }
      if (pfd.revents & (POLLERR | POLLNVAL))
        return failedResult({IoErrorCode::BackendFailure, 0},
                            terminalProgress(sent));
      // send(..., MSG_NOSIGNAL): a plain ::write on a closed peer raises
      // SIGPIPE, which kills embedders that do not ignore it.
      const auto result = ::send(fd_, spec.writeBytes.data() + sent,
                                 spec.writeBytes.size() - sent, MSG_NOSIGNAL);
      if (result < 0) {
        if (errno == EINTR) continue;
        return failedResult(
            {IoErrorCode::BackendFailure, static_cast<std::uint32_t>(errno)},
            terminalProgress(sent));
      }
      if (result == 0)
        return failedResult({IoErrorCode::BackendFailure, 0},
                            terminalProgress(sent));
      sent += static_cast<std::size_t>(result);
    }
    return BackendResult{Complete{{}, std::monostate{}, terminalProgress(sent)},
                         {}};
  }

  // length == 0 asks for zero bytes: complete trivially rather than
  // inventing a default chunk the frozen spec never named.
  if (spec.length == 0) {
    return BackendResult{Complete{{}, std::monostate{}, terminalProgress(0)},
                         {}};
  }

  std::vector<std::byte> out;
  auto limit =
      spec.length > 0 ? spec.length : static_cast<std::uint64_t>(65536);
  if (context.budget.maxBytes != 0)
    limit = std::min<std::uint64_t>(limit, context.budget.maxBytes);
  std::array<char, 4096> buffer{};
  bool timedOut = false;
  while (out.size() < limit) {
    if (context.stop.stop_requested()) break;
    pollfd pfd{fd_, POLLIN, 0};
    const auto ready = ::poll(&pfd, 1, 200);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return failedResult(
          {IoErrorCode::BackendFailure, static_cast<std::uint32_t>(errno)},
          terminalProgress(out.size()));
    }
    if (ready == 0) {
      // Poll timeout is not EOF: the peer may simply be idle. Retained
      // bytes still publish; an empty read reports TimedOut, not
      // EndOfStream.
      timedOut = true;
      break;
    }
    if (pfd.revents & (POLLERR | POLLNVAL))
      return failedResult({IoErrorCode::BackendFailure, 0},
                          terminalProgress(out.size()));
    // POLLHUP still reaches read() (which then reports EOF). POLLPRI counts
    // as readable too: urgent data precedes the normal stream on TCP, and
    // treating it as nothing-to-read would spin the poll loop.
    if ((pfd.revents & (POLLIN | POLLHUP | POLLPRI)) == 0) continue;
    const auto toRead =
        std::min<std::size_t>(buffer.size(), limit - out.size());
    const auto received = ::read(fd_, buffer.data(), toRead);
    if (received < 0) {
      if (errno == EINTR) continue;
      return failedResult(
          {IoErrorCode::BackendFailure, static_cast<std::uint32_t>(errno)},
          terminalProgress(out.size()));
    }
    if (received == 0) break; // orderly EOF
    const auto *bytes = reinterpret_cast<const std::byte *>(buffer.data());
    out.insert(out.end(), bytes, bytes + received);
  }

  const auto progress = terminalProgress(out.size());
  if (context.stop.stop_requested()) {
    if (out.empty()) return BackendResult{Cancelled{{}, progress, false}, {}};
    return payloadOutcome(context.payloads, std::move(out), progress, payload,
                          [progress](const PayloadRange &range, IoProgress) {
                            return Cancelled{range, progress, false};
                          });
  }
  if (timedOut) {
    if (out.empty()) return BackendResult{TimedOut{{}, progress, false}, {}};
    return payloadOutcome(context.payloads, std::move(out), progress, payload,
                          [progress](const PayloadRange &range, IoProgress) {
                            return TimedOut{range, progress, false};
                          });
  }
  if (out.empty()) return BackendResult{EndOfStream{progress}, {}};
  return payloadOutcome(context.payloads, std::move(out), progress, payload,
                        [progress](const PayloadRange &range, IoProgress) {
                          return Complete{range, std::monostate{}, progress};
                        });
}

// -- UnionSourceAdapter -------------------------------------------------------

auto UnionSourceAdapter::prepare(const CellRequest &request,
                                 OwnerContext &context)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  std::size_t index = 0;
  try {
    index = select_ ? select_(request) : 0;
  } catch (...) {
    // A throwing selector must not escape prepare() into IoService::admit;
    // convert it to the same typed refusal an out-of-range index gets.
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
  }
  if (index >= members_.size() || members_[index] == nullptr)
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});

  auto memberPrepared = members_[index]->prepare(request, context);
  if (!memberPrepared) return std::unexpected(memberPrepared.error());

  // Record the route so abandonStaged can forward to this member alone:
  // route_ is keyed by token (unknown until enqueue) and publish may
  // already have consumed it by the time a terminal abandon runs.
  {
    const std::lock_guard lock(routeMutex_);
    stagedRoute_[static_cast<std::uint32_t>(request.specification)] = index;
  }

  const auto memberIndex = static_cast<std::uint32_t>(index);
  std::vector<std::byte> bytes(sizeof(memberIndex) +
                               memberPrepared->bytes.size());
  std::memcpy(bytes.data(), &memberIndex, sizeof(memberIndex));
  if (!memberPrepared->bytes.empty())
    std::memcpy(bytes.data() + sizeof(memberIndex),
                memberPrepared->bytes.data(), memberPrepared->bytes.size());
  return PreparedOperation{request.key, std::move(bytes)};
}

void UnionSourceAdapter::execute(const BackendCommand &command,
                                 BackendContext &context,
                                 CompletionSink &sink) noexcept {
  std::uint32_t index{};
  std::vector<std::byte> memberBytes;
  if (command.operation.bytes.size() >= sizeof(index)) {
    std::memcpy(&index, command.operation.bytes.data(), sizeof(index));
    memberBytes.assign(command.operation.bytes.begin() + sizeof(index),
                       command.operation.bytes.end());
  }
  if (index >= members_.size() || members_[index] == nullptr) {
    sink.terminalCandidate(
        command.token,
        BackendResult{Failed{{IoErrorCode::InvalidRequest, 0},
                             {},
                             IoProgress{.phase = IoPhase::Terminal}},
                      {}},
        {});
    // The request was already admitted, so the reserved terminal slot
    // needs its release acknowledgement or the completion never
    // materializes (same contract every other adapter's execute keeps).
    sink.releaseAcknowledged({PrivateReleaseToken{command.token}});
    return;
  }
  {
    const std::lock_guard lock(routeMutex_);
    route_[command.token.value] = index;
  }
  const BackendCommand memberCommand{
      command.token, PreparedOperation{command.operation.key, memberBytes}};
  members_[index]->execute(memberCommand, context, sink);
}

auto UnionSourceAdapter::publish(const NativeCompletion &completion,
                                 OwnerContext &context)
    -> std::expected<OwnerPublication, IoErrorInfo> {
  std::size_t index{};
  {
    const std::lock_guard lock(routeMutex_);
    const auto it = route_.find(completion.token.value);
    if (it == route_.end()) {
      // No route means execute() never ran for this token -- the scheduler
      // cancelled it before dispatch. The completion already carries the
      // correct terminal outcome (e.g. Cancelled); forwarding it unchanged
      // keeps a pre-start cancel from being rewritten to Failed.
      return OwnerPublication{completion.result.outcome};
    }
    index = it->second;
    route_.erase(it);
  }
  if (index >= members_.size() || members_[index] == nullptr)
    return OwnerPublication{completion.result.outcome};
  return members_[index]->publish(completion, context);
}

void UnionSourceAdapter::abandonStaged(const OwnerCell specification) noexcept {
  const auto key = static_cast<std::uint32_t>(specification);
  std::optional<std::size_t> routed;
  {
    const std::lock_guard lock(routeMutex_);
    if (const auto it = stagedRoute_.find(key); it != stagedRoute_.end()) {
      routed = it->second;
      stagedRoute_.erase(it);
    }
  }
  if (routed && *routed < members_.size() && members_[*routed] != nullptr) {
    members_[*routed]->abandonStaged(specification);
    return;
  }
  // No recorded route: the spec never prepared through this union (a
  // pre-prepare refusal, or a spec staged on a member directly). Broadcast
  // is the only option left -- each member's abandon is a no-op for keys
  // it never held.
  for (auto *member : members_) {
    if (member != nullptr) member->abandonStaged(specification);
  }
}

} // namespace xanadu::vortex
