/**
 * @file execution.hpp
 * @brief Frontend-neutral execution of VQL through direct or Vortex engines.
 */
#ifndef COMMON_XANADU_VQL_EXECUTION_HPP
#define COMMON_XANADU_VQL_EXECUTION_HPP

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {

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
  QueryExecutionMode mode{QueryExecutionMode::Direct};
  std::size_t maxCycles{100'000};
};

/// Runs VQL on one of its real execution engines and preserves error classes.
class QueryExecutor {
public:
  QueryExecutor(VQLEngine &directEngine, zigzag::vortex::VortexVM &vm,
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
  executeDirect(const QueryExpression &query,
                const std::vector<zigzag::CellRef> &contextCells);
  [[nodiscard]] std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
  executeVortex(const QueryExpression &query,
                const std::vector<zigzag::CellRef> &contextCells);

  VQLEngine &directEngine_;
  zigzag::vortex::VortexVM &vm_;
  QueryExecutionOptions options_;
  std::string disassembly_;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_EXECUTION_HPP
