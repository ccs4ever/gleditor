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
#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/prose_hits.hpp"
#include "common/xanadu/vql/script_runner.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

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
  if (hasIoEffect(query)) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Unsupported,
        .message = std::string{vortex::frontend_codes::vqlEffectNotCompilable} +
                   ": a script I/O action parks and resumes; run the script "
                   "through ScriptRunner, which never blocks"});
  }
  QueryExpression effectiveQuery = query;
  if (session_ != nullptr) {
    for (const auto &[name, def] : session_->userDefs()) {
      if (std::ranges::none_of(
              effectiveQuery.defs,
              [&](const DefDeclaration &d) { return d.name == name; })) {
        effectiveQuery.defs.push_back(def);
      }
    }
  }
  try {
    const auto eligibility = analyzeFrameEligibility(effectiveQuery);
    if (!eligibility)
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = eligibility.error().message});
    validateSignatures(effectiveQuery);
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
  return executeVortex(effectiveQuery, contextCells);
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
  const auto preCompileMark = coordinator_.core().arena().mark();
  try {
    // PROSE-HITS REGISTRATION SITE
    struct ProseHitsCache {
      std::string lastNeedle;
      bool hasNeedle{false};
      std::vector<zigzag::CellRef> hits;
    };
    auto cache = std::make_shared<ProseHitsCache>();
    const RestoreScope restoreProseHits{
        [this] { vm_.setRelationProvider("prose_hits", nullptr); }};
    vm_.setRelationProvider(
        "prose_hits",
        [this, cache](const zigzag::vortex::RelationQuery &rq)
            -> zigzag::vortex::RelationResult {
          if (rq.arguments.empty()) {
            return std::unexpected("prose_hits expects 1 needle argument");
          }
          const auto needleCell    = rq.arguments[0];
          const std::string needle = rq.arena.textOf(needleCell);
          if (!cache->hasNeedle || cache->lastNeedle != needle) {
            cache->lastNeedle = needle;
            cache->hasNeedle  = true;
            cache->hits       = proseHits(coordinator_, needle, rq.arena);
          }
          if (rq.position < 0 ||
              static_cast<std::size_t>(rq.position) >= cache->hits.size()) {
            return std::optional<zigzag::vortex::RelationAnswer>{std::nullopt};
          }
          const auto hitCell =
              cache->hits[static_cast<std::size_t>(rq.position)];
          return std::optional<zigzag::vortex::RelationAnswer>{
              zigzag::vortex::RelationAnswer{
                  .nextPosition = rq.position + 1,
                  .limit        = static_cast<std::int64_t>(cache->hits.size()),
                  .answers      = {hitCell},
              }};
        });

    VQLCompiler compiler(coordinator_, vm_);
    CompilationOptions compilation;
    compilation.contextCells = contextCells;
    compilation.initialBindings =
        bindingSource_ ? bindingSource_->get() : bindings_;
    const std::size_t preCompileViewCount =
        session_ ? session_->dynamicViews().viewCells.size() : 0U;
    if (session_) {
      compilation.dynamicViews    = &session_->dynamicViews();
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
      zigzag::expectMarkOrder(
          coordinator_.core().arena().release(preCompileMark));
      vm_.pruneAssemblyState();
      return std::vector<zigzag::CellRef>{};
    }
    const auto result = compiler.compile(query, compilation);
    disassembly_      = result.disassembly;
    if (!result.success) {
      zigzag::expectMarkOrder(
          coordinator_.core().arena().release(preCompileMark));
      vm_.pruneAssemblyState();
      return std::unexpected(QueryExecutionError{
          .category = result.category.value_or(QueryErrorCategory::Unsupported),
          .message  = result.errorMessage,
      });
    }

    if (!result.cursor || !result.terminalOpcode) {
      zigzag::expectMarkOrder(
          coordinator_.core().arena().release(preCompileMark));
      vm_.pruneAssemblyState();
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
    const auto preRunTrailSize = coordinator_.core().arena().trailSize();
    const auto preRunCellCount = coordinator_.core().arena().cellCount();

    const auto execution = vm_.run(cursor, options_.maxCycles);
    if (!execution.success) {
      vm_.abortScopedChoices(cursor);
      vm_.setCursorOpcode(cursor, std::nullopt);
      vm_.clearChoicePoints();
      if (coordinator_.core().arena().trailSize() == preRunTrailSize) {
        zigzag::expectMarkOrder(
            coordinator_.core().arena().release(preCompileMark));
        vm_.pruneAssemblyState();
      } else {
        zigzag::expectMarkOrder(
            coordinator_.core().arena().discard(preCompileMark));
      }
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
      session_->publishUserDefs(query.defs);
    }
    vm_.abortScopedChoices(cursor);
    vm_.setCursorOpcode(cursor, std::nullopt);
    vm_.clearChoicePoints();

    const auto &arena = coordinator_.core().arena();
    const bool noDynamicViewsChanged =
        !session_ ||
        (session_->dynamicViews().viewCells.size() == preCompileViewCount &&
         session_->dynamicViews().shapeHandles.empty() &&
         session_->dynamicViews().cellLenses.empty());
    const bool noMutations =
        noDynamicViewsChanged && arena.trailSize() == preRunTrailSize &&
        arena.cellCount() == preRunCellCount &&
        arena.spaceCount() == preCompileMark.spaceCount &&
        std::ranges::none_of(result.sessionBindings, [](const auto &binding) {
          return binding.kind ==
                 CompilationResult::SessionBinding::Kind::Stream;
        });
    const bool noNewResultCells =
        std::ranges::all_of(cells, [&](zigzag::CellRef c) {
          const auto dense = arena.denseOf(c);
          return !dense.has_value() || *dense < preCompileMark.cellCount;
        });

    if (noMutations && noNewResultCells) {
      zigzag::expectMarkOrder(
          coordinator_.core().arena().release(preCompileMark));
      vm_.pruneAssemblyState();
    } else {
      zigzag::expectMarkOrder(
          coordinator_.core().arena().discard(preCompileMark));
    }
    return cells;
  } catch (const TypeContractError &error) {
    vm_.clearChoicePoints();
    zigzag::expectMarkOrder(
        coordinator_.core().arena().release(preCompileMark));
    vm_.pruneAssemblyState();
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    vm_.clearChoicePoints();
    zigzag::expectMarkOrder(
        coordinator_.core().arena().release(preCompileMark));
    vm_.pruneAssemblyState();
    return std::unexpected(runtimeError(error));
  }
}

} // namespace xanadu::vql
