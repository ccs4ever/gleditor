#ifndef COMMON_XANADU_VORTEX_ASSEMBLY_TRANSACTION_HPP
#define COMMON_XANADU_VORTEX_ASSEMBLY_TRANSACTION_HPP

#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {

/// Publish graph assembly only after the complete program has validated.
class AssemblyTransaction {
public:
  explicit AssemblyTransaction(VortexVM &vm)
      : vm_(vm), mark_(vm.core().arena().mark()) {}
  AssemblyTransaction(const AssemblyTransaction &)            = delete;
  AssemblyTransaction &operator=(const AssemblyTransaction &) = delete;
  ~AssemblyTransaction() {
    if (committed_)
      vm_.core().arena().discard(mark_);
    else {
      vm_.core().arena().release(mark_);
      vm_.pruneAssemblyState();
    }
  }
  AssemblyTransaction *commit() noexcept {
    committed_ = true;
    return this;
  }

private:
  VortexVM &vm_;
  Mark mark_;
  bool committed_{false};
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_ASSEMBLY_TRANSACTION_HPP
