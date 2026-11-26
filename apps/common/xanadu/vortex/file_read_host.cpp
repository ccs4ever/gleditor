#include "common/xanadu/vortex/file_read_host.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

auto bytesOf(const std::string_view text) -> std::vector<std::byte> {
  std::vector<std::byte> out(text.size());
  if (!text.empty()) std::memcpy(out.data(), text.data(), text.size());
  return out;
}

/// The endpoint and capability an open outcome minted, read back through the
/// outcome's own d.io.endpoint / d.io.capability fields: owner -field->
/// occurrence -field-> clone view, whose clone master is the canonical cell.
auto openedEndpointOf(const zigzag::ArenaManifold &arena,
                      const IoCellSchema &schema, const OutcomeCell outcome)
    -> std::optional<OpenedEndpoint> {
  const auto field = [&](const IoDimension which) -> std::optional<CellRef> {
    const auto dim = schema.dim(which);
    const auto occ = arena.linked(outcome.cell, dim, DimVector::POS);
    if (!occ) return std::nullopt;
    const auto view = arena.linked(*occ, dim, DimVector::POS);
    if (!view) return std::nullopt;
    const auto canonical =
        arena.cloneMaster(*view, schema.dim(IoDimension::Clone));
    if (!canonical || *canonical == *view) return std::nullopt;
    return *canonical;
  };
  const auto endpoint   = field(IoDimension::Endpoint);
  const auto capability = field(IoDimension::Capability);
  if (!endpoint || !capability) return std::nullopt;
  const auto endpointInc   = arena.incarnationOf(*endpoint);
  const auto capabilityInc = arena.incarnationOf(*capability);
  if (!endpointInc || !capabilityInc) return std::nullopt;
  return OpenedEndpoint{EndpointCell{*endpoint, *endpointInc},
                        CapabilityCell{*capability, *capabilityInc}};
}

/// The canonical form of @p locator: symlinks and `..` in whatever part of
/// the path exists are resolved, the rest is lexically normalised. nullopt
/// for a locator that cannot be judged: one with an embedded NUL (the OS
/// would open only the prefix before it, a different path from the one the
/// grant saw) or one the filesystem refuses to resolve.
auto canonicalLocator(const std::string_view locator)
    -> std::optional<std::filesystem::path> {
  if (locator.empty() || locator.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(
      std::filesystem::path{std::string{locator}}, ec);
  if (ec) return std::nullopt;
  return canonical;
}

/// True when @p path has @p root as a proper prefix by whole components.
auto strictlyBeneath(const std::filesystem::path &path,
                     const std::filesystem::path &root) -> bool {
  const auto mismatch =
      std::mismatch(root.begin(), root.end(), path.begin(), path.end());
  return mismatch.first == root.end() && mismatch.second != path.end();
}

/// The open grant of a file host: read always, plus whatever of the
/// mutating rights the request asked for and @p writeGrant approves for the
/// canonical locator. The refusal is a typed value (`Unsupported`, detail
/// `fileDetailNoWriteGrant`) the language sees as a `Rejected` outcome.
auto makeOpenGrant(WriteGrant writeGrant) -> ArenaIoCellStore::OpenAuthorizer {
  return [grant = std::move(writeGrant)](const OpenSpec &spec)
             -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
    const auto wanted = spec.requestedRights & fileRightsMutating;
    auto rights       = fileRightRead;
    if (wanted != 0) {
      const auto denied = std::unexpected{
          IoErrorInfo{IoErrorCode::Unsupported, fileDetailNoWriteGrant}};
      if (!grant) return denied;
      const auto canonical = canonicalLocator(
          std::string_view{reinterpret_cast<const char *>(spec.locator.data()),
                           spec.locator.size()});
      if (!canonical) return denied;
      const auto allowed = grant(canonical->string());
      if ((wanted & ~allowed) != 0) return denied;
      // Narrowed to what was asked, never widened to what the policy would
      // have allowed.
      rights |= wanted;
    }
    return ArenaIoCellStore::OpenGrant{.rights      = rights,
                                       .consistency = ioConsistencyLive};
  };
}

auto makeService(const FileReadHostOptions &options, PayloadResolver &payloads,
                 IoCellStore &cells) -> std::unique_ptr<IoService> {
  auto service = std::make_unique<IoService>(
      IoBudgetSnapshot{options.requests, options.budgetBytes,
                       options.budgetBytes, options.budgetBytes,
                       options.endpointRequests},
      std::make_unique<BoundedIoScheduler>(
          BoundedIoSchedulerOptions{options.workers, options.outstanding,
                                    options.budgetBytes, options.budgetBytes},
          payloads),
      payloads, cells);
  return service;
}

} // namespace

auto writeRootGrant(const std::vector<std::filesystem::path> &roots,
                    const std::uint64_t rights) -> WriteGrant {
  std::vector<std::filesystem::path> canonicalRoots;
  for (const auto &root : roots) {
    if (auto canonical = canonicalLocator(root.string())) {
      // weakly_canonical keeps the empty last element of "dir/" when dir does
      // not exist yet, which would never match a file name beneath it.
      if (!canonical->has_filename()) *canonical = canonical->parent_path();
      canonicalRoots.push_back(*std::move(canonical));
    }
  }
  return [canonicalRoots = std::move(canonicalRoots),
          rights](const std::string_view locator) -> std::uint64_t {
    const auto canonical = canonicalLocator(locator);
    if (!canonical) return 0;
    const bool inside =
        std::ranges::any_of(canonicalRoots, [&](const auto &root) {
          return strictlyBeneath(*canonical, root);
        });
    return inside ? rights : 0;
  };
}

std::string_view toString(const StageReadError error) noexcept {
  switch (error) {
  case StageReadError::NotAFileCapability:
    return "NotAFileCapability";
  case StageReadError::OutOfBounds:
    return "OutOfBounds";
  case StageReadError::Refused:
    return "Refused";
  }
  return "Unknown";
}

FileReadHost::FileReadHost(zigzag::vortex::VortexVM &vm,
                           zigzag::ArenaManifold &arena,
                           FileReadHostOptions options)
    : arena_(arena), options_(options),
      schema_(makeIoCellSchema(arena).value()),
      cells_(
          arena, schema_, payloads_,
          [this, files = makeFileRequestDecoder(arena, fileDataAdapterKey,
                                                options.payloadBytes)](
              const CapabilityCell capability, const OwnerCell specification)
              -> std::expected<CellRequest, IoErrorInfo> {
            // A capture's spec cells are the coordinator's to recognise;
            // everything else is a file operation.
            if (capture_) {
              if (auto request = capture_->coordinator().decodeAppendRequest(
                      capability, specification))
                return *request;
            }
            return files(capability, specification);
          },
          makeFileCloseDecoder(adapter_), makeOpenGrant(options.writeGrant)),
      service_(makeService(options, payloads_, cells_)),
      host_(*service_, arena, vm),
      diagnostics_(FrontendDiagnosticRegistry::withBuiltins()),
      bridge_(host_, arena, schema_, diagnostics_) {
  if (options_.clock) service_->setClock(options_.clock);
  service_->registerAdapter(fileOpenAdapterKey, adapter_);
  service_->registerAdapter(fileDataAdapterKey, adapter_);
  service_->registerAdapter(fileCloseAdapterKey, adapter_);
}

FileReadHost::~FileReadHost() {
  if (service_) {
    static_cast<void>(service_->requestShutdown(ShutdownMode::CancelPending));
  }
  // The capture host outlives the service (the workers run the coordinator's
  // adapter until the service joins them), so its runs must be abandoned here,
  // while the service they reap through is still alive.
  if (capture_) capture_->abandonAll();
}

auto FileReadHost::pump(const std::size_t budget) -> std::size_t {
  const auto work = host_.pumpIo(budget);
  std::erase_if(deferredReaps_, [&](const DeferredReap &deferred) {
    if (arena_.matches(deferred.request.cell, deferred.request.incarnation)) {
      // A no-op while the service still tracks the request or a view names it.
      host_.reapRetiredRequest(deferred.request);
      if (arena_.matches(deferred.request.cell, deferred.request.incarnation))
        return false;
    }
    // One attempt: with the request gone only a view some other owner holds
    // can still refuse, and that owner reaps its own side.
    static_cast<void>(reapIoEndpoint(arena_, schema_, deferred.endpoint));
    return true;
  });
  return work;
}

void FileReadHost::reapWhenEnded(const RequestCell closeRequest,
                                 const EndpointCell endpoint) {
  deferredReaps_.push_back({closeRequest, endpoint});
}

auto FileReadHost::attachCapture(CaptureByteSink &sink,
                                 CapturePublisher *publisher,
                                 CaptureHostOptions options) -> CaptureHost & {
  if (!capture_) {
    capture_ =
        std::make_unique<CaptureHost>(arena_, schema_, payloads_, *service_,
                                      sink, publisher, std::move(options));
  }
  return *capture_;
}

auto FileReadHost::openSpec(const std::string_view path,
                            const std::uint64_t rights) const -> OpenSpec {
  // A write-mode open names the canonical path the grant will judge, so the
  // worker opens that path and not a spelling that resolved differently.
  std::string locator{path};
  if ((rights & fileRightsMutating) != 0) {
    if (const auto canonical = canonicalLocator(path))
      locator = canonical->string();
  }
  return OpenSpec{.adapterKey      = fileOpenAdapterKey,
                  .locator         = bytesOf(locator),
                  .requestedRights = fileRightRead | rights,
                  .consistency     = ioConsistencyLive,
                  .deadline        = service_->now() + options_.openDeadline};
}

auto FileReadHost::capabilityOf(const OutcomeCell open) const
    -> std::optional<CapabilityCell> {
  const auto tag = ioOutcomeTag(arena_, open);
  if (!tag || *tag != IoOutcomeTag::Complete) return std::nullopt;
  if (const auto opened = openedEndpointOf(arena_, schema_, open))
    return opened->initialCapability;
  return std::nullopt;
}

auto FileReadHost::closeCapability(const CapabilityCell capability)
    -> std::expected<SubmissionCell, FrontendDiagnostic> {
  const auto endpoint = capabilityEndpoint(arena_, schema_, capability);
  if (!endpoint) {
    return std::unexpected{
        FrontendDiagnostic{std::string{frontend_codes::cellRefused},
                           "the capability names no endpoint to close",
                           {}}};
  }
  return bridge_.wrap(host_.closeEndpoint(*endpoint, CloseMode::Graceful));
}

auto FileReadHost::beginOpen(const std::string_view path,
                             const std::uint64_t rights)
    -> std::expected<RequestCell, FrontendDiagnostic> {
  const auto submitted = host_.admitOpen(openSpec(path, rights));
  if (const auto *admitted = std::get_if<Admitted>(&submitted.value)) {
    return admitted->request;
  }
  const auto refusal = std::get<Refused>(submitted.value).outcome;
  const auto tag     = ioOutcomeTag(arena_, refusal);
  return std::unexpected{FrontendDiagnostic{
      std::string{frontend_codes::cellRefused},
      "open refused before admission: " +
          std::string{tag ? toString(*tag) : "unreadable outcome"},
      {}}};
}

auto FileReadHost::openedCapability(const RequestCell open)
    -> std::optional<std::expected<CapabilityCell, FrontendDiagnostic>> {
  const auto polled = service_->poll(open);
  if (!polled.outcome) return std::nullopt;
  const auto tag = ioOutcomeTag(arena_, *polled.outcome);
  if (tag && *tag == IoOutcomeTag::Complete) {
    if (const auto opened =
            openedEndpointOf(arena_, schema_, *polled.outcome)) {
      return opened->initialCapability;
    }
  }
  return std::unexpected{FrontendDiagnostic{
      std::string{frontend_codes::cellRefused},
      "open ended " + std::string{tag ? toString(*tag) : "unreadable"} +
          " without an endpoint",
      {}}};
}

auto FileReadHost::stageOp(const CapabilityCell capability,
                           const std::string_view tag, FileOpSpec op)
    -> std::expected<CellRef, StageReadError> {
  const auto endpoint = capabilityEndpoint(arena_, schema_, capability);
  if (!endpoint) return std::unexpected{StageReadError::NotAFileCapability};
  const auto open = adapter_.openIdOf(*endpoint);
  if (!open) return std::unexpected{StageReadError::NotAFileCapability};
  const auto spec   = arena_.makeCell(tag);
  const auto staged = adapter_.stageAt(
      spec, FileEndpointOp{.open = *open, .op = std::move(op)});
  if (!staged) {
    if (const auto incarnation = arena_.incarnationOf(spec)) {
      static_cast<void>(arena_.freeCell(spec, *incarnation));
    }
    return std::unexpected{StageReadError::Refused};
  }
  return spec;
}

auto FileReadHost::stageRead(const CapabilityCell capability,
                             const std::uint64_t offset,
                             const std::uint64_t length)
    -> std::expected<CellRef, StageReadError> {
  if (length > options_.payloadBytes ||
      offset > std::numeric_limits<std::uint64_t>::max() - length) {
    return std::unexpected{StageReadError::OutOfBounds};
  }
  return stageOp(capability, fileReadSpecTag,
                 FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                            .range = MediaRange{offset, length}});
}

auto FileReadHost::stageWrite(const CapabilityCell capability,
                              const std::uint64_t offset,
                              std::vector<std::byte> bytes)
    -> std::expected<CellRef, StageReadError> {
  if (bytes.size() > options_.writeBytes ||
      offset > std::numeric_limits<std::uint64_t>::max() - bytes.size()) {
    return std::unexpected{StageReadError::OutOfBounds};
  }
  return stageOp(capability, fileWriteSpecTag,
                 FileOpSpec{.kind       = FileOpSpec::Kind::Write,
                            .range      = MediaRange{offset, 0},
                            .writeBytes = std::move(bytes)});
}

auto FileReadHost::stageAppend(const CapabilityCell capability,
                               std::vector<std::byte> bytes)
    -> std::expected<CellRef, StageReadError> {
  if (bytes.size() > options_.writeBytes)
    return std::unexpected{StageReadError::OutOfBounds};
  return stageOp(capability, fileAppendSpecTag,
                 FileOpSpec{.kind       = FileOpSpec::Kind::Append,
                            .writeBytes = std::move(bytes)});
}

auto FileReadHost::stageSync(const CapabilityCell capability)
    -> std::expected<CellRef, StageReadError> {
  return stageOp(capability, fileSyncSpecTag,
                 FileOpSpec{.kind = FileOpSpec::Kind::Sync});
}

} // namespace xanadu::vortex
