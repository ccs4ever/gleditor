#include "common/xanadu/vortex/source_adapters.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace xanadu::vortex {

namespace {

IoProgress terminalProgress(std::uint64_t transferred) {
  return IoProgress{.acceptedBytes    = transferred,
                    .transferredBytes = transferred,
                    .consumedBytes    = transferred,
                    .phase            = IoPhase::Terminal};
}

BackendResult conflictResult(std::uint64_t expected, std::uint64_t observed,
                             IoProgress progress) {
  return BackendResult{
      Conflict{Generation{expected}, Generation{observed}, progress}, {}};
}

} // namespace

std::uint32_t StagedByteSourceAdapter::stage(StreamOpSpec spec) {
  const std::lock_guard lock(mutex_);
  const auto key = nextKey_++;
  staged_[key]   = std::move(spec);
  return key;
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
  if (spec.expectedGeneration != 0 && current != 0 &&
      spec.expectedGeneration != current)
    return std::unexpected(IoErrorInfo{IoErrorCode::Stale});

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

  std::optional<StreamOpSpec> spec;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it != staged_.end()) {
      spec = it->second;
      staged_.erase(it);
    }
  }

  BackendResult result;
  if (!spec) {
    result =
        BackendResult{Failed{{IoErrorCode::InvalidRequest, 0}, {}, {}}, {}};
  } else {
    try {
      result = perform(*spec, context);
    } catch (...) {
      result =
          BackendResult{Failed{{IoErrorCode::BackendFailure, 0}, {}, {}}, {}};
    }
  }
  sink.terminalCandidate(command.token, std::move(result), {});
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

auto InMemoryStreamAdapter::perform(const StreamOpSpec &spec, BackendContext &)
    -> BackendResult {
  const std::lock_guard lock(stateMutex_);

  if (spec.expectedGeneration != 0 && spec.expectedGeneration != generation_)
    return conflictResult(spec.expectedGeneration, generation_,
                          IoProgress{.phase = IoPhase::Terminal});

  if (spec.kind == StreamOpKind::Write) {
    const auto end = spec.offset + spec.writeBytes.size();
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
  const auto toRead = std::min(spec.length, available);
  std::vector<std::byte> out(
      content_.begin() +
          static_cast<std::vector<std::byte>::difference_type>(spec.offset),
      content_.begin() + static_cast<std::vector<std::byte>::difference_type>(
                             spec.offset + toRead));
  return BackendResult{Complete{{}, std::move(out), terminalProgress(toRead)},
                       {}};
}

// -- ProcessIoAdapter ---------------------------------------------------------

ProcessIoAdapter::ProcessIoAdapter(std::string command)
    : command_(std::move(command)) {}

auto ProcessIoAdapter::perform(const StreamOpSpec &spec, BackendContext &)
    -> BackendResult {
  if (spec.kind != StreamOpKind::Read)
    return BackendResult{Failed{{IoErrorCode::Unsupported, 0}, {}, {}}, {}};

  FILE *pipe = ::popen(command_.c_str(), "r");
  if (pipe == nullptr)
    return BackendResult{Failed{{IoErrorCode::BackendFailure, 0}, {}, {}}, {}};

  std::vector<std::byte> out;
  const auto limit =
      spec.length > 0 ? spec.length : static_cast<std::uint64_t>(1 << 20);
  std::array<char, 4096> buffer{};
  while (out.size() < limit) {
    const auto toRead =
        std::min<std::size_t>(buffer.size(), limit - out.size());
    const auto read = std::fread(buffer.data(), 1, toRead, pipe);
    if (read == 0) break;
    const auto *bytes = reinterpret_cast<const std::byte *>(buffer.data());
    out.insert(out.end(), bytes, bytes + read);
  }
  const auto exitStatus = ::pclose(pipe);
  if (exitStatus != 0 && out.empty())
    return BackendResult{Failed{{IoErrorCode::BackendFailure,
                                 static_cast<std::uint32_t>(exitStatus)},
                                {},
                                {}},
                         {}};
  return BackendResult{
      Complete{{}, std::move(out), terminalProgress(out.size())}, {}};
}

// -- NetIoAdapter
// --------------------------------------------------------------

auto NetIoAdapter::perform(const StreamOpSpec &spec, BackendContext &context)
    -> BackendResult {
  if (spec.kind == StreamOpKind::Write) {
    std::size_t sent{};
    while (sent < spec.writeBytes.size()) {
      if (context.stop.stop_requested())
        return BackendResult{Cancelled{{}, {}, true}, {}};
      const auto result = ::write(fd_, spec.writeBytes.data() + sent,
                                  spec.writeBytes.size() - sent);
      if (result <= 0)
        return BackendResult{Failed{{IoErrorCode::BackendFailure, 0}, {}, {}},
                             {}};
      sent += static_cast<std::size_t>(result);
    }
    return BackendResult{Complete{{}, std::monostate{}, terminalProgress(sent)},
                         {}};
  }

  std::vector<std::byte> out;
  const auto limit =
      spec.length > 0 ? spec.length : static_cast<std::uint64_t>(65536);
  std::array<char, 4096> buffer{};
  while (out.size() < limit) {
    pollfd pfd{fd_, POLLIN, 0};
    const auto ready = ::poll(&pfd, 1, 200);
    if (ready <= 0 || context.stop.stop_requested()) break;
    const auto toRead =
        std::min<std::size_t>(buffer.size(), limit - out.size());
    const auto received = ::read(fd_, buffer.data(), toRead);
    if (received <= 0) break;
    const auto *bytes = reinterpret_cast<const std::byte *>(buffer.data());
    out.insert(out.end(), bytes, bytes + received);
  }
  if (out.empty()) return BackendResult{EndOfStream{terminalProgress(0)}, {}};
  return BackendResult{
      Complete{{}, std::move(out), terminalProgress(out.size())}, {}};
}

} // namespace xanadu::vortex
