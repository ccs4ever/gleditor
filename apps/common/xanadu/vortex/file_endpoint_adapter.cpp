#include "common/xanadu/vortex/file_endpoint_adapter.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vortex {
namespace {

/// Open command encoding: [flags:1][locator bytes]. flags bit0 is writable,
/// taken from `OpenSpec::requestedRights & fileRightWrite` at prepare time.
constexpr std::uint8_t openFlagWritable = 1;

IoProgress terminalProgress(std::uint64_t transferred = 0) {
  return IoProgress{.acceptedBytes    = transferred,
                    .transferredBytes = transferred,
                    .consumedBytes    = transferred,
                    .phase            = IoPhase::Terminal};
}

void reportTerminal(CompletionSink &sink, PrivateRequestToken token,
                    IoOutcome outcome) noexcept {
  sink.terminalCandidate(token, BackendResult{std::move(outcome), {}}, {});
  sink.releaseAcknowledged({PrivateReleaseToken{token}});
}

} // namespace

std::expected<std::uint32_t, std::string>
FileEndpointAdapter::stageAt(std::uint32_t key, FileEndpointOp spec) {
  const std::lock_guard lock(mutex_);
  const auto it = files_.find(spec.locator);
  if (it == files_.end()) return std::unexpected("no opened file for locator");
  // FileSourceAdapter::stageAt holds its own mutex_; the endpoint mutex is
  // the outer lock in the only endpoint->file order any path takes, so
  // this cannot deadlock.
  auto staged = it->second->stageAt(key, spec.op);
  if (!staged) return staged;
  stagedLocator_[key] = spec.locator;
  return *staged;
}

void FileEndpointAdapter::abandon(const std::uint32_t key) {
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto route = stagedLocator_.find(key);
    if (route == stagedLocator_.end()) return;
    const auto it = files_.find(route->second);
    if (it != files_.end()) file = it->second;
    stagedLocator_.erase(route);
  }
  // The per-file staged entry is keyed by the same spec cell ref, so the
  // file adapter drops exactly the op this route named.
  if (file) file->abandon(key);
}

void FileEndpointAdapter::abandonStaged(
    const OwnerCell specification) noexcept {
  abandon(static_cast<std::uint32_t>(specification));
}

auto FileEndpointAdapter::prepare(const CellRequest &request,
                                  OwnerContext &context)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  if (request.open) {
    if (!request.openSpec)
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    const auto &spec = *request.openSpec;
    // FileSourceAdapter declares Live only (plan §7.2): a request whose
    // consistency mask is nonzero and lacks the Live bit demands a
    // consistency the backend cannot meet -- refuse before admission.
    if (spec.consistency != 0 && (spec.consistency & ioConsistencyLive) == 0)
      return std::unexpected(IoErrorInfo{IoErrorCode::Unsupported});
    const bool writable = (spec.requestedRights & fileRightWrite) != 0;
    std::vector<std::byte> bytes;
    bytes.reserve(spec.locator.size() + 1);
    bytes.push_back(static_cast<std::byte>(writable ? openFlagWritable : 0));
    bytes.insert(bytes.end(), spec.locator.begin(), spec.locator.end());
    return PreparedOperation{request.key, std::move(bytes)};
  }
  // Data op: route through the staged key to the file that owns it, then
  // delegate the whole prepare -- generation check, StableRead/writable
  // refusals, command encoding -- to that file's adapter.
  const auto key = static_cast<std::uint32_t>(request.specification);
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto route = stagedLocator_.find(key);
    if (route == stagedLocator_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    const auto it = files_.find(route->second);
    if (it == files_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    file = it->second;
  }
  return file->prepare(request, context);
}

void FileEndpointAdapter::execute(const BackendCommand &command,
                                  BackendContext &context,
                                  CompletionSink &sink) noexcept {
  if (command.operation.key == fileOpenAdapterKey) {
    const auto &bytes = command.operation.bytes;
    if (bytes.empty()) {
      reportTerminal(
          sink, command.token,
          Failed{{IoErrorCode::InvalidRequest, 0}, {}, terminalProgress()});
      return;
    }
    const bool writable =
        (static_cast<std::uint8_t>(bytes.front()) & openFlagWritable) != 0;
    const std::string locator(reinterpret_cast<const char *>(bytes.data() + 1),
                              bytes.size() - 1);
    // The open itself runs here, on the scheduler worker -- never the
    // owner thread (plan §7.2). The descriptor is adopted by the per-file
    // adapter so every later op stays on this inode.
    const int fd = ::open(locator.c_str(), writable ? O_RDWR : O_RDONLY);
    if (fd < 0) {
      reportTerminal(sink, command.token,
                     Failed{{IoErrorCode::BackendFailure,
                             static_cast<std::uint32_t>(errno)},
                            {},
                            terminalProgress()});
      return;
    }
    auto opened = std::make_shared<FileSourceAdapter>(fd, locator, writable);
    {
      const std::lock_guard lock(mutex_);
      files_[locator] = std::move(opened);
    }
    // The service mints the endpoint/capability cells and overwrites
    // Complete.value for an open request; monostate is the placeholder.
    reportTerminal(sink, command.token,
                   Complete{{}, std::monostate{}, terminalProgress()});
    return;
  }
  // Data op: decode the staged key prepare encoded, route to the owning
  // file and forward the SAME command. FileSourceAdapter::execute consumes
  // its staged entry, performs the transfer and reports
  // terminalCandidate/releaseAcknowledged itself, so this adapter must not
  // report again.
  std::uint32_t key{};
  if (command.operation.bytes.size() >= sizeof(key))
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto route = stagedLocator_.find(key);
    if (route != stagedLocator_.end()) {
      const auto locator = route->second;
      stagedLocator_.erase(route);
      const auto it = files_.find(locator);
      if (it != files_.end()) file = it->second;
    }
  }
  if (!file) {
    reportTerminal(
        sink, command.token,
        Failed{{IoErrorCode::InvalidRequest, 0}, {}, terminalProgress()});
    return;
  }
  file->execute(command, context, sink);
}

auto FileEndpointAdapter::publish(const NativeCompletion &completion,
                                  OwnerContext &)
    -> std::expected<OwnerPublication, IoErrorInfo> {
  return OwnerPublication{completion.result.outcome};
}

auto FileEndpointAdapter::capabilities(std::string_view locator) const
    -> std::optional<FileOpenCapabilities> {
  const std::lock_guard lock(mutex_);
  const auto it = files_.find(std::string(locator));
  if (it == files_.end()) return std::nullopt;
  return it->second->probeCapabilities();
}

auto FileEndpointAdapter::openedCount() const -> std::size_t {
  const std::lock_guard lock(mutex_);
  return files_.size();
}

auto FileEndpointAdapter::fileFor(std::string_view locator)
    -> FileSourceAdapter * {
  const std::lock_guard lock(mutex_);
  const auto it = files_.find(std::string(locator));
  return it == files_.end() ? nullptr : it->second.get();
}

auto makeFileRequestDecoder(zigzag::ArenaManifold &arena, AdapterKey dataKey,
                            std::size_t payloadBoundBytes)
    -> std::function<std::expected<CellRequest, IoErrorInfo>(CapabilityCell,
                                                             OwnerCell)> {
  return [&arena, dataKey, payloadBoundBytes](CapabilityCell capability,
                                              OwnerCell specification)
             -> std::expected<CellRequest, IoErrorInfo> {
    const auto text                  = arena.textOf(specification);
    CellRequest::Direction direction = CellRequest::Direction::Read;
    if (text == fileWriteSpecTag) {
      direction = CellRequest::Direction::Write;
    } else if (text != fileReadSpecTag && text != fileStableSpecTag) {
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    }
    return CellRequest{
        .key        = dataKey,
        .capability = capability,
        // Reads may retain up to payloadBoundBytes; writes produce no
        // payload, so they declare none (completionBoundBytes still covers
        // diagnostics -- at least one bound must be nonzero).
        .completionBoundBytes = 256,
        .payloadBoundBytes =
            direction == CellRequest::Direction::Write ? 0 : payloadBoundBytes,
        .direction     = direction,
        .specification = specification};
  };
}

} // namespace xanadu::vortex
