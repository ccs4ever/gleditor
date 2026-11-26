#ifndef XANADU_VORTEX_IO_SERVICE_HPP
#define XANADU_VORTEX_IO_SERVICE_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/vortex/io_scheduler.hpp"

namespace xanadu::vortex {

struct IoBudgetSnapshot {
  std::size_t maxRequests{};
  std::size_t maxCommandBytes{};
  std::size_t maxCompletionBytes{};
  std::size_t maxPayloadBytes{};
  std::size_t maxEndpointConcurrency{};
};
struct PollResult {
  IoPhase phase{IoPhase::Preparing};
  std::optional<OutcomeCell> outcome;
  std::optional<IoProgress> progress;
};
enum class CloseMode : std::uint8_t { Graceful, Abort };

class IoCellStore {
public:
  virtual auto authorizeOpen(const OpenSpec &)
      -> std::expected<void, IoErrorInfo> = 0;
  virtual auto freezeRequest(CapabilityCell, OwnerCell)
      -> std::expected<CellRequest, IoErrorInfo> = 0;
  virtual auto freezeClose(EndpointCell, CloseMode)
      -> std::expected<CellRequest, IoErrorInfo> = 0;
  virtual auto mintRequest(const CellRequest &)
      -> std::expected<RequestCell, IoErrorInfo>       = 0;
  virtual auto mintRefusal(IoErrorInfo) -> OutcomeCell = 0;
  virtual auto publishTerminal(RequestCell, const IoOutcome &,
                               const IoProgress &,
                               const std::optional<PayloadLease> &)
      -> std::expected<OutcomeCell, IoErrorInfo> = 0;
  [[nodiscard]] virtual auto isOutcomeCurrent(OutcomeCell) const noexcept
      -> bool {
    return true;
  }
  /// Resolve the terminal outcome cell a request published by reading the
  /// cell graph, independent of service state. `poll` consults this for
  /// requests already retired: the arena link outlives the service index.
  [[nodiscard]] virtual auto outcomeFor(const RequestCell)
      -> std::optional<OutcomeCell> = 0;
  virtual auto mintOpenedEndpoint(const OpenSpec &)
      -> std::expected<OpenedEndpoint, IoErrorInfo> = 0;
  /// Mint the waiter cell for an await registration. Refuses with a typed
  /// error -- Stale for a dead request/cursor handle, the cell mint's own
  /// code, BackendFailure for an arena write that failed mid-graph -- so
  /// await() never collapses every refusal into one guessed code.
  virtual auto mintWaiter(RequestCell, OwnerWaiterRegistration)
      -> std::expected<WaiterCell, IoErrorInfo>  = 0;
  virtual void retireWaiter(WaiterCell) noexcept = 0;
  /// The service has dropped a request's state for good: take its cells out
  /// of the ranks later submits walk. Optional; the outcome link must stay.
  virtual void reapRequest(RequestCell) noexcept {}
  virtual ~IoCellStore() = default;
};

class IoService final : private CompletionSink {
public:
  IoService(IoBudgetSnapshot, std::unique_ptr<IoScheduler>, PayloadResolver &,
            IoCellStore &);
  ~IoService();
  IoService(const IoService &)                     = delete;
  auto operator=(const IoService &) -> IoService & = delete;

  void registerAdapter(AdapterKey, IoAdapter &);
  [[nodiscard]] auto admitOpen(OpenSpec) -> SubmitResult;
  [[nodiscard]] auto submit(CapabilityCell, OwnerCell requestSpec)
      -> SubmitResult;
  [[nodiscard]] auto poll(RequestCell) -> PollResult;
  [[nodiscard]] auto await(RequestCell, OwnerWaiterRegistration) -> AwaitResult;
  [[nodiscard]] auto cancelWaiter(WaiterCell) -> WaiterCancelResult;
  void retireWaiter(WaiterCell);
  [[nodiscard]] auto cancelRequest(RequestCell) -> RequestCancelResult;
  void retireRequest(RequestCell);
  /// Forget a request whose cells the owner has rolled back (or is about to):
  /// nothing can name its outcome any more. It is not cancelled, so the world
  /// effect runs to its own end; its terminal is then dropped unpublished and
  /// its budget freed. A request already terminal is forgotten at once, and a
  /// stale or unknown handle is a no-op. Refused while a waiter is registered.
  void orphanRequest(RequestCell);
  /// Free the graph of a request the service no longer indexes. A request
  /// retired while a submission view still named it was kept whole by the
  /// reaper and nothing re-reaps it once the view leaves; the owner calls this
  /// after releasing the view. A request still indexed is left alone (it is
  /// retired, not reaped, through retireRequest); a stale handle is a no-op.
  void reapRetiredRequest(RequestCell);
  [[nodiscard]] auto closeEndpoint(EndpointCell, CloseMode) -> SubmitResult;
  [[nodiscard]] auto pump(std::size_t completionBudget) -> std::size_t;
  [[nodiscard]] auto requestShutdown(ShutdownMode) -> ShutdownResult;

  /// Mints a `Rejected`-shaped refusal outcome without admitting any
  /// request. Used by `IoHost` for a profile refusal (§9): the check runs
  /// entirely ahead of this service, but the typed outcome cell it
  /// reports is minted through the same owner-only cell store.
  [[nodiscard]] auto refuse(IoErrorInfo reason) -> OutcomeCell;

private:
  struct LaneKey {
    zigzag::CellRef endpoint{};
    AllocationIncarnation incarnation{};
    CellRequest::Direction direction{CellRequest::Direction::Control};
    auto operator<=>(const LaneKey &) const = default;
  };
  struct RequestState {
    RequestCell cell;
    PrivateRequestToken token;
    std::optional<SchedulerReservation> reservation;
    IoAdapter *adapter{};
    std::optional<OpenSpec> open;
    std::optional<PreparedOperation> prepared;
    /// The owner cell whose staged entry this request consumed at prepare;
    /// handed back to the adapter via abandonStaged if the request ends
    /// without execute.
    OwnerCell specification{};
    std::optional<LaneKey> lane;
    bool enqueued{};
    IoPhase phase{IoPhase::Queued};
    IoProgress progress{};
    std::optional<OutcomeCell> outcome;
    std::optional<IoOutcome> terminalRecord;
    std::shared_ptr<const PayloadOwnerToken> terminalPayload;
    std::optional<BackendResult> candidate;
    std::optional<PayloadLease> payload;
    bool released{};
    bool cancelRequested{};
    bool synthetic{};
    bool closingEndpoint{};
    bool terminalQueued{};
    bool retireAfterTerminal{};
    bool orphaned{};
    std::vector<std::pair<WaiterCell, OwnerWaiterRegistration>> waiters;
  };
  struct SinkState {
    struct Event {
      PrivateRequestToken token;
      std::optional<IoProgress> progress;
      std::optional<BackendResult> candidate;
      std::optional<PayloadLease> payload;
      std::optional<BackendReleaseAck> release;
      bool queued{};
    };
    /// A request's admitted byte bounds, kept apart from its Event: the
    /// owner's pump moves an Event out and erases it as soon as it has
    /// delivered one progress or release, but a terminal candidate arriving
    /// afterwards must still be checked against the bounds.
    struct Limits {
      std::size_t completion{};
      std::size_t payload{};
    };
    std::mutex mutex;
    std::unordered_map<std::uint64_t, Limits> limits;
    std::unordered_map<std::uint64_t, Event> events;
    std::deque<std::uint64_t> ready;
  };
  auto admit(CellRequest, OpenSpec *) -> SubmitResult;
  void progress(PrivateRequestToken, IoProgress) noexcept override;
  void terminalCandidate(PrivateRequestToken, BackendResult,
                         std::optional<PayloadLease>) noexcept override;
  void releaseAcknowledged(BackendReleaseAck) noexcept override;
  void startLaneHead(const LaneKey &);
  void queueTerminal(RequestState &);
  /// Drop a request from both indexes once its terminal outcome is
  /// settled AND still resolvable without the state: poll()/await() fall
  /// back to cells_.outcomeFor(request), which reads only the arena
  /// request->outcome link, so a terminal that never linked one (a
  /// publishTerminal throw or refusal) keeps the state rather than
  /// stranding late polls, awaits and parked waiters. Shared by
  /// pump()'s terminal pass, the poll/await paths that materialize an
  /// outcome the pass could not publish, and retireRequest(); refuses
  /// while waiters are still registered, mirroring retireRequest().
  void eraseRequestState(std::uint64_t tokenKey);
  /// Drop an orphaned request's state without touching any cell.
  void forgetOrphan(std::uint64_t tokenKey);
  [[nodiscard]] auto ensureTerminalOutcome(RequestState &)
      -> std::optional<OutcomeCell>;

  IoBudgetSnapshot budget_;
  std::unique_ptr<IoScheduler> scheduler_;
  PayloadResolver &payloads_;
  IoCellStore &cells_;
  std::map<AdapterKey, IoAdapter *> adapters_;
  std::unordered_map<std::uint64_t, RequestState> requests_;
  // Both cell indexes key the full handle (dense index + allocation
  // incarnation): the arena can recycle a bare CellRef after a release,
  // and a stale entry must never shadow the new allocation's mapping.
  std::map<std::pair<zigzag::CellRef, AllocationIncarnation>, std::uint64_t>
      requestByCell_;
  std::map<std::pair<zigzag::CellRef, AllocationIncarnation>, std::uint64_t>
      waiterToToken_;
  std::map<LaneKey, std::uint64_t> activeLanes_;
  std::map<LaneKey, std::deque<std::uint64_t>> laneQueues_;
  using EndpointKey = std::pair<zigzag::CellRef, AllocationIncarnation>;
  /// An endpoint with a close admitted. `settled` flips once the close's
  /// terminal outcome is recorded; only a settled entry may be evicted to
  /// bound the set, so an in-flight close always keeps refusing a second.
  struct ClosingEndpoint {
    std::uint64_t sequence{};
    bool settled{};
  };
  std::map<EndpointKey, ClosingEndpoint> closingEndpoints_;
  /// Admission order of closingEndpoints_ entries; an element whose sequence
  /// no longer matches the map's is stale (erased and re-closed) and skipped.
  std::deque<std::pair<EndpointKey, std::uint64_t>> closingOrder_;
  std::uint64_t closingSequence_{};
  std::deque<std::uint64_t> terminalQueue_;
  std::unique_ptr<SinkState> sinkState_;
  bool shuttingDown_{};
};

} // namespace xanadu::vortex

#endif
