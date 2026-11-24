#ifndef XANADU_VORTEX_IO_HOST_HPP
#define XANADU_VORTEX_IO_HOST_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <thread>

#include "common/xanadu/vortex/io_service.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace xanadu::vortex {

/// Owner-thread bridge between the asynchronous I/O service and VM cursors.
/// Workers never call this class: pumpIo publishes completions and resumes
/// only the cursor registered by the corresponding waiter.
class IoHost final {
public:
  IoHost(IoService &service, zigzag::ArenaManifold &arena,
         zigzag::vortex::VortexVM &vm);
  ~IoHost();

  IoHost(const IoHost &)            = delete;
  IoHost &operator=(const IoHost &) = delete;
  IoHost(IoHost &&)                 = delete;
  IoHost &operator=(IoHost &&)      = delete;

  [[nodiscard]] auto awaitCell(RequestCell request, zigzag::CellRef cursor)
      -> AwaitResult;
  [[nodiscard]] auto cancelWaiter(WaiterCell waiter) -> WaiterCancelResult;
  void retireWaiter(WaiterCell waiter);

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

  IoService &service_;
  zigzag::ArenaManifold &arena_;
  zigzag::vortex::VortexVM &vm_;
  const std::thread::id owner_;
  std::map<std::uint64_t, ParkedAwait> parked_;
  std::uint64_t nextId_{1};
  std::uint64_t nextVisit_{1};
};

} // namespace xanadu::vortex

#endif
