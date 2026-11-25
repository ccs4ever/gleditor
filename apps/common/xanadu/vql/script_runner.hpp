#ifndef XANADU_VQL_SCRIPT_RUNNER_HPP
#define XANADU_VQL_SCRIPT_RUNNER_HPP

/**
 * @file script_runner.hpp
 * @brief Runs a VQL script whose top-level statements include I/O script
 *        actions (`effect { ... }`), parking at an unfinished await and
 *        resuming the same action when the host wakes it (plan §10, M7-VQL).
 *
 * The runner sits above `QueryExecutor`: it cuts the script at each script
 * action, runs each stretch of ordinary statements through the executor
 * exactly as before, and runs the action itself through the shared
 * `FrontendBridge` (submit, match, await). It is host C++ because it parks a
 * native activation across a pump; VQL's own evaluation stays on the Vortex VM
 * and the VM has no Submit opcode, so a script action has no bytecode form.
 *
 * Multiplicity: an action submits once each time the runner reaches it.
 * Resuming a parked action re-awaits the request it already holds and never
 * submits again. No query, row, filter or projection can reach a submit: the
 * parser refuses those with VQL_IO_QUERY_CONTEXT.
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vortex/frontend_bridge.hpp"
#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vortex/frontend_token.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/execution.hpp"

namespace xanadu::vql {

class VQLEngine;

/// The rows a script ends with: those of its final executed statement. A
/// script action's value is its outcome cell, followed by the retained
/// payload text when -- and only when -- the outcome's terminal tag exposes
/// one (Complete, or Partial/Failed/Cancelled/TimedOut that kept bytes).
using ScriptRows = std::vector<zigzag::CellRef>;

/// `Completed{rows} | Parked{FrontendToken} | Failed{FrontendDiagnostic}`.
/// The token's `activation` is the suspended script, its `request` the
/// request the action holds, and its `position` the index of the parked
/// script action among the script's statements.
using ScriptRunResult = vortex::FrontendRunResult<ScriptRows>;

/// Stage a bounded read on a capability and mint the request specification a
/// submit takes. The embedding program supplies it: tests stage against the
/// fake backend, vquery against the file endpoint adapter.
using StageReadFn =
    std::function<std::expected<zigzag::CellRef, vortex::FrontendDiagnostic>(
        vortex::CapabilityCell, std::uint64_t offset, std::uint64_t length)>;

/// What a script action needs from its embedding program; the runner owns
/// none of it and every reference must outlive the runner.
struct ScriptIoPorts {
  vortex::FrontendBridge &bridge;
  vortex::IoHost &host;
  vortex::PayloadResolver &payloads;
  const vortex::FrontendDiagnosticRegistry &diagnostics;
  StageReadFn stageRead;
};

/// The ports over a `FileReadHost`: reads are staged on the file endpoint
/// adapter, and a stage the adapter refuses is `VQL_IO_EFFECT_ARGUMENT`.
[[nodiscard]] auto fileScriptPorts(vortex::FileReadHost &host) -> ScriptIoPorts;

/// A woken token and why it came back.
struct ScriptWake {
  vortex::FrontendToken token;
  vortex::FrontendWake why{};
};

/// Whether @p query holds a script I/O action (and so needs the runner).
[[nodiscard]] bool hasIoEffect(const QueryExpression &query) noexcept;

class ScriptRunner final : public vortex::FrontendRunner {
public:
  /// @p vm must be the VM @p ports' IoHost parks on and the one @p session
  /// runs its queries on, so the core has one write guard.
  ScriptRunner(VQLEngine &session, zigzag::vortex::VortexVM &vm,
               ScriptIoPorts ports, QueryExecutionOptions options = {});
  /// Cancels every live activation's waiter (never its request), so the host
  /// holds no route to a dead runner.
  ~ScriptRunner() override;

  ScriptRunner(const ScriptRunner &)            = delete;
  ScriptRunner &operator=(const ScriptRunner &) = delete;

  /// Start @p query. A script with no I/O action runs to `Completed` at once.
  [[nodiscard]] auto run(QueryExpression query) -> ScriptRunResult;
  /// Continue the activation @p wake names after the host woke it: re-await
  /// the same request, then keep going. Never submits the parked action again.
  [[nodiscard]] auto resume(ScriptWake wake) -> ScriptRunResult;

  /// The next wake `IoHost::pumpIo` delivered, oldest first.
  [[nodiscard]] auto takeWake() -> std::optional<ScriptWake>;
  /// Live activations that are parked on a request.
  [[nodiscard]] auto parkedCount() const noexcept -> std::size_t {
    return parked_.size();
  }

  /// The managed submission sum the most recent action bound to its
  /// `$submission` variable; it lives until the next action or the runner.
  [[nodiscard]] auto lastSubmission() const noexcept
      -> std::optional<vortex::SubmissionCell> {
    return submission_;
  }

  void wake(vortex::FrontendToken token, vortex::FrontendWake why) override;

private:
  struct Activation;

  [[nodiscard]] auto advance(const std::shared_ptr<Activation> &activation)
      -> ScriptRunResult;
  [[nodiscard]] auto runAction(const std::shared_ptr<Activation> &activation,
                               const IoEffectStatement &action)
      -> ScriptRunResult;
  [[nodiscard]] auto finishAction(const std::shared_ptr<Activation> &activation,
                                  vortex::OutcomeCell outcome)
      -> ScriptRunResult;
  [[nodiscard]] auto
  settle(const std::shared_ptr<Activation> &activation,
         vortex::FrontendRunResult<vortex::OutcomeCell> awaited)
      -> ScriptRunResult;
  [[nodiscard]] auto operand(const IoOperand &value, std::size_t position) const
      -> std::expected<std::uint64_t, vortex::FrontendDiagnostic>;
  [[nodiscard]] auto capability(const std::string &name,
                                std::size_t position) const
      -> std::expected<vortex::CapabilityCell, vortex::FrontendDiagnostic>;
  [[nodiscard]] auto diagnose(std::string_view code, std::string message,
                              std::optional<std::size_t> position = {}) const
      -> vortex::FrontendDiagnostic;
  void retire(const std::shared_ptr<Activation> &activation) noexcept;

  VQLEngine &session_;
  zigzag::vortex::VortexVM &vm_;
  ScriptIoPorts ports_;
  QueryExecutionOptions options_;
  std::optional<vortex::SubmissionCell> submission_;
  std::deque<ScriptWake> woken_;
  std::map<vortex::ContinuationId, vortex::WaiterCell> parked_;
};

/// Drive @p first to its end the way a CLI does, through the host and never
/// around it: while the script is parked, @p pumpOnce publishes completions
/// and delivers wakes (it must not block), and @p idle runs when that
/// produced no wake for this script, so the caller decides how to wait. There
/// is no blocking read and no synchronous fallback anywhere on this path.
[[nodiscard]] auto
driveToCompletion(ScriptRunner &runner, ScriptRunResult first,
                  gleditor::cpp26::function_ref<void()> pumpOnce,
                  gleditor::cpp26::function_ref<void()> idle)
    -> std::expected<ScriptRows, vortex::FrontendDiagnostic>;

} // namespace xanadu::vql

#endif
