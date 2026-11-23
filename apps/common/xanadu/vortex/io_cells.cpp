#include "common/xanadu/vortex/io_cells.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace xanadu::vortex {
namespace {

constexpr std::array<std::string_view,
                     static_cast<std::size_t>(IoDimension::Count)>
    dimensionNames{
        "d.io.kind",    "d.io.state",      "d.io.endpoint",   "d.io.capability",
        "d.io.request", "d.io.payload",    "d.io.progress",   "d.io.outcome",
        "d.io.waiter",  "d.io.occurrence", "d.io.queue",      "d.io.clone",
        "d.io.source",  "d.io.range",      "d.io.provenance", "d.io.lease"};

template <typename T, std::size_t N>
std::string_view enumName(const T tag,
                          const std::array<std::string_view, N> &names) {
  const auto index = static_cast<std::size_t>(tag);
  return index < names.size() ? names[index] : std::string_view{};
}

zigzag::CellRef cloneTail(const zigzag::ArenaManifold &arena,
                          zigzag::CellRef canonical,
                          const zigzag::DimRef cloneDim) {
  auto tail = canonical;
  for (std::size_t visited = 0; visited <= arena.traversalBound(); ++visited) {
    const auto next = arena.linked(tail, cloneDim);
    if (!next.has_value()) {
      return tail;
    }
    if (*next == tail || *next == canonical) {
      return zigzag::noCell;
    }
    tail = *next;
  }
  return zigzag::noCell;
}

bool link(zigzag::ArenaManifold &arena, const zigzag::CellRef from,
          const zigzag::DimRef dim, const zigzag::CellRef to) {
  return arena.link(from, dim, zigzag::DimVector::POS, to).has_value();
}

bool markRuntimeOnly(zigzag::ArenaManifold &arena, const zigzag::CellRef cell) {
  return arena.markRuntimeOnly(cell).has_value();
}

void rollback(zigzag::ArenaManifold &arena, const zigzag::Mark &mark) noexcept {
  static_cast<void>(arena.release(mark));
}

struct MintedCell {
  zigzag::CellRef cell;
  zigzag::ArenaManifold::AllocationIncarnation incarnation;
};

std::expected<MintedCell, IoCellError>
mintTaggedCell(zigzag::ArenaManifold &arena, const IoCellTag tag) {
  const auto mark      = arena.mark();
  zigzag::CellRef cell = zigzag::noCell;
  try {
    cell                   = arena.makeCell();
    const auto incarnation = arena.incarnationOf(cell);
    if (!incarnation || !arena.setRuntimeTag(cell, *incarnation,
                                             static_cast<std::uint32_t>(tag))) {
      rollback(arena, mark);
      return std::unexpected(IoCellError::RuntimeOnlyRefused);
    }
  } catch (...) {
    rollback(arena, mark);
    throw;
  }
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation) {
    rollback(arena, mark);
    return std::unexpected(IoCellError::InvalidCanonicalCell);
  }
  if (const auto released = arena.release(mark); !released.has_value()) {
    return std::unexpected(IoCellError::MarkRefused);
  }
  return MintedCell{.cell = cell, .incarnation = *incarnation};
}

template <typename Wrapper>
std::optional<Wrapper> capture(const zigzag::ArenaManifold &arena,
                               const zigzag::CellRef cell) {
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation) {
    return std::nullopt;
  }
  return Wrapper{.cell = cell, .incarnation = *incarnation};
}

bool current(const zigzag::ArenaManifold &arena, const zigzag::CellRef cell,
             const zigzag::ArenaManifold::AllocationIncarnation incarnation) {
  return arena.matches(cell, incarnation);
}

} // namespace

std::string_view toString(const IoCellTag tag) noexcept {
  constexpr std::array<std::string_view, 9> names{
      "Endpoint", "Capability",      "Request",        "Payload", "Progress",
      "Outcome",  "ReadObservation", "CaptureReceipt", "Waiter"};
  return enumName(tag, names);
}

std::string_view toString(const IoOutcomeTag tag) noexcept {
  constexpr std::array<std::string_view, 12> names{
      "Rejected",        "Complete",  "EndOfStream", "Partial",
      "Failed",          "Cancelled", "TimedOut",    "Overflow",
      "MessageTooLarge", "Stale",     "Conflict",    "Unknown"};
  return enumName(tag, names);
}

std::expected<IoCellTag, IoCellError> ioCellTag(
    const zigzag::ArenaManifold &arena, const zigzag::CellRef cell,
    const zigzag::ArenaManifold::AllocationIncarnation incarnation) noexcept {
  if (!arena.matches(cell, incarnation)) {
    return std::unexpected(IoCellError::InvalidCanonicalCell);
  }
  const auto raw = arena.runtimeTagOf(cell, incarnation);
  if (!raw || *raw > static_cast<std::uint32_t>(IoCellTag::Waiter)) {
    return std::unexpected(IoCellError::InvalidCanonicalCell);
  }
  return static_cast<IoCellTag>(*raw);
}

std::expected<EndpointCell, IoCellError>
makeEndpointCell(zigzag::ArenaManifold &arena) {
  const auto minted = mintTaggedCell(arena, IoCellTag::Endpoint);
  if (!minted) return std::unexpected(minted.error());
  return EndpointCell{.cell = minted->cell, .incarnation = minted->incarnation};
}

std::expected<CapabilityCell, IoCellError>
makeCapabilityCell(zigzag::ArenaManifold &arena) {
  const auto minted = mintTaggedCell(arena, IoCellTag::Capability);
  if (!minted) return std::unexpected(minted.error());
  return CapabilityCell{.cell        = minted->cell,
                        .incarnation = minted->incarnation};
}

std::expected<IoCellSchema, IoCellError>
makeIoCellSchema(zigzag::ArenaManifold &arena) {
  const auto mark = arena.mark();
  IoCellSchema schema;
  try {
    for (std::size_t i = 0; i < dimensionNames.size(); ++i) {
      const auto dimension = arena.makeCell(dimensionNames[i]);
      if (!markRuntimeOnly(arena, dimension)) {
        rollback(arena, mark);
        return std::unexpected(IoCellError::RuntimeOnlyRefused);
      }
      const auto incarnation = arena.incarnationOf(dimension);
      if (!incarnation) {
        rollback(arena, mark);
        return std::unexpected(IoCellError::InvalidCanonicalCell);
      }
      schema.dims[i] = {.cell = dimension, .incarnation = *incarnation};
    }
    const auto queueHead = arena.makeCell("d.io.queue.head");
    if (!markRuntimeOnly(arena, queueHead)) {
      rollback(arena, mark);
      return std::unexpected(IoCellError::RuntimeOnlyRefused);
    }
    const auto queueIncarnation = arena.incarnationOf(queueHead);
    if (!queueIncarnation) {
      rollback(arena, mark);
      return std::unexpected(IoCellError::InvalidCanonicalCell);
    }
    schema.queueHead = {.cell = queueHead, .incarnation = *queueIncarnation};
  } catch (...) {
    rollback(arena, mark);
    throw;
  }
  if (const auto released = arena.release(mark); !released.has_value()) {
    return std::unexpected(IoCellError::MarkRefused);
  }
  return schema;
}

std::expected<IoRequestGraph, IoCellError>
appendIoRequest(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                const EndpointCell endpoint, const CapabilityCell capability) {
  const bool validSchema =
      current(arena, schema.queueHead.cell, schema.queueHead.incarnation) &&
      std::ranges::all_of(schema.dims, [&](const auto &dimension) {
        return current(arena, dimension.cell, dimension.incarnation);
      });
  if (!validSchema || !current(arena, endpoint.cell, endpoint.incarnation) ||
      !current(arena, capability.cell, capability.incarnation)) {
    return std::unexpected(IoCellError::InvalidCanonicalCell);
  }
  const auto endpointTag =
      ioCellTag(arena, endpoint.cell, endpoint.incarnation);
  const auto capabilityTag =
      ioCellTag(arena, capability.cell, capability.incarnation);
  if (!endpointTag || *endpointTag != IoCellTag::Endpoint || !capabilityTag ||
      *capabilityTag != IoCellTag::Capability) {
    return std::unexpected(IoCellError::InvalidCanonicalCell);
  }

  const auto mark = arena.mark();
  IoRequestGraph graph;
  try {
    graph.request.cell              = arena.makeCell();
    graph.endpointOccurrence.cell   = arena.makeCell("endpoint occurrence");
    graph.endpointView.cell         = arena.makeCell("endpoint view");
    graph.capabilityOccurrence.cell = arena.makeCell("capability occurrence");
    graph.capabilityView.cell       = arena.makeCell("capability view");
    for (const auto cell :
         {graph.request.cell, graph.endpointOccurrence.cell,
          graph.endpointView.cell, graph.capabilityOccurrence.cell,
          graph.capabilityView.cell}) {
      if (!markRuntimeOnly(arena, cell)) {
        rollback(arena, mark);
        return std::unexpected(IoCellError::RuntimeOnlyRefused);
      }
    }
    const auto requestIncarnation = arena.incarnationOf(graph.request.cell);
    if (!requestIncarnation ||
        !arena.setRuntimeTag(graph.request.cell, *requestIncarnation,
                             static_cast<std::uint32_t>(IoCellTag::Request))) {
      rollback(arena, mark);
      return std::unexpected(IoCellError::RuntimeOnlyRefused);
    }

    const auto endpointTail =
        cloneTail(arena, endpoint.cell, schema.dim(IoDimension::Clone));
    const auto capabilityTail =
        cloneTail(arena, capability.cell, schema.dim(IoDimension::Clone));
    auto queueTail = schema.queueHead.cell;
    while (const auto next =
               arena.linked(queueTail, schema.dim(IoDimension::Queue))) {
      if (*next == queueTail || *next == schema.queueHead.cell) {
        rollback(arena, mark);
        return std::unexpected(IoCellError::LinkRefused);
      }
      queueTail = *next;
    }

    const bool linked =
        endpointTail != zigzag::noCell && capabilityTail != zigzag::noCell &&
        link(arena, graph.request.cell, schema.dim(IoDimension::Endpoint),
             graph.endpointOccurrence.cell) &&
        link(arena, graph.endpointOccurrence.cell,
             schema.dim(IoDimension::Endpoint), graph.endpointView.cell) &&
        link(arena, endpointTail, schema.dim(IoDimension::Clone),
             graph.endpointView.cell) &&
        link(arena, graph.request.cell, schema.dim(IoDimension::Capability),
             graph.capabilityOccurrence.cell) &&
        link(arena, graph.capabilityOccurrence.cell,
             schema.dim(IoDimension::Capability), graph.capabilityView.cell) &&
        link(arena, capabilityTail, schema.dim(IoDimension::Clone),
             graph.capabilityView.cell) &&
        link(arena, queueTail, schema.dim(IoDimension::Queue),
             graph.request.cell);
    if (!linked) {
      rollback(arena, mark);
      return std::unexpected(IoCellError::LinkRefused);
    }

    const auto request = capture<RequestCell>(arena, graph.request.cell);
    const auto endpointOccurrence =
        capture<IoOccurrenceCell>(arena, graph.endpointOccurrence.cell);
    const auto endpointView =
        capture<IoOccurrenceCell>(arena, graph.endpointView.cell);
    const auto capabilityOccurrence =
        capture<IoOccurrenceCell>(arena, graph.capabilityOccurrence.cell);
    const auto capabilityView =
        capture<IoOccurrenceCell>(arena, graph.capabilityView.cell);
    if (!request || !endpointOccurrence || !endpointView ||
        !capabilityOccurrence || !capabilityView) {
      rollback(arena, mark);
      return std::unexpected(IoCellError::InvalidCanonicalCell);
    }
    graph.request              = *request;
    graph.endpointOccurrence   = *endpointOccurrence;
    graph.endpointView         = *endpointView;
    graph.capabilityOccurrence = *capabilityOccurrence;
    graph.capabilityView       = *capabilityView;
  } catch (...) {
    rollback(arena, mark);
    throw;
  }
  if (const auto released = arena.release(mark); !released.has_value()) {
    return std::unexpected(IoCellError::MarkRefused);
  }
  return graph;
}

} // namespace xanadu::vortex
