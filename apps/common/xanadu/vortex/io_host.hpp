#ifndef XANADU_VORTEX_IO_HOST_HPP
#define XANADU_VORTEX_IO_HOST_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "common/xanadu/vortex/frontend_token.hpp"
#include "common/xanadu/vortex/io_profile.hpp"
#include "common/xanadu/vortex/io_service.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vortex {

/// Owner-thread bridge between the asynchronous I/O service and VM cursors.
/// Workers never call this class: pumpIo publishes completions and resumes
/// only the cursor registered by the corresponding waiter.
class IoHost final {
public:
  IoHost(IoService &service, zigzag::ArenaManifold &arena,
         zigzag::vortex::VortexVM &vm,
         IoExecutionProfile profile = IoExecutionProfile::Interactive);
  ~IoHost();

  IoHost(const IoHost &)            = delete;
  IoHost &operator=(const IoHost &) = delete;
  IoHost(IoHost &&)                 = delete;
  IoHost &operator=(IoHost &&)      = delete;

  [[nodiscard]] auto awaitCell(RequestCell request, zigzag::CellRef cursor)
      -> AwaitResult;
  [[nodiscard]] auto cancelWaiter(WaiterCell waiter) -> WaiterCancelResult;
  void retireWaiter(WaiterCell waiter);
  /// Retire a request whose outcome the owner has fully consumed. Await-
  /// driven requests have no other retirement path, so without this the
  /// service's request index grows monotonically; call it only when the
  /// outcome will never be polled or awaited again (a rolled-back outcome
  /// cell may still need `ensureTerminalOutcome` rematerialization, so
  /// retire after the arena state that could invalidate it is committed).
  void retireRequest(RequestCell request);
  /// Forget a request whose cells were rolled back; see
  /// IoService::orphanRequest. It is never cancelled.
  auto orphanRequest(RequestCell request) -> IoService::OrphanResult;
  [[nodiscard]] auto orphanCount() const -> std::uint64_t;
  /// Retire @p request only when it is already terminal, so its outcome is
  /// readable and nobody is cancelled by the call. False for a request still
  /// running (which `retireRequest` would cancel), or a stale one.
  auto retireIfTerminal(RequestCell request) -> bool;
  /// Reap the graph of a request that is already retired; see
  /// IoService::reapRetiredRequest.
  void reapRetiredRequest(RequestCell request);

  /// The immutable per-run profile applied before every admission below.
  [[nodiscard]] auto profile() const noexcept -> IoExecutionProfile;

  /// §9's generic gate: a direct `submit`/`admitOpen` reached while this
  /// host's profile forbids it (an actor turn) is refused before the
  /// request ever reaches `IoService`. Core/VM never see this check; it
  /// runs only here, ahead of the service boundary.
  /// @p within, when present, ends the request as `TimedOut` that long after
  /// admission, on the service's clock (see IoService::submit).
  [[nodiscard]] auto
  submitCell(CapabilityCell capability, zigzag::CellRef requestSpec,
             std::optional<std::chrono::milliseconds> within = std::nullopt)
      -> SubmitResult;
  [[nodiscard]] auto admitOpen(OpenSpec spec) -> SubmitResult;
  /// A refusal a front end decided before any request existed (a write over
  /// the host's bound): the same `Rejected` value an admission refusal is,
  /// minted through the owner-only cell store.
  [[nodiscard]] auto refuse(IoErrorInfo reason) -> SubmitResult;
  /// Ask a request's backend to stop. The request's outcome is `Cancelled`
  /// (or whatever the backend had already finished); a terminal request is
  /// `AlreadyTerminal`, a value and not an error. It never retires the
  /// request.
  [[nodiscard]] auto cancelRequest(RequestCell request) -> RequestCancelResult;
  /// Close an endpoint through the same gate as any other admission; the
  /// close is its own admitted request.
  [[nodiscard]] auto closeEndpoint(EndpointCell endpoint, CloseMode mode)
      -> SubmitResult;
  /// The clock the service reads request deadlines on; a language that keeps
  /// its own deadline reads this one so the two cannot disagree.
  [[nodiscard]] auto now() const -> std::chrono::steady_clock::time_point;
  /// Whether @p request already has its terminal outcome.
  [[nodiscard]] auto isTerminal(RequestCell request) -> bool;

  enum class WakeRouteError : std::uint8_t {
    NotParked,     ///< no parked await matches the token's request, waiter
                   ///< and continuation
    AlreadyRouted, ///< the continuation already has a runner
  };
  /// Bind a parked await's wakeup to its owning runner (plan §10). The
  /// token must name an await this host parked; when `pumpIo` later resumes
  /// (or abandons) that cursor, the same token is handed to @p runner. The
  /// runner need not outlive the route: delivery re-checks the runner's
  /// `routeLife()` just before each call and skips a dead one, a cancelled
  /// waiter drops the route silently, and ~IoHost drops what is left without
  /// calling anyone. Routing the same token to the same runner and activation
  /// again is accepted; another runner or activation is `AlreadyRouted`.
  /// Host-side by nature: the route is native state beside the parked entry
  /// it belongs to.
  [[nodiscard]] auto routeWake(FrontendToken token, FrontendRunner &runner)
      -> std::expected<void, WakeRouteError>;
  /// Drop the route of @p continuation without touching the parked await, for
  /// a runner that stops caring about one wake. True when a route was
  /// removed.
  auto unrouteWake(ContinuationId continuation) -> bool;
  /// Drop every route to @p runner; what a runner's teardown calls. Returns
  /// how many were removed.
  auto unrouteRunner(const FrontendRunner &runner) -> std::size_t;
  /// Wakes whose delivery threw and so were contained: the token was
  /// abandoned, its VM continuation retired. Diagnostic counter.
  [[nodiscard]] auto failedWakes() const noexcept -> std::size_t {
    return failedWakes_;
  }

  /// A runner that is not stepping an Await opcode collects the outcome
  /// `pumpIo` delivered to a woken token: the VM's parked outcome for
  /// @p continuation on @p cursor, retired in the same call so it can be
  /// taken once. nullopt when nothing was delivered (or the outcome cell was
  /// rolled back); the runner then re-awaits the same request, which
  /// rematerializes it.
  [[nodiscard]] auto takeResumedOutcome(zigzag::CellRef cursor,
                                        ContinuationId continuation)
      -> std::optional<OutcomeCell>;

  /// The budget covers service completions and waiter checks/resumptions.
  /// Pending waiters are visited round-robin, so a large parked set cannot
  /// monopolize the owner thread or starve later entries.
  [[nodiscard]] auto pumpIo(std::size_t workBudget) -> std::size_t;

private:
  struct ParkedAwait {
    RequestCell request;
    WaiterCell waiter;
    zigzag::CellRef cursor;
    AllocationIncarnation cursorIncarnation;
    ContinuationId continuation{};
  };

  struct WakeRoute {
    FrontendRunner *runner{};
    std::weak_ptr<const void> life;
    FrontendToken token;
  };
  struct PendingWake {
    WakeRoute route;
    FrontendWake why{};
    zigzag::CellRef cursor{};
    AllocationIncarnation cursorIncarnation{};
  };

  void requireOwner() const;
  void collectWake(const ParkedAwait &parked, FrontendWake why,
                   std::vector<PendingWake> &wakes);
  void abandon(ParkedAwait &parked) noexcept;
  /// The VM-facing await handler: awaitCell with its exceptions turned into
  /// a typed refusal value.
  [[nodiscard]] auto awaitForVm(RequestCell request, zigzag::CellRef cursor)
      -> AwaitResult;

  IoService &service_;
  zigzag::ArenaManifold &arena_;
  zigzag::vortex::VortexVM &vm_;
  const IoExecutionProfile profile_;
  const std::thread::id owner_;
  std::map<std::uint64_t, ParkedAwait> parked_;
  std::map<ContinuationId, WakeRoute> routes_;
  std::uint64_t nextId_{1};
  std::uint64_t nextVisit_{1};
  std::size_t failedWakes_{};
};

} // namespace xanadu::vortex

#endif
