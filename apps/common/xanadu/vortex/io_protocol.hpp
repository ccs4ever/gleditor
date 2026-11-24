#ifndef XANADU_VORTEX_IO_PROTOCOL_HPP
#define XANADU_VORTEX_IO_PROTOCOL_HPP

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
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
using IoResultValue =
    std::variant<std::monostate, OpenedEndpoint, std::vector<std::byte>>;
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
  std::size_t completionBoundBytes{};
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
};
struct BackendContext {
  std::stop_token stop;
  PayloadResolver &payloads;
  BackendBudget budget;
};
struct AdmissionCost {
  std::size_t commandBytes{}, terminalEntries{1}, completionBytes{};
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
  IoOutcome outcome{Failed{{IoErrorCode::BackendFailure, 0}, {}, {}}};
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
  virtual ~IoAdapter()                                = default;
};

} // namespace xanadu::vortex

#endif
