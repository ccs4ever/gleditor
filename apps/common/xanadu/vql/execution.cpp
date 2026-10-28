/**
 * @file execution.cpp
 * @brief Frontend-neutral VQL execution and error normalization.
 */
#include "common/xanadu/vql/execution.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <ranges>
#include <unordered_set>

#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vql {
namespace {

template <typename Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

void validateSignatures(const QueryExpression &query) {
  for (const auto &definition : query.defs)
    validateFunctionSignature(definition);
}

bool budgetFailure(const std::string_view message) {
  std::string lowered;
  lowered.reserve(message.size());
  std::ranges::transform(message, std::back_inserter(lowered), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return lowered.find("budget") != std::string::npos ||
         lowered.find("max cycles") != std::string::npos ||
         lowered.find("work limit") != std::string::npos;
}

QueryExecutionError runtimeError(const std::exception &error) {
  return {.category = budgetFailure(error.what())
                          ? QueryExecutionError::Category::Budget
                          : QueryExecutionError::Category::Runtime,
          .message  = error.what()};
}

} // namespace

QueryExecutor::QueryExecutor(VQLEngine &directEngine,
                             zigzag::vortex::VortexVM &vm,
                             QueryExecutionOptions options)
    : directEngine_(directEngine), vm_(vm), options_(options) {}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::execute(const std::string_view source,
                       const std::vector<zigzag::CellRef> &contextCells) {
  try {
    const auto query = Parser(source).parseQuery();
    return execute(query, contextCells);
  } catch (const ParseError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Parse,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::execute(const QueryExpression &query,
                       const std::vector<zigzag::CellRef> &contextCells) {
  disassembly_.clear();
  if (&directEngine_.core() != &vm_.core()) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Runtime,
        .message  = "VQL executor engines must share one VortexCore",
    });
  }
  try {
    validateSignatures(query);
  } catch (const TypeContractError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  }
  if (options_.mode == QueryExecutionMode::Direct)
    return executeDirect(query, contextCells);
  return executeVortex(query, contextCells);
}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::executeDirect(const QueryExpression &query,
                             const std::vector<zigzag::CellRef> &contextCells) {
  try {
    if (query.defs.empty() && query.mounts.empty() &&
        query.namespaceDeclarations.empty() && query.body.statements.empty() &&
        std::holds_alternative<PathExpression>(query.expr)) {
      return directEngine_.evaluatePath(std::get<PathExpression>(query.expr),
                                        contextCells);
    }

    // Context anchors consult `.` after the engine has entered a function.
    // Save and restore the binding so one frontend request cannot leak context.
    const auto savedContext = directEngine_.getVariable(".");
    const auto restore      = [this, savedContext] {
      directEngine_.setVariable(".", savedContext);
    };
    const RestoreScope restoreScope{restore};
    directEngine_.setVariable(".", contextCells);
    return directEngine_.execute(query);
  } catch (const TypeContractError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::executeVortex(const QueryExpression &query,
                             const std::vector<zigzag::CellRef> &contextCells) {
  if (options_.maxCycles == 0) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Budget,
        .message  = "VQL execution cycle budget must be positive",
    });
  }
  try {
    const auto before = vm_.activeCursors();
    const std::unordered_set<zigzag::CellRef> oldCursors(before.begin(),
                                                         before.end());
    VQLCompiler compiler(directEngine_.coordinator(), vm_);
    CompilationOptions compilation;
    compilation.contextCells    = contextCells;
    compilation.initialBindings = directEngine_.variables();
    const auto result           = compiler.compile(query, compilation);
    disassembly_                = result.disassembly;
    if (!result.success) {
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = result.errorMessage,
      });
    }

    const auto after  = vm_.activeCursors();
    const auto cursor = std::ranges::find_if(after, [&](const auto candidate) {
      return !oldCursors.contains(candidate);
    });
    if (cursor == after.end()) {
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Runtime,
          .message  = "compiled VQL did not create an execution cursor",
      });
    }

    // A frontend request has no resume handle. Keep raw VM stops resumable,
    // but release this request's cursor on success, failure, or exception.
    const RestoreScope finishRequest{
        [this, id = *cursor] { vm_.setCursorOpcode(id, std::nullopt); }};
    const auto execution = vm_.run(*cursor, options_.maxCycles);
    if (!execution.success) {
      const auto category =
          execution.contractViolation ==
                  zigzag::vortex::ContractViolationKind::Postcondition
              ? QueryExecutionError::Category::TypeContract
              : (budgetFailure(execution.errorMessage)
                     ? QueryExecutionError::Category::Budget
                     : QueryExecutionError::Category::Runtime);
      return std::unexpected(QueryExecutionError{
          .category = category,
          .message  = execution.errorMessage,
      });
    }

    std::vector<zigzag::CellRef> cells;
    std::optional<zigzag::CellRef> halt;
    for (const auto opcode : result.generatedOpcodes | std::views::reverse) {
      if (vm_.getOpcodeKind(opcode) == zigzag::vortex::OpcodeKind::Halt) {
        halt = opcode;
        break;
      }
    }
    if (!halt) {
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = "compiled VQL has no Halt instruction",
      });
    }
    for (const auto slot : directEngine_.core().inputsOf(*halt)) {
      const auto values = vm_.resultCells(slot);
      cells.insert(cells.end(), values.begin(), values.end());
    }
    return cells;
  } catch (const TypeContractError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
}

} // namespace xanadu::vql
