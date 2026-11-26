#include "common/xanadu/vortex/io_service.hpp"

#include <gleditor/logging.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace xanadu::vortex {
namespace {

/// Settled close tombstones kept to refuse a submit on a closed endpoint
/// before it reaches the adapter. A tombstone is a pair of ids, so the cap
/// prices the pre-check at tens of KiB; the adapter's typed failure on a
/// closed endpoint is the backstop once one is evicted.
constexpr std::size_t kClosingTombstoneBound = 1024;

auto tokenKey(const PrivateRequestToken token) noexcept -> std::uint64_t {
  return token.value;
}

auto errorOutcome(const IoErrorInfo error) -> IoOutcome {
  return Failed{error, {}, IoProgress{.phase = IoPhase::Terminal}};
}

auto resultBytes(const BackendResult &result) noexcept -> std::size_t {
  // Outcome payloads are PayloadRanges (leased chunks), not inline bytes, so
  // the completion-byte bound sees only the worker->owner `staged` channel;
  // the payload bound is enforced separately in terminalCandidate.
  return result.staged.size();
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

/// Whether the outcome variant has a payload field at all (§3.2): Complete,
/// Failed, Cancelled and TimedOut carry an optional one, Partial a required
/// one; every other variant is byte-free and a payload lease attached to it
/// is a contract violation.
auto outcomeCanCarryPayload(const IoOutcome &outcome) noexcept -> bool {
  return std::holds_alternative<Complete>(outcome) ||
         std::holds_alternative<Partial>(outcome) ||
         std::holds_alternative<Failed>(outcome) ||
         std::holds_alternative<Cancelled>(outcome) ||
         std::holds_alternative<TimedOut>(outcome);
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
  // An open's real output is the minted endpoint cells, not bytes, but it
  // still declares a small nonzero envelope: a zero envelope would hand the
  // backend an unlimited byte budget while every produced byte failed the
  // terminal bound.
  CellRequest request{.key                  = spec.adapterKey,
                      .completionBoundBytes = 256,
                      .payloadBoundBytes    = 256,
                      .open                 = true,
                      .openSpec             = spec};
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
    // Same staged-entry rule admit() keeps: a refused submission consumes
    // the staged spec exactly like execute would have, so the adapter's
    // staging table cannot fill with a spec no request will ever run.
    if (const auto adapter = adapters_.find(frozen->key);
        adapter != adapters_.end()) {
      adapter->second->abandonStaged(frozen->specification);
    }
    return {Refused{cells_.mintRefusal({IoErrorCode::Conflict, 0})}};
  }
  return admit(std::move(*frozen), nullptr);
}

auto IoService::admit(CellRequest request, OpenSpec *open) -> SubmitResult {
  if (shuttingDown_) {
    // Reachable only if shutdown raced between submit()/admitOpen()'s own
    // check and this call; the staged spec still gets consumed so the
    // adapter's table cannot fill with a spec no request will ever run.
    if (const auto adapter = adapters_.find(request.key);
        adapter != adapters_.end()) {
      adapter->second->abandonStaged(request.specification);
    }
    return {
        Refused{cells_.mintRefusal({IoErrorCode::SchedulerUnavailable, 0})}};
  }
  const auto adapterPosition = adapters_.find(request.key);
  if (adapterPosition == adapters_.end()) {
    return {Refused{cells_.mintRefusal({IoErrorCode::Unsupported, 0})}};
  }
  IoAdapter *const adapter = adapterPosition->second;
  // Every refusal past adapter resolution abandons the staged entry the
  // request's specification names: a refused submission consumes it exactly
  // like execute would have, so staging tables cannot fill with dead specs.
  const auto refuseAdmission = [this, adapter,
                                &request](const IoErrorInfo error) {
    adapter->abandonStaged(request.specification);
    return SubmitResult{Refused{cells_.mintRefusal(error)}};
  };
  if (requests_.size() >= budget_.maxRequests) {
    return refuseAdmission({IoErrorCode::BudgetExceeded, 0});
  }
  // The decoder must declare at least one real byte channel. A zero bound
  // forbids that channel at the terminal check; zero on BOTH would
  // guarantee every produced byte fails -- refuse instead of admitting a
  // doomed request.
  if (request.completionBoundBytes == 0 && request.payloadBoundBytes == 0) {
    return refuseAdmission({IoErrorCode::InvalidRequest, 0});
  }

  OwnerContext owner;
  // prepare() is not noexcept: an adapter throw is a backend failure, and
  // like every refusal past adapter resolution it still consumes the
  // staged spec through refuseAdmission.
  auto prepared = [&]() -> std::expected<PreparedOperation, IoErrorInfo> {
    try {
      return adapter->prepare(request, owner);
    } catch (...) {
      return std::unexpected(IoErrorInfo{IoErrorCode::BackendFailure, 0});
    }
  }();
  if (!prepared) return refuseAdmission(prepared.error());
  if (prepared->bytes.size() > budget_.maxCommandBytes) {
    return refuseAdmission({IoErrorCode::BudgetExceeded, 0});
  }
  if (request.completionBoundBytes > budget_.maxCompletionBytes ||
      request.payloadBoundBytes > budget_.maxPayloadBytes) {
    return refuseAdmission({IoErrorCode::BudgetExceeded, 1});
  }
  if (request.payloadBoundBytes >
      std::numeric_limits<std::size_t>::max() - request.completionBoundBytes) {
    return refuseAdmission({IoErrorCode::BudgetExceeded, 1});
  }
  const auto cost =
      AdmissionCost{.commandBytes    = prepared->bytes.size(),
                    .terminalEntries = 1,
                    .completionBytes = request.completionBoundBytes +
                                       request.payloadBoundBytes,
                    .payloadBytes = request.payloadBoundBytes};
  auto reservation = scheduler_->reserve(cost);
  if (!reservation) return refuseAdmission(reservation.error());

  const auto requestCell = cells_.mintRequest(request);
  if (!requestCell) {
    scheduler_->releaseUnused(*reservation);
    return refuseAdmission(requestCell.error());
  }
  const auto token = reservation->token;
  {
    std::scoped_lock lock(sinkState_->mutex);
    auto &event                         = sinkState_->events[tokenKey(token)];
    event.token                         = token;
    sinkState_->limits[tokenKey(token)] = {request.completionBoundBytes,
                                           request.payloadBoundBytes};
  }
  RequestState state{.cell          = *requestCell,
                     .token         = token,
                     .reservation   = *reservation,
                     .adapter       = adapter,
                     .prepared      = std::move(*prepared),
                     .specification = request.specification};
  state.closingEndpoint = request.closeOperation;
  if (open) state.open = std::move(*open);
  if (request.endpoint) {
    state.lane = LaneKey{request.endpoint->cell, request.endpoint->incarnation,
                         request.direction};
  } else {
    state.lane = LaneKey{0, token.value, CellRequest::Direction::Control};
  }
  requests_.emplace(tokenKey(token), std::move(state));
  requestByCell_.emplace(std::pair{requestCell->cell, requestCell->incarnation},
                         tokenKey(token));
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
  // A head that already carries a terminal candidate -- synthetic from
  // cancelRequest or the enqueue-invariant failure -- must never be
  // enqueued: the scheduler would run real side effects for work the owner
  // already cancelled. Its terminal is already (or will be) queued.
  if (found == requests_.end() || found->second.enqueued ||
      found->second.candidate || found->second.outcome ||
      found->second.cancelRequested || !found->second.reservation ||
      !found->second.prepared) {
    return;
  }
  if (lane.endpoint != 0) {
    // Only endpoints whose head the scheduler actually holds occupy a
    // concurrency slot. Counting deferred heads too would let cap+1 waiting
    // endpoints each see the others and defer forever; the candidate lane's
    // own endpoint is the one being admitted here.
    std::set<std::pair<zigzag::CellRef, AllocationIncarnation>> busyEndpoints;
    for (const auto &[activeLane, token] : activeLanes_) {
      const auto running = requests_.find(token);
      if (running != requests_.end() && running->second.enqueued &&
          activeLane.endpoint != 0) {
        busyEndpoints.emplace(
            std::pair{activeLane.endpoint, activeLane.incarnation});
      }
    }
    busyEndpoints.emplace(std::pair{lane.endpoint, lane.incarnation});
    if (busyEndpoints.size() > budget_.maxEndpointConcurrency) return;
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

void IoService::eraseRequestState(const std::uint64_t tokenKey) {
  const auto found = requests_.find(tokenKey);
  if (found == requests_.end()) return;
  // A live waiter still needs the request index: its await resolves
  // through poll(), which finds the outcome via requestByCell_ or the
  // arena link. Same rule retireRequest() keeps.
  if (!found->second.waiters.empty()) return;
  // The outcome must stay resolvable once the state is gone: poll() and
  // await() fall back to cells_.outcomeFor(request), which reads only
  // the arena request->outcome link. A terminal that never linked one --
  // publishTerminal threw (the durable terminalRecord dies with the
  // state) or refused (state.outcome holds an unlinked Rejected cell) --
  // would strand every late poll/await and parked IoHost waiter, so the
  // state stays until a materialization produces a linked outcome or
  // the service stops. A throwing store cannot prove resolvability
  // either; keep the state rather than risk the same strand.
  std::optional<OutcomeCell> linked;
  try {
    linked = cells_.outcomeFor(found->second.cell);
  } catch (...) {
    return;
  }
  if (!linked) return;
  requestByCell_.erase(
      std::pair{found->second.cell.cell, found->second.cell.incarnation});
  cells_.reapRequest(found->second.cell);
  {
    std::scoped_lock lock(sinkState_->mutex);
    sinkState_->limits.erase(tokenKey);
  }
  requests_.erase(found);
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
  try {
    const auto materialized = cells_.publishTerminal(
        state.cell, *state.terminalRecord, state.progress, lease);
    if (materialized) {
      state.outcome = *materialized;
    } else {
      // Known M1 gap: a persistent publishTerminal refusal re-mints a
      // refusal cell on every poll/await that reaches this path --
      // state.outcome stays unset so pump()'s terminal pass can still
      // retry the real publish, which means request retirement does NOT
      // bound the growth: each retry mints one runtime-only cell until
      // publishTerminal succeeds, the request state drops, or the
      // service stops.
      return cells_.mintRefusal(materialized.error());
    }
  } catch (...) {
    // A throwing cell store must not escape poll/await: degrade to a
    // refusal mint, and to "still pending" if even that throws -- the
    // terminal record stays durable for the next retry.
    try {
      return cells_.mintRefusal({IoErrorCode::BackendFailure, 2});
    } catch (...) {
      return std::nullopt;
    }
  }
  return state.outcome;
}

auto IoService::poll(const RequestCell request) -> PollResult {
  const auto found =
      requestByCell_.find(std::pair{request.cell, request.incarnation});
  if (found == requestByCell_.end()) {
    // A retired request keeps its arena outcome link: report the terminal
    // cell with no live progress (phase/progress died with the state).
    if (const auto outcome = cells_.outcomeFor(request)) {
      return {.phase    = IoPhase::Terminal,
              .outcome  = outcome,
              .progress = std::nullopt};
    }
    return {};
  }
  const auto state = requests_.find(found->second);
  if (state == requests_.end() ||
      state->second.cell.incarnation != request.incarnation)
    return {};
  const auto outcome  = ensureTerminalOutcome(state->second);
  const auto phase    = state->second.phase;
  const auto progress = state->second.progress;
  // A retireRequest that landed while the outcome cell could not be
  // published is honored here: the terminal pass already ran, so this
  // materialization is the last chance to drop the request state (the
  // maps, indexes and terminalPayload lease would otherwise leak until
  // ~IoService). Capture phase/progress first -- the erase invalidates
  // the state reference.
  if (state->second.outcome && state->second.retireAfterTerminal) {
    eraseRequestState(state->first);
  }
  return {.phase = phase, .outcome = outcome, .progress = progress};
}

auto IoService::await(const RequestCell request,
                      const OwnerWaiterRegistration registration)
    -> AwaitResult {
  const auto found =
      requestByCell_.find(std::pair{request.cell, request.incarnation});
  if (found == requestByCell_.end()) {
    // A retired request keeps its arena outcome link (poll()'s rule):
    // report the terminal outcome Ready rather than minting a Stale
    // refusal for work that actually completed.
    if (const auto outcome = cells_.outcomeFor(request)) {
      return {Ready{*outcome}};
    }
    return {Ready{cells_.mintRefusal({IoErrorCode::Stale, 0})}};
  }
  auto state = requests_.find(found->second);
  if (state == requests_.end() ||
      state->second.cell.incarnation != request.incarnation) {
    return {Ready{cells_.mintRefusal({IoErrorCode::Stale, 0})}};
  }
  if (const auto outcome = ensureTerminalOutcome(state->second)) {
    // Same retireAfterTerminal honoring as poll(): the terminal pass
    // already ran, so a materialized outcome is the last chance to drop
    // the request state. The waiter this await was about to mint is
    // moot -- the outcome is Ready now.
    if (state->second.outcome && state->second.retireAfterTerminal) {
      eraseRequestState(state->first);
    }
    return {Ready{*outcome}};
  }
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
  if (!waiter) {
    // The store types its refusal: a dead request/cursor is Stale, an
    // arena write failure is BackendFailure -- await() reports the code
    // the mint actually hit rather than guessing BudgetExceeded.
    return {Ready{cells_.mintRefusal(waiter.error())}};
  }
  state->second.waiters.emplace_back(*waiter, registration);
  waiterToToken_.insert_or_assign(std::pair{waiter->cell, waiter->incarnation},
                                  found->second);
  return {Pending{registration.continuation, *waiter}};
}

auto IoService::cancelWaiter(const WaiterCell waiter) -> WaiterCancelResult {
  const auto token =
      waiterToToken_.find(std::pair{waiter.cell, waiter.incarnation});
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
  const auto found =
      requestByCell_.find(std::pair{request.cell, request.incarnation});
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
      // A terminal outcome must not carry a non-Terminal progress phase;
      // copy the last reported counters but force the phase.
      auto terminalProgress  = state->second.progress;
      terminalProgress.phase = IoPhase::Terminal;
      state->second.candidate =
          BackendResult{Cancelled{{}, terminalProgress, false}, {}};
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
  // M1 gap, documented not redesigned: requests_ entries leave only through
  // here (or shutdown), so production callers must retire each request once
  // its outcome cell is consumed or the map grows monotonically.
  const auto indexed =
      requestByCell_.find(std::pair{request.cell, request.incarnation});
  if (indexed == requestByCell_.end()) return;
  const auto found = requests_.find(indexed->second);
  if (found == requests_.end() || found->second.cell != request) return;
  if (!found->second.outcome) {
    found->second.retireAfterTerminal = true;
    static_cast<void>(cancelRequest(request));
    return;
  }
  if (!found->second.waiters.empty()) return;
  // Same gate the terminal pass and poll/await keep: state.outcome may
  // be an unlinked refusal cell, so only a request whose arena link
  // resolves may leave the indexes -- erasing here would strand the
  // late poll/await exactly like the other three call sites.
  eraseRequestState(found->first);
}

auto IoService::orphanRequest(const RequestCell request) -> OrphanResult {
  const auto indexed =
      requestByCell_.find(std::pair{request.cell, request.incarnation});
  if (indexed == requestByCell_.end()) return OrphanResult::Unknown;
  const auto found = requests_.find(indexed->second);
  if (found == requests_.end() || found->second.cell != request) {
    return OrphanResult::Unknown;
  }
  if (!found->second.waiters.empty()) {
    // A waiter still holds a route to the request's outcome, so it is not
    // unnameable yet; the caller decides what to do with the refusal.
    GLEDITOR_LOG_DEBUG("vortex.io", "orphan refused: request has {} waiters",
                       found->second.waiters.size());
    return OrphanResult::HasWaiters;
  }
  ++orphanCount_;
  if (found->second.outcome || found->second.terminalRecord) {
    GLEDITOR_LOG_DEBUG("vortex.io", "terminal request forgotten as orphan");
    forgetOrphan(found->first);
    return OrphanResult::Forgotten;
  }
  GLEDITOR_LOG_DEBUG("vortex.io", "running request orphaned");
  found->second.orphaned = true;
  return OrphanResult::Orphaned;
}

void IoService::forgetOrphan(const std::uint64_t tokenKey) {
  const auto found = requests_.find(tokenKey);
  if (found == requests_.end()) return;
  requestByCell_.erase(
      std::pair{found->second.cell.cell, found->second.cell.incarnation});
  {
    std::scoped_lock lock(sinkState_->mutex);
    sinkState_->limits.erase(tokenKey);
  }
  requests_.erase(found);
}

void IoService::reapRetiredRequest(const RequestCell request) {
  if (requestByCell_.contains(std::pair{request.cell, request.incarnation})) {
    return;
  }
  cells_.reapRequest(request);
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
    closingEndpoints_[endpointKey] = {++closingSequence_, false};
    closingOrder_.emplace_back(endpointKey, closingSequence_);
    // A completed close keeps its tombstone so later submits keep refusing
    // Conflict; bound the set so endpoint churn cannot grow it forever.
    // Eviction only weakens the pre-check -- the adapter still fails typed
    // on the closed endpoint -- and takes the oldest *settled* entry, never
    // a close still in flight. When every entry is in flight the set is as
    // large as the closes the admission budget allows at once.
    const auto stale = [this](const auto &entry) {
      const auto found = closingEndpoints_.find(entry.first);
      return found == closingEndpoints_.end() ||
             found->second.sequence != entry.second;
    };
    if (closingOrder_.size() > 2 * kClosingTombstoneBound) {
      // Closes that failed erase their map entry but leave an order record;
      // drop them in bulk so the order list cannot outgrow the set.
      std::erase_if(closingOrder_, stale);
    }
    for (auto at = closingOrder_.begin();
         closingEndpoints_.size() > kClosingTombstoneBound &&
         at != closingOrder_.end();) {
      const auto entry = closingEndpoints_.find(at->first);
      if (entry == closingEndpoints_.end() ||
          entry->second.sequence != at->second) {
        at = closingOrder_.erase(at);
      } else if (entry->second.settled) {
        closingEndpoints_.erase(entry);
        at = closingOrder_.erase(at);
      } else {
        ++at;
      }
    }
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
      if (event.progress && !state.outcome) {
        // A late progress event must not regress the phase after the
        // terminal outcome was published.
        state.progress = *event.progress;
        state.phase    = event.progress->phase;
      }
      if (event.candidate && !state.candidate) {
        state.candidate = std::move(event.candidate);
        state.payload   = std::move(event.payload);
      } else if (event.payload) {
        // A second candidate for an already-decided request is dropped with
        // its result; its payload lease must still be released or the chunk
        // stays pinned forever.
        payloads_.release(event.payload->lease);
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

    // Everything from moving the candidate out to recording the terminal
    // outcome can throw: adapter->publish is not noexcept, and the cell
    // mints and payload retains allocate. A throw must still leave the
    // request with exactly one typed terminal outcome and the lane tail
    // below advancing -- never a wedged head with a leaked request.
    IoOutcome outcome = Failed{{IoErrorCode::BackendFailure, 0},
                               {},
                               IoProgress{.phase = IoPhase::Terminal}};
    // An open's adapter binds its native resource (a descriptor) to the
    // endpoint in openSettled or drops it; a throw before that hook ran would
    // leave the resource open with nothing able to address it.
    bool openReported = false;
    try {
      NativeCompletion completion{
          .token    = state.token,
          .result   = std::move(*state.candidate),
          .payload  = std::move(state.payload),
          .released = BackendReleaseAck{PrivateReleaseToken{state.token}}};
      const LeaseRelease releasePayload(
          payloads_, completion.payload
                         ? std::optional{completion.payload->lease}
                         : std::nullopt);
      if (state.synthetic) {
        outcome = std::move(completion.result.outcome);
      } else {
        OwnerContext owner;
        auto publication = state.adapter->publish(completion, owner);
        outcome          = publication ? std::move(publication->outcome)
                                       : errorOutcome(publication.error());
      }
      if (state.open) {
        std::optional<OpenedEndpoint> minted;
        // An orphan's request cell is gone, so an endpoint minted for it
        // could never be named or reaped; the adapter is still told, with none.
        if (auto *complete = std::get_if<Complete>(&outcome);
            complete != nullptr && !state.orphaned) {
          const auto opened = cells_.mintOpenedEndpoint(*state.open);
          if (opened) {
            complete->value = *opened;
            minted          = *opened;
          } else {
            outcome = errorOutcome(opened.error());
          }
        }
        openReported = true; // the hook is noexcept: it runs exactly once
        state.adapter->openSettled(state.token, minted);
      }
      // A payload lease may only ride an outcome variant that has a
      // payload field; attaching one to EndOfStream/Overflow/... would
      // mint a cell the §3.2 contract says cannot carry bytes.
      if (completion.payload && !outcomeCanCarryPayload(outcome)) {
        outcome = errorOutcome({IoErrorCode::InvalidRequest, 4});
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
    } catch (...) {
      // The adapter or a cell mint threw mid-materialization. The request
      // still gets exactly one typed terminal outcome; whatever partial
      // state the throw left is superseded here. release() is idempotent,
      // so re-releasing a lease the completion's guard may already have
      // dropped is a no-op.
      if (state.payload) {
        payloads_.release(state.payload->lease);
        state.payload.reset();
      }
      state.terminalPayload.reset();
      if (state.open && !openReported) {
        state.adapter->openSettled(state.token, std::nullopt);
      }
      state.phase          = IoPhase::Terminal;
      state.progress.phase = IoPhase::Terminal;
      outcome              = Failed{{IoErrorCode::BackendFailure, 0},
                                    {},
                       IoProgress{.phase = IoPhase::Terminal}};
      state.terminalRecord = outcome;
    }

    // publishTerminal throws on the same allocation paths. The terminal
    // record is already durable in state, so a later poll/await
    // rematerializes through ensureTerminalOutcome; leave state.outcome
    // unset rather than escape pump() with the lane still held.
    std::optional<PayloadLease> retainedPayload;
    if (state.terminalPayload) {
      retainedPayload = PayloadLease{state.terminalPayload->range,
                                     state.terminalPayload->lease};
    }
    // An orphan's cells are gone: publishing would mint a refusal for a
    // request nothing can name.
    if (!state.orphaned) try {
        const auto materialized = cells_.publishTerminal(
            state.cell, outcome, state.progress, retainedPayload);
        if (!materialized) {
          // Propagate publishTerminal's typed refusal (Stale on a dead
          // request cell, InvalidRequest on a malformed outcome) exactly
          // like ensureTerminalOutcome does -- minting BackendFailure here
          // would type the same terminal differently depending on which
          // path materialized it first.
          state.outcome = cells_.mintRefusal(materialized.error());
        } else {
          state.outcome = *materialized;
        }
      } catch (...) {
        // terminalQueued suppresses requeue: with state.outcome still unset
        // a late sink event would otherwise pass queueTerminal's gate and
        // run this whole materialization a second time.
        state.terminalQueued = true;
        GLEDITOR_LOG_DEBUG(
            "vortex.io",
            "terminal publication threw; outcome stays rematerializable");
      }
    // Terminal reached: if execute never ran -- cancel-before-start, a
    // scheduler-side removal, an enqueue-invariant failure -- the staged
    // entry this request's specification named is still sitting in the
    // adapter. Hand it back so staging tables cannot fill with dead specs.
    state.adapter->abandonStaged(state.specification);
    for (const auto &[waiter, registration] : state.waiters) {
      static_cast<void>(registration);
      cells_.retireWaiter(waiter);
      waiterToToken_.erase(std::pair{waiter.cell, waiter.incarnation});
    }
    state.waiters.clear();
    {
      std::scoped_lock lock(sinkState_->mutex);
      sinkState_->events.erase(key);
      sinkState_->limits.erase(key);
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
      } else if (const auto closing = closingEndpoints_.find(
                     {state.lane->endpoint, state.lane->incarnation});
                 closing != closingEndpoints_.end()) {
        closing->second.settled = true;
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
    if (state.orphaned) {
      forgetOrphan(key);
    } else if (state.retireAfterTerminal) {
      eraseRequestState(key);
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

auto IoService::refuse(const IoErrorInfo reason) -> OutcomeCell {
  return cells_.mintRefusal(reason);
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
    const auto found = sinkState_->limits.find(tokenKey(token));
    if (found != sinkState_->limits.end()) {
      completionLimit = found->second.completion;
      payloadLimit    = found->second.payload;
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
    } else {
      // A second candidate for an already-decided request is dropped with
      // its result; its payload lease must still be released or the chunk
      // stays pinned forever.
      try {
        GLEDITOR_LOG_DEBUG("vortex.io", "duplicate terminal candidate dropped");
      } catch (...) {
        // Logging must never terminate a noexcept sink callback.
      }
      if (payload) payloads_.release(payload->lease);
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
  if (!event.release) {
    event.release = ack;
  } else {
    try {
      GLEDITOR_LOG_DEBUG("vortex.io", "duplicate release ack dropped");
    } catch (...) {
      // Logging must never terminate a noexcept sink callback.
    }
  }
  if (!event.queued) {
    event.queued = true;
    sinkState_->ready.push_back(tokenKey(ack.token.request));
  }
}

} // namespace xanadu::vortex
