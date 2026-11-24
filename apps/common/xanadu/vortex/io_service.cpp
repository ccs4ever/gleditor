#include "common/xanadu/vortex/io_service.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace xanadu::vortex {
namespace {

auto tokenKey(const PrivateRequestToken token) noexcept -> std::uint64_t {
  return token.value;
}

auto errorOutcome(const IoErrorInfo error) -> IoOutcome {
  return Failed{error, {}, IoProgress{.phase = IoPhase::Terminal}};
}

auto resultBytes(const BackendResult &result) noexcept -> std::size_t {
  std::size_t valueBytes = 0;
  if (const auto *complete = std::get_if<Complete>(&result.outcome)) {
    if (const auto *bytes =
            std::get_if<std::vector<std::byte>>(&complete->value))
      valueBytes = bytes->size();
  }
  return std::max(valueBytes, result.staged.size());
}

auto outcomePayloadLength(const IoOutcome &outcome) noexcept
    -> std::optional<std::uint64_t> {
  return std::visit(
      []<typename T>(const T &value) -> std::optional<std::uint64_t> {
        if constexpr (std::is_same_v<T, Complete> ||
                      std::is_same_v<T, Failed> ||
                      std::is_same_v<T, Cancelled> ||
                      std::is_same_v<T, TimedOut>) {
          if (value.payload) return value.payload->length;
        } else if constexpr (std::is_same_v<T, Partial>) {
          return value.payload.length;
        }
        return std::nullopt;
      },
      outcome);
}

auto outcomePayloadRange(const IoOutcome &outcome) noexcept
    -> std::optional<PayloadRange> {
  return std::visit(
      []<typename T>(const T &value) -> std::optional<PayloadRange> {
        if constexpr (std::is_same_v<T, Complete> ||
                      std::is_same_v<T, Failed> ||
                      std::is_same_v<T, Cancelled> ||
                      std::is_same_v<T, TimedOut>) {
          return value.payload;
        } else if constexpr (std::is_same_v<T, Partial>) {
          return value.payload;
        }
        return std::nullopt;
      },
      outcome);
}

class LeaseRelease final {
public:
  LeaseRelease(PayloadResolver &resolver, const std::optional<LeaseId> lease)
      : resolver_(resolver), lease_(lease) {}
  ~LeaseRelease() {
    if (lease_) resolver_.release(*lease_);
  }
  LeaseRelease(const LeaseRelease &)                     = delete;
  auto operator=(const LeaseRelease &) -> LeaseRelease & = delete;

private:
  PayloadResolver &resolver_;
  std::optional<LeaseId> lease_;
};

} // namespace

IoService::IoService(const IoBudgetSnapshot budget,
                     std::unique_ptr<IoScheduler> scheduler,
                     PayloadResolver &payloads, IoCellStore &cells)
    : budget_(budget), scheduler_(std::move(scheduler)), payloads_(payloads),
      cells_(cells), sinkState_(std::make_unique<SinkState>()) {
  if (!scheduler_ || budget_.maxRequests == 0 || budget_.maxCommandBytes == 0 ||
      budget_.maxCompletionBytes == 0 || budget_.maxEndpointConcurrency == 0) {
    throw std::invalid_argument(
        "I/O service requires a scheduler and complete budgets");
  }
}

IoService::~IoService() {
  if (!shuttingDown_)
    static_cast<void>(requestShutdown(ShutdownMode::CancelPending));
  // Workers retain this as CompletionSink until they stop. Destruction is the
  // one synchronous boundary; requestShutdown itself remains nonblocking.
  scheduler_.reset();
  sinkState_.reset();
}

void IoService::registerAdapter(const AdapterKey key, IoAdapter &adapter) {
  if (shuttingDown_ || !adapters_.emplace(key, &adapter).second) {
    throw std::invalid_argument(
        "I/O adapter key is already registered or service is stopping");
  }
}

auto IoService::admitOpen(OpenSpec spec) -> SubmitResult {
  if (shuttingDown_) {
    return {
        Refused{cells_.mintRefusal({IoErrorCode::SchedulerUnavailable, 0})}};
  }
  auto authorized = cells_.authorizeOpen(spec);
  if (!authorized) return {Refused{cells_.mintRefusal(authorized.error())}};
  if (spec.locator.size() > budget_.maxCommandBytes) {
    return {Refused{cells_.mintRefusal({IoErrorCode::BudgetExceeded, 0})}};
  }
  const auto adapter = adapters_.find(spec.adapterKey);
  if (adapter == adapters_.end()) {
    return {Refused{cells_.mintRefusal({IoErrorCode::Unsupported, 0})}};
  }
  CellRequest request{.key = spec.adapterKey, .open = true, .openSpec = spec};
  return admit(std::move(request), &spec);
}

auto IoService::submit(const CapabilityCell capability,
                       const OwnerCell requestSpec) -> SubmitResult {
  if (shuttingDown_) {
    return {
        Refused{cells_.mintRefusal({IoErrorCode::SchedulerUnavailable, 0})}};
  }
  auto frozen = cells_.freezeRequest(capability, requestSpec);
  if (!frozen) return {Refused{cells_.mintRefusal(frozen.error())}};
  if (frozen->endpoint &&
      closingEndpoints_.contains(
          {frozen->endpoint->cell, frozen->endpoint->incarnation})) {
    return {Refused{cells_.mintRefusal({IoErrorCode::Conflict, 0})}};
  }
  return admit(std::move(*frozen), nullptr);
}

auto IoService::admit(CellRequest request, OpenSpec *open) -> SubmitResult {
  if (shuttingDown_) {
    return {
        Refused{cells_.mintRefusal({IoErrorCode::SchedulerUnavailable, 0})}};
  }
  const auto adapterPosition = adapters_.find(request.key);
  if (adapterPosition == adapters_.end()) {
    return {Refused{cells_.mintRefusal({IoErrorCode::Unsupported, 0})}};
  }
  if (requests_.size() >= budget_.maxRequests) {
    return {Refused{cells_.mintRefusal({IoErrorCode::BudgetExceeded, 0})}};
  }

  OwnerContext owner;
  auto prepared = adapterPosition->second->prepare(request, owner);
  if (!prepared) return {Refused{cells_.mintRefusal(prepared.error())}};
  if (prepared->bytes.size() > budget_.maxCommandBytes) {
    return {Refused{cells_.mintRefusal({IoErrorCode::BudgetExceeded, 0})}};
  }
  if (request.completionBoundBytes > budget_.maxCompletionBytes ||
      request.payloadBoundBytes > budget_.maxPayloadBytes) {
    return {Refused{cells_.mintRefusal({IoErrorCode::BudgetExceeded, 1})}};
  }
  if (request.payloadBoundBytes >
      std::numeric_limits<std::size_t>::max() - request.completionBoundBytes) {
    return {Refused{cells_.mintRefusal({IoErrorCode::BudgetExceeded, 1})}};
  }
  const auto cost =
      AdmissionCost{.commandBytes    = prepared->bytes.size(),
                    .terminalEntries = 1,
                    .completionBytes = request.completionBoundBytes +
                                       request.payloadBoundBytes};
  auto reservation = scheduler_->reserve(cost);
  if (!reservation) return {Refused{cells_.mintRefusal(reservation.error())}};

  const auto requestCell = cells_.mintRequest(request);
  if (!requestCell) {
    scheduler_->releaseUnused(*reservation);
    return {Refused{cells_.mintRefusal(requestCell.error())}};
  }
  const auto token = reservation->token;
  {
    std::scoped_lock lock(sinkState_->mutex);
    auto &event           = sinkState_->events[tokenKey(token)];
    event.token           = token;
    event.completionLimit = request.completionBoundBytes;
    event.payloadLimit    = request.payloadBoundBytes;
  }
  RequestState state{.cell        = *requestCell,
                     .token       = token,
                     .reservation = *reservation,
                     .adapter     = adapterPosition->second,
                     .prepared    = std::move(*prepared)};
  state.closingEndpoint = request.closeOperation;
  if (open) state.open = std::move(*open);
  if (request.endpoint) {
    state.lane = LaneKey{request.endpoint->cell, request.endpoint->incarnation,
                         request.direction};
  } else {
    state.lane = LaneKey{0, token.value, CellRequest::Direction::Control};
  }
  requests_.emplace(tokenKey(token), std::move(state));
  requestByCell_.emplace(requestCell->cell, tokenKey(token));
  if (const auto lane = requests_.at(tokenKey(token)).lane) {
    if (activeLanes_.contains(*lane)) {
      laneQueues_[*lane].push_back(tokenKey(token));
    } else {
      activeLanes_.emplace(*lane, tokenKey(token));
      startLaneHead(*lane);
    }
  }
  return {Admitted{*requestCell}};
}

void IoService::startLaneHead(const LaneKey &lane) {
  const auto active = activeLanes_.find(lane);
  if (active == activeLanes_.end()) return;
  const auto found = requests_.find(active->second);
  if (found == requests_.end() || found->second.enqueued ||
      !found->second.reservation || !found->second.prepared) {
    return;
  }
  if (lane.endpoint != 0) {
    std::map<std::pair<zigzag::CellRef, AllocationIncarnation>, bool>
        activeEndpoints;
    for (const auto &[activeLane, token] : activeLanes_) {
      static_cast<void>(token);
      if (activeLane.endpoint != 0) {
        activeEndpoints.emplace(
            std::pair{activeLane.endpoint, activeLane.incarnation}, true);
      }
    }
    if (activeEndpoints.size() > budget_.maxEndpointConcurrency) return;
  }
  auto &state = found->second;
  if (state.closingEndpoint) {
    for (const auto &[activeLane, activeToken] : activeLanes_) {
      if (activeLane.endpoint != lane.endpoint ||
          activeLane.incarnation != lane.incarnation || activeLane == lane) {
        continue;
      }
      const auto active = requests_.find(activeToken);
      if (active != requests_.end() && !active->second.outcome) return;
    }
  }
  BackendCommand command{.token     = state.token,
                         .operation = std::move(*state.prepared)};
  state.prepared.reset();
  const auto queued = scheduler_->enqueue(
      *state.reservation, std::move(command), *state.adapter, *this);
  if (queued == EnqueueResult::ReservationInvariantViolated) {
    state.candidate =
        BackendResult{errorOutcome({IoErrorCode::SchedulerUnavailable, 0}), {}};
    state.released       = true;
    state.synthetic      = true;
    state.progress.phase = IoPhase::Terminal;
    queueTerminal(state);
  } else {
    state.enqueued = true;
  }
}

void IoService::queueTerminal(RequestState &state) {
  if (state.candidate && state.released && !state.outcome &&
      !state.terminalQueued) {
    state.terminalQueued = true;
    terminalQueue_.push_back(tokenKey(state.token));
  }
}

auto IoService::ensureTerminalOutcome(RequestState &state)
    -> std::optional<OutcomeCell> {
  if (state.outcome && cells_.isOutcomeCurrent(*state.outcome))
    return state.outcome;
  if (!state.terminalRecord) return state.outcome;

  std::optional<PayloadLease> lease;
  if (state.terminalPayload) {
    lease = PayloadLease{state.terminalPayload->range,
                         state.terminalPayload->lease};
  }
  const auto materialized = cells_.publishTerminal(
      state.cell, *state.terminalRecord, state.progress, lease);
  if (materialized) {
    state.outcome = *materialized;
  } else {
    return cells_.mintRefusal(materialized.error());
  }
  return state.outcome;
}

auto IoService::poll(const RequestCell request) -> PollResult {
  const auto found = requestByCell_.find(request.cell);
  if (found == requestByCell_.end()) return {};
  const auto state = requests_.find(found->second);
  if (state == requests_.end() ||
      state->second.cell.incarnation != request.incarnation)
    return {};
  const auto outcome = ensureTerminalOutcome(state->second);
  return {.phase    = state->second.phase,
          .outcome  = outcome,
          .progress = state->second.progress};
}

auto IoService::await(const RequestCell request,
                      const OwnerWaiterRegistration registration)
    -> AwaitResult {
  const auto found = requestByCell_.find(request.cell);
  if (found == requestByCell_.end()) {
    return {Ready{cells_.mintRefusal({IoErrorCode::Stale, 0})}};
  }
  auto state = requests_.find(found->second);
  if (state == requests_.end() ||
      state->second.cell.incarnation != request.incarnation) {
    return {Ready{cells_.mintRefusal({IoErrorCode::Stale, 0})}};
  }
  if (const auto outcome = ensureTerminalOutcome(state->second))
    return {Ready{*outcome}};
  const auto duplicate =
      std::ranges::find_if(state->second.waiters, [&](const auto &waiter) {
        const auto &registered = waiter.second;
        return registered.cursor == registration.cursor &&
               registered.cursorIncarnation == registration.cursorIncarnation &&
               registered.continuation == registration.continuation;
      });
  if (duplicate != state->second.waiters.end()) {
    return {Pending{registration.continuation, duplicate->first}};
  }
  const auto waiter = cells_.mintWaiter(request, registration);
  state->second.waiters.emplace_back(waiter, registration);
  waiterToToken_.insert_or_assign(waiter.cell, found->second);
  return {Pending{registration.continuation, waiter}};
}

auto IoService::cancelWaiter(const WaiterCell waiter) -> WaiterCancelResult {
  const auto token = waiterToToken_.find(waiter.cell);
  if (token == waiterToToken_.end())
    return {WaiterCancelState::AlreadyComplete, {}};
  const auto state = requests_.find(token->second);
  if (state == requests_.end()) return {WaiterCancelState::AlreadyComplete, {}};
  const auto position =
      std::ranges::find_if(state->second.waiters, [&](const auto &entry) {
        return entry.first == waiter;
      });
  if (position == state->second.waiters.end())
    return {WaiterCancelState::AlreadyComplete, {}};
  cells_.retireWaiter(waiter);
  state->second.waiters.erase(position);
  waiterToToken_.erase(token);
  return {WaiterCancelState::Removed, {}};
}

void IoService::retireWaiter(const WaiterCell waiter) {
  static_cast<void>(cancelWaiter(waiter));
}

auto IoService::cancelRequest(const RequestCell request)
    -> RequestCancelResult {
  const auto found = requestByCell_.find(request.cell);
  if (found == requestByCell_.end()) {
    return {RequestCancelState::Refused, request,
            cells_.mintRefusal({IoErrorCode::Stale, 0})};
  }
  auto state = requests_.find(found->second);
  if (state == requests_.end() ||
      state->second.cell.incarnation != request.incarnation) {
    return {RequestCancelState::Refused, request,
            cells_.mintRefusal({IoErrorCode::Stale, 0})};
  }
  if (state->second.outcome)
    return {RequestCancelState::AlreadyTerminal, request, {}};
  if (!state->second.cancelRequested) {
    if (!state->second.enqueued) {
      state->second.cancelRequested = true;
      state->second.phase           = IoPhase::Cancelling;
      if (state->second.lane) {
        const auto queue = laneQueues_.find(*state->second.lane);
        if (queue != laneQueues_.end()) {
          std::erase(queue->second, found->second);
        }
      }
      state->second.candidate =
          BackendResult{Cancelled{{}, state->second.progress, false}, {}};
      state->second.released  = true;
      state->second.synthetic = true;
      queueTerminal(state->second);
    } else {
      const auto result = scheduler_->cancel(state->second.token);
      if (result == CancelQueuedResult::AlreadyTerminal) {
        return {RequestCancelState::AlreadyTerminal, request, {}};
      }
      state->second.cancelRequested = true;
      state->second.phase           = IoPhase::Cancelling;
    }
  }
  return {RequestCancelState::Accepted, request, {}};
}

void IoService::retireRequest(const RequestCell request) {
  const auto indexed = requestByCell_.find(request.cell);
  if (indexed == requestByCell_.end()) return;
  const auto found = requests_.find(indexed->second);
  if (found == requests_.end() || found->second.cell != request) return;
  if (!found->second.outcome) {
    found->second.retireAfterTerminal = true;
    static_cast<void>(cancelRequest(request));
    return;
  }
  if (!found->second.waiters.empty()) return;
  requestByCell_.erase(indexed);
  requests_.erase(found);
}

auto IoService::closeEndpoint(const EndpointCell endpoint, const CloseMode mode)
    -> SubmitResult {
  if (shuttingDown_) {
    return {
        Refused{cells_.mintRefusal({IoErrorCode::SchedulerUnavailable, 0})}};
  }
  auto frozen = cells_.freezeClose(endpoint, mode);
  if (!frozen) return {Refused{cells_.mintRefusal(frozen.error())}};
  const auto endpointKey = std::pair{endpoint.cell, endpoint.incarnation};
  if (closingEndpoints_.contains(endpointKey)) {
    return {Refused{cells_.mintRefusal({IoErrorCode::Conflict, 0})}};
  }
  frozen->endpoint       = endpoint;
  frozen->direction      = CellRequest::Direction::Control;
  frozen->closeOperation = true;
  auto result            = admit(std::move(*frozen), nullptr);
  if (std::holds_alternative<Admitted>(result.value)) {
    closingEndpoints_.emplace(endpointKey, true);
  }
  return result;
}

auto IoService::pump(const std::size_t completionBudget) -> std::size_t {
  auto delivered = scheduler_->pump(completionBudget);
  auto remaining =
      completionBudget > delivered ? completionBudget - delivered : 0;
  while (remaining > 0) {
    SinkState::Event event;
    bool haveEvent = false;
    {
      std::scoped_lock lock(sinkState_->mutex);
      if (!sinkState_->ready.empty()) {
        const auto key = sinkState_->ready.front();
        sinkState_->ready.pop_front();
        const auto found = sinkState_->events.find(key);
        if (found != sinkState_->events.end()) {
          event = std::move(found->second);
          sinkState_->events.erase(found);
          haveEvent = true;
        }
      }
    }
    if (!haveEvent) break;
    const auto position = requests_.find(tokenKey(event.token));
    if (position != requests_.end()) {
      auto &state = position->second;
      if (event.progress) {
        state.progress = *event.progress;
        state.phase    = event.progress->phase;
      }
      if (event.candidate && !state.candidate) {
        state.candidate = std::move(event.candidate);
        state.payload   = std::move(event.payload);
      }
      if (event.release) state.released = true;
      queueTerminal(state);
    } else if (event.payload) {
      payloads_.release(event.payload->lease);
    }
    --remaining;
    ++delivered;
  }
  while (remaining > 0 && !terminalQueue_.empty()) {
    const auto key = terminalQueue_.front();
    terminalQueue_.pop_front();
    const auto found = requests_.find(key);
    if (found == requests_.end()) continue;
    auto &state          = found->second;
    state.terminalQueued = false;
    if (!state.candidate || !state.released || state.outcome) continue;
    NativeCompletion completion{
        .token    = state.token,
        .result   = std::move(*state.candidate),
        .payload  = std::move(state.payload),
        .released = BackendReleaseAck{PrivateReleaseToken{state.token}}};
    const LeaseRelease releasePayload(
        payloads_, completion.payload ? std::optional{completion.payload->lease}
                                      : std::nullopt);
    IoOutcome outcome;
    if (state.synthetic) {
      outcome = std::move(completion.result.outcome);
    } else {
      OwnerContext owner;
      auto publication = state.adapter->publish(completion, owner);
      outcome          = publication ? std::move(publication->outcome)
                                     : errorOutcome(publication.error());
    }
    if (state.open) {
      if (auto *complete = std::get_if<Complete>(&outcome)) {
        const auto opened = cells_.mintOpenedEndpoint(*state.open);
        if (opened) {
          complete->value = *opened;
        } else {
          outcome = errorOutcome(opened.error());
        }
      }
    }
    auto payloadRange = outcomePayloadRange(outcome);
    if (completion.payload && payloadRange &&
        completion.payload->range != *payloadRange) {
      outcome = errorOutcome({IoErrorCode::InvalidRequest, 3});
      payloadRange.reset();
      state.terminalPayload.reset();
    } else if (completion.payload) {
      payloadRange = completion.payload->range;
    }
    if (payloadRange) {
      std::expected<std::shared_ptr<const PayloadOwnerToken>, IoError> owner =
          std::unexpected{IoError{IoErrorCode::Unresolved, 0}};
      if (completion.payload && completion.payload->range == *payloadRange) {
        owner = payloads_.retainOwner(*completion.payload);
      } else {
        auto retained = payloads_.retain(*payloadRange);
        if (retained) {
          owner = payloads_.retainOwner(*retained);
          payloads_.release(retained->lease);
        } else {
          owner = std::unexpected{retained.error()};
        }
      }
      if (owner) {
        state.terminalPayload = std::move(*owner);
      } else {
        state.terminalPayload.reset();
        outcome = errorOutcome(owner.error());
      }
    }
    state.phase          = IoPhase::Terminal;
    state.progress.phase = IoPhase::Terminal;
    state.terminalRecord = outcome;
    std::optional<PayloadLease> retainedPayload;
    if (state.terminalPayload) {
      retainedPayload = PayloadLease{state.terminalPayload->range,
                                     state.terminalPayload->lease};
    }
    const auto materialized = cells_.publishTerminal(
        state.cell, outcome, state.progress, retainedPayload);
    if (!materialized) {
      const auto failed = cells_.mintRefusal({IoErrorCode::BackendFailure, 1});
      state.outcome     = failed;
    } else {
      state.outcome = *materialized;
    }
    for (const auto &[waiter, registration] : state.waiters) {
      static_cast<void>(registration);
      cells_.retireWaiter(waiter);
      waiterToToken_.erase(waiter.cell);
    }
    state.waiters.clear();
    {
      std::scoped_lock lock(sinkState_->mutex);
      sinkState_->events.erase(key);
    }
    if (state.reservation) scheduler_->releaseUnused(*state.reservation);
    if (state.lane) {
      const auto lane   = *state.lane;
      const auto active = activeLanes_.find(lane);
      if (active != activeLanes_.end() && active->second == key) {
        activeLanes_.erase(active);
        const auto queued = laneQueues_.find(lane);
        if (queued != laneQueues_.end() && !queued->second.empty()) {
          const auto next = queued->second.front();
          queued->second.pop_front();
          activeLanes_.emplace(lane, next);
          startLaneHead(lane);
        }
      }
    }
    if (state.closingEndpoint && state.lane) {
      const auto *failed    = std::get_if<Failed>(&outcome);
      const auto *stale     = std::get_if<Stale>(&outcome);
      const auto *cancelled = std::get_if<Cancelled>(&outcome);
      if (failed != nullptr || stale != nullptr ||
          (cancelled != nullptr && !cancelled->uncertain)) {
        closingEndpoints_.erase(
            {state.lane->endpoint, state.lane->incarnation});
      }
    }
    {
      std::vector<LaneKey> waitingLanes;
      waitingLanes.reserve(activeLanes_.size());
      for (const auto &[lane, activeToken] : activeLanes_) {
        static_cast<void>(activeToken);
        waitingLanes.push_back(lane);
      }
      for (const auto &lane : waitingLanes) startLaneHead(lane);
    }
    --remaining;
    ++delivered;
    if (state.retireAfterTerminal) {
      requestByCell_.erase(state.cell.cell);
      requests_.erase(key);
    }
  }
  return delivered;
}

auto IoService::requestShutdown(const ShutdownMode mode) -> ShutdownResult {
  shuttingDown_ = true;
  if (mode == ShutdownMode::CancelPending) {
    std::vector<RequestCell> active;
    active.reserve(requests_.size());
    for (const auto &[key, state] : requests_) {
      static_cast<void>(key);
      if (!state.outcome) active.push_back(state.cell);
    }
    for (const auto request : active) static_cast<void>(cancelRequest(request));
  }
  return scheduler_->requestStop(mode);
}

void IoService::progress(const PrivateRequestToken token,
                         const IoProgress progressValue) noexcept {
  std::scoped_lock lock(sinkState_->mutex);
  auto &event    = sinkState_->events[tokenKey(token)];
  event.token    = token;
  event.progress = progressValue;
  if (!event.queued) {
    event.queued = true;
    sinkState_->ready.push_back(tokenKey(token));
  }
}

void IoService::terminalCandidate(
    const PrivateRequestToken token, BackendResult result,
    std::optional<PayloadLease> payload) noexcept {
  const LeaseRelease releaseInput(
      payloads_, payload ? std::optional{payload->lease} : std::nullopt);
  std::size_t completionLimit = 0;
  std::size_t payloadLimit    = 0;
  {
    std::scoped_lock lock(sinkState_->mutex);
    const auto found = sinkState_->events.find(tokenKey(token));
    if (found != sinkState_->events.end()) {
      completionLimit = found->second.completionLimit;
      payloadLimit    = found->second.payloadLimit;
    }
  }
  const bool oversizedResult = resultBytes(result) > completionLimit;
  const auto outcomeBytes    = outcomePayloadLength(result.outcome);
  const bool oversizedPayload =
      (payload && payload->range.length > payloadLimit) ||
      (outcomeBytes && *outcomeBytes > payloadLimit);
  if (oversizedResult || oversizedPayload) {
    result.outcome = errorOutcome({IoErrorCode::BudgetExceeded, 0});
    result.staged.clear();
    payload.reset();
  }
  if (payload) {
    try {
      auto retained = payloads_.retain(payload->range);
      if (retained) {
        payload = std::move(*retained);
      } else {
        result.outcome = Failed{
            retained.error(), {}, IoProgress{.phase = IoPhase::Terminal}};
        payload.reset();
      }
    } catch (...) {
      result.outcome = Failed{{IoErrorCode::BackendFailure, 0},
                              {},
                              IoProgress{.phase = IoPhase::Terminal}};
      payload.reset();
    }
  }
  {
    std::scoped_lock lock(sinkState_->mutex);
    auto &event = sinkState_->events[tokenKey(token)];
    event.token = token;
    if (!event.candidate) {
      event.candidate = std::move(result);
      event.payload   = std::move(payload);
    } else if (payload) {
      payloads_.release(payload->lease);
    }
    if (!event.queued) {
      event.queued = true;
      sinkState_->ready.push_back(tokenKey(token));
    }
  }
}

void IoService::releaseAcknowledged(const BackendReleaseAck ack) noexcept {
  std::scoped_lock lock(sinkState_->mutex);
  auto &event = sinkState_->events[tokenKey(ack.token.request)];
  event.token = ack.token.request;
  if (!event.release) event.release = ack;
  if (!event.queued) {
    event.queued = true;
    sinkState_->ready.push_back(tokenKey(ack.token.request));
  }
}

} // namespace xanadu::vortex
