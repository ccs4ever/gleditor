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
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

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
         lowered.find("work limit") != std::string::npos ||
         lowered.find("activation frame depth limit") != std::string::npos ||
         (lowered.find("scoped choice") != std::string::npos &&
          lowered.find("limit") != std::string::npos) ||
         (lowered.find("limit") != std::string::npos &&
          (lowered.find("guard-scope") != std::string::npos ||
           lowered.find("mutation-group") != std::string::npos ||
           lowered.find("projection") != std::string::npos ||
           lowered.find("vqlmasklinks") != std::string::npos));
}

QueryExecutionError runtimeError(const std::exception &error) {
  if (const auto *refusal = dynamic_cast<const QueryRefusal *>(&error))
    return {.category = refusal->category(), .message = error.what()};
  return {.category = budgetFailure(error.what())
                          ? QueryExecutionError::Category::Budget
                          : QueryExecutionError::Category::Runtime,
          .message  = error.what()};
}

bool declaresOnly(const QueryExpression &query) {
  if (!query.defs.empty() || !query.body.statements.empty()) return false;
  if (query.mounts.empty() && query.namespaceDeclarations.empty()) return false;
  const auto *block = std::get_if<ExecutionBlock>(&query.expr);
  if (block == nullptr || !block->bindings.empty() || block->where)
    return false;
  const auto *ret = std::get_if<ReturnClause>(&block->action.clause);
  return ret != nullptr && ret->items.empty();
}

void publishMounts(
    VQLEngine *session,
    const std::vector<CompilationResult::SessionBinding> &events) {
  if (session == nullptr) return;
  using Kind = CompilationResult::SessionBinding::Kind;
  for (const auto &event : events) {
    if (event.kind == Kind::Mount)
      session->publishMount(
          event.name, VQLEngine::SessionMount{.direction = event.direction,
                                              .roots     = event.slots});
    else if (event.kind == Kind::Unmount)
      session->publishUnmount(event.name);
  }
}

} // namespace

QueryExecutor::QueryExecutor(MultiStoreCoordinator &coordinator,
                             zigzag::vortex::VortexVM &vm,
                             QueryExecutionOptions options,
                             ContractBindings bindings)
    : coordinator_(coordinator), bindings_(std::move(bindings)), vm_(vm),
      options_(options) {}

QueryExecutor::QueryExecutor(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                             QueryExecutionOptions options)
    : QueryExecutor(session.coordinator(), vm, options) {
  session_       = &session;
  bindingSource_ = std::cref(session.variables());
}

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
  if (&coordinator_.core() != &vm_.core()) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Runtime,
        .message  = "VQL executor engines must share one VortexCore",
    });
  }
  try {
    const auto eligibility = analyzeFrameEligibility(query);
    if (!eligibility)
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = eligibility.error().message});
    validateSignatures(query);
  } catch (const TypeContractError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
  if (options_.mode != QueryExecutionMode::Vortex)
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Compile,
        .message =
            "direct execution was removed; VQL requires Vortex compilation",
    });
  return executeVortex(query, contextCells);
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
  const auto priorContractCoordinator = vm_.contractCoordinator();
  const RestoreScope restoreContractCoordinator{
      [this, priorContractCoordinator] {
        vm_.setContractCoordinator(priorContractCoordinator);
      }};
  try {
    // PROSE-HITS REGISTRATION SITE: the card that lowers `find` with
    // RelationNext registers a provider here that calls proseHits()
    // (vql/prose_hits.hpp) with coordinator_ and its arena.
    VQLCompiler compiler(coordinator_, vm_);
    CompilationOptions compilation;
    compilation.contextCells = contextCells;
    compilation.initialBindings =
        bindingSource_ ? bindingSource_->get() : bindings_;
    if (session_) {
      compilation.initialFunctors = session_->compilerFunctors();
      for (const auto &[name, mount] : session_->compilerMounts())
        compilation.mounts.emplace(
            name, CompilationOptions::Mount{.direction = mount.direction,
                                            .roots     = mount.roots});
    }
    if (declaresOnly(query)) {
      // Mounts and unmounts alone are session state; compiling a program for
      // them would mint cells in the arena for nothing.
      const auto events = compiler.declareMounts(query, compilation);
      publishMounts(session_, events);
      return std::vector<zigzag::CellRef>{};
    }
    const auto result = compiler.compile(query, compilation);
    disassembly_      = result.disassembly;
    if (!result.success) {
      return std::unexpected(QueryExecutionError{
          .category = result.category.value_or(QueryErrorCategory::Unsupported),
          .message  = result.errorMessage,
      });
    }

    if (!result.cursor || !result.terminalOpcode) {
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = "compiled VQL has no executable activation metadata",
      });
    }
    const auto cursor = *result.cursor;

    // A frontend request has no resume handle. Keep raw VM stops resumable,
    // but release this request's cursor on success, failure, or exception.
    const RestoreScope finishRequest{[this, id = cursor] {
      vm_.abortScopedChoices(id);
      vm_.setCursorOpcode(id, std::nullopt);
    }};
    const auto execution = vm_.run(cursor, options_.maxCycles);
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
    for (const auto slot : result.resultSlots) {
      const auto values = vm_.resultCells(slot);
      cells.insert(cells.end(), values.begin(), values.end());
    }
    if (session_) {
      ContractBindings published;
      std::unordered_map<std::string, std::vector<PathStep>> functors;
      publishMounts(session_, result.sessionBindings);
      for (const auto &binding : result.sessionBindings) {
        using Kind = CompilationResult::SessionBinding::Kind;
        if (binding.kind == Kind::Mount || binding.kind == Kind::Unmount)
          continue;
        if (binding.kind == CompilationResult::SessionBinding::Kind::Functor) {
          functors.emplace(binding.name, binding.functor);
          continue;
        }
        auto &values = published[binding.name];
        for (const auto slot : binding.slots) {
          const auto slotValues = vm_.resultCells(slot);
          values.insert(values.end(), slotValues.begin(), slotValues.end());
        }
      }
      session_->publishCompilerBindings(std::move(published),
                                        std::move(functors));
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
