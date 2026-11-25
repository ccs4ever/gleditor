/**
 * @file execution.hpp
 * @brief Frontend-neutral compilation and VM execution of VQL.
 */
#ifndef COMMON_XANADU_VQL_EXECUTION_HPP
#define COMMON_XANADU_VQL_EXECUTION_HPP

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/query_error.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

namespace xanadu::vql {

class VQLEngine;

enum class QueryExecutionMode : std::uint8_t { Direct, Vortex };

/// The executor reports the same typed error as VQLEngine::tryExecute.
using QueryExecutionError = QueryError;

struct QueryExecutionOptions {
  QueryExecutionMode mode{QueryExecutionMode::Vortex};
  std::size_t maxCycles{100'000};
};

class ProgramRun;

/// A script that ran to its end: the rows of its final statement, and whether
/// that statement is an I/O action (so its first row is an outcome cell).
struct ProgramResult {
  std::vector<zigzag::CellRef> rows;
  bool finalIsIoEffect{false};
};

/// A script stopped at an Await that is not ready. The run it holds resumes at
/// the same instruction, so nothing before the Await executes again.
struct ParkedProgram {
  std::shared_ptr<ProgramRun> run;
  zigzag::CellRef cursor{};
  zigzag::vortex::PendingAwait pending;
  /// Index of the script statement the Await belongs to.
  std::optional<std::size_t> statement;
};

using ProgramStep = std::variant<ProgramResult, ParkedProgram>;

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
  /// Compile @p query and run it until it ends or parks on an Await. A script
  /// holding an I/O action may park; `execute` refuses those because it has no
  /// way to wait.
  [[nodiscard]] std::expected<ProgramStep, QueryExecutionError>
  begin(const QueryExpression &query,
        const std::vector<zigzag::CellRef> &contextCells = {});
  /// Continue a parked program from the Await it stopped at, once the host has
  /// delivered its outcome. Consumes @p parked.
  [[nodiscard]] std::expected<ProgramStep, QueryExecutionError>
  resume(ParkedProgram &parked);
  [[nodiscard]] std::string_view disassembly() const noexcept {
    return disassembly_;
  }

private:
  [[nodiscard]] std::expected<QueryExpression, QueryExecutionError>
  prepare(const QueryExpression &query);
  [[nodiscard]] std::expected<ProgramStep, QueryExecutionError>
  beginVortex(const QueryExpression &query,
              const std::vector<zigzag::CellRef> &contextCells);
  [[nodiscard]] std::expected<ProgramStep, QueryExecutionError>
  advance(std::shared_ptr<ProgramRun> run);
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
