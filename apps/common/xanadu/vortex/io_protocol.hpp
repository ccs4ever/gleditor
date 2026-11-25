#ifndef XANADU_VORTEX_IO_PROTOCOL_HPP
#define XANADU_VORTEX_IO_PROTOCOL_HPP

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/io_error.hpp"

namespace xanadu::vortex {

using ContinuationId = std::uint64_t;
using OwnerCell      = zigzag::CellRef;

struct PrivateRequestToken {
  std::uint64_t value{};
  friend bool operator==(const PrivateRequestToken &,
                         const PrivateRequestToken &) = default;
};
struct PrivateReleaseToken {
  PrivateRequestToken request;
  friend bool operator==(const PrivateReleaseToken &,
                         const PrivateReleaseToken &) = default;
};

enum class IoPhase : std::uint8_t {
  Preparing,
  Queued,
  Running,
  Cancelling,
  Terminal
};
struct IoProgress {
  std::uint64_t acceptedBytes{};
  std::uint64_t transferredBytes{};
  std::uint64_t consumedBytes{};
  std::optional<std::uint64_t> sourceSequence;
  IoPhase phase{IoPhase::Preparing};
};
enum class PartialCause : std::uint8_t {
  EndOfStream,
  Cancelled,
  TimedOut,
  Overflow,
  BackendFailure
};
enum class OversizeDisposition : std::uint8_t { Retained, Consumed, Unknown };
struct Generation {
  std::uint64_t value{};
};
struct GapRange {
  std::uint64_t firstSequence{}, lastSequence{};
};
struct OpenedEndpoint {
  EndpointCell endpoint;
  CapabilityCell initialCapability;
};
/// Terminal result details for a successful live read (plan §7.2): the
/// adapter's observation id, the source generation and consistency it
/// observed, and the ACTUAL bytes-read extent -- which may be shorter than
/// requested at EOF. The retained bytes ride the outcome's `payload`
/// field; the cell store mints the ReadObservation cell binding them, so
/// an owner maps a completed read to its observation through the outcome.
struct ReadObservationResult {
  std::uint64_t observationId{};
  std::uint64_t sourceGeneration{};
  std::uint64_t readOffset{};
  std::uint64_t readLength{};
  std::string locator;
  SourceConsistency consistency{SourceConsistency::Live};
};
/// Terminal result details that travel inside the outcome itself.
/// Byte payloads never live here: adapters route produced bytes through
/// PayloadResolver (publishTransientBytes) and name the retained range in
/// the outcome's `payload` field instead (plan §3.2, §4).
using IoResultValue =
    std::variant<std::monostate, OpenedEndpoint, ReadObservationResult>;
struct Complete {
  std::optional<PayloadRange> payload;
  IoResultValue value;
  IoProgress progress;
};
struct EndOfStream {
  IoProgress progress;
};
struct Partial {
  PayloadRange payload;
  PartialCause cause;
  IoProgress progress;
};
struct Failed {
  IoErrorInfo reason;
  std::optional<PayloadRange> payload;
  IoProgress progress;
};
struct Cancelled {
  std::optional<PayloadRange> payload;
  IoProgress progress;
  bool uncertain{};
};
struct TimedOut {
  std::optional<PayloadRange> payload;
  IoProgress progress;
  bool uncertain{};
};
struct Overflow {
  GapRange gap;
  IoProgress progress;
};
struct MessageTooLarge {
  std::uint64_t required{};
  OversizeDisposition disposition{};
  IoProgress progress;
};
struct Stale {
  Generation expected, observed;
};
struct Conflict {
  Generation expected, observed;
  IoProgress progress;
};
struct Unknown {
  IoProgress lastKnown;
};
using IoOutcome =
    std::variant<Complete, EndOfStream, Partial, Failed, Cancelled, TimedOut,
                 Overflow, MessageTooLarge, Stale, Conflict, Unknown>;

struct Admitted {
  RequestCell request;
};
struct Refused {
  OutcomeCell outcome;
};
struct SubmitResult {
  std::variant<Admitted, Refused> value;
};
struct Ready {
  OutcomeCell outcome;
};
struct Pending {
  ContinuationId continuation{};
  WaiterCell waiter;
};
struct AwaitResult {
  std::variant<Ready, Pending> value;
};
struct OwnerWaiterRegistration {
  OwnerCell cursor{};
  AllocationIncarnation cursorIncarnation{};
  ContinuationId continuation{};
};
enum class WaiterCancelState : std::uint8_t {
  Removed,
  AlreadyComplete,
  Refused
};
struct WaiterCancelResult {
  WaiterCancelState state{};
  std::optional<OutcomeCell> refusal;
};
enum class RequestCancelState : std::uint8_t {
  Accepted,
  AlreadyTerminal,
  Refused
};
struct RequestCancelResult {
  RequestCancelState state{};
  RequestCell request;
  std::optional<OutcomeCell> refusal;
};

/// Consistency mask bits for `OpenSpec::consistency` /
/// `OpenGrant::consistency` (plan §7.2, §8.5): one bit per
/// `SourceConsistency` a requester may demand and a grant may allow.
/// Bits beyond these stay adapter-defined (the masks are uint64 and the
/// grant check is a bitmask subset test); file/media adapters use exactly
/// these two.
inline constexpr std::uint64_t ioConsistencyLive   = 1ULL << 0;
inline constexpr std::uint64_t ioConsistencyStable = 1ULL << 1;

struct AdapterKey {
  std::uint32_t sourceKind{};
  std::uint32_t operation{};
  friend bool operator==(const AdapterKey &, const AdapterKey &) = default;
  auto operator<=>(const AdapterKey &) const                     = default;
};
struct OpenSpec {
  AdapterKey adapterKey;
  std::vector<std::byte> locator;
  std::optional<CapabilityCell> authCapability;
  std::uint64_t requestedRights{};
  std::uint64_t consistency{};
  std::chrono::steady_clock::time_point deadline;
};
struct CellRequest {
  AdapterKey key;
  CapabilityCell capability;
  std::optional<EndpointCell> endpoint;
  /// Declared bound on staged diagnostic bytes the adapter may attach to the
  /// terminal outcome; 0 forbids staged bytes on this request. The
  /// RequestDecoder MUST declare a bound covering everything the adapter may
  /// emit -- the service cannot know per-adapter, so an undeclared channel
  /// fails every outcome that touches it.
  std::size_t completionBoundBytes{};
  /// Declared bound on payload bytes the request may retain, including on
  /// Failed/Cancelled/TimedOut outcomes; 0 forbids payload bytes outright.
  /// At least one of the two bounds must be nonzero for a non-open request --
  /// the service refuses a request that declares no output envelope at all.
  std::size_t payloadBoundBytes{};
  bool closeOperation{};
  enum class Direction : std::uint8_t {
    Read,
    Write,
    Control
  } direction{Direction::Control};
  OwnerCell specification{};
  bool open{};
  std::optional<OpenSpec> openSpec;
};
struct OwnerContext {};
struct BackendBudget {
  std::size_t maxBytes{};
  /// The part of `maxBytes` the request's payload bound allows a published
  /// payload to occupy; a payload larger than this is refused by the service
  /// after the backend has already read and allocated it, so an adapter that
  /// sizes a read clamps to it. std::nullopt: the caller named no separate
  /// payload bound.
  std::optional<std::size_t> maxPayloadBytes{};
};
struct BackendContext {
  std::stop_token stop;
  PayloadResolver &payloads;
  BackendBudget budget;
};
struct AdmissionCost {
  std::size_t commandBytes{}, terminalEntries{1}, completionBytes{};
  /// Of `completionBytes`, the share a published payload may occupy.
  std::optional<std::size_t> payloadBytes{};
};
struct PreparedOperation {
  AdapterKey key;
  std::vector<std::byte> bytes;
};
struct BackendCommand {
  PrivateRequestToken token;
  PreparedOperation operation;
};
struct BackendReleaseAck {
  PrivateReleaseToken token;
};
struct BackendResult {
  IoOutcome outcome{Failed{{IoErrorCode::BackendFailure, 0},
                           {},
                           IoProgress{.phase = IoPhase::Terminal}}};
  std::vector<std::byte> staged;
};
struct NativeCompletion {
  PrivateRequestToken token;
  BackendResult result;
  std::optional<PayloadLease> payload;
  BackendReleaseAck released;
};
struct OwnerPublication {
  IoOutcome outcome;
};

class CompletionSink {
public:
  virtual void progress(PrivateRequestToken, IoProgress) noexcept      = 0;
  virtual void terminalCandidate(PrivateRequestToken, BackendResult,
                                 std::optional<PayloadLease>) noexcept = 0;
  virtual void releaseAcknowledged(BackendReleaseAck) noexcept         = 0;
  virtual ~CompletionSink() = default;
};
class IoAdapter {
public:
  // prepare and execute must honor the request's declared byte bounds. The
  // service checks returned sizes before retaining or publishing them; adapters
  // must not allocate unbounded staging buffers before those checks.
  virtual auto prepare(const CellRequest &, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> = 0;
  virtual void execute(const BackendCommand &, BackendContext &,
                       CompletionSink &) noexcept      = 0;
  virtual auto publish(const NativeCompletion &, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> = 0;
  /// A request whose `specification` named a staged entry but that never
  /// reaches execute -- refused at admission, cancelled before start, or
  /// failed an enqueue invariant -- would otherwise pin that entry until the
  /// adapter is destroyed. The service calls this once per such request so
  /// staging adapters can drop the entry; adapters that stage nothing (and
  /// requests whose specification names no entry) inherit the no-op.
  virtual void abandonStaged(OwnerCell) noexcept {}
  /// Owner-thread: called once per open request when its terminal outcome
  /// is settled. @p opened carries the endpoint/capability the service
  /// minted for it, or nullopt when the open produced none (it failed, was
  /// cancelled, or the mint was refused). Keyed by the request's private
  /// token, so concurrent opens of one locator bind unambiguously -- an
  /// adapter that holds per-open resources binds them to the endpoint here
  /// or drops them.
  virtual void openSettled(PrivateRequestToken,
                           const std::optional<OpenedEndpoint> &) noexcept {}
  virtual ~IoAdapter() = default;
};

} // namespace xanadu::vortex

#endif
