#include "common/xanadu/vortex/arena_io_cell_store.hpp" // IWYU pragma: associated

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace xanadu::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

auto cellError(const IoCellError error) noexcept -> IoErrorInfo {
  switch (error) {
  case IoCellError::StaleCell:
  case IoCellError::StaleSchema:
    return {IoErrorCode::Stale, static_cast<std::uint32_t>(error)};
  case IoCellError::InvalidCapability:
    return {IoErrorCode::InvalidCapability, static_cast<std::uint32_t>(error)};
  case IoCellError::PayloadRefused:
    return {IoErrorCode::Unresolved, static_cast<std::uint32_t>(error)};
  case IoCellError::WrongTag:
  case IoCellError::LinkOccupied:
  case IoCellError::AlreadyTerminal:
  case IoCellError::InvalidOutcome:
  case IoCellError::MalformedGraph:
  case IoCellError::ArenaRefused:
    return {IoErrorCode::InvalidRequest, static_cast<std::uint32_t>(error)};
  }
  return {IoErrorCode::InvalidRequest};
}

auto outcomeTag(const IoOutcome &outcome) noexcept -> IoOutcomeTag {
  return std::visit(
      []<typename T>(const T &) -> IoOutcomeTag {
        if constexpr (std::is_same_v<T, Complete>)
          return IoOutcomeTag::Complete;
        else if constexpr (std::is_same_v<T, EndOfStream>)
          return IoOutcomeTag::EndOfStream;
        else if constexpr (std::is_same_v<T, Partial>)
          return IoOutcomeTag::Partial;
        else if constexpr (std::is_same_v<T, Failed>)
          return IoOutcomeTag::Failed;
        else if constexpr (std::is_same_v<T, Cancelled>)
          return IoOutcomeTag::Cancelled;
        else if constexpr (std::is_same_v<T, TimedOut>)
          return IoOutcomeTag::TimedOut;
        else if constexpr (std::is_same_v<T, Overflow>)
          return IoOutcomeTag::Overflow;
        else if constexpr (std::is_same_v<T, MessageTooLarge>)
          return IoOutcomeTag::MessageTooLarge;
        else if constexpr (std::is_same_v<T, Stale>)
          return IoOutcomeTag::Stale;
        else if constexpr (std::is_same_v<T, Conflict>)
          return IoOutcomeTag::Conflict;
        else
          return IoOutcomeTag::Unknown;
      },
      outcome);
}

auto number(const std::uint64_t value) -> std::string {
  return std::to_string(value);
}

auto phaseName(const IoPhase phase) noexcept -> std::string_view {
  switch (phase) {
  case IoPhase::Preparing:
    return "Preparing";
  case IoPhase::Queued:
    return "Queued";
  case IoPhase::Running:
    return "Running";
  case IoPhase::Cancelling:
    return "Cancelling";
  case IoPhase::Terminal:
    return "Terminal";
  }
  return "Unknown";
}

auto appendField(ArenaManifold &arena, const CellRef record,
                 const DimRef rankDim, const DimRef valueDim,
                 const std::string_view label, const std::string_view value)
    -> bool {
  const auto field              = arena.makeCell(label);
  const auto fieldIncarnation   = arena.incarnationOf(field);
  const auto content            = arena.makeCell(value);
  const auto contentIncarnation = arena.incarnationOf(content);
  if (!fieldIncarnation || !contentIncarnation ||
      !arena.markRuntimeOnly(field) || !arena.markRuntimeOnly(content)) {
    return false;
  }

  auto tail = record;
  for (std::size_t visited = 0; visited <= arena.traversalBound(); ++visited) {
    const auto next = arena.linked(tail, rankDim, DimVector::POS);
    if (!next) break;
    tail = *next;
    if (visited == arena.traversalBound()) return false;
  }
  return arena.link(tail, rankDim, DimVector::POS, field) &&
         arena.link(field, valueDim, DimVector::POS, content);
}

auto attachCanonical(ArenaManifold &arena, const IoCellSchema &schema,
                     const CellRef owner, const IoDimension field,
                     const CellRef canonical) -> bool {
  const auto fieldDim = schema.dim(field);
  const auto cloneDim = schema.dim(IoDimension::Clone);
  auto tail           = canonical;
  for (std::size_t visited = 0; visited <= arena.traversalBound(); ++visited) {
    const auto next = arena.linked(tail, cloneDim, DimVector::POS);
    if (!next) break;
    tail = *next;
    if (visited == arena.traversalBound()) return false;
  }
  const auto occurrence = arena.makeCell("io field occurrence");
  const auto view       = arena.makeCell("io clone view");
  if (!arena.incarnationOf(occurrence) || !arena.incarnationOf(view) ||
      !arena.markRuntimeOnly(occurrence) || !arena.markRuntimeOnly(view) ||
      arena.linked(owner, fieldDim, DimVector::POS) ||
      arena.linked(tail, cloneDim, DimVector::POS) ||
      arena.linked(view, cloneDim, DimVector::NEG)) {
    return false;
  }
  return arena.link(owner, fieldDim, DimVector::POS, occurrence) &&
         arena.link(occurrence, fieldDim, DimVector::POS, view) &&
         arena.link(tail, cloneDim, DimVector::POS, view);
}

auto addProgress(ArenaManifold &arena, const IoCellSchema &schema,
                 const CellRef outcome, const IoProgress &progress) -> bool {
  const auto occurrence            = arena.makeCell("io progress occurrence");
  const auto occurrenceIncarnation = arena.incarnationOf(occurrence);
  const auto progressCell          = arena.makeCell("io progress");
  const auto progressIncarnation   = arena.incarnationOf(progressCell);
  if (!occurrenceIncarnation || !progressIncarnation ||
      !arena.markRuntimeOnly(occurrence) ||
      !arena.markRuntimeOnly(progressCell) ||
      !arena.setRuntimeTag(progressCell, *progressIncarnation,
                           static_cast<std::uint32_t>(IoCellTag::Progress))) {
    return false;
  }
  const auto dim = schema.dim(IoDimension::Progress);
  if (!arena.link(outcome, dim, DimVector::POS, occurrence) ||
      !arena.link(occurrence, dim, DimVector::POS, progressCell)) {
    return false;
  }
  const auto seq = progress.sourceSequence ? number(*progress.sourceSequence)
                                           : std::string{"none"};
  return appendField(arena, progressCell, schema.dim(IoDimension::Occurrence),
                     schema.dim(IoDimension::State), "acceptedBytes",
                     number(progress.acceptedBytes)) &&
         appendField(arena, progressCell, schema.dim(IoDimension::Occurrence),
                     schema.dim(IoDimension::State), "transferredBytes",
                     number(progress.transferredBytes)) &&
         appendField(arena, progressCell, schema.dim(IoDimension::Occurrence),
                     schema.dim(IoDimension::State), "consumedBytes",
                     number(progress.consumedBytes)) &&
         appendField(arena, progressCell, schema.dim(IoDimension::Occurrence),
                     schema.dim(IoDimension::State), "sourceSequence", seq) &&
         appendField(arena, progressCell, schema.dim(IoDimension::Occurrence),
                     schema.dim(IoDimension::State), "phase",
                     phaseName(progress.phase));
}

auto addPayload(ArenaManifold &arena, const IoCellSchema &schema,
                PayloadResolver &resolver, const CellRef outcome,
                const PayloadRange &range,
                const std::optional<PayloadLease> &providedLease) -> bool {
  std::optional<PayloadLease> retained;
  const PayloadLease *lease = nullptr;
  if (providedLease) {
    if (providedLease->range != range) return false;
    lease = &*providedLease;
  } else {
    auto result = resolver.retain(range);
    if (!result) return false;
    retained = std::move(*result);
    lease    = &*retained;
  }
  auto payload = makePayloadCell(arena, resolver, *lease);
  if (retained) resolver.release(retained->lease);
  if (!payload) return false;

  const auto occurrence  = arena.makeCell("io payload occurrence");
  const auto incarnation = arena.incarnationOf(occurrence);
  if (!incarnation || !arena.markRuntimeOnly(occurrence)) return false;
  const auto dim = schema.dim(IoDimension::Payload);
  if (!arena.link(outcome, dim, DimVector::POS, occurrence) ||
      !arena.link(occurrence, dim, DimVector::POS, payload->cell)) {
    return false;
  }

  const auto rangeRecord      = arena.makeCell("payload range");
  const auto rangeIncarnation = arena.incarnationOf(rangeRecord);
  if (!rangeIncarnation || !arena.markRuntimeOnly(rangeRecord) ||
      !arena.markRuntimeOnly(rangeRecord)) {
    return false;
  }
  const auto rangeOccurrence = arena.makeCell("payload range metadata");
  const auto rangeOccurrenceIncarnation = arena.incarnationOf(rangeOccurrence);
  if (!rangeOccurrenceIncarnation || !arena.markRuntimeOnly(rangeOccurrence) ||
      !arena.link(payload->cell, schema.dim(IoDimension::Range), DimVector::POS,
                  rangeOccurrence) ||
      !arena.link(rangeOccurrence, schema.dim(IoDimension::Range),
                  DimVector::POS, rangeRecord)) {
    return false;
  }
  constexpr std::array labels{"sourceKind", "source", "generation",
                              "occurrence", "offset", "length"};
  const auto kind = std::to_string(static_cast<unsigned>(range.origin.kind));
  const std::array values{kind,
                          number(range.origin.source),
                          number(range.origin.generation),
                          number(range.origin.occurrence),
                          number(range.offset),
                          number(range.length)};
  for (std::size_t index = 0; index < labels.size(); ++index) {
    if (!appendField(arena, rangeRecord, schema.dim(IoDimension::Occurrence),
                     schema.dim(IoDimension::State), labels[index],
                     values[index])) {
      return false;
    }
  }
  return true;
}

auto addOutcomeDetails(ArenaManifold &arena, const IoCellSchema &schema,
                       const CellRef outcome, const IoOutcome &result) -> bool {
  const auto add = [&](const std::string_view label,
                       const std::string_view value) {
    return appendField(arena, outcome, schema.dim(IoDimension::Occurrence),
                       schema.dim(IoDimension::State), label, value);
  };
  return std::visit(
      [&](const auto &value) -> bool {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Complete>) {
          const auto *opened = std::get_if<OpenedEndpoint>(&value.value);
          // monostate carries no inline details; byte output travels only
          // through the outcome's payload range, never IoResultValue.
          if (opened == nullptr) {
            return true;
          }
          const auto endpointTag = ioCellTag(arena, opened->endpoint.cell,
                                             opened->endpoint.incarnation);
          const auto capabilityTag =
              ioCellTag(arena, opened->initialCapability.cell,
                        opened->initialCapability.incarnation);
          const auto bound =
              capabilityEndpoint(arena, schema, opened->initialCapability);
          return endpointTag && *endpointTag == IoCellTag::Endpoint &&
                 capabilityTag && *capabilityTag == IoCellTag::Capability &&
                 bound && *bound == opened->endpoint &&
                 attachCanonical(arena, schema, outcome, IoDimension::Endpoint,
                                 opened->endpoint.cell) &&
                 attachCanonical(arena, schema, outcome,
                                 IoDimension::Capability,
                                 opened->initialCapability.cell);
        } else if constexpr (std::is_same_v<T, Failed>) {
          return add("errorCode", toString(value.reason.code)) &&
                 add("stableDetail", number(value.reason.stableDetail));
        } else if constexpr (std::is_same_v<T, Partial>) {
          return add("cause", number(static_cast<unsigned>(value.cause)));
        } else if constexpr (std::is_same_v<T, Cancelled> ||
                             std::is_same_v<T, TimedOut>) {
          return add("uncertain", value.uncertain ? "true" : "false");
        } else if constexpr (std::is_same_v<T, Overflow>) {
          return add("gapFirst", number(value.gap.firstSequence)) &&
                 add("gapLast", number(value.gap.lastSequence));
        } else if constexpr (std::is_same_v<T, MessageTooLarge>) {
          return add("required", number(value.required)) &&
                 add("disposition",
                     number(static_cast<unsigned>(value.disposition)));
        } else if constexpr (std::is_same_v<T, Stale>) {
          return add("expectedGeneration", number(value.expected.value)) &&
                 add("observedGeneration", number(value.observed.value));
        } else if constexpr (std::is_same_v<T, Conflict>) {
          return add("expectedGeneration", number(value.expected.value)) &&
                 add("observedGeneration", number(value.observed.value));
        } else if constexpr (std::is_same_v<T, Unknown>) {
          return add("lastKnownPhase", phaseName(value.lastKnown.phase));
        }
        return true;
      },
      result);
}

auto outcomePayload(const IoOutcome &outcome) -> std::optional<PayloadRange> {
  return std::visit(
      []<typename T>(const T &value) -> std::optional<PayloadRange> {
        if constexpr (std::is_same_v<T, Complete> ||
                      std::is_same_v<T, Failed> ||
                      std::is_same_v<T, Cancelled> ||
                      std::is_same_v<T, TimedOut>) {
          return value.payload;
        } else if constexpr (std::is_same_v<T, Partial>) {
          return value.payload;
        }
        return std::nullopt;
      },
      outcome);
}

} // namespace

ArenaIoCellStore::ArenaIoCellStore(zigzag::ArenaManifold &arena,
                                   IoCellSchema schema,
                                   PayloadResolver &payloads,
                                   RequestDecoder decoder,
                                   CloseDecoder closeDecoder,
                                   OpenAuthorizer openAuthorizer)
    : arena_(arena), schema_(std::move(schema)), payloads_(payloads),
      decoder_(std::move(decoder)), closeDecoder_(std::move(closeDecoder)),
      openAuthorizer_(std::move(openAuthorizer)) {}

auto ArenaIoCellStore::freezeRequest(const CapabilityCell capability,
                                     const OwnerCell specification)
    -> std::expected<CellRequest, IoErrorInfo> {
  if (!decoder_ || !arena_.incarnationOf(specification)) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidRequest, 0}};
  }
  const auto capabilityTag =
      ioCellTag(arena_, capability.cell, capability.incarnation);
  if (!capabilityTag || *capabilityTag != IoCellTag::Capability) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 0}};
  }
  auto frozen = decoder_(capability, specification);
  if (!frozen) return std::unexpected{frozen.error()};
  if (frozen->capability != capability || frozen->open ||
      frozen->specification != specification) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidRequest, 1}};
  }
  const auto bound = capabilityEndpoint(arena_, schema_, capability);
  if (!bound) return std::unexpected{cellError(bound.error())};
  if (frozen->endpoint && *frozen->endpoint != *bound) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 1}};
  }
  frozen->endpoint = *bound;
  return frozen;
}

auto ArenaIoCellStore::freezeClose(const EndpointCell endpoint,
                                   const CloseMode mode)
    -> std::expected<CellRequest, IoErrorInfo> {
  const auto tag = ioCellTag(arena_, endpoint.cell, endpoint.incarnation);
  if (!tag || *tag != IoCellTag::Endpoint) {
    return std::unexpected{IoErrorInfo{IoErrorCode::Stale, 1}};
  }
  if (!closeDecoder_) {
    return std::unexpected{IoErrorInfo{IoErrorCode::Unsupported, 0}};
  }
  auto frozen = closeDecoder_(endpoint, mode);
  if (!frozen) return std::unexpected{frozen.error()};
  if (frozen->endpoint != endpoint ||
      frozen->direction != CellRequest::Direction::Control || frozen->open) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidRequest, 2}};
  }
  return frozen;
}

auto ArenaIoCellStore::authorizeOpen(const OpenSpec &spec)
    -> std::expected<void, IoErrorInfo> {
  const auto grant = openGrant(spec);
  if (!grant) return std::unexpected{grant.error()};
  return {};
}

auto ArenaIoCellStore::openGrant(const OpenSpec &spec)
    -> std::expected<OpenGrant, IoErrorInfo> {
  if (!openAuthorizer_) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 0}};
  }
  if (spec.authCapability) {
    const auto tag = ioCellTag(arena_, spec.authCapability->cell,
                               spec.authCapability->incarnation);
    if (!tag || *tag != IoCellTag::Capability) {
      return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 1}};
    }
  }
  std::expected<OpenGrant, IoErrorInfo> grant =
      std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 4}};
  try {
    grant = openAuthorizer_(spec);
  } catch (...) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 4}};
  }
  if (!grant) return std::unexpected{grant.error()};
  if ((spec.requestedRights & ~grant->rights) != 0 ||
      (spec.consistency & ~grant->consistency) != 0) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 2}};
  }
  return *grant;
}

auto ArenaIoCellStore::mintRequest(const CellRequest &request)
    -> std::expected<RequestCell, IoErrorInfo> {
  if (request.open) {
    auto cell = makeOpenRequestCell(arena_);
    if (!cell) return std::unexpected{cellError(cell.error())};
    return *cell;
  }
  if (!request.endpoint) {
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 2}};
  }
  const auto graph =
      appendIoRequest(arena_, schema_, *request.endpoint, request.capability);
  if (!graph) return std::unexpected{cellError(graph.error())};
  return graph->request;
}

auto ArenaIoCellStore::mintRefusal(const IoErrorInfo) -> OutcomeCell {
  const auto outcome =
      makeOutcomeCell(arena_, schema_, std::nullopt, IoOutcomeTag::Rejected);
  return outcome ? *outcome : OutcomeCell{};
}

auto ArenaIoCellStore::publishTerminal(const RequestCell request,
                                       const IoOutcome &outcome,
                                       const IoProgress &progress,
                                       const std::optional<PayloadLease> &lease)
    -> std::expected<OutcomeCell, IoErrorInfo> {
  const auto requestTag = ioCellTag(arena_, request.cell, request.incarnation);
  if (!requestTag || *requestTag != IoCellTag::Request) {
    return std::unexpected{IoErrorInfo{IoErrorCode::Stale, 0}};
  }
  const auto mark = arena_.mark();
  const auto cell =
      makeOutcomeCell(arena_, schema_, request, outcomeTag(outcome));
  if (!cell) {
    static_cast<void>(arena_.release(mark));
    return std::unexpected{cellError(cell.error())};
  }
  auto payload = outcomePayload(outcome);
  if (lease) {
    if (payload && *payload != lease->range) {
      static_cast<void>(arena_.release(mark));
      return std::unexpected{IoErrorInfo{IoErrorCode::InvalidRequest, 3}};
    }
    payload = lease->range;
  }
  if (!addProgress(arena_, schema_, cell->cell, progress) ||
      !addOutcomeDetails(arena_, schema_, cell->cell, outcome) ||
      (payload &&
       !addPayload(arena_, schema_, payloads_, cell->cell, *payload, lease))) {
    static_cast<void>(arena_.release(mark));
    return std::unexpected{IoErrorInfo{IoErrorCode::BackendFailure, 0}};
  }
  if (!arena_.discard(mark)) {
    return std::unexpected{IoErrorInfo{IoErrorCode::BackendFailure, 1}};
  }
  return *cell;
}

auto ArenaIoCellStore::isOutcomeCurrent(
    const OutcomeCell outcome) const noexcept -> bool {
  return arena_.matches(outcome.cell, outcome.incarnation) &&
         ioOutcomeTag(arena_, outcome).has_value();
}

auto ArenaIoCellStore::mintOpenedEndpoint(const OpenSpec &spec)
    -> std::expected<OpenedEndpoint, IoErrorInfo> {
  auto grant = openGrant(spec);
  if (!grant) return std::unexpected{grant.error()};
  const auto mark = arena_.mark();
  auto endpoint   = makeEndpointCell(arena_, schema_);
  if (!endpoint) {
    static_cast<void>(arena_.release(mark));
    return std::unexpected{cellError(endpoint.error())};
  }
  auto capability = makeCapabilityCell(arena_, schema_, *endpoint);
  if (!capability) {
    static_cast<void>(arena_.release(mark));
    return std::unexpected{cellError(capability.error())};
  }
  if (spec.authCapability &&
      !attachCanonical(arena_, schema_, capability->cell,
                       IoDimension::Capability, spec.authCapability->cell)) {
    static_cast<void>(arena_.release(mark));
    return std::unexpected{IoErrorInfo{IoErrorCode::InvalidCapability, 3}};
  }
  if (!appendField(arena_, capability->cell,
                   schema_.dim(IoDimension::Occurrence),
                   schema_.dim(IoDimension::State), "rights",
                   number(spec.requestedRights)) ||
      !appendField(arena_, capability->cell,
                   schema_.dim(IoDimension::Occurrence),
                   schema_.dim(IoDimension::State), "consistency",
                   number(spec.consistency))) {
    static_cast<void>(arena_.release(mark));
    return std::unexpected{IoErrorInfo{IoErrorCode::BackendFailure, 3}};
  }
  if (!arena_.discard(mark)) {
    return std::unexpected{IoErrorInfo{IoErrorCode::BackendFailure, 2}};
  }
  return OpenedEndpoint{*endpoint, *capability};
}

auto ArenaIoCellStore::mintWaiter(const RequestCell request,
                                  const OwnerWaiterRegistration registration)
    -> WaiterCell {
  if (!arena_.matches(request.cell, request.incarnation) ||
      !arena_.matches(registration.cursor, registration.cursorIncarnation)) {
    return {};
  }
  auto waiter = makeWaiterCell(arena_);
  if (!waiter) return {};
  const auto requestOccurrence = arena_.makeCell("io waiter request");
  const auto requestOccurrenceIncarnation =
      arena_.incarnationOf(requestOccurrence);
  const auto dim = schema_.dim(IoDimension::Request);
  if (!requestOccurrenceIncarnation ||
      !arena_.markRuntimeOnly(requestOccurrence) ||
      !arena_.link(waiter->cell, dim, DimVector::POS, requestOccurrence) ||
      !arena_.link(requestOccurrence, dim, DimVector::POS, request.cell)) {
    return {};
  }
  const auto values = std::array{
      std::pair{std::string_view{"cursor"}, number(registration.cursor)},
      std::pair{std::string_view{"cursorIncarnation"},
                number(registration.cursorIncarnation)},
      std::pair{std::string_view{"continuation"},
                number(registration.continuation)}};
  for (const auto &[label, value] : values) {
    if (!appendField(arena_, waiter->cell, schema_.dim(IoDimension::Occurrence),
                     schema_.dim(IoDimension::State), label, value)) {
      return {};
    }
  }
  return *waiter;
}

void ArenaIoCellStore::retireWaiter(const WaiterCell waiter) noexcept {
  if (!arena_.matches(waiter.cell, waiter.incarnation) ||
      !ioCellTag(arena_, waiter.cell, waiter.incarnation) ||
      *ioCellTag(arena_, waiter.cell, waiter.incarnation) !=
          IoCellTag::Waiter) {
    return;
  }
  const auto dim        = schema_.dim(IoDimension::Request);
  const auto occurrence = arena_.linked(waiter.cell, dim, DimVector::POS);
  if (occurrence) {
    static_cast<void>(arena_.unlink(waiter.cell, dim, DimVector::POS));
    static_cast<void>(arena_.unlink(*occurrence, dim, DimVector::POS));
  }
}

} // namespace xanadu::vortex
