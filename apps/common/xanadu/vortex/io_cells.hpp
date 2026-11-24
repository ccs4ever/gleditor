#ifndef XANADU_VORTEX_IO_CELLS_HPP
#define XANADU_VORTEX_IO_CELLS_HPP

/**
 * @file io_cells.hpp
 * @brief The Vortex I/O cell schema (design/vortex-io-implementation-plan.md
 *        §3.1): d.io.* dimensions, typed cell tags, and the request graph.
 *
 * Every request field is a private occurrence cell whose posward neighbour on
 * the field's dimension is a clone view of the shared canonical cell, so a
 * canonical endpoint or capability is never a fan-in target and every cell
 * keeps one neighbour per direction. An endpoint's queue is a rank of request
 * cells hanging off a private queue occurrence. Tags live in the arena's
 * incarnation-keyed runtime tag, so cell text cannot forge one, and every
 * I/O cell is runtime-only, so Structure refuses to persist it.
 */

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vortex {

using AllocationIncarnation = zigzag::ArenaManifold::AllocationIncarnation;

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
};
inline constexpr std::size_t ioDimensionCount = 16;

/// An owner-only reference to one live allocation of a cell carrying @p Tag.
template <IoCellTag Tag> struct IoCellHandle {
  static constexpr IoCellTag tag = Tag;
  zigzag::CellRef cell{};
  AllocationIncarnation incarnation{};
  friend bool operator==(const IoCellHandle &, const IoCellHandle &) = default;
};

using EndpointCell   = IoCellHandle<IoCellTag::Endpoint>;
using CapabilityCell = IoCellHandle<IoCellTag::Capability>;
using RequestCell    = IoCellHandle<IoCellTag::Request>;
using PayloadCell    = IoCellHandle<IoCellTag::Payload>;
using OutcomeCell    = IoCellHandle<IoCellTag::Outcome>;
using WaiterCell     = IoCellHandle<IoCellTag::Waiter>;

/// A private, untagged structural cell: a field occurrence, clone view or
/// queue head.
struct IoOccurrenceCell {
  zigzag::CellRef cell{};
  AllocationIncarnation incarnation{};
  friend bool operator==(const IoOccurrenceCell &,
                         const IoOccurrenceCell &) = default;
};

/// The d.io.* dimensions as this arena holds them.
struct IoCellSchema {
  struct DimensionCell {
    zigzag::DimRef cell{};
    AllocationIncarnation incarnation{};
  };
  std::array<DimensionCell, ioDimensionCount> dims{};

  [[nodiscard]] zigzag::DimRef dim(IoDimension which) const noexcept {
    return dims[static_cast<std::size_t>(which)].cell;
  }
};

struct IoRequestGraph {
  RequestCell request;
  IoOccurrenceCell endpointOccurrence;
  IoOccurrenceCell endpointView;
  IoOccurrenceCell capabilityOccurrence;
  IoOccurrenceCell capabilityView;
};

enum class IoCellError : std::uint8_t {
  StaleCell,         ///< the incarnation no longer names the allocation
  WrongTag,          ///< the cell is not tagged as the expected I/O cell
  StaleSchema,       ///< a d.io.* dimension was rolled back
  InvalidCapability, ///< the capability is not bound to the endpoint
  LinkOccupied,      ///< linking would overwrite an existing neighbour
  AlreadyTerminal,   ///< the request already has its one outcome
  InvalidOutcome,    ///< Rejected with a request, or another tag without one
  MalformedGraph,    ///< a field occurrence or view is missing or cyclic
  PayloadRefused,    ///< the resolver refused to pin the payload
  ArenaRefused,      ///< the arena refused a write
};

[[nodiscard]] std::string_view toString(IoCellTag tag) noexcept;
[[nodiscard]] std::string_view toString(IoOutcomeTag tag) noexcept;
[[nodiscard]] std::string_view toString(IoCellError error) noexcept;
[[nodiscard]] std::string_view dimensionName(IoDimension which) noexcept;

[[nodiscard]] std::expected<IoCellTag, IoCellError>
ioCellTag(const zigzag::ArenaManifold &arena, zigzag::CellRef cell,
          AllocationIncarnation incarnation) noexcept;
[[nodiscard]] std::expected<IoOutcomeTag, IoCellError>
ioOutcomeTag(const zigzag::ArenaManifold &arena, OutcomeCell outcome) noexcept;

/// The d.io.* dimensions through the arena's dimension registry
/// (ArenaManifold::ensureDimension): minted on first use, found thereafter.
[[nodiscard]] std::expected<IoCellSchema, IoCellError>
makeIoCellSchema(zigzag::ArenaManifold &arena);

[[nodiscard]] std::expected<EndpointCell, IoCellError>
makeEndpointCell(zigzag::ArenaManifold &arena, const IoCellSchema &schema);
/// Mint an owner-only runtime request that has not yet acquired endpoint
/// authority, as required for asynchronous Open.
[[nodiscard]] std::expected<RequestCell, IoCellError>
makeOpenRequestCell(zigzag::ArenaManifold &arena);
/// Mint a transient owner-only waiter cell.
[[nodiscard]] std::expected<WaiterCell, IoCellError>
makeWaiterCell(zigzag::ArenaManifold &arena);
[[nodiscard]] std::expected<IoOccurrenceCell, IoCellError>
endpointQueue(const zigzag::ArenaManifold &arena, const IoCellSchema &schema,
              EndpointCell endpoint);

/// A capability bound to @p endpoint through its own endpoint occurrence.
[[nodiscard]] std::expected<CapabilityCell, IoCellError>
makeCapabilityCell(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                   EndpointCell endpoint);
[[nodiscard]] std::expected<EndpointCell, IoCellError>
capabilityEndpoint(const zigzag::ArenaManifold &arena,
                   const IoCellSchema &schema, CapabilityCell capability);

/// Mint a request on @p capability and append it to @p endpoint's queue.
/// Refuses, having changed nothing, unless the capability names the endpoint.
[[nodiscard]] std::expected<IoRequestGraph, IoCellError>
appendIoRequest(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                EndpointCell endpoint, CapabilityCell capability);
[[nodiscard]] std::expected<EndpointCell, IoCellError>
requestEndpoint(const zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                RequestCell request);
[[nodiscard]] std::expected<CapabilityCell, IoCellError>
requestCapability(const zigzag::ArenaManifold &arena,
                  const IoCellSchema &schema, RequestCell request);
/// @p endpoint's queue in admission order.
[[nodiscard]] std::expected<std::vector<RequestCell>, IoCellError>
queuedRequests(const zigzag::ArenaManifold &arena, const IoCellSchema &schema,
               EndpointCell endpoint);

/// An outcome cell. Rejected has no request, since nothing was admitted;
/// every other tag is the one terminal outcome of @p request.
[[nodiscard]] std::expected<OutcomeCell, IoCellError>
makeOutcomeCell(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                std::optional<RequestCell> request, IoOutcomeTag tag);

/// A payload cell owning a fresh resolver lease on @p lease's range; rollback
/// or overwrite of the cell, or arena destruction, releases that lease.
[[nodiscard]] std::expected<PayloadCell, IoCellError>
makePayloadCell(zigzag::ArenaManifold &arena, PayloadResolver &resolver,
                const PayloadLease &lease);
[[nodiscard]] std::optional<PayloadRange>
payloadRangeOf(const zigzag::ArenaManifold &arena, PayloadCell payload);

} // namespace xanadu::vortex

#endif
