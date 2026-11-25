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
 *
 * The graph is the topology and the tags, an inspectable owner-thread view
 * of native state: request operation, state and arguments, endpoint state
 * and capability rights live in IoService and the adapters, and nothing
 * rebuilds them from cells (plan §3.1, reconciliation paragraph).
 */

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
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
  /// A capture transaction's owner-only cell (plan §3.1 "Capture"). Its
  /// graph holds the append request and receipt links; the selected source
  /// intervals, map reservation and phase live in the coordinator's pending
  /// record and the journal, and the cell's decimal fields only mirror them.
  Capture,
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

using EndpointCell        = IoCellHandle<IoCellTag::Endpoint>;
using CapabilityCell      = IoCellHandle<IoCellTag::Capability>;
using RequestCell         = IoCellHandle<IoCellTag::Request>;
using PayloadCell         = IoCellHandle<IoCellTag::Payload>;
using OutcomeCell         = IoCellHandle<IoCellTag::Outcome>;
using ReadObservationCell = IoCellHandle<IoCellTag::ReadObservation>;
using WaiterCell          = IoCellHandle<IoCellTag::Waiter>;
using CaptureCell         = IoCellHandle<IoCellTag::Capture>;
using CaptureReceiptCell  = IoCellHandle<IoCellTag::CaptureReceipt>;

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

/// A field's private occurrence and the clone view it points at.
struct IoFieldCells {
  IoOccurrenceCell occurrence;
  IoOccurrenceCell view;
};

/// The one way any I/O cell reaches a canonical cell (endpoint, capability,
/// request): owner -field-> occurrence -field-> clone view, the view
/// appended to @p canonical's d.io.clone rank. The canonical cell is never
/// a target on @p field, so two owners never evict each other. Not
/// transactional: call it inside one.
[[nodiscard]] std::expected<IoFieldCells, IoCellError>
attachClonedField(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                  zigzag::CellRef owner, IoDimension field,
                  zigzag::CellRef canonical);
/// The canonical cell behind @p owner's @p field.
[[nodiscard]] std::expected<zigzag::CellRef, IoCellError>
canonicalOfField(const zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                 zigzag::CellRef owner, IoDimension field);
/// Remove @p owner's @p field view from its canonical's clone rank, joining
/// the neighbours so the rank stays one chain. A no-op without the field.
void detachClonedField(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                       zigzag::CellRef owner, IoDimension field) noexcept;
/// Take a retired request out of its endpoint's queue and its endpoint and
/// capability clone ranks, so those ranks (walked on every submit) stay as
/// long as the live requests rather than every request ever made. The
/// request keeps its outcome link: a late poll still resolves. The request,
/// occurrence and view cells themselves are not freed: an ArenaManifold
/// releases cells only by truncating to a mark, so arena cell count still
/// grows with every request; only the rank walks are bounded here.
void reapIoRequest(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
                   RequestCell request) noexcept;

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

/// Declared capability a source endpoint offers for a read's consistency
/// requirement (plan §7.2). `Live` is best-effort observation of whatever
/// the backend returns; `Stable` requires a backend-proven immutable
/// snapshot/CAS the source must actually offer. Shared protocol
/// vocabulary: consumed by `ReadObservationRecord`, file capabilities and
/// the `OpenSpec`/`OpenGrant` consistency masks.
enum class SourceConsistency : std::uint8_t { Live, Stable };

/// The typed record a ReadObservation cell's owner lease carries (plan
/// §7.2): what a live read observed -- the source locator, the generation
/// and consistency at transfer time, the ACTUAL bytes-read extent (which
/// may be shorter than requested at EOF), and the exact retained bytes it
/// binds.
struct ReadObservationRecord {
  std::uint64_t observationId{};
  std::uint64_t sourceGeneration{};
  std::string locator;
  SourceConsistency consistency{SourceConsistency::Live};
  /// First source offset of the retained bytes.
  std::uint64_t offset{};
  /// Retained byte count: the actual extent read, not the request's.
  std::uint64_t length{};
  /// The retained bytes this observation binds; absent for a zero-byte
  /// read. Resolved/read through PayloadResolver like any payload range.
  std::optional<PayloadRange> payload;
};

/// What a ReadObservation cell's owner lease holds: the record plus, when
/// the read retained bytes, the PayloadOwnerToken pinning them -- the same
/// token a PayloadCell holds, so the bound bytes stay readable through
/// PayloadOwnerToken::read while the observation cell lives, independent
/// of the outcome's payload cell.
struct ReadObservationOwnerToken {
  ReadObservationRecord record;
  std::shared_ptr<const PayloadOwnerToken> payload;
};

/// A payload cell owning a fresh resolver lease on @p lease's range; rollback
/// or overwrite of the cell, or arena destruction, releases that lease.
[[nodiscard]] std::expected<PayloadCell, IoCellError>
makePayloadCell(zigzag::ArenaManifold &arena, PayloadResolver &resolver,
                const PayloadLease &lease);
[[nodiscard]] std::optional<PayloadRange>
payloadRangeOf(const zigzag::ArenaManifold &arena, PayloadCell payload);

/// The payload range an outcome cell carries, if any: walks the outcome's
/// d.io.payload occurrence to its payload cell and reads the owner lease's
/// range. This is the owner-side read path for consumers decoding retained
/// bytes (resolve the range through PayloadResolver afterwards).
[[nodiscard]] std::optional<PayloadRange>
outcomeCellPayloadRange(const zigzag::ArenaManifold &arena,
                        const IoCellSchema &schema, OutcomeCell outcome);

/// Mint a ReadObservation cell recording @p record (plan §7.2). When
/// record.payload names a range, the cell takes its own owner lease on
/// those bytes -- independent of the outcome's payload cell -- so the
/// observation keeps them readable while it lives.
[[nodiscard]] std::expected<ReadObservationCell, IoCellError>
makeReadObservationCell(zigzag::ArenaManifold &arena, PayloadResolver &resolver,
                        const ReadObservationRecord &record);
/// The record a ReadObservation cell carries, or nullopt when the cell is
/// stale, wrong-tag or holds no owner lease.
[[nodiscard]] std::optional<ReadObservationRecord>
readObservationOf(const zigzag::ArenaManifold &arena,
                  ReadObservationCell observation);
/// The ReadObservation record an outcome cell links, if any: walks the
/// outcome's d.io.source occurrence to its observation cell (the cell
/// store mints one for a Complete carrying ReadObservationResult).
[[nodiscard]] std::optional<ReadObservationRecord>
outcomeCellReadObservation(const zigzag::ArenaManifold &arena,
                           const IoCellSchema &schema, OutcomeCell outcome);

} // namespace xanadu::vortex

#endif
