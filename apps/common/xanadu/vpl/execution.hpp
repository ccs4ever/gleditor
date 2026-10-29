#ifndef COMMON_XANADU_VPL_EXECUTION_HPP
#define COMMON_XANADU_VPL_EXECUTION_HPP

#include <expected>
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
};

class Executor {
public:
  Executor(zigzag::vortex::VortexCore &core, zigzag::vortex::VortexVM &vm)
      : core_(core), vm_(vm) {}

  [[nodiscard]] std::expected<ExecutionResult, ExecutionError>
  execute(std::string_view source, std::size_t maxCycles = 100000,
          bool optimize = false);
  [[nodiscard]] std::expected<ExecutionResult, ExecutionError>
  execute(const AstNode &ast, std::size_t maxCycles = 100000,
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

private:
  [[nodiscard]] std::expected<ExecutionResult, ExecutionError>
  run(CompilationResult compiled, std::size_t maxCycles);

  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  std::unordered_map<std::string, VplView> bindings_;
  std::unordered_map<zigzag::CellRef, std::vector<zigzag::CellRef>>
      dependencies_;
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_EXECUTION_HPP
