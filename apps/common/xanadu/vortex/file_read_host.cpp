#include "common/xanadu/vortex/file_read_host.hpp"

#include <cstring>
#include <limits>
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
      cells_(arena, schema_, payloads_,
             makeFileRequestDecoder(arena, fileDataAdapterKey,
                                    options.payloadBytes),
             makeFileCloseDecoder(adapter_),
             [](const OpenSpec &)
                 -> std::expected<ArenaIoCellStore::OpenGrant, IoErrorInfo> {
               // A read-only host: whatever a program asks to open it may only
               // read, so no capability it hands a language can write.
               return ArenaIoCellStore::OpenGrant{
                   .rights = fileRightRead, .consistency = ioConsistencyLive};
             }),
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
}

auto FileReadHost::openSpec(const std::string_view path) const -> OpenSpec {
  return OpenSpec{.adapterKey      = fileOpenAdapterKey,
                  .locator         = bytesOf(path),
                  .requestedRights = fileRightRead,
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

auto FileReadHost::beginOpen(const std::string_view path)
    -> std::expected<RequestCell, FrontendDiagnostic> {
  const auto submitted = host_.admitOpen(openSpec(path));
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

auto FileReadHost::stageRead(const CapabilityCell capability,
                             const std::uint64_t offset,
                             const std::uint64_t length)
    -> std::expected<CellRef, StageReadError> {
  const auto endpoint = capabilityEndpoint(arena_, schema_, capability);
  if (!endpoint) return std::unexpected{StageReadError::NotAFileCapability};
  const auto open = adapter_.openIdOf(*endpoint);
  if (!open) return std::unexpected{StageReadError::NotAFileCapability};
  if (length > options_.payloadBytes ||
      offset > std::numeric_limits<std::uint64_t>::max() - length) {
    return std::unexpected{StageReadError::OutOfBounds};
  }
  const auto spec   = arena_.makeCell(fileReadSpecTag);
  const auto staged = adapter_.stageAt(
      spec,
      FileEndpointOp{.open = *open,
                     .op   = FileOpSpec{.kind  = FileOpSpec::Kind::Read,
                                        .range = MediaRange{offset, length}}});
  if (!staged) {
    if (const auto incarnation = arena_.incarnationOf(spec)) {
      static_cast<void>(arena_.freeCell(spec, *incarnation));
    }
    return std::unexpected{StageReadError::Refused};
  }
  return spec;
}

} // namespace xanadu::vortex
