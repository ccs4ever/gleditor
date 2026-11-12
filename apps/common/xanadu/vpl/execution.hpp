#ifndef COMMON_XANADU_VPL_EXECUTION_HPP
#define COMMON_XANADU_VPL_EXECUTION_HPP

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vpl/compiler.hpp"

namespace xanadu::vpl {

struct ExecutionError {
  enum class Stage {
    Configuration,
    Compile,
    Runtime,
    Budget
  } stage{Stage::Compile};
  std::string message;
};

struct ExecutionResult {
  VplView view;
  std::string disassembly;
  /// The program contained `⎕ x`: the VM performs no I/O, so the host decides
  /// what printing the final view means (card V13).
  bool print{false};
};

/// The dimension an enclosure's box hangs its content off. Lowering (card V10)
/// links box -> content along it and the executor reads it back, so the two
/// agree on one name.
inline constexpr std::string_view enclosureDimensionName = "d.enclosure";

/// Checks an assembled instruction chain for a branch whose target is not an
/// instruction of the chain, which the VM would follow into the end of the
/// program without complaint. Returns a message naming the first offender.
/// Labels minted outside any loop body can only be checked here, because their
/// forward branches are emitted before the label is placed.
[[nodiscard]] std::optional<std::string>
unplacedBranchTarget(const zigzag::vortex::VortexVM &vm,
                     std::span<const zigzag::CellRef> chain);

/// The cycle budget an evaluation gets unless the caller sets one. It mirrors
/// the default of VortexVM::run (vortex_vm.hpp), which this layer does not own;
/// keep the two equal.
inline constexpr std::size_t kDefaultCycleBudget = 100000;

class Executor {
public:
  Executor(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm)
      : core_(core), vm_(vm) {}

  [[nodiscard]] std::expected<ExecutionResult, ExecutionError>
  execute(std::string_view source, std::size_t maxCycles = kDefaultCycleBudget,
          bool optimize = false);
  [[nodiscard]] std::expected<ExecutionResult, ExecutionError>
  execute(const AstNode &ast, std::size_t maxCycles = kDefaultCycleBudget,
          bool optimize = false);

  Executor *setVariable(std::string_view name, VplView value);
  [[nodiscard]] std::optional<VplView> getVariable(std::string_view name) const;
  void clearVariables() noexcept {
    bindings_.clear();
    dependencies_.clear();
  }
  [[nodiscard]] zigzag::DimRef resolveDimension(std::string_view name);
  [[nodiscard]] std::span<const zigzag::CellRef>
  dependenciesOf(zigzag::CellRef cell) const noexcept;
  [[nodiscard]] const std::unordered_map<std::string, VplView> &
  variables() const noexcept {
    return bindings_;
  }

  /// Runs an already compiled program (its cursor must be spawned) and turns
  /// the result slot into a view, updating the variables it assigned. execute()
  /// is compile plus this; it is public so a graph assembled by hand can be
  /// run and its result read back through exactly the same code.
  [[nodiscard]] std::expected<ExecutionResult, ExecutionError>
  run(CompilationResult compiled, std::size_t maxCycles = kDefaultCycleBudget);

private:
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  std::unordered_map<std::string, VplView> bindings_;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>>
      dependencies_;
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_EXECUTION_HPP
