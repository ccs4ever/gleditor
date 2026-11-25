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
  if (hasIoEffect(query)) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Unsupported,
        .message  = std::string{vortex::frontend_codes::vqlEffectNeedsRunner} +
                   ": a script I/O action can park and resume; run the "
                   "script through ScriptRunner (or begin/resume), which "
                   "never blocks"});
  }
  const auto prepared = prepare(query);
  if (!prepared) return std::unexpected(prepared.error());
  return executeVortex(*prepared, contextCells);
}

std::expected<QueryExpression, QueryExecutionError>
QueryExecutor::prepare(const QueryExpression &query) {
  disassembly_.clear();
  if (&coordinator_.core() != &vm_.core()) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Runtime,
        .message  = "VQL executor engines must share one VortexCore",
    });
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
  return effectiveQuery;
}

/// One compiled program between its first step and its end: what a parked
/// script keeps so the same cursor resumes at the same Await. Everything the
/// run changed on the VM (the contract coordinator, the prose_hits provider,
/// the cursor) is undone when the run ends, finishes or is abandoned.
class ProgramRun {
public:
  ProgramRun(zigzag::vortex::VortexVM &vm, CompilationResult compiled,
             QueryExpression query)
      : vm_(vm), compiled_(std::move(compiled)), query_(std::move(query)) {}
  ~ProgramRun() { close(); }
  ProgramRun(const ProgramRun &)            = delete;
  ProgramRun &operator=(const ProgramRun &) = delete;

  void onClose(std::function<void()> undo) { undo_.push_back(std::move(undo)); }
  void close() noexcept {
    if (closed_) return;
    closed_ = true;
    try {
      vm_.abortScopedChoices(cursor());
      vm_.setCursorOpcode(cursor(), std::nullopt);
    } catch (const std::exception &) {
    }
    for (auto undo = undo_.rbegin(); undo != undo_.rend(); ++undo) {
      try {
        (*undo)();
      } catch (const std::exception &) {
      }
    }
  }
  [[nodiscard]] zigzag::CellRef cursor() const { return *compiled_.cursor; }

  zigzag::vortex::VortexVM &vm_;
  CompilationResult compiled_;
  QueryExpression query_;
  zigzag::Mark preCompileMark;
  bool markHeld{true};
  std::uint32_t preRunTrailSize{};
  std::uint32_t preRunCellCount{};
  std::size_t preCompileViewCount{};

private:
  std::vector<std::function<void()>> undo_;
  bool closed_{false};
};

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::executeVortex(const QueryExpression &query,
                             const std::vector<zigzag::CellRef> &contextCells) {
  auto step = beginVortex(query, contextCells);
  if (!step) return std::unexpected(std::move(step.error()));
  if (auto *done = std::get_if<ProgramResult>(&*step))
    return std::move(done->rows);
  // Only a program holding an I/O action can park, and execute() refuses those
  // before compiling.
  return std::unexpected(QueryExecutionError{
      .category = QueryExecutionError::Category::Runtime,
      .message  = "a synchronous execution parked on an await"});
}

std::expected<ProgramStep, QueryExecutionError>
QueryExecutor::begin(const QueryExpression &query,
                     const std::vector<zigzag::CellRef> &contextCells) {
  const auto prepared = prepare(query);
  if (!prepared) return std::unexpected(prepared.error());
  return beginVortex(*prepared, contextCells);
}

std::expected<ProgramStep, QueryExecutionError>
QueryExecutor::resume(ParkedProgram &parked) {
  if (!parked.run)
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Runtime,
        .message  = "the parked program is already finished"});
  auto run = std::move(parked.run);
  return advance(std::move(run));
}

std::expected<ProgramStep, QueryExecutionError>
QueryExecutor::beginVortex(const QueryExpression &query,
                           const std::vector<zigzag::CellRef> &contextCells) {
  if (options_.maxCycles == 0) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Budget,
        .message  = "VQL execution cycle budget must be positive",
    });
  }
  const auto priorContractCoordinator = vm_.contractCoordinator();
  const auto preCompileMark           = coordinator_.core().arena().mark();
  const std::size_t preCompileViewCount =
      session_ ? session_->dynamicViews().viewCells.size() : 0U;
  // Until the program has a run to own them, a refusal undoes these itself.
  std::vector<std::function<void()>> undo;
  const auto undoAll = [&undo] {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) (*it)();
    undo.clear();
  };
  undo.push_back([this, priorContractCoordinator] {
    vm_.setContractCoordinator(priorContractCoordinator);
  });
  try {
    // PROSE-HITS REGISTRATION SITE
    struct ProseHitsCache {
      std::string lastNeedle;
      bool hasNeedle{false};
      std::vector<zigzag::CellRef> hits;
    };
    auto cache = std::make_shared<ProseHitsCache>();
    undo.push_back([this] { vm_.setRelationProvider("prose_hits", nullptr); });
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
      undoAll();
      return ProgramStep{ProgramResult{}};
    }
    auto result = compiler.compile(query, compilation);
    if (!result.success) {
      zigzag::expectMarkOrder(
          coordinator_.core().arena().release(preCompileMark));
      vm_.pruneAssemblyState();
      disassembly_ = result.disassembly;
      undoAll();
      return std::unexpected(QueryExecutionError{
          .category = result.category.value_or(QueryErrorCategory::Unsupported),
          .message  = result.errorMessage,
          .statement = result.failingStatement,
      });
    }
    disassembly_ = result.disassembly;

    if (!result.cursor || !result.terminalOpcode) {
      zigzag::expectMarkOrder(
          coordinator_.core().arena().release(preCompileMark));
      vm_.pruneAssemblyState();
      undoAll();
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = "compiled VQL has no executable activation metadata",
      });
    }
    auto run = std::make_shared<ProgramRun>(vm_, std::move(result), query);
    run->preCompileMark      = preCompileMark;
    run->preCompileViewCount = preCompileViewCount;
    run->preRunTrailSize     = coordinator_.core().arena().trailSize();
    run->preRunCellCount     = coordinator_.core().arena().cellCount();
    for (auto &action : undo) run->onClose(std::move(action));
    undo.clear();
    return advance(std::move(run));
  } catch (const TypeContractError &error) {
    vm_.clearChoicePoints();
    zigzag::expectMarkOrder(
        coordinator_.core().arena().release(preCompileMark));
    vm_.pruneAssemblyState();
    undoAll();
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    vm_.clearChoicePoints();
    zigzag::expectMarkOrder(
        coordinator_.core().arena().release(preCompileMark));
    vm_.pruneAssemblyState();
    undoAll();
    return std::unexpected(runtimeError(error));
  }
}

std::expected<ProgramStep, QueryExecutionError>
QueryExecutor::advance(std::shared_ptr<ProgramRun> run) {
  auto &arena       = coordinator_.core().arena();
  const auto cursor = run->cursor();
  // A mark still held here is the pre-compile one; once the run has parked it
  // was cut (see below) and the remaining paths have nothing to release.
  const auto settleMark = [&](const bool keep) {
    if (!run->markHeld) return;
    run->markHeld = false;
    zigzag::expectMarkOrder(keep ? arena.discard(run->preCompileMark)
                                 : arena.release(run->preCompileMark));
    if (!keep) vm_.pruneAssemblyState();
  };
  try {
    const auto execution = vm_.run(cursor, options_.maxCycles);
    if (execution.disposition == zigzag::vortex::StepDisposition::Pending &&
        execution.pending) {
      // A parked program has run an I/O action and so changed the arena; its
      // cells are kept and the mark is cut now, because another parked program
      // may take a mark of its own before this one ends and marks end in
      // order.
      settleMark(true);
      ParkedProgram parked{
          .run = run, .cursor = cursor, .pending = *execution.pending};
      const auto at = vm_.getCursorOpcode(cursor);
      for (const auto &site : run->compiled_.ioActions)
        if (at && site.awaitOpcode == *at) parked.statement = site.statement;
      return ProgramStep{std::move(parked)};
    }
    if (!execution.success) {
      vm_.abortScopedChoices(cursor);
      vm_.setCursorOpcode(cursor, std::nullopt);
      vm_.clearChoicePoints();
      if (run->markHeld) {
        settleMark(arena.trailSize() != run->preRunTrailSize);
      }
      const auto category =
          execution.contractViolation ==
                  zigzag::vortex::ContractViolationKind::Postcondition
              ? QueryExecutionError::Category::TypeContract
              : (budgetFailure(execution.errorMessage)
                     ? QueryExecutionError::Category::Budget
                     : QueryExecutionError::Category::Runtime);
      std::optional<std::size_t> statement;
      for (const auto &site : run->compiled_.ioActions)
        if (execution.failingClause &&
            (site.submitOpcode == *execution.failingClause ||
             site.awaitOpcode == *execution.failingClause))
          statement = site.statement;
      return std::unexpected(QueryExecutionError{
          .category  = category,
          .message   = execution.errorMessage,
          .statement = statement,
      });
    }

    std::vector<zigzag::CellRef> cells;
    for (const auto slot : run->compiled_.resultSlots) {
      const auto values = vm_.resultCells(slot);
      cells.insert(cells.end(), values.begin(), values.end());
    }
    if (session_) {
      ContractBindings published;
      std::unordered_map<std::string, std::vector<PathStep>> functors;
      publishMounts(session_, run->compiled_.sessionBindings);
      for (const auto &binding : run->compiled_.sessionBindings) {
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
      session_->publishUserDefs(run->query_.defs);
    }
    vm_.abortScopedChoices(cursor);
    vm_.setCursorOpcode(cursor, std::nullopt);
    vm_.clearChoicePoints();

    if (run->markHeld) {
      const bool noDynamicViewsChanged =
          !session_ || (session_->dynamicViews().viewCells.size() ==
                            run->preCompileViewCount &&
                        session_->dynamicViews().shapeHandles.empty() &&
                        session_->dynamicViews().cellLenses.empty());
      const bool noMutations =
          noDynamicViewsChanged && arena.trailSize() == run->preRunTrailSize &&
          arena.cellCount() == run->preRunCellCount &&
          arena.spaceCount() == run->preCompileMark.spaceCount &&
          std::ranges::none_of(
              run->compiled_.sessionBindings, [](const auto &binding) {
                return binding.kind ==
                       CompilationResult::SessionBinding::Kind::Stream;
              });
      const bool noNewResultCells =
          std::ranges::all_of(cells, [&](zigzag::CellRef c) {
            const auto dense = arena.denseOf(c);
            return !dense.has_value() || *dense < run->preCompileMark.cellCount;
          });
      settleMark(!(noMutations && noNewResultCells));
    }
    ProgramResult done{.rows            = std::move(cells),
                       .finalIsIoEffect = run->compiled_.finalIsIoEffect};
    run->close();
    return ProgramStep{std::move(done)};
  } catch (const TypeContractError &error) {
    vm_.clearChoicePoints();
    if (run->markHeld) {
      run->markHeld = false;
      zigzag::expectMarkOrder(arena.release(run->preCompileMark));
      vm_.pruneAssemblyState();
    }
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    vm_.clearChoicePoints();
    if (run->markHeld) {
      run->markHeld = false;
      zigzag::expectMarkOrder(arena.release(run->preCompileMark));
      vm_.pruneAssemblyState();
    }
    return std::unexpected(runtimeError(error));
  }
}

} // namespace xanadu::vql
