/**
 * @file run_vm.cpp
 * @brief Card L1: the run loop. Build a LowerContext, lower, run the block on
 *        the VM, read the result back; and the Driver's evaluation half.
 */
#include "common/xanadu/vlace/run_vm.hpp"
#include "common/xanadu/vlace/kernel.hpp"
#include "common/xanadu/vlace/render_error.hpp"

#include <algorithm>
#include <exception>
#include <functional>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vlace::lower {

namespace {

/// The Compiler-level run limit, kept in the Scalars card slot.
struct RunState {
  std::optional<std::size_t> maxCycles;
};

/// Every run is capped, even when the arena has its own work fuel: the arena
/// is a second governor, not a replacement for the cycle cap.
std::size_t cycleBudget(LowerContext &ctx) {
  const auto limit = ctx.state().cardState<RunState>(Card::Scalars).maxCycles;
  return std::min(limit.value_or(defaultMaxCycles), defaultMaxCycles);
}

/// Releases a run's cursor on every exit, failure included, as the VQL request
/// scope does: its choice points are dropped and its opcode is cleared, so a
/// failed evaluation leaves nothing live for the next one.
class CursorRelease {
public:
  CursorRelease(zigzag::vortex::VortexVM &vm, zigzag::CellRef cursor) noexcept
      : vm_(vm), cursor_(cursor) {}
  CursorRelease(const CursorRelease &)            = delete;
  CursorRelease &operator=(const CursorRelease &) = delete;
  ~CursorRelease() {
    vm_.abortScopedChoices(cursor_);
    vm_.setCursorOpcode(cursor_, std::nullopt);
  }

private:
  zigzag::vortex::VortexVM &vm_;
  zigzag::CellRef cursor_;
};

/// The VM's own safety limits are sized for small programs: its frame depth
/// (128) is below what lazy recursion needs, and it refuses to create more than
/// 65,536 closure values (a lazy program makes two per thunk). Raise them for
/// one run and restore the caller's on every exit, failure included, so a
/// VLace run leaves no trace in a VM shared with other languages. The cycle
/// and frame-depth budgets are what bound a VLace run; the closure ceilings
/// only keep the VM's limits from firing first.
inline constexpr std::size_t closureValueCeiling       = 1U << 24;
inline constexpr std::size_t closureEnvironmentCeiling = 1U << 28;

class RunLimits {
public:
  explicit RunLimits(zigzag::vortex::VortexVM &vm)
      : vm_(vm), frame_(vm.frameLimits()), closure_(vm.closureLimits()) {
    auto frame     = frame_;
    frame.maxDepth = evaluationNestingBudget;
    vm_.setFrameLimits(frame);
    auto closure = closure_;
    closure.maxValuesCreated =
        std::max(closure.maxValuesCreated, closureValueCeiling);
    closure.maxEnvironmentCellsCreated =
        std::max(closure.maxEnvironmentCellsCreated, closureEnvironmentCeiling);
    vm_.setClosureLimits(closure);
  }
  RunLimits(const RunLimits &)            = delete;
  RunLimits &operator=(const RunLimits &) = delete;
  ~RunLimits() {
    vm_.setFrameLimits(frame_);
    vm_.setClosureLimits(closure_);
  }

private:
  zigzag::vortex::VortexVM &vm_;
  zigzag::vortex::FrameLimits frame_;
  zigzag::vortex::LogicalClosureLimits closure_;
};

/// A thrown arena fuel or lowering failure as an EvalError.
EvalError exceptionToEval(const std::exception &error) {
  if (const auto *render = dynamic_cast<const RenderFailure *>(&error))
    return render->error();
  if (auto budget = rewordBudgetFailure(error.what())) return *budget;
  return EvalError{.code   = CompileError::UnexpectedToken,
                   .detail = std::string("vlace: ") + error.what(),
                   .loc    = {}};
}

/// Which stage failed, so the driver can name the phase.
struct StagedError {
  EvalError error;
  bool compilePhase{false};
};

/// Lower with @p lowerBody, run, read back. Lowering failures are the compile
/// phase; VM failures and read-back failures are the eval phase.
std::expected<ValuePtr, StagedError> evaluateStaged(
    Compiler &compiler,
    std::function<Lowered<std::optional<Operand>>(LowerContext &)> lowerBody) {
  const auto compileFailure = [](EvalError e) {
    return std::unexpected(
        StagedError{.error = std::move(e), .compilePhase = true});
  };
  const auto evalFailure = [](EvalError e) {
    return std::unexpected(
        StagedError{.error = std::move(e), .compilePhase = false});
  };
  LowerContext ctx(compiler);
  std::optional<Operand> result;
  try {
    auto lowered = lowerBody(ctx);
    if (!lowered) return compileFailure(std::move(lowered.error()));
    // A program with no result expression evaluates to 0, as it always has.
    result = lowered->value_or(ctx.emitConstant(std::int64_t{0}));
  } catch (const LoweringBudgetExceeded &budget) {
    return compileFailure(budget.error());
  } catch (const std::exception &error) {
    return compileFailure(exceptionToEval(error));
  }
  try {
    auto ran = runBlock(ctx, ctx.swapBlock(Block{}), *result);
    if (!ran) return evalFailure(std::move(ran.error()));
    auto value = readbackCell(compiler, ran->cell);
    if (!value) return evalFailure(std::move(value.error()));
    return *value;
  } catch (const std::exception &error) {
    return evalFailure(exceptionToEval(error));
  }
}

} // namespace

Compiler *setCycleLimit(Compiler &compiler, std::optional<std::size_t> cycles) {
  compiler.lowerState().cardState<RunState>(Card::Scalars).maxCycles = cycles;
  return &compiler;
}

Lowered<Operand> runBlock(LowerContext &ctx, Block block, Operand result) {
  if (!block.entry) return result; // nothing to execute: constants only
  auto &vm = ctx.vm();
  const RunLimits limits(vm);
  beginEvaluation(ctx);
  const auto cursor = vm.spawnCursor(*block.entry, "vlace_eval");
  const CursorRelease release(vm, cursor);
  zigzag::vortex::ExecutionResult run;
  try {
    run = vm.run(cursor, cycleBudget(ctx));
  } catch (const std::exception &error) {
    return std::unexpected(exceptionToEval(error));
  }
  if (!run.success) return std::unexpected(failureToEval(run));
  return result;
}

std::expected<ValuePtr, EvalError> evaluateExpressionOnVm(Compiler &compiler,
                                                          const Expr &expr) {
  auto value = evaluateStaged(
      compiler, [&expr](LowerContext &ctx) -> Lowered<std::optional<Operand>> {
        auto r = lowerExpr(ctx, *expr);
        if (!r) return std::unexpected(r.error());
        return std::optional<Operand>{*r};
      });
  if (!value) return std::unexpected(std::move(value.error().error));
  return *value;
}

std::expected<ValuePtr, EvalError> evaluateProgramOnVm(Compiler &compiler,
                                                       const Program &prog) {
  auto value = evaluateStaged(
      compiler, [&prog](LowerContext &ctx) { return lowerProgram(ctx, prog); });
  if (!value) return std::unexpected(std::move(value.error().error));
  return *value;
}

std::expected<DriverResult, DriverError> driverEvaluate(const Program &prog,
                                                        DriverResult partial) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  Compiler compiler(core, vm);
  const auto driverFailure = [](const EvalError &e, std::string_view phase) {
    return std::unexpected(DriverError{.code   = e.code,
                                       .detail = e.detail,
                                       .loc    = e.loc,
                                       .phase  = std::string(phase)});
  };
  auto value = evaluateStaged(
      compiler, [&prog](LowerContext &ctx) { return lowerProgram(ctx, prog); });
  if (!value)
    return driverFailure(value.error().error,
                         value.error().compilePhase ? "compile" : "eval");
  try {
    partial.value = compiler.render(*value);
  } catch (const std::exception &error) {
    return driverFailure(exceptionToEval(error), "eval");
  }
  return partial;
}

} // namespace xanadu::vlace::lower
