#ifndef XANADU_VORTEX_IO_CELLS_HPP
#define XANADU_VORTEX_IO_CELLS_HPP

#include <array>
#include <expected>
#include <string_view>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vortex {

enum class IoCellTag : std::uint8_t {
  Endpoint,
  Capability,
  Request,
  Payload,
  Progress,
  Outcome,
  ReadObservation,
  CaptureReceipt,
  Waiter,
};

enum class IoOutcomeTag : std::uint8_t {
  Rejected,
  Complete,
  EndOfStream,
  Partial,
  Failed,
  Cancelled,
  TimedOut,
  Overflow,
  MessageTooLarge,
  Stale,
  Conflict,
  Unknown,
};

enum class IoDimension : std::uint8_t {
  Kind,
  State,
  Endpoint,
  Capability,
  Request,
  Payload,
  Progress,
  Outcome,
  Waiter,
  Occurrence,
  Queue,
  Clone,
  Source,
  Range,
  Provenance,
  Lease,
  Count,
};

struct IoCellSchema {
  struct DimensionCell {
    zigzag::DimRef cell{zigzag::noCell};
    zigzag::ArenaManifold::AllocationIncarnation incarnation{};
  };
  struct QueueCell {
    zigzag::CellRef cell{zigzag::noCell};
    zigzag::ArenaManifold::AllocationIncarnation incarnation{};
  };

  std::array<DimensionCell, static_cast<std::size_t>(IoDimension::Count)>
      dims{};
  QueueCell queueHead{};

  [[nodiscard]] zigzag::DimRef dim(IoDimension which) const noexcept {
    return dims[static_cast<std::size_t>(which)].cell;
  }
};

struct EndpointCell {
  zigzag::CellRef cell{zigzag::noCell};
  zigzag::ArenaManifold::AllocationIncarnation incarnation{};
};

struct CapabilityCell {
  zigzag::CellRef cell{zigzag::noCell};
  zigzag::ArenaManifold::AllocationIncarnation incarnation{};
};

struct RequestCell {
  zigzag::CellRef cell{zigzag::noCell};
  zigzag::ArenaManifold::AllocationIncarnation incarnation{};
};

struct IoOccurrenceCell {
  zigzag::CellRef cell{zigzag::noCell};
  zigzag::ArenaManifold::AllocationIncarnation incarnation{};
};

struct IoRequestGraph {
  RequestCell request{};
  IoOccurrenceCell endpointOccurrence{};
  IoOccurrenceCell endpointView{};
  IoOccurrenceCell capabilityOccurrence{};
  IoOccurrenceCell capabilityView{};
};

enum class IoCellError : std::uint8_t {
  InvalidCanonicalCell,
  RuntimeOnlyRefused,
  LinkRefused,
  MarkRefused,
};

[[nodiscard]] std::string_view toString(IoCellTag tag) noexcept;
[[nodiscard]] std::string_view toString(IoOutcomeTag tag) noexcept;
[[nodiscard]] std::expected<IoCellTag, IoCellError>
ioCellTag(const zigzag::ArenaManifold &arena, zigzag::CellRef cell,
          zigzag::ArenaManifold::AllocationIncarnation incarnation) noexcept;
[[nodiscard]] std::expected<EndpointCell, IoCellError>
makeEndpointCell(zigzag::ArenaManifold &arena);
[[nodiscard]] std::expected<CapabilityCell, IoCellError>
makeCapabilityCell(zigzag::ArenaManifold &arena);
[[nodiscard]] std::expected<IoCellSchema, IoCellError>
makeIoCellSchema(zigzag::ArenaManifold &arena);
[[nodiscard]] std::expected<IoRequestGraph, IoCellError>
appendIoRequest(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                EndpointCell endpoint, CapabilityCell capability);

} // namespace xanadu::vortex

#endif
