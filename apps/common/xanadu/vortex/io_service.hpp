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
  virtual auto mintOpenedEndpoint(const OpenSpec &)
      -> std::expected<OpenedEndpoint, IoErrorInfo> = 0;
  virtual auto mintWaiter(RequestCell, OwnerWaiterRegistration)
      -> WaiterCell                              = 0;
  virtual void retireWaiter(WaiterCell) noexcept = 0;
  virtual ~IoCellStore()                         = default;
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
  [[nodiscard]] auto closeEndpoint(EndpointCell, CloseMode) -> SubmitResult;
  [[nodiscard]] auto pump(std::size_t completionBudget) -> std::size_t;
  [[nodiscard]] auto requestShutdown(ShutdownMode) -> ShutdownResult;

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
    std::vector<std::pair<WaiterCell, OwnerWaiterRegistration>> waiters;
  };
  struct SinkState {
    struct Event {
      PrivateRequestToken token;
      std::size_t completionLimit{};
      std::size_t payloadLimit{};
      std::optional<IoProgress> progress;
      std::optional<BackendResult> candidate;
      std::optional<PayloadLease> payload;
      std::optional<BackendReleaseAck> release;
      bool queued{};
    };
    std::mutex mutex;
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
  [[nodiscard]] auto ensureTerminalOutcome(RequestState &)
      -> std::optional<OutcomeCell>;

  IoBudgetSnapshot budget_;
  std::unique_ptr<IoScheduler> scheduler_;
  PayloadResolver &payloads_;
  IoCellStore &cells_;
  std::map<AdapterKey, IoAdapter *> adapters_;
  std::unordered_map<std::uint64_t, RequestState> requests_;
  std::unordered_map<zigzag::CellRef, std::uint64_t> requestByCell_;
  std::unordered_map<zigzag::CellRef, std::uint64_t> waiterToToken_;
  std::map<LaneKey, std::uint64_t> activeLanes_;
  std::map<LaneKey, std::deque<std::uint64_t>> laneQueues_;
  std::map<std::pair<zigzag::CellRef, AllocationIncarnation>, bool>
      closingEndpoints_;
  std::deque<std::uint64_t> terminalQueue_;
  std::unique_ptr<SinkState> sinkState_;
  bool shuttingDown_{};
};

} // namespace xanadu::vortex

#endif
