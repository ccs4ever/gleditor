#include "common/xanadu/vortex/io_host.hpp"

#include <algorithm>
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
        return awaitCell(request, cursor);
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
                        const zigzag::CellRef requestSpec) -> SubmitResult {
  requireOwner();
  if (!policyFor(profile_).allowDirectSubmit) {
    return {Refused{service_.refuse(profileRefusal())}};
  }
  return service_.submit(capability, requestSpec);
}

auto IoHost::admitOpen(OpenSpec spec) -> SubmitResult {
  requireOwner();
  if (!policyFor(profile_).allowDirectSubmit) {
    return {Refused{service_.refuse(profileRefusal())}};
  }
  return service_.admitOpen(std::move(spec));
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
  auto result   = service_.await(request, registration);
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
  parked_.emplace(nextId_, ParkedAwait{request, pending->waiter, cursor,
                                       *cursorIncarnation, continuation});
  if (nextVisit_ == 0) nextVisit_ = nextId_;
  ++nextId_;
  return result;
}

auto IoHost::cancelWaiter(const WaiterCell waiter) -> WaiterCancelResult {
  requireOwner();
  auto result = service_.cancelWaiter(waiter);
  for (auto it = parked_.begin(); it != parked_.end(); ++it) {
    if (it->second.waiter == waiter) {
      vm_.retireAwait(it->second.cursor, it->second.cursorIncarnation,
                      it->second.continuation);
      parked_.erase(it);
      break;
    }
  }
  return result;
}

void IoHost::retireWaiter(const WaiterCell waiter) {
  static_cast<void>(cancelWaiter(waiter));
}

void IoHost::retireRequest(const RequestCell request) {
  requireOwner();
  service_.retireRequest(request);
}

auto IoHost::pumpIo(const std::size_t workBudget) -> std::size_t {
  requireOwner();
  if (workBudget == 0) return 0;
  const auto completed  = service_.pump(workBudget);
  std::size_t work      = completed;
  std::size_t remaining = workBudget - std::min(workBudget, completed);

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
      abandon(parked);
      parked_.erase(it);
      continue;
    }
    const auto status = service_.poll(parked.request);
    if (!status.outcome) continue;
    if (!arena_.matches(status.outcome->cell, status.outcome->incarnation) ||
        !ioOutcomeTag(arena_, *status.outcome)) {
      abandon(parked);
      parked_.erase(it);
      continue;
    }

    const auto resumed =
        vm_.resumeAwait(parked.cursor, parked.cursorIncarnation,
                        parked.continuation, *status.outcome);
    if (!resumed.success) {
      abandon(parked);
      parked_.erase(it);
      continue;
    }
    service_.retireWaiter(parked.waiter);
    parked_.erase(it);
  }
  return work;
}

} // namespace xanadu::vortex
