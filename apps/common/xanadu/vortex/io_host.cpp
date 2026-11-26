#include "common/xanadu/vortex/io_host.hpp"

#include <algorithm>
#include <gleditor/logging.hpp>
#include <stdexcept>
#include <utility>

namespace xanadu::vortex {

IoHost::IoHost(IoService &service, zigzag::ArenaManifold &arena,
               zigzag::vortex::VortexVM &vm, const IoExecutionProfile profile)
    : service_(service), arena_(arena), vm_(vm), profile_(profile),
      owner_(std::this_thread::get_id()) {
  if (vm_.hasAwaitHandler()) {
    throw std::invalid_argument("VM already has an I/O await host");
  }
  vm_.setAwaitHandler(
      [this](const RequestCell request, const zigzag::CellRef cursor) {
        return awaitForVm(request, cursor);
      });
}

IoHost::~IoHost() {
  // The teardown below clears the VM's await handler and retires waiters
  // on the service -- owner-thread operations like every other verb, so
  // the same contract guards destruction (a cross-thread destroy is a
  // logic_error, which terminates the noexcept destructor).
  requireOwner();
  vm_.setAwaitHandler({});
  for (auto &[id, parked] : parked_) {
    static_cast<void>(id);
    abandon(parked);
  }
}

void IoHost::requireOwner() const {
  if (std::this_thread::get_id() != owner_) {
    throw std::logic_error("I/O host used outside its owner thread");
  }
}

void IoHost::abandon(ParkedAwait &parked) noexcept {
  service_.retireWaiter(parked.waiter);
  vm_.retireAwait(parked.cursor, parked.cursorIncarnation, parked.continuation);
}

auto IoHost::profile() const noexcept -> IoExecutionProfile { return profile_; }

auto IoHost::submitCell(const CapabilityCell capability,
                        const zigzag::CellRef requestSpec,
                        const std::optional<std::chrono::milliseconds> within)
    -> SubmitResult {
  requireOwner();
  if (!policyFor(profile_).allowDirectSubmit) {
    return {Refused{service_.refuse(profileRefusal())}};
  }
  return service_.submit(capability, requestSpec, within);
}

auto IoHost::cancelRequest(const RequestCell request) -> RequestCancelResult {
  requireOwner();
  return service_.cancelRequest(request);
}

auto IoHost::closeEndpoint(const EndpointCell endpoint, const CloseMode mode)
    -> SubmitResult {
  requireOwner();
  if (!policyFor(profile_).allowDirectSubmit) {
    return {Refused{service_.refuse(profileRefusal())}};
  }
  return service_.closeEndpoint(endpoint, mode);
}

auto IoHost::now() const -> std::chrono::steady_clock::time_point {
  return service_.now();
}

auto IoHost::isTerminal(const RequestCell request) -> bool {
  requireOwner();
  return service_.poll(request).outcome.has_value();
}

auto IoHost::admitOpen(OpenSpec spec) -> SubmitResult {
  requireOwner();
  if (!policyFor(profile_).allowDirectSubmit) {
    return {Refused{service_.refuse(profileRefusal())}};
  }
  return service_.admitOpen(std::move(spec));
}

auto IoHost::awaitForVm(const RequestCell request, const zigzag::CellRef cursor)
    -> AwaitResult {
  // VortexVM::step reports every failure as a value, but it calls this
  // handler outside the try that converts exceptions: whatever escapes here
  // propagates out of run() on the owner thread. awaitCell's documented
  // throws (stale cursor, arena allocation limit, identity exhaustion) are
  // therefore turned into the same typed refusal a profile gate yields. The
  // owner check stays outside: a call from the wrong thread must throw and
  // mint nothing, not turn into a refusal cell minted off the owner.
  requireOwner();
  try {
    return awaitCell(request, cursor);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.host", "await refused: {}", error.what());
  }
  try {
    return {
        Ready{service_.refuse(IoErrorInfo{IoErrorCode::BackendFailure, 0})}};
  } catch (const std::exception &error) {
    // Minting the refusal can fail for the very reason the await did (an
    // exhausted arena). The request is a live cell that is not an outcome,
    // so handing it back as the outcome makes the VM fail the step by value
    // -- "stale or invalid OutcomeCell" -- instead of throwing.
    GLEDITOR_LOG_WARN("vortex.io.host", "await refusal not minted: {}",
                      error.what());
  }
  return {Ready{OutcomeCell{request.cell, request.incarnation}}};
}

auto IoHost::awaitCell(const RequestCell request, const zigzag::CellRef cursor)
    -> AwaitResult {
  requireOwner();
  if (!policyFor(profile_).allowAwaitSuspension) {
    // §9: an actor turn's direct Await is refused before admission, same
    // as Submit; the VM sees a terminal Ready carrying the typed refusal,
    // never a park, so no continuation or waiter is ever created for it.
    return {Ready{service_.refuse(profileRefusal())}};
  }
  const auto cursorIncarnation = arena_.incarnationOf(cursor);
  if (!cursorIncarnation) {
    throw std::invalid_argument("I/O await cursor is stale");
  }

  for (const auto &[id, parked] : parked_) {
    static_cast<void>(id);
    if (parked.request == request && parked.cursor == cursor &&
        parked.cursorIncarnation == *cursorIncarnation) {
      return {Pending{parked.continuation, parked.waiter}};
    }
  }

  const auto continuation = vm_.createContinuation(cursor, *cursorIncarnation);
  const OwnerWaiterRegistration registration{.cursor = cursor,
                                             .cursorIncarnation =
                                                 *cursorIncarnation,
                                             .continuation = continuation};
  // The continuation is issued before the service can refuse or throw; every
  // exit that does not park it must retire it or it sits in the VM until
  // ~IoHost.
  AwaitResult result;
  try {
    result = service_.await(request, registration);
  } catch (...) {
    vm_.retireAwait(cursor, *cursorIncarnation, continuation);
    throw;
  }
  auto *pending = std::get_if<Pending>(&result.value);
  if (!pending) {
    vm_.retireAwait(cursor, *cursorIncarnation, continuation);
    return result;
  }
  const auto waiterTag =
      ioCellTag(arena_, pending->waiter.cell, pending->waiter.incarnation);
  if (pending->continuation != continuation ||
      !arena_.matches(pending->waiter.cell, pending->waiter.incarnation) ||
      !waiterTag || *waiterTag != IoCellTag::Waiter) {
    service_.retireWaiter(pending->waiter);
    vm_.retireAwait(cursor, *cursorIncarnation, continuation);
    throw std::runtime_error("I/O service returned a stale waiter");
  }

  const auto parked =
      vm_.parkAwait(cursor, *cursorIncarnation, continuation, pending->waiter);
  if (!parked.success) {
    service_.retireWaiter(pending->waiter);
    vm_.retireAwait(cursor, *cursorIncarnation, continuation);
    throw std::runtime_error("VM refused to park an I/O await");
  }
  if (nextId_ == 0) {
    service_.retireWaiter(pending->waiter);
    vm_.retireAwait(cursor, *cursorIncarnation, continuation);
    throw std::overflow_error("I/O host waiter identity space exhausted");
  }
  try {
    parked_.emplace(nextId_, ParkedAwait{request, pending->waiter, cursor,
                                         *cursorIncarnation, continuation});
  } catch (...) {
    // The VM already parked and the service registered the waiter; an
    // allocation failure here must not leave either behind.
    service_.retireWaiter(pending->waiter);
    vm_.retireAwait(cursor, *cursorIncarnation, continuation);
    throw;
  }
  if (nextVisit_ == 0) nextVisit_ = nextId_;
  ++nextId_;
  return result;
}

auto IoHost::cancelWaiter(const WaiterCell waiter) -> WaiterCancelResult {
  requireOwner();
  const auto result = service_.cancelWaiter(waiter);
  if (result.state == WaiterCancelState::Removed) {
    // The service actually dropped a still-pending waiter: this await can
    // never resume, so release the VM continuation and the parked entry.
    for (auto it = parked_.begin(); it != parked_.end(); ++it) {
      if (it->second.waiter == waiter) {
        vm_.retireAwait(it->second.cursor, it->second.cursorIncarnation,
                        it->second.continuation);
        routes_.erase(it->second.continuation);
        parked_.erase(it);
        break;
      }
    }
  }
  // AlreadyComplete means the service already retired the waiter -- most
  // often because the request went terminal and pumpIo has not reached
  // this parked entry yet (a work budget can stop the pass between
  // service_.pump and the resume). The outcome is still deliverable
  // through service_.poll, so the entry must stay until pumpIo resumes or
  // abandons it; erasing it here would strand the continuation forever.
  // Refused likewise leaves the waiter live.
  return result;
}

void IoHost::retireWaiter(const WaiterCell waiter) {
  static_cast<void>(cancelWaiter(waiter));
}

void IoHost::retireRequest(const RequestCell request) {
  requireOwner();
  service_.retireRequest(request);
}

auto IoHost::orphanRequest(const RequestCell request)
    -> IoService::OrphanResult {
  requireOwner();
  return service_.orphanRequest(request);
}

auto IoHost::orphanCount() const -> std::uint64_t {
  requireOwner();
  return service_.orphanCount();
}

auto IoHost::retireIfTerminal(const RequestCell request) -> bool {
  requireOwner();
  if (!service_.poll(request).outcome) return false;
  service_.retireRequest(request);
  return true;
}

void IoHost::reapRetiredRequest(const RequestCell request) {
  requireOwner();
  service_.reapRetiredRequest(request);
}

auto IoHost::routeWake(FrontendToken token, FrontendRunner &runner)
    -> std::expected<void, WakeRouteError> {
  requireOwner();
  const auto matches = std::ranges::any_of(parked_, [&](const auto &entry) {
    const auto &parked = entry.second;
    return parked.continuation == token.continuation &&
           parked.request == token.request && parked.waiter == token.waiter;
  });
  if (!matches) return std::unexpected{WakeRouteError::NotParked};
  const auto continuation = token.continuation;
  if (const auto existing = routes_.find(continuation);
      existing != routes_.end()) {
    const auto &held = existing->second;
    if (held.runner == &runner && !held.life.expired() &&
        held.token.activation == token.activation) {
      return {};
    }
    return std::unexpected{WakeRouteError::AlreadyRouted};
  }
  routes_.emplace(continuation,
                  WakeRoute{&runner, runner.routeLife(), std::move(token)});
  return {};
}

auto IoHost::unrouteWake(const ContinuationId continuation) -> bool {
  requireOwner();
  return routes_.erase(continuation) != 0;
}

auto IoHost::unrouteRunner(const FrontendRunner &runner) -> std::size_t {
  requireOwner();
  return std::erase_if(routes_, [&](const auto &entry) {
    return entry.second.runner == &runner;
  });
}

auto IoHost::takeResumedOutcome(const zigzag::CellRef cursor,
                                const ContinuationId continuation)
    -> std::optional<OutcomeCell> {
  requireOwner();
  const auto incarnation = arena_.incarnationOf(cursor);
  if (!incarnation) return std::nullopt;
  const auto outcome = vm_.takeAwaitOutcome(cursor, *incarnation, continuation);
  if (outcome) vm_.retireAwait(cursor, *incarnation, continuation);
  return outcome;
}

void IoHost::collectWake(const ParkedAwait &parked, const FrontendWake why,
                         std::vector<PendingWake> &wakes) {
  const auto it = routes_.find(parked.continuation);
  if (it == routes_.end()) return;
  wakes.push_back(
      {std::move(it->second), why, parked.cursor, parked.cursorIncarnation});
  routes_.erase(it);
}

auto IoHost::pumpIo(const std::size_t workBudget) -> std::size_t {
  requireOwner();
  if (workBudget == 0) return 0;
  const auto completed  = service_.pump(workBudget);
  std::size_t work      = completed;
  std::size_t remaining = workBudget - std::min(workBudget, completed);
  // Wakeups are delivered after the pass so a runner that re-awaits from
  // inside wake() sees a consistent parked set and cannot be revisited by
  // the loop that woke it.
  std::vector<PendingWake> wakes;

  while (remaining > 0 && !parked_.empty()) {
    auto it = parked_.lower_bound(nextVisit_);
    if (it == parked_.end()) it = parked_.begin();
    const auto id = it->first;
    auto &parked  = it->second;
    nextVisit_    = id + 1;
    if (nextVisit_ == 0) nextVisit_ = parked_.begin()->first;
    --remaining;
    ++work;

    if (!arena_.matches(parked.cursor, parked.cursorIncarnation) ||
        !arena_.matches(parked.request.cell, parked.request.incarnation)) {
      collectWake(parked, FrontendWake::Abandoned, wakes);
      abandon(parked);
      parked_.erase(it);
      continue;
    }
    const auto status = service_.poll(parked.request);
    if (!status.outcome) continue;
    if (!arena_.matches(status.outcome->cell, status.outcome->incarnation) ||
        !ioOutcomeTag(arena_, *status.outcome)) {
      collectWake(parked, FrontendWake::Abandoned, wakes);
      abandon(parked);
      parked_.erase(it);
      continue;
    }

    const auto resumed =
        vm_.resumeAwait(parked.cursor, parked.cursorIncarnation,
                        parked.continuation, *status.outcome);
    if (!resumed.success) {
      collectWake(parked, FrontendWake::Abandoned, wakes);
      abandon(parked);
      parked_.erase(it);
      continue;
    }
    service_.retireWaiter(parked.waiter);
    collectWake(parked, FrontendWake::Resumable, wakes);
    parked_.erase(it);
  }
  for (auto &wake : wakes) {
    // Re-checked per call: an earlier wake in this batch may have destroyed
    // this runner, and a runner that died without cancelling its waiter left
    // only an address behind.
    if (wake.route.life.expired()) {
      GLEDITOR_LOG_DEBUG("vortex.io.host", "wake skipped: runner is gone");
      continue;
    }
    const auto continuation = wake.route.token.continuation;
    try {
      wake.route.runner->wake(std::move(wake.route.token), wake.why);
    } catch (const std::exception &error) {
      // The cursor was already resumed and the route consumed, so the token
      // is Failed: release its VM continuation and carry on with the batch.
      ++failedWakes_;
      GLEDITOR_LOG_WARN("vortex.io.host", "wake failed, token abandoned: {}",
                        error.what());
      vm_.retireAwait(wake.cursor, wake.cursorIncarnation, continuation);
    } catch (...) {
      ++failedWakes_;
      GLEDITOR_LOG_WARN("vortex.io.host",
                        "wake failed with a non-standard exception");
      vm_.retireAwait(wake.cursor, wake.cursorIncarnation, continuation);
    }
  }
  return work;
}

} // namespace xanadu::vortex
