/**
 * @file vpl_engine.hpp
 * @brief Thin facade that evaluates VPL on the Vortex VM.
 *
 * VPLEngine owns an arena, a core, a VM and a standard library, compiles each
 * expression with VPLCompiler and runs the program through Executor. No AST is
 * interpreted by host code: an expression that the compiler does not lower is
 * refused with the compiler's named message.
 */
#ifndef COMMON_XANADU_VPL_VPL_ENGINE_HPP
#define COMMON_XANADU_VPL_VPL_ENGINE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/ast.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/vpl/parser.hpp"
#include "common/xanadu/vpl/view.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu {
class Store;
} // namespace xanadu

namespace xanadu::vpl {

/// Thrown by VPLEngine::evaluate when compiling or running fails. A
/// runtime_error, so callers that only care that evaluation failed keep
/// working; stage() tells a refused form (Compile) from an exhausted budget
/// (Budget).
class VPLExecutionError : public std::runtime_error {
public:
  VPLExecutionError(ExecutionError::Stage stage, const std::string &message)
      : std::runtime_error(message), stage_(stage) {}
  [[nodiscard]] ExecutionError::Stage stage() const noexcept { return stage_; }

private:
  ExecutionError::Stage stage_;
};

class VPLEngine {
public:
  VPLEngine();
  explicit VPLEngine(zigzag::ArenaManifold &arena);
  explicit VPLEngine(zigzag::vortex::VortexCore &core);
  explicit VPLEngine(Store &store);
  ~VPLEngine();

  VPLEngine(const VPLEngine &)            = delete;
  VPLEngine &operator=(const VPLEngine &) = delete;

  VPLEngine *withConsoleEffects(bool enabled) noexcept {
    consoleEffects_ = enabled;
    return this;
  }

  /// Compiles an AST node and runs it on the VM. Throws VPLExecutionError.
  VplView evaluate(const AstNode &node);

  /// Lexes, parses, compiles and runs a VPL source expression on the VM.
  VplView evaluate(std::string_view source);

  /// Logical VM cycles each evaluation may spend before it fails with a Budget
  /// error.
  VPLEngine *setCycleBudget(std::size_t cycles) noexcept {
    cycleBudget_ = cycles;
    return this;
  }
  [[nodiscard]] std::size_t cycleBudget() const noexcept {
    return cycleBudget_;
  }
  /// Cycles the VM has executed in total; it only moves when the VM runs.
  [[nodiscard]] std::uint64_t totalCycles() const noexcept {
    return vm_->totalCycles();
  }

  /// Variable environment bindings.
  void setVariable(std::string_view name, VplView value);
  [[nodiscard]] std::optional<VplView> getVariable(std::string_view name) const;
  void clearVariables() noexcept;

  /// Operand identities for an output row, retained across variable bindings.
  [[nodiscard]] std::span<const zigzag::CellRef>
  dependenciesOf(zigzag::CellRef cell) const;

  /// Dimension resolution: resolves or mints named dimension cell.
  zigzag::DimRef resolveDimension(std::string_view name);

  // Accessors
  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return *core_; }
  [[nodiscard]] const zigzag::vortex::VortexCore &core() const noexcept {
    return *core_;
  }
  [[nodiscard]] zigzag::ArenaManifold &arena() noexcept {
    return core_->arena();
  }
  [[nodiscard]] const zigzag::ArenaManifold &arena() const noexcept {
    return core_->arena();
  }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return *vm_; }
  [[nodiscard]] zigzag::vortex::VortexStdLib &stdlib() noexcept {
    return *stdlib_;
  }
  [[nodiscard]] Store *store() const noexcept { return store_; }
  VPLEngine *setStore(Store *store) noexcept {
    store_ = store;
    return this;
  }

  /// Default dimension for 1D arrays (defaults to "d.1").
  [[nodiscard]] zigzag::DimRef defaultDim();

private:
  /// Builds the executor once core_ and vm_ point at live objects.
  void wire();
  VplView unwrap(std::expected<ExecutionResult, ExecutionError> result);

  std::unique_ptr<zigzag::ArenaManifold> ownedArena_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexCore> ownedCore_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexVM> ownedVm_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexStdLib> ownedStdlib_{nullptr};
  std::unique_ptr<Executor> executor_;

  zigzag::vortex::VortexCore *core_{nullptr};
  zigzag::vortex::VortexVM *vm_{nullptr};
  zigzag::vortex::VortexStdLib *stdlib_{nullptr};
  Store *store_{nullptr};
  bool consoleEffects_{true};
  std::size_t cycleBudget_{kDefaultCycleBudget};

  std::optional<zigzag::DimRef> defaultDim_;
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_VPL_ENGINE_HPP
