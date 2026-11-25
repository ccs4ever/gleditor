#include "common/xanadu/vortex/file_endpoint_adapter.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <gleditor/logging.hpp>

#include "common/xanadu/vortex/arena_io_cell_store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vortex {
namespace {

/// Open command encoding: [flags:1][locator bytes]. flags bit0 is writable,
/// taken from `OpenSpec::requestedRights & fileRightWrite` at prepare time.
constexpr std::uint8_t openFlagWritable = 1;

/// Spdlog category for this adapter's diagnostics.
constexpr const char *fileLogCategory = "vortex.io.file";

/// Declared bound on staged diagnostic bytes a file request's terminal
/// outcome may carry. File outcomes carry typed fields, not bytes, so this
/// only has to be nonzero (the service refuses a request with no output
/// envelope when a write declares no payload) and cover a short
/// diagnostic; 256 bytes holds an error description with room to spare
/// without letting a request reserve real budget.
constexpr std::size_t fileCompletionBoundBytes = 256;

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

std::expected<StageKey, std::string>
FileEndpointAdapter::stageAt(const StageKey key, FileEndpointOp spec) {
  const std::lock_guard lock(mutex_);
  const auto it = files_.find(spec.open);
  if (it == files_.end()) return std::unexpected("no live open for this id");
  // A duplicate must be refused before the file adapter sees it: two opens
  // have separate staging tables, so only this map notices the same key
  // staged against a different file, and overwriting the route would send
  // the first op's execute to the wrong inode.
  if (stagedOpen_.contains(key))
    return std::unexpected("staging key already in use");
  // FileSourceAdapter::stageAt holds its own mutex_; the endpoint mutex is
  // the outer lock in the only endpoint->file order any path takes, so
  // this cannot deadlock.
  auto staged = it->second.file->stageAt(key, std::move(spec.op));
  if (!staged) return staged;
  stagedOpen_.emplace(key, spec.open);
  return *staged;
}

void FileEndpointAdapter::abandon(const StageKey key) {
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto route = stagedOpen_.find(key);
    if (route == stagedOpen_.end()) return;
    const auto it = files_.find(route->second);
    if (it != files_.end()) file = it->second.file;
    stagedOpen_.erase(route);
  }
  // The per-file staged entry is keyed by the same spec cell ref, so the
  // file adapter drops exactly the op this route named.
  if (file) file->abandon(key);
}

auto FileEndpointAdapter::release(const FileOpenId open)
    -> std::expected<FileEndpointAdapter *, std::string> {
  std::shared_ptr<FileSourceAdapter> file;
  std::vector<StageKey> dropped;
  {
    const std::lock_guard lock(mutex_);
    const auto it = files_.find(open);
    if (it == files_.end()) return std::unexpected("no live open for this id");
    file = std::move(it->second.file);
    files_.erase(it);
    std::erase_if(bound_, [open](const auto &entry) {
      return entry.second.open == open;
    });
    for (auto route = stagedOpen_.begin(); route != stagedOpen_.end();) {
      if (route->second == open) {
        dropped.push_back(route->first);
        route = stagedOpen_.erase(route);
      } else {
        ++route;
      }
    }
  }
  for (const auto key : dropped) file->abandon(key);
  GLEDITOR_LOG_DEBUG(fileLogCategory, "released open {} ({} staged dropped)",
                     open.value, dropped.size());
  return this;
}

void FileEndpointAdapter::abandonStaged(
    const OwnerCell specification) noexcept {
  abandon(specification);
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
  if (request.closeOperation) {
    if (!request.endpoint)
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    const auto open = openIdOf(*request.endpoint);
    if (!open) return std::unexpected(IoErrorInfo{IoErrorCode::Stale});
    std::vector<std::byte> bytes(sizeof(open->value));
    std::memcpy(bytes.data(), &open->value, sizeof(open->value));
    return PreparedOperation{request.key, std::move(bytes)};
  }
  // Data op: route through the staged key to the file that owns it, then
  // delegate the whole prepare -- generation check, StableRead/writable
  // refusals, command encoding -- to that file's adapter.
  const StageKey key = request.specification;
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto route = stagedOpen_.find(key);
    if (route == stagedOpen_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    const auto it = files_.find(route->second);
    if (it == files_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    file = it->second.file;
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
      GLEDITOR_LOG_DEBUG(fileLogCategory, "open failed (errno {})", errno);
      reportTerminal(sink, command.token,
                     Failed{{IoErrorCode::BackendFailure,
                             static_cast<std::uint32_t>(errno)},
                            {},
                            terminalProgress()});
      return;
    }
    // Everything that can throw sits in this try: execute is noexcept.
    std::shared_ptr<FileSourceAdapter> opened;
    try {
      opened = std::make_shared<FileSourceAdapter>(fd, locator, writable);
      const std::lock_guard lock(mutex_);
      files_.emplace(nextOpenId_, OpenFile{locator, opened});
      pendingOpens_.emplace(command.token.value, nextOpenId_);
      ++nextOpenId_.value;
    } catch (...) {
      GLEDITOR_LOG_WARN(fileLogCategory,
                        "open registration threw; reporting BackendFailure");
      // Only a constructed adapter owns (and closes) the descriptor.
      if (!opened) ::close(fd);
      reportTerminal(
          sink, command.token,
          Failed{{IoErrorCode::BackendFailure, 0}, {}, terminalProgress()});
      return;
    }
    // The service mints the endpoint/capability cells and overwrites
    // Complete.value for an open request; monostate is the placeholder.
    reportTerminal(sink, command.token,
                   Complete{{}, std::monostate{}, terminalProgress()});
    return;
  }
  if (command.operation.key == fileCloseAdapterKey) {
    FileOpenId open{};
    if (command.operation.bytes.size() >= sizeof(open.value))
      std::memcpy(&open.value, command.operation.bytes.data(),
                  sizeof(open.value));
    // In-flight data ops hold their own shared_ptr to the file, so the
    // descriptor closes only after the last of them lets go.
    const auto released = release(open);
    reportTerminal(
        sink, command.token,
        released ? IoOutcome{Complete{{}, std::monostate{}, terminalProgress()}}
                 : IoOutcome{Failed{
                       {IoErrorCode::Stale, 0}, {}, terminalProgress()}});
    return;
  }
  // Data op: decode the staged key prepare encoded, route to the owning
  // file and forward the SAME command. FileSourceAdapter::execute consumes
  // its staged entry, performs the transfer and reports
  // terminalCandidate/releaseAcknowledged itself, so this adapter must not
  // report again.
  StageKey key{};
  if (command.operation.bytes.size() >= sizeof(key))
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto route = stagedOpen_.find(key);
    if (route != stagedOpen_.end()) {
      const auto open = route->second;
      stagedOpen_.erase(route);
      const auto it = files_.find(open);
      if (it != files_.end()) file = it->second.file;
    }
  }
  if (!file) {
    GLEDITOR_LOG_DEBUG(fileLogCategory,
                       "data op dropped: staged key {} has no live open", key);
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

auto FileEndpointAdapter::capabilities(const FileOpenId open) const
    -> std::optional<FileOpenCapabilities> {
  std::shared_ptr<FileSourceAdapter> file;
  {
    const std::lock_guard lock(mutex_);
    const auto it = files_.find(open);
    if (it == files_.end()) return std::nullopt;
    file = it->second.file;
  }
  return file->probeCapabilities();
}

auto FileEndpointAdapter::openedCount() const -> std::size_t {
  const std::lock_guard lock(mutex_);
  return files_.size();
}

void FileEndpointAdapter::openSettled(
    const PrivateRequestToken request,
    const std::optional<OpenedEndpoint> &opened) noexcept {
  try {
    std::shared_ptr<FileSourceAdapter> dropped;
    const std::lock_guard lock(mutex_);
    const auto pending = pendingOpens_.find(request.value);
    if (pending == pendingOpens_.end()) return;
    const auto open = pending->second;
    pendingOpens_.erase(pending);
    if (opened) {
      bound_.insert_or_assign(
          EndpointKey{opened->endpoint.cell, opened->endpoint.incarnation},
          Binding{open, opened->initialCapability});
      return;
    }
    // The open completed natively but no endpoint was minted for it:
    // nothing can ever address it, so let the descriptor go.
    if (const auto it = files_.find(open); it != files_.end()) {
      dropped = std::move(it->second.file);
      files_.erase(it);
    }
  } catch (...) {
    GLEDITOR_LOG_WARN(fileLogCategory, "openSettled threw; binding skipped");
  }
}

auto FileEndpointAdapter::openIdOf(const EndpointCell endpoint) const
    -> std::optional<FileOpenId> {
  const std::lock_guard lock(mutex_);
  const auto it = bound_.find(EndpointKey{endpoint.cell, endpoint.incarnation});
  if (it == bound_.end()) return std::nullopt;
  return it->second.open;
}

auto FileEndpointAdapter::capabilityOf(const EndpointCell endpoint) const
    -> std::optional<CapabilityCell> {
  const std::lock_guard lock(mutex_);
  const auto it = bound_.find(EndpointKey{endpoint.cell, endpoint.incarnation});
  if (it == bound_.end()) return std::nullopt;
  return it->second.capability;
}

auto FileEndpointAdapter::fileFor(const FileOpenId open)
    -> std::shared_ptr<FileSourceAdapter> {
  const std::lock_guard lock(mutex_);
  const auto it = files_.find(open);
  return it == files_.end() ? nullptr : it->second.file;
}

auto makeFileCloseDecoder(const FileEndpointAdapter &adapter)
    -> std::function<std::expected<CellRequest, IoErrorInfo>(EndpointCell,
                                                             CloseMode)> {
  return [&adapter](const EndpointCell endpoint,
                    CloseMode) -> std::expected<CellRequest, IoErrorInfo> {
    const auto capability = adapter.capabilityOf(endpoint);
    if (!capability) return std::unexpected(IoErrorInfo{IoErrorCode::Stale});
    return CellRequest{.key                  = fileCloseAdapterKey,
                       .capability           = *capability,
                       .endpoint             = endpoint,
                       .completionBoundBytes = fileCompletionBoundBytes,
                       .closeOperation       = true,
                       .direction            = CellRequest::Direction::Control};
  };
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
        .completionBoundBytes = fileCompletionBoundBytes,
        .payloadBoundBytes =
            direction == CellRequest::Direction::Write ? 0 : payloadBoundBytes,
        .direction     = direction,
        .specification = specification};
  };
}

} // namespace xanadu::vortex
