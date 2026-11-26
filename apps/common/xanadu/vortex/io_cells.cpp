#include "common/xanadu/vortex/io_cells.hpp"

#include <algorithm>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

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

constexpr std::array<std::string_view, 12> cellTagNames{
    "Endpoint", "Capability", "Request",         "Payload",
    "Progress", "Outcome",    "ReadObservation", "CaptureReceipt",
    "Waiter",   "Capture",    "Submission",      "StaleBinding"};

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

} // namespace

/// owner -field-> occurrence -field-> view, the view appended to
/// @p canonical's clone rank.
std::expected<IoFieldCells, IoCellError>
attachClonedField(ArenaManifold &arena, const IoCellSchema &schema,
                  const CellRef owner, const IoDimension field,
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
  return IoFieldCells{.occurrence = *occurrence, .view = *view};
}

/// The canonical cell behind @p owner's @p field.
std::expected<CellRef, IoCellError> canonicalOfField(const ArenaManifold &arena,
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

void detachFromRank(ArenaManifold &arena, const CellRef member,
                    const DimRef dim) noexcept {
  // The shared deletion plan (P7) along one rank, Repair: the member's
  // neighbours are joined so the rank stays walkable. link() can throw (a
  // copy-on-write shadow allocation, the link-run limit), and a throw out of a
  // noexcept function would terminate the owner thread. Catching leaves the
  // rank cut at this member rather than relinked: a retired member the walks
  // can no longer reach is the lesser failure, and it is logged so a bound
  // that bites is visible.
  try {
    static_cast<void>(
        arena.detachFromRank(member, dim, zigzag::DeletionMode::Repair));
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.cells", "rank detach left a cut rank: {}",
                      error.what());
  }
}

void detachClonedField(ArenaManifold &arena, const IoCellSchema &schema,
                       const CellRef owner, const IoDimension field) noexcept {
  const auto occurrence =
      arena.linked(owner, schema.dim(field), DimVector::POS);
  if (!occurrence) return;
  const auto view =
      arena.linked(*occurrence, schema.dim(field), DimVector::POS);
  if (!view) return;
  detachFromRank(arena, *view, schema.dim(IoDimension::Clone));
}

namespace {

/// A canonical cell that other cells view through its clone rank: the rank's
/// head, with followers and no predecessor. Freeing it, or the graph hanging
/// off it, would leave those views naming a freed master.
bool heldByViews(const ArenaManifold &arena, const CellRef cell,
                 const DimRef cloneDim) {
  return arena.linked(cell, cloneDim, DimVector::POS).has_value() &&
         !arena.linked(cell, cloneDim, DimVector::NEG).has_value();
}

/// Unlink and free @p root and every cell reachable from it along @p dims,
/// children before parents, so a holder of the root sees a stale handle and
/// never a half-freed graph. A reachable cell some other cell still views
/// (heldByViews) is left whole, with everything below it; the views that this
/// graph itself filed on other canonical cells leave their ranks first. The
/// caller has already dealt with @p root's own clone rank.
void freeOwnedGraph(ArenaManifold &arena, const IoCellSchema &schema,
                    const CellRef root, const std::span<const IoDimension> dims,
                    const std::string_view what) {
  const auto cloneDim = schema.dim(IoDimension::Clone);
  std::vector<CellRef> owned{root};
  std::unordered_set<CellRef> seen{root};
  for (std::size_t at = 0; at < owned.size(); ++at) {
    for (const auto which : dims) {
      const auto next =
          arena.linked(owned[at], schema.dim(which), DimVector::POS);
      if (!next || !seen.insert(*next).second) continue;
      if (heldByViews(arena, *next, cloneDim)) {
        // Keep the held cell and its subgraph; only this graph's link to it
        // goes.
        static_cast<void>(
            arena.unlink(owned[at], schema.dim(which), DimVector::POS));
        GLEDITOR_LOG_DEBUG("vortex.io.cells",
                           "{} reap kept a cell other cells view", what);
        continue;
      }
      owned.push_back(*next);
    }
  }
  for (const auto cell : owned) {
    // A view this graph filed on a canonical cell's clone rank, such as an
    // outcome's endpoint field, leaves the rank with the cell.
    if (cell != root) detachFromRank(arena, cell, cloneDim);
    for (const auto which : dims) {
      if (arena.linked(cell, schema.dim(which), DimVector::POS)) {
        static_cast<void>(
            arena.unlink(cell, schema.dim(which), DimVector::POS));
      }
    }
  }
  for (const auto cell : owned | std::views::reverse) {
    const auto incarnation = arena.incarnationOf(cell);
    if (!incarnation) continue;
    if (const auto freed = arena.freeCell(cell, *incarnation); !freed) {
      GLEDITOR_LOG_DEBUG("vortex.io.cells", "{} cell kept: {}", what,
                         zigzag::toString(freed.error()));
    }
  }
}

} // namespace

void reapIoRequest(ArenaManifold &arena, const IoCellSchema &schema,
                   const RequestCell request) noexcept {
  if (!arena.matches(request.cell, request.incarnation)) return;
  const auto tag = ioCellTag(arena, request.cell, request.incarnation);
  if (!tag || *tag != IoCellTag::Request) return;
  detachFromRank(arena, request.cell, schema.dim(IoDimension::Queue));
  detachClonedField(arena, schema, request.cell, IoDimension::Endpoint);
  detachClonedField(arena, schema, request.cell, IoDimension::Capability);
  // Only the request's own subgraph is freed, and only once nothing else
  // holds it. A capture's Request field is a view on the request's clone rank;
  // it names the request until the capture detaches, and the request's outcome
  // is how the capture's commit finds the append's result. Freeing under it
  // would strand both, so the request stays whole until the views are gone.
  const auto cloneDim = schema.dim(IoDimension::Clone);
  try {
    if (arena.linked(request.cell, cloneDim, DimVector::POS) ||
        arena.linked(request.cell, cloneDim, DimVector::NEG)) {
      GLEDITOR_LOG_DEBUG("vortex.io.cells",
                         "request still has clone views; not freed");
      return;
    }
    // The cells hanging off the request by the dimensions only a request
    // owns: its endpoint and capability fields, its outcome and what an
    // outcome carries. Clone, queue and the waiter and request fields are the
    // shared ranks, so a walk along them would leave the request's graph.
    constexpr std::array ownedDims{
        IoDimension::Endpoint, IoDimension::Capability, IoDimension::Outcome,
        IoDimension::Payload,  IoDimension::Source,     IoDimension::Occurrence,
        IoDimension::State,    IoDimension::Progress,   IoDimension::Range};
    freeOwnedGraph(arena, schema, request.cell, ownedDims, "request");
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.cells", "request reap stopped: {}",
                      error.what());
  }
}

bool reapIoEndpoint(ArenaManifold &arena, const IoCellSchema &schema,
                    const EndpointCell endpoint) noexcept {
  if (!arena.matches(endpoint.cell, endpoint.incarnation)) return false;
  const auto tag = ioCellTag(arena, endpoint.cell, endpoint.incarnation);
  if (!tag || *tag != IoCellTag::Endpoint) return false;
  const auto cloneDim = schema.dim(IoDimension::Clone);
  const auto fieldDim = schema.dim(IoDimension::Endpoint);
  try {
    const auto queue = arena.linked(
        endpoint.cell, schema.dim(IoDimension::Occurrence), DimVector::POS);
    if (queue &&
        arena.linked(*queue, schema.dim(IoDimension::Queue), DimVector::POS)) {
      GLEDITOR_LOG_DEBUG("vortex.io.cells",
                         "endpoint kept: requests still queued");
      return false;
    }
    // Every view on the endpoint's clone rank must be a capability's endpoint
    // field (view <- occurrence <- capability); one request view, or any other
    // owner, means the graph is still in use.
    std::vector<CellRef> capabilities;
    auto at = endpoint.cell;
    for (std::size_t visited = 0; visited <= arena.traversalBound();
         ++visited) {
      const auto view = arena.linked(at, cloneDim, DimVector::POS);
      if (!view) break;
      at                    = *view;
      const auto occurrence = arena.linked(*view, fieldDim, DimVector::NEG);
      const auto owner =
          occurrence ? arena.linked(*occurrence, fieldDim, DimVector::NEG)
                     : std::nullopt;
      const auto ownerIncarnation =
          owner ? arena.incarnationOf(*owner) : std::nullopt;
      if (!ownerIncarnation) return false;
      const auto ownerTag = ioCellTag(arena, *owner, *ownerIncarnation);
      if (!ownerTag || *ownerTag != IoCellTag::Capability) {
        GLEDITOR_LOG_DEBUG("vortex.io.cells",
                           "endpoint kept: a {} still views it",
                           ownerTag ? std::string{toString(*ownerTag)}
                                    : std::string{"stale cell"});
        return false;
      }
      // A request that still views this capability keeps it.
      if (arena.linked(*owner, cloneDim, DimVector::POS) ||
          arena.linked(*owner, cloneDim, DimVector::NEG)) {
        GLEDITOR_LOG_DEBUG("vortex.io.cells",
                           "endpoint kept: a capability has views");
        return false;
      }
      capabilities.push_back(*owner);
    }
    constexpr std::array capabilityDims{
        IoDimension::Endpoint, IoDimension::Capability, IoDimension::Occurrence,
        IoDimension::State};
    for (const auto capability : capabilities) {
      freeOwnedGraph(arena, schema, capability, capabilityDims, "capability");
    }
    // What remains of the endpoint is its queue; the clone rank is empty now.
    if (arena.linked(endpoint.cell, cloneDim, DimVector::POS)) return false;
    constexpr std::array endpointDims{IoDimension::Occurrence};
    freeOwnedGraph(arena, schema, endpoint.cell, endpointDims, "endpoint");
    return !arena.matches(endpoint.cell, endpoint.incarnation);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.cells", "endpoint reap stopped: {}",
                      error.what());
    return false;
  }
}

void reapIoWaiter(ArenaManifold &arena, const IoCellSchema &schema,
                  const WaiterCell waiter) noexcept {
  if (!arena.matches(waiter.cell, waiter.incarnation)) return;
  // The request view leaves the request's clone rank first: that is what lets
  // the request itself be reaped later.
  detachClonedField(arena, schema, waiter.cell, IoDimension::Request);
  try {
    const auto cloneDim = schema.dim(IoDimension::Clone);
    if (arena.linked(waiter.cell, cloneDim, DimVector::POS) ||
        arena.linked(waiter.cell, cloneDim, DimVector::NEG)) {
      GLEDITOR_LOG_DEBUG("vortex.io.cells", "waiter still has clone views");
      return;
    }
    // The request field (occurrence and view) and the cursor, incarnation and
    // continuation records the host's registration left on the occurrence and
    // state ranks.
    constexpr std::array ownedDims{IoDimension::Request,
                                   IoDimension::Occurrence, IoDimension::State};
    freeOwnedGraph(arena, schema, waiter.cell, ownedDims, "waiter");
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.cells", "waiter reap stopped: {}",
                      error.what());
  }
}

bool appendIoField(ArenaManifold &arena, const CellRef record,
                   const DimRef rankDim, const DimRef valueDim,
                   const std::string_view label, const std::string_view value) {
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
  return static_cast<bool>(
      arena.link(tail, rankDim, DimVector::POS, field) &&
      arena.link(field, valueDim, DimVector::POS, content));
}

bool addIoProgressFields(
    ArenaManifold &arena, const IoCellSchema &schema, const CellRef outcome,
    const std::span<const std::pair<std::string_view, std::string_view>>
        fields) {
  const auto occurrence            = arena.makeCell("io progress occurrence");
  const auto occurrenceIncarnation = arena.incarnationOf(occurrence);
  const auto progressCell          = arena.makeCell("io progress");
  const auto progressIncarnation   = arena.incarnationOf(progressCell);
  if (!occurrenceIncarnation || !progressIncarnation ||
      !arena.markRuntimeOnly(occurrence) ||
      !arena.markRuntimeOnly(progressCell) ||
      !arena.setRuntimeTag(progressCell, *progressIncarnation,
                           encodeTag(IoCellTag::Progress, std::nullopt))) {
    return false;
  }
  const auto dim = schema.dim(IoDimension::Progress);
  if (!arena.link(outcome, dim, DimVector::POS, occurrence) ||
      !arena.link(occurrence, dim, DimVector::POS, progressCell)) {
    return false;
  }
  return std::ranges::all_of(fields, [&](const auto &field) {
    return appendIoField(
        arena, progressCell, schema.dim(IoDimension::Occurrence),
        schema.dim(IoDimension::State), field.first, field.second);
  });
}

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
    const auto field = attachClonedField(arena, schema, capability->cell,
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
      canonicalOfField(arena, schema, capability.cell, IoDimension::Endpoint);
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
    const auto endpointField = attachClonedField(
        arena, schema, request->cell, IoDimension::Endpoint, endpoint.cell);
    if (!endpointField) return std::unexpected{endpointField.error()};
    const auto capabilityField = attachClonedField(
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
      canonicalOfField(arena, schema, request.cell, IoDimension::Endpoint);
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
      canonicalOfField(arena, schema, request.cell, IoDimension::Capability);
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

std::expected<OutcomeCell, IoCellError>
makeDetachedOutcomeCell(ArenaManifold &arena, const IoCellSchema &schema,
                        const IoOutcomeTag tag) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  return transact(arena, [&]() -> std::expected<OutcomeCell, IoCellError> {
    const auto outcome = mint(arena, toString(tag), IoCellTag::Outcome, tag);
    if (!outcome) return std::unexpected{outcome.error()};
    return OutcomeCell{.cell        = outcome->cell,
                       .incarnation = outcome->incarnation};
  });
}

std::expected<IoOccurrenceCell, IoCellError>
makeStaleBindingCell(ArenaManifold &arena) {
  return transact(arena, [&] {
    return mint(arena, "stale binding", IoCellTag::StaleBinding);
  });
}

std::expected<SubmissionCell, IoCellError>
makeAdmittedSubmissionCell(ArenaManifold &arena, const IoCellSchema &schema,
                           const RequestCell request) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, request); !ok) {
    return std::unexpected{ok.error()};
  }
  return transact(arena, [&]() -> std::expected<SubmissionCell, IoCellError> {
    const auto submission = mint(arena, "admitted", IoCellTag::Submission);
    if (!submission) return std::unexpected{submission.error()};
    const auto field = attachClonedField(arena, schema, submission->cell,
                                         IoDimension::Request, request.cell);
    if (!field) return std::unexpected{field.error()};
    return SubmissionCell{.cell        = submission->cell,
                          .incarnation = submission->incarnation};
  });
}

std::expected<SubmissionCell, IoCellError>
makeRefusedSubmissionCell(ArenaManifold &arena, const IoCellSchema &schema,
                          const OutcomeCell outcome) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto tag = ioOutcomeTag(arena, outcome); !tag) {
    return std::unexpected{tag.error()};
  }
  return transact(arena, [&]() -> std::expected<SubmissionCell, IoCellError> {
    const auto submission = mint(arena, "refused", IoCellTag::Submission);
    if (!submission) return std::unexpected{submission.error()};
    const auto field = attachClonedField(arena, schema, submission->cell,
                                         IoDimension::Outcome, outcome.cell);
    if (!field) return std::unexpected{field.error()};
    return SubmissionCell{.cell        = submission->cell,
                          .incarnation = submission->incarnation};
  });
}

std::expected<IoSubmissionTag, IoCellError>
submissionTag(const ArenaManifold &arena, const IoCellSchema &schema,
              const SubmissionCell submission) {
  if (!schemaCurrent(arena, schema)) {
    return std::unexpected{IoCellError::StaleSchema};
  }
  if (const auto ok = require(arena, submission); !ok) {
    return std::unexpected{ok.error()};
  }
  const bool admitted =
      arena
          .linked(submission.cell, schema.dim(IoDimension::Request),
                  DimVector::POS)
          .has_value();
  const bool refused =
      arena
          .linked(submission.cell, schema.dim(IoDimension::Outcome),
                  DimVector::POS)
          .has_value();
  if (admitted == refused) return std::unexpected{IoCellError::MalformedGraph};
  return admitted ? IoSubmissionTag::Admitted : IoSubmissionTag::Refused;
}

std::expected<RequestCell, IoCellError>
submissionRequest(const ArenaManifold &arena, const IoCellSchema &schema,
                  const SubmissionCell submission) {
  const auto tag = submissionTag(arena, schema, submission);
  if (!tag) return std::unexpected{tag.error()};
  if (*tag != IoSubmissionTag::Admitted) {
    return std::unexpected{IoCellError::WrongTag};
  }
  const auto canonical =
      canonicalOfField(arena, schema, submission.cell, IoDimension::Request);
  if (!canonical) return std::unexpected{canonical.error()};
  return handleOf<IoCellTag::Request>(arena, *canonical);
}

std::expected<OutcomeCell, IoCellError>
submissionOutcome(const ArenaManifold &arena, const IoCellSchema &schema,
                  const SubmissionCell submission) {
  const auto tag = submissionTag(arena, schema, submission);
  if (!tag) return std::unexpected{tag.error()};
  if (*tag != IoSubmissionTag::Refused) {
    return std::unexpected{IoCellError::WrongTag};
  }
  const auto canonical =
      canonicalOfField(arena, schema, submission.cell, IoDimension::Outcome);
  if (!canonical) return std::unexpected{canonical.error()};
  return handleOf<IoCellTag::Outcome>(arena, *canonical);
}

void releaseSubmissionCell(ArenaManifold &arena, const IoCellSchema &schema,
                           const SubmissionCell submission) noexcept {
  if (!arena.matches(submission.cell, submission.incarnation)) return;
  try {
    std::vector<CellRef> owned{submission.cell};
    for (const auto field : {IoDimension::Request, IoDimension::Outcome}) {
      detachClonedField(arena, schema, submission.cell, field);
      const auto dim = schema.dim(field);
      const auto occurrence =
          arena.linked(submission.cell, dim, DimVector::POS);
      if (!occurrence) continue;
      owned.push_back(*occurrence);
      if (const auto view = arena.linked(*occurrence, dim, DimVector::POS)) {
        static_cast<void>(arena.unlink(*occurrence, dim, DimVector::POS));
        owned.push_back(*view);
      }
      static_cast<void>(arena.unlink(submission.cell, dim, DimVector::POS));
    }
    for (const auto cell : owned | std::views::reverse) {
      const auto incarnation = arena.incarnationOf(cell);
      if (!incarnation) continue;
      if (const auto freed = arena.freeCell(cell, *incarnation); !freed) {
        GLEDITOR_LOG_DEBUG("vortex.io.cells", "submission cell kept: {}",
                           zigzag::toString(freed.error()));
      }
    }
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.cells", "submission release stopped: {}",
                      error.what());
  }
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

std::expected<ReadObservationCell, IoCellError>
makeReadObservationCell(ArenaManifold &arena, PayloadResolver &resolver,
                        const ReadObservationRecord &record) {
  // Pin the bound bytes before minting: a refused retain must not leave a
  // cell claiming a binding it does not hold. retain -> retainOwner ->
  // release mirrors addPayload's non-provided-lease path.
  std::shared_ptr<const PayloadOwnerToken> payload;
  if (record.payload) {
    auto retained = resolver.retain(*record.payload);
    if (!retained) return std::unexpected{IoCellError::PayloadRefused};
    auto owner = resolver.retainOwner(*retained);
    resolver.release(retained->lease);
    if (!owner) return std::unexpected{IoCellError::PayloadRefused};
    payload = std::move(*owner);
  }
  return transact(
      arena, [&]() -> std::expected<ReadObservationCell, IoCellError> {
        const auto cell =
            mint(arena, "io read observation", IoCellTag::ReadObservation);
        if (!cell) return std::unexpected{cell.error()};
        auto token = std::make_shared<ReadObservationOwnerToken>(
            ReadObservationOwnerToken{.record = record, .payload = payload});
        if (!arena.setOwnerLease(cell->cell, cell->incarnation,
                                 std::static_pointer_cast<void>(token))) {
          return std::unexpected{IoCellError::ArenaRefused};
        }
        return ReadObservationCell{.cell        = cell->cell,
                                   .incarnation = cell->incarnation};
      });
}

std::optional<ReadObservationRecord>
readObservationOf(const ArenaManifold &arena, const ReadObservationCell cell) {
  if (const auto ok = require(arena, cell); !ok) return std::nullopt;
  // Only makeReadObservationCell puts an owner lease on a
  // ReadObservation-tagged cell, and it is always a
  // ReadObservationOwnerToken.
  const auto owner = std::static_pointer_cast<const ReadObservationOwnerToken>(
      arena.ownerLeaseOf(cell.cell, cell.incarnation));
  if (!owner) return std::nullopt;
  return owner->record;
}

std::optional<ReadObservationRecord>
outcomeCellReadObservation(const ArenaManifold &arena,
                           const IoCellSchema &schema,
                           const OutcomeCell outcome) {
  // The cell store links outcome -d.io.source-> occurrence -d.io.source->
  // observation cell for a Complete carrying ReadObservationResult.
  const auto dim        = schema.dim(IoDimension::Source);
  const auto occurrence = arena.linked(outcome.cell, dim, DimVector::POS);
  if (!occurrence) return std::nullopt;
  const auto observationCell = arena.linked(*occurrence, dim, DimVector::POS);
  if (!observationCell) return std::nullopt;
  const auto incarnation = arena.incarnationOf(*observationCell);
  if (!incarnation) return std::nullopt;
  return readObservationOf(arena,
                           ReadObservationCell{.cell        = *observationCell,
                                               .incarnation = *incarnation});
}

} // namespace xanadu::vortex
