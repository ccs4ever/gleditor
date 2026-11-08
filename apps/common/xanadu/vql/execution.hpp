/**
 * @file execution.hpp
 * @brief Frontend-neutral compilation and VM execution of VQL.
 */
#ifndef COMMON_XANADU_VQL_EXECUTION_HPP
#define COMMON_XANADU_VQL_EXECUTION_HPP

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/contracts.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

namespace xanadu::vql {

class VQLEngine;

enum class QueryExecutionMode : std::uint8_t { Direct, Vortex };

struct QueryExecutionError {
  enum class Category : std::uint8_t {
    Parse,
    Compile,
    TypeContract,
    Runtime,
    Budget,
  };

  Category category{Category::Runtime};
  std::string message;
};

struct QueryExecutionOptions {
  QueryExecutionMode mode{QueryExecutionMode::Vortex};
  std::size_t maxCycles{100'000};
};

/// Compiles VQL to instructions and runs them on the Vortex VM.
class QueryExecutor {
public:
  QueryExecutor(MultiStoreCoordinator &coordinator,
                zigzag::vortex::VortexVM &vm,
                QueryExecutionOptions options = {},
                ContractBindings bindings     = {});
  QueryExecutor(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                QueryExecutionOptions options = {});

  [[nodiscard]] std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
  execute(std::string_view source,
          const std::vector<zigzag::CellRef> &contextCells = {});
  [[nodiscard]] std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
  execute(const QueryExpression &query,
          const std::vector<zigzag::CellRef> &contextCells = {});
  [[nodiscard]] std::string_view disassembly() const noexcept {
    return disassembly_;
  }

private:
  [[nodiscard]] std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
  executeVortex(const QueryExpression &query,
                const std::vector<zigzag::CellRef> &contextCells);

  MultiStoreCoordinator &coordinator_;
  ContractBindings bindings_;
  std::optional<std::reference_wrapper<const ContractBindings>> bindingSource_;
  VQLEngine *session_{nullptr};
  zigzag::vortex::VortexVM &vm_;
  QueryExecutionOptions options_;
  std::string disassembly_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_EXECUTION_HPP
