#include "common/xanadu/vortex/io_cells.hpp"

#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>

#include <gleditor/logging.hpp>

namespace xanadu::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

constexpr std::array<std::string_view, ioDimensionCount> dimensionNames{
    "d.io.kind",    "d.io.state",      "d.io.endpoint",   "d.io.capability",
    "d.io.request", "d.io.payload",    "d.io.progress",   "d.io.outcome",
    "d.io.waiter",  "d.io.occurrence", "d.io.queue",      "d.io.clone",
    "d.io.source",  "d.io.range",      "d.io.provenance", "d.io.lease"};

constexpr std::array<std::string_view, 9> cellTagNames{
    "Endpoint", "Capability",      "Request",        "Payload", "Progress",
    "Outcome",  "ReadObservation", "CaptureReceipt", "Waiter"};

constexpr std::array<std::string_view, 12> outcomeTagNames{
    "Rejected",        "Complete",  "EndOfStream", "Partial",
    "Failed",          "Cancelled", "TimedOut",    "Overflow",
    "MessageTooLarge", "Stale",     "Conflict",    "Unknown"};

// The arena's runtime tag is one opaque word; this is its private layout:
// the cell tag in the low byte, then a presence bit and the outcome tag.
constexpr std::uint32_t outcomePresent = 1U << 8U;
constexpr unsigned outcomeShift        = 9U;
constexpr std::uint32_t byteMask       = 0xffU;

constexpr std::uint32_t encodeTag(const IoCellTag tag,
                                  const std::optional<IoOutcomeTag> outcome) {
  auto word = static_cast<std::uint32_t>(tag);
  if (outcome) {
    word |=
        outcomePresent | (static_cast<std::uint32_t>(*outcome) << outcomeShift);
  }
  return word;
}

template <std::size_t N>
std::string_view nameAt(const std::array<std::string_view, N> &names,
                        const std::size_t index) noexcept {
  return index < names.size() ? names[index] : std::string_view{};
}

/// Run @p body under a choice point: a refusal or exception undoes every
/// cell, link, tag and lease it made, success keeps them.
template <typename Body>
auto transact(ArenaManifold &arena, Body &&body) -> decltype(body()) {
  const auto mark = arena.mark();
  try {
    auto result = std::forward<Body>(body)();
    if (!result) {
      static_cast<void>(arena.release(mark));
      return result;
    }
    if (!arena.discard(mark)) {
      // A refused discard leaves the mark outstanding; release it best-effort
      // so it cannot wedge a later release().
      static_cast<void>(arena.release(mark));
      return std::unexpected{IoCellError::ArenaRefused};
    }
    return result;
  } catch (...) {
    static_cast<void>(arena.release(mark));
    throw;
  }
}

std::expected<IoOccurrenceCell, IoCellError>
mint(ArenaManifold &arena, const std::string_view text,
     const std::optional<IoCellTag> tag           = std::nullopt,
     const std::optional<IoOutcomeTag> outcomeTag = std::nullopt) {
  const auto cell        = arena.makeCell(text);
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation || !arena.markRuntimeOnly(cell)) {
    return std::unexpected{IoCellError::ArenaRefused};
  }
  if (tag &&
      !arena.setRuntimeTag(cell, *incarnation, encodeTag(*tag, outcomeTag))) {
    return std::unexpected{IoCellError::ArenaRefused};
  }
  return IoOccurrenceCell{.cell = cell, .incarnation = *incarnation};
}

/// link() replaces whatever was there; the I/O graph never may.
std::expected<void, IoCellError> linkFresh(ArenaManifold &arena,
                                           const CellRef from, const DimRef dim,
                                           const CellRef to) {
  if (arena.linked(from, dim, DimVector::POS) ||
      arena.linked(to, dim, DimVector::NEG)) {
    return std::unexpected{IoCellError::LinkOccupied};
  }
  if (!arena.link(from, dim, DimVector::POS, to)) {
    return std::unexpected{IoCellError::ArenaRefused};
  }
  return {};
}

std::optional<CellRef> rankTail(const ArenaManifold &arena, const CellRef head,
                                const DimRef dim) {
  auto tail = head;
  for (std::size_t visited = 0; visited <= arena.traversalBound(); ++visited) {
    const auto next = arena.linked(tail, dim, DimVector::POS);
    if (!next) return tail;
    if (*next == head) return std::nullopt;
    tail = *next;
  }
  return std::nullopt;
}

bool schemaCurrent(const ArenaManifold &arena, const IoCellSchema &schema) {
  return std::ranges::all_of(schema.dims, [&](const auto &dim) {
    return arena.matches(dim.cell, dim.incarnation);
  });
}

template <IoCellTag Tag>
std::expected<void, IoCellError> require(const ArenaManifold &arena,
                                         const IoCellHandle<Tag> handle) {
  const auto tag = ioCellTag(arena, handle.cell, handle.incarnation);
  if (!tag) return std::unexpected{tag.error()};
  if (*tag != Tag) return std::unexpected{IoCellError::WrongTag};
  return {};
}

template <IoCellTag Tag>
std::expected<IoCellHandle<Tag>, IoCellError>
handleOf(const ArenaManifold &arena, const CellRef cell) {
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation) return std::unexpected{IoCellError::StaleCell};
  const IoCellHandle<Tag> handle{.cell = cell, .incarnation = *incarnation};
  if (const auto ok = require(arena, handle); !ok) {
    return std::unexpected{ok.error()};
  }
  return handle;
}

struct FieldCells {
  IoOccurrenceCell occurrence;
  IoOccurrenceCell view;
};

/// owner -field-> occurrence -field-> view, the view appended to
/// @p canonical's clone rank.
std::expected<FieldCells, IoCellError> attachField(ArenaManifold &arena,
                                                   const IoCellSchema &schema,
                                                   const CellRef owner,
                                                   const IoDimension field,
                                                   const CellRef canonical) {
  const auto fieldDim = schema.dim(field);
  const auto cloneDim = schema.dim(IoDimension::Clone);
  const auto tail     = rankTail(arena, canonical, cloneDim);
  if (!tail) return std::unexpected{IoCellError::MalformedGraph};
  auto occurrence = mint(arena, "io field occurrence");
  if (!occurrence) return std::unexpected{occurrence.error()};
  auto view = mint(arena, "io clone view");
  if (!view) return std::unexpected{view.error()};
  for (const auto &[from, dim, to] :
       {std::tuple{owner, fieldDim, occurrence->cell},
        std::tuple{occurrence->cell, fieldDim, view->cell},
        std::tuple{*tail, cloneDim, view->cell}}) {
    if (const auto linked = linkFresh(arena, from, dim, to); !linked) {
      return std::unexpected{linked.error()};
    }
  }
  return FieldCells{.occurrence = *occurrence, .view = *view};
}

/// The canonical cell behind @p owner's @p field.
std::expected<CellRef, IoCellError> fieldCanonical(const ArenaManifold &arena,
                                                   const IoCellSchema &schema,
                                                   const CellRef owner,
                                                   const IoDimension field) {
  const auto fieldDim   = schema.dim(field);
  const auto occurrence = arena.linked(owner, fieldDim, DimVector::POS);
  if (!occurrence) return std::unexpected{IoCellError::MalformedGraph};
  const auto view = arena.linked(*occurrence, fieldDim, DimVector::POS);
  if (!view) return std::unexpected{IoCellError::MalformedGraph};
  const auto canonical =
      arena.cloneMaster(*view, schema.dim(IoDimension::Clone));
  if (!canonical || *canonical == *view) {
    return std::unexpected{IoCellError::MalformedGraph};
  }
  return *canonical;
}

} // namespace

std::string_view toString(const IoCellTag tag) noexcept {
  return nameAt(cellTagNames, static_cast<std::size_t>(tag));
}

std::string_view toString(const IoOutcomeTag tag) noexcept {
  return nameAt(outcomeTagNames, static_cast<std::size_t>(tag));
}

std::string_view toString(const IoCellError error) noexcept {
  switch (error) {
  case IoCellError::StaleCell:
    return "stale cell";
  case IoCellError::WrongTag:
    return "wrong I/O cell tag";
  case IoCellError::StaleSchema:
    return "stale d.io schema";
  case IoCellError::InvalidCapability:
    return "capability not bound to endpoint";
  case IoCellError::LinkOccupied:
    return "link occupied";
  case IoCellError::AlreadyTerminal:
    return "request already terminal";
  case IoCellError::InvalidOutcome:
    return "outcome does not match admission";
  case IoCellError::MalformedGraph:
    return "malformed I/O graph";
  case IoCellError::PayloadRefused:
    return "payload refused";
  case IoCellError::ArenaRefused:
    return "arena refused";
  }
  return "unknown I/O cell error";
}

std::string_view dimensionName(const IoDimension which) noexcept {
  return nameAt(dimensionNames, static_cast<std::size_t>(which));
}

std::expected<IoCellTag, IoCellError>
ioCellTag(const ArenaManifold &arena, const CellRef cell,
          const AllocationIncarnation incarnation) noexcept {
  if (!arena.matches(cell, incarnation)) {
    return std::unexpected{IoCellError::StaleCell};
  }
  const auto word = arena.runtimeTagOf(cell, incarnation);
  if (!word || (*word & byteMask) >= cellTagNames.size()) {
    return std::unexpected{IoCellError::WrongTag};
  }
  return static_cast<IoCellTag>(*word & byteMask);
}

std::expected<IoOutcomeTag, IoCellError>
ioOutcomeTag(const ArenaManifold &arena, const OutcomeCell outcome) noexcept {
  if (const auto ok = require(arena, outcome); !ok) {
    return std::unexpected{ok.error()};
  }
  const auto word = arena.runtimeTagOf(outcome.cell, outcome.incarnation);
  const auto raw  = (*word >> outcomeShift) & byteMask;
  if ((*word & outcomePresent) == 0 || raw >= outcomeTagNames.size()) {
    return std::unexpected{IoCellError::WrongTag};
  }
  return static_cast<IoOutcomeTag>(raw);
}

std::expected<IoCellSchema, IoCellError>
makeIoCellSchema(ArenaManifold &arena) {
  IoCellSchema schema;
  for (std::size_t i = 0; i < dimensionNames.size(); ++i) {
    const auto dim         = arena.ensureDimension(dimensionNames[i]);
    const auto incarnation = arena.incarnationOf(dim);
    // A base-store dimension has no arena incarnation; M0's arena is the
    // only host of I/O state, so that is refused rather than half-validated.
    if (!incarnation) return std::unexpected{IoCellError::StaleSchema};
    schema.dims.at(i) = {.cell = dim, .incarnation = *incarnation};
  }
  return schema;
}

std::expected<EndpointCell, IoCellError>
makeEndpointCell(ArenaManifold &arena, const IoCellSchema &schema) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  return transact(arena, [&]() -> std::expected<EndpointCell, IoCellError> {
    const auto endpoint = mint(arena, "io endpoint", IoCellTag::Endpoint);
    if (!endpoint) return std::unexpected{endpoint.error()};
    const auto queue = mint(arena, "io endpoint queue");
    if (!queue) return std::unexpected{queue.error()};
    if (const auto linked =
            linkFresh(arena, endpoint->cell,
                      schema.dim(IoDimension::Occurrence), queue->cell);
        !linked) {
      return std::unexpected{linked.error()};
    }
    return EndpointCell{.cell        = endpoint->cell,
                        .incarnation = endpoint->incarnation};
  });
}

std::expected<RequestCell, IoCellError>
makeOpenRequestCell(ArenaManifold &arena) {
  return transact(arena, [&]() -> std::expected<RequestCell, IoCellError> {
    const auto request = mint(arena, "io open request", IoCellTag::Request);
    if (!request) return std::unexpected{request.error()};
    return RequestCell{.cell        = request->cell,
                       .incarnation = request->incarnation};
  });
}

std::expected<WaiterCell, IoCellError> makeWaiterCell(ArenaManifold &arena) {
  return transact(arena, [&]() -> std::expected<WaiterCell, IoCellError> {
    const auto waiter = mint(arena, "io waiter", IoCellTag::Waiter);
    if (!waiter) return std::unexpected{waiter.error()};
    return WaiterCell{.cell = waiter->cell, .incarnation = waiter->incarnation};
  });
}

std::expected<IoOccurrenceCell, IoCellError>
endpointQueue(const ArenaManifold &arena, const IoCellSchema &schema,
              const EndpointCell endpoint) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, endpoint); !ok) {
    return std::unexpected{ok.error()};
  }
  const auto queue = arena.linked(
      endpoint.cell, schema.dim(IoDimension::Occurrence), DimVector::POS);
  if (!queue) return std::unexpected{IoCellError::MalformedGraph};
  const auto incarnation = arena.incarnationOf(*queue);
  if (!incarnation) return std::unexpected{IoCellError::MalformedGraph};
  return IoOccurrenceCell{.cell = *queue, .incarnation = *incarnation};
}

std::expected<CapabilityCell, IoCellError>
makeCapabilityCell(ArenaManifold &arena, const IoCellSchema &schema,
                   const EndpointCell endpoint) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, endpoint); !ok) {
    return std::unexpected{ok.error()};
  }
  return transact(arena, [&]() -> std::expected<CapabilityCell, IoCellError> {
    const auto capability = mint(arena, "io capability", IoCellTag::Capability);
    if (!capability) return std::unexpected{capability.error()};
    const auto field = attachField(arena, schema, capability->cell,
                                   IoDimension::Endpoint, endpoint.cell);
    if (!field) return std::unexpected{field.error()};
    return CapabilityCell{.cell        = capability->cell,
                          .incarnation = capability->incarnation};
  });
}

std::expected<EndpointCell, IoCellError>
capabilityEndpoint(const ArenaManifold &arena, const IoCellSchema &schema,
                   const CapabilityCell capability) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, capability); !ok) {
    return std::unexpected{ok.error()};
  }
  const auto canonical =
      fieldCanonical(arena, schema, capability.cell, IoDimension::Endpoint);
  if (!canonical) return std::unexpected{canonical.error()};
  return handleOf<IoCellTag::Endpoint>(arena, *canonical);
}

std::expected<IoRequestGraph, IoCellError>
appendIoRequest(ArenaManifold &arena, const IoCellSchema &schema,
                const EndpointCell endpoint, const CapabilityCell capability) {
  const auto queue = endpointQueue(arena, schema, endpoint);
  if (!queue) return std::unexpected{queue.error()};
  const auto bound = capabilityEndpoint(arena, schema, capability);
  if (!bound) return std::unexpected{bound.error()};
  if (*bound != endpoint) {
    GLEDITOR_LOG_DEBUG("vortex.io", "request refused: capability bound to "
                                    "another endpoint");
    return std::unexpected{IoCellError::InvalidCapability};
  }
  return transact(arena, [&]() -> std::expected<IoRequestGraph, IoCellError> {
    const auto request = mint(arena, "io request", IoCellTag::Request);
    if (!request) return std::unexpected{request.error()};
    const auto endpointField = attachField(
        arena, schema, request->cell, IoDimension::Endpoint, endpoint.cell);
    if (!endpointField) return std::unexpected{endpointField.error()};
    const auto capabilityField = attachField(
        arena, schema, request->cell, IoDimension::Capability, capability.cell);
    if (!capabilityField) return std::unexpected{capabilityField.error()};
    const auto queueDim = schema.dim(IoDimension::Queue);
    const auto tail     = rankTail(arena, queue->cell, queueDim);
    if (!tail) return std::unexpected{IoCellError::MalformedGraph};
    if (const auto linked = linkFresh(arena, *tail, queueDim, request->cell);
        !linked) {
      return std::unexpected{linked.error()};
    }
    return IoRequestGraph{
        .request              = RequestCell{.cell        = request->cell,
                                            .incarnation = request->incarnation},
        .endpointOccurrence   = endpointField->occurrence,
        .endpointView         = endpointField->view,
        .capabilityOccurrence = capabilityField->occurrence,
        .capabilityView       = capabilityField->view,
    };
  });
}

std::expected<EndpointCell, IoCellError>
requestEndpoint(const ArenaManifold &arena, const IoCellSchema &schema,
                const RequestCell request) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, request); !ok) {
    return std::unexpected{ok.error()};
  }
  const auto canonical =
      fieldCanonical(arena, schema, request.cell, IoDimension::Endpoint);
  if (!canonical) return std::unexpected{canonical.error()};
  return handleOf<IoCellTag::Endpoint>(arena, *canonical);
}

std::expected<CapabilityCell, IoCellError>
requestCapability(const ArenaManifold &arena, const IoCellSchema &schema,
                  const RequestCell request) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, request); !ok) {
    return std::unexpected{ok.error()};
  }
  const auto canonical =
      fieldCanonical(arena, schema, request.cell, IoDimension::Capability);
  if (!canonical) return std::unexpected{canonical.error()};
  return handleOf<IoCellTag::Capability>(arena, *canonical);
}

std::expected<std::vector<RequestCell>, IoCellError>
queuedRequests(const ArenaManifold &arena, const IoCellSchema &schema,
               const EndpointCell endpoint) {
  const auto queue = endpointQueue(arena, schema, endpoint);
  if (!queue) return std::unexpected{queue.error()};
  const auto queueDim = schema.dim(IoDimension::Queue);
  std::vector<RequestCell> requests;
  auto at = arena.linked(queue->cell, queueDim, DimVector::POS);
  while (at) {
    if (requests.size() > arena.traversalBound()) {
      return std::unexpected{IoCellError::MalformedGraph};
    }
    const auto request = handleOf<IoCellTag::Request>(arena, *at);
    if (!request) return std::unexpected{request.error()};
    requests.push_back(*request);
    at = arena.linked(*at, queueDim, DimVector::POS);
  }
  return requests;
}

std::expected<OutcomeCell, IoCellError>
makeOutcomeCell(ArenaManifold &arena, const IoCellSchema &schema,
                const std::optional<RequestCell> request,
                const IoOutcomeTag tag) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (request.has_value() == (IoOutcomeTag::Rejected == tag)) {
    return std::unexpected{IoCellError::InvalidOutcome};
  }
  const auto outcomeDim = schema.dim(IoDimension::Outcome);
  if (request) {
    if (const auto ok = require(arena, *request); !ok) {
      return std::unexpected{ok.error()};
    }
    if (arena.linked(request->cell, outcomeDim, DimVector::POS)) {
      return std::unexpected{IoCellError::AlreadyTerminal};
    }
  }
  return transact(arena, [&]() -> std::expected<OutcomeCell, IoCellError> {
    const auto outcome = mint(arena, toString(tag), IoCellTag::Outcome, tag);
    if (!outcome) return std::unexpected{outcome.error()};
    if (request) {
      if (const auto linked =
              linkFresh(arena, request->cell, outcomeDim, outcome->cell);
          !linked) {
        return std::unexpected{linked.error()};
      }
    }
    return OutcomeCell{.cell        = outcome->cell,
                       .incarnation = outcome->incarnation};
  });
}

std::expected<PayloadCell, IoCellError>
makePayloadCell(ArenaManifold &arena, PayloadResolver &resolver,
                const PayloadLease &lease) {
  const auto token = resolver.retainOwner(lease);
  if (!token) return std::unexpected{IoCellError::PayloadRefused};
  return transact(arena, [&]() -> std::expected<PayloadCell, IoCellError> {
    const auto payload = mint(arena, "io payload", IoCellTag::Payload);
    if (!payload) return std::unexpected{payload.error()};
    if (!arena.setOwnerLease(
            payload->cell, payload->incarnation,
            std::const_pointer_cast<PayloadOwnerToken>(*token))) {
      return std::unexpected{IoCellError::ArenaRefused};
    }
    return PayloadCell{.cell        = payload->cell,
                       .incarnation = payload->incarnation};
  });
}

std::optional<PayloadRange> payloadRangeOf(const ArenaManifold &arena,
                                           const PayloadCell payload) {
  if (!require(arena, payload)) return std::nullopt;
  // Only makePayloadCell puts an owner lease on a Payload-tagged cell, and it
  // is always a PayloadOwnerToken.
  const auto owner = std::static_pointer_cast<const PayloadOwnerToken>(
      arena.ownerLeaseOf(payload.cell, payload.incarnation));
  if (!owner) return std::nullopt;
  return owner->range;
}

std::optional<PayloadRange> outcomeCellPayloadRange(const ArenaManifold &arena,
                                                    const IoCellSchema &schema,
                                                    const OutcomeCell outcome) {
  // addPayload links outcome -d.io.payload-> occurrence -d.io.payload->
  // payload cell; the payload cell's owner lease names the retained range.
  const auto dim        = schema.dim(IoDimension::Payload);
  const auto occurrence = arena.linked(outcome.cell, dim, DimVector::POS);
  if (!occurrence) return std::nullopt;
  const auto payloadCell = arena.linked(*occurrence, dim, DimVector::POS);
  if (!payloadCell) return std::nullopt;
  const auto incarnation = arena.incarnationOf(*payloadCell);
  if (!incarnation) return std::nullopt;
  return payloadRangeOf(
      arena, PayloadCell{.cell = *payloadCell, .incarnation = *incarnation});
}

} // namespace xanadu::vortex
