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
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_copy_out.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include <gleditor/logging.hpp>

namespace xanadu::vql {
namespace {

template <typename Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

/// One query run's claim on the arena: everything minted from construction on
/// is the program's, and is released unless settle() carries results out.
class ProgramArea {
public:
  /// Why the program's cells were kept, by name.
  struct RetainedProgram {
    std::string because;
  };

  /// @p retained is told why whenever the program is kept, by any path.
  ProgramArea(zigzag::vortex::VortexVM &vm,
              std::optional<std::string> &retained)
      : vm_(vm), retainedSink_(retained), mark_(vm.core().arena().mark()),
        dimensionsBefore_(vm.core().arena().dimensions().size()) {}

  /// The program's opcode cells, once compiled: only to learn whether it can
  /// make a closure value, which no result check can see from the arena.
  void setProgram(std::span<const zigzag::CellRef> ops) {
    createsClosures_ =
        std::ranges::any_of(ops, [this](const zigzag::CellRef op) {
          return vm_.getOpcodeKind(op) ==
                 zigzag::vortex::OpcodeKind::CreateClosure;
        });
  }

  /// The program is compiled and attached to home; what the run writes from
  /// here is the run's. Attaching it wrote home (a +d.spin entry and a cursor
  /// on +d.cursors), which release() rightly undoes, so that write must not
  /// count as the run's own: a second mark gives the arena a point after it
  /// to ask from, and it is discarded before the first is released.
  void beginRun() { runMark_ = vm_.core().arena().mark(); }

  ProgramArea(const ProgramArea &)            = delete;
  ProgramArea &operator=(const ProgramArea &) = delete;
  ~ProgramArea() {
    if (settled_) return;
    // A run that fails after a committed mutation keeps that mutation, as it
    // did before: release() would undo it.
    if (mintedDimensions()) {
      retainedSink_ = std::string{dimensionReason};
      retain();
    } else if (wroteOlderCell()) {
      retainedSink_ = std::string{olderCellReason};
      retain();
    } else
      release();
  }

  /// Release the program, then re-mint the results it minted. A shape that
  /// cannot be copied exactly keeps the whole program instead -- the arena is
  /// then as it was before this existed -- and the refusal is returned.
  std::optional<RetainedProgram>
  settle(std::vector<zigzag::CellRef> &cells,
         std::span<std::vector<zigzag::CellRef> *const> alsoLive) {
    // A dimension the run named is a name the session keeps: release() would
    // un-name it while later queries still resolve it. The arena records
    // writes, not names, so this is the one question it cannot answer.
    if (mintedDimensions()) {
      retain();
      return RetainedProgram{std::string{dimensionReason}};
    }
    // release() undoes every write the run made to a cell older than the
    // mark, which is a mutation's intended effect (a sort, an apply_shape, a
    // weave). The arena says exactly whether there was one.
    if (wroteOlderCell()) {
      retain();
      return RetainedProgram{std::string{olderCellReason}};
    }
    std::vector<zigzag::CellRef> live{cells};
    for (const auto *extra : alsoLive)
      live.insert(live.end(), extra->begin(), extra->end());
    // A closure value names its descriptor and environment cells by integer
    // reference, which re-minting a cell would leave dangling, and a closure
    // can sit behind any result or binding. Inspecting them costs arena work
    // a tight budget notices, so a program that can make one keeps itself.
    // Known leak: such a query keeps its program (about 142 cells a run)
    // until a read-only VM accessor for closure values is approved, which
    // would let the check be exact and work-free.
    if (createsClosures_) {
      retain();
      return RetainedProgram{
          "the program creates closure values, which name program cells"};
    }
    auto captured = zigzag::CopyOut::capture(
        vm_.core().arena(), mark_, live, vm_.core().dims().runtimeObjectClaim);
    if (!captured) {
      retain();
      return RetainedProgram{zigzag::toString(captured.error())};
    }
    release();
    const auto replacements = captured->materialize(vm_.core().arena());
    zigzag::applyCopyOut(replacements, cells);
    for (auto *extra : alsoLive) zigzag::applyCopyOut(replacements, *extra);
    return std::nullopt;
  }

  /// Keep the program: some published state names its cells.
  void retain() {
    settled_ = true;
    endRun();
    zigzag::expectMarkOrder(vm_.core().arena().discard(mark_));
  }

private:
  [[nodiscard]] bool mintedDimensions() const {
    return vm_.core().arena().dimensions().size() != dimensionsBefore_;
  }

  [[nodiscard]] bool wroteOlderCell() const {
    if (!runMark_) return false;
    return vm_.core().arena().wroteOlderCellSince(mark_, runMark_);
  }

  void endRun() {
    if (!runMark_) return;
    zigzag::expectMarkOrder(vm_.core().arena().discard(*runMark_));
    runMark_.reset();
  }

  void release() {
    settled_ = true;
    endRun();
    zigzag::expectMarkOrder(vm_.core().arena().release(mark_));
    vm_.pruneAssemblyState();
  }

  static constexpr std::string_view dimensionReason =
      "the run named a dimension the session keeps";
  static constexpr std::string_view olderCellReason =
      "the run writes cells that existed before it";

  zigzag::vortex::VortexVM &vm_;
  std::optional<std::string> &retainedSink_;
  zigzag::Mark mark_;
  std::optional<zigzag::Mark> runMark_;
  bool createsClosures_{false};
  std::size_t dimensionsBefore_;
  bool settled_{false};
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
  // A parse error runs nothing, so it must not report the last query's reason.
  programRetained_.reset();
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
  programRetained_.reset();
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
    programRetained_.reset();
    ProgramArea area(vm_, programRetained_);
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
    area.setProgram(result.generatedOpcodes);
    area.beginRun();

    std::vector<zigzag::CellRef> cells;
    ContractBindings published;
    std::unordered_map<std::string, std::vector<PathStep>> functors;
    {
      // A frontend request has no resume handle. Keep raw VM stops resumable,
      // but release this request's cursor on success, failure, or exception.
      // It goes before the program's cells do.
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

      for (const auto slot : result.resultSlots) {
        const auto values = vm_.resultCells(slot);
        cells.insert(cells.end(), values.begin(), values.end());
      }
      if (session_) {
        for (const auto &binding : result.sessionBindings) {
          using Kind = CompilationResult::SessionBinding::Kind;
          if (binding.kind == Kind::Mount || binding.kind == Kind::Unmount)
            continue;
          if (binding.kind == Kind::Functor) {
            functors.emplace(binding.name, binding.functor);
            continue;
          }
          auto &values = published[binding.name];
          for (const auto slot : binding.slots) {
            const auto slotValues = vm_.resultCells(slot);
            values.insert(values.end(), slotValues.begin(), slotValues.end());
          }
        }
      }
    }

    // The program's cells go now, and the results it minted are carried out
    // first. A mount's roots are program slots a later query reads, which a
    // value copy cannot stand in for, so a run that publishes one keeps its
    // program.
    const bool mountRoots =
        std::ranges::any_of(result.sessionBindings, [](const auto &binding) {
          return binding.kind ==
                     CompilationResult::SessionBinding::Kind::Mount &&
                 !binding.slots.empty();
        });
    if (session_ && mountRoots) {
      area.retain();
      programRetained_ = "session mount roots are program slots";
    } else {
      std::vector<std::vector<zigzag::CellRef> *> alsoLive;
      for (auto &[name, values] : published) alsoLive.push_back(&values);
      if (const auto refused = area.settle(cells, alsoLive)) {
        programRetained_ = refused->because;
        GLEDITOR_LOG_DEBUG("vql.execution",
                           "program cells retained after the run: {}",
                           *programRetained_);
      }
    }
    if (session_) {
      publishMounts(session_, result.sessionBindings);
      session_->publishCompilerBindings(std::move(published),
                                        std::move(functors));
      session_->publishUserDefs(query.defs);
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
