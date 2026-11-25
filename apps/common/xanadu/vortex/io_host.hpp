#ifndef XANADU_VORTEX_IO_HOST_HPP
#define XANADU_VORTEX_IO_HOST_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <thread>

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

  /// The immutable per-run profile applied before every admission below.
  [[nodiscard]] auto profile() const noexcept -> IoExecutionProfile;

  /// §9's generic gate: a direct `submit`/`admitOpen` reached while this
  /// host's profile forbids it (an actor turn) is refused before the
  /// request ever reaches `IoService`. Core/VM never see this check; it
  /// runs only here, ahead of the service boundary.
  [[nodiscard]] auto submitCell(CapabilityCell capability,
                                zigzag::CellRef requestSpec) -> SubmitResult;
  [[nodiscard]] auto admitOpen(OpenSpec spec) -> SubmitResult;

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

  void requireOwner() const;
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
  std::uint64_t nextId_{1};
  std::uint64_t nextVisit_{1};
};

} // namespace xanadu::vortex

#endif
