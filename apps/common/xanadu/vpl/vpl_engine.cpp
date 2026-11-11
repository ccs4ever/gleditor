/**
 * @file vpl_engine.cpp
 * @brief VPLEngine: the facade over VPLCompiler and Executor.
 */
#include "common/xanadu/vpl/vpl_engine.hpp"

#include "common/xanadu/store.hpp"

namespace xanadu::vpl {

// core_/vm_/stdlib_ are raw aliases into unique_ptrs built in the constructor
// body, so each depends on a preceding *body* statement rather than only on
// constructor parameters; moving them into the member initializer list would
// capture a still-null pointer. See the cppcoreguidelines-prefer-member-
// initializer note in .clang-tidy for the MultiStoreCoordinator instance.
VPLEngine::VPLEngine() {
  ownedArena_ = std::make_unique<zigzag::ArenaManifold>();
  ownedCore_  = std::make_unique<zigzag::vortex::VortexCore>(*ownedArena_);
  ownedVm_    = std::make_unique<zigzag::vortex::VortexVM>(*ownedCore_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*ownedCore_, *ownedVm_);

  // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
  core_   = ownedCore_.get();
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
  // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
  wire();
}

VPLEngine::VPLEngine(zigzag::ArenaManifold &arena) {
  ownedCore_ = std::make_unique<zigzag::vortex::VortexCore>(arena);
  ownedVm_   = std::make_unique<zigzag::vortex::VortexVM>(*ownedCore_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*ownedCore_, *ownedVm_);

  // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
  core_   = ownedCore_.get();
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
  // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
  wire();
}

VPLEngine::VPLEngine(zigzag::vortex::VortexCore &core) : core_(&core) {
  ownedVm_ = std::make_unique<zigzag::vortex::VortexVM>(*core_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*core_, *ownedVm_);

  // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
  // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
  wire();
}

VPLEngine::VPLEngine(Store &store) : store_(&store) {
  ownedArena_ = std::make_unique<zigzag::ArenaManifold>();
  ownedCore_  = std::make_unique<zigzag::vortex::VortexCore>(*ownedArena_);
  ownedVm_    = std::make_unique<zigzag::vortex::VortexVM>(*ownedCore_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*ownedCore_, *ownedVm_);

  // NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer)
  core_   = ownedCore_.get();
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
  // NOLINTEND(cppcoreguidelines-prefer-member-initializer)
  wire();
}

VPLEngine::~VPLEngine() = default;

void VPLEngine::wire() { executor_ = std::make_unique<Executor>(*core_, *vm_); }

zigzag::DimRef VPLEngine::defaultDim() {
  if (!defaultDim_) {
    defaultDim_ = resolveDimension("d.1");
  }
  return *defaultDim_;
}

zigzag::DimRef VPLEngine::resolveDimension(std::string_view name) {
  return executor_->resolveDimension(name);
}

void VPLEngine::setVariable(std::string_view name, VplView value) {
  executor_->setVariable(name, std::move(value));
}

std::optional<VplView> VPLEngine::getVariable(std::string_view name) const {
  return executor_->getVariable(name);
}

void VPLEngine::clearVariables() noexcept { executor_->clearVariables(); }

std::span<const zigzag::CellRef>
VPLEngine::dependenciesOf(const zigzag::CellRef cell) const {
  return executor_->dependenciesOf(cell);
}

VplView
VPLEngine::unwrap(std::expected<ExecutionResult, ExecutionError> result) {
  if (!result) {
    throw VPLExecutionError(result.error().stage, result.error().message);
  }
  return std::move(result->view);
}

VplView VPLEngine::evaluate(std::string_view source) {
  return unwrap(executor_->execute(source, cycleBudget_));
}

VplView VPLEngine::evaluate(const AstNode &node) {
  return unwrap(executor_->execute(node, cycleBudget_));
}

} // namespace xanadu::vpl
