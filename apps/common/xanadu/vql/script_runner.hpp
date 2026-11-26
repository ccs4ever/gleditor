#ifndef XANADU_VQL_SCRIPT_RUNNER_HPP
#define XANADU_VQL_SCRIPT_RUNNER_HPP

/**
 * @file script_runner.hpp
 * @brief Runs a VQL script whose top-level statements include I/O script
 *        actions (`effect { ... }`), parking at an unfinished await and
 *        resuming the same action when the host wakes it (plan §10, M7-VQL).
 *
 * The compiler lowers a script action to Vortex bytecode (MakeTerm, Submit, a
 * branch on the submission's arm, Await). The runner is the host loop around
 * that one compiled program: it runs it through `QueryExecutor::begin`, parks
 * the token when the program stops at an unfinished Await, and `resume` runs
 * the same cursor on from that Await. It answers the program's Submit (the VM
 * hook) by decoding the request description, staging the read and admitting it
 * through the shared `FrontendBridge`, and it reads a finished action's
 * payload, which lives outside the arena. One runner per VM: it owns the VM's
 * submit hook.
 *
 * Multiplicity: a Submit executes once each time the program reaches it.
 * Resuming a parked action continues at its Await and never submits again. No
 * query, row, filter or projection can reach a submit: the parser refuses
 * those with VQL_IO_QUERY_CONTEXT.
 */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
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
  /// runs its queries on, so the core has one write guard. A VM has one
  /// Submit hook, so a second runner on a VM that already has one is refused
  /// with `std::logic_error` rather than silently replacing the first.
  ScriptRunner(VQLEngine &session, zigzag::vortex::VortexVM &vm,
               ScriptIoPorts ports, QueryExecutionOptions options = {});
  /// Cancels every live activation's waiter (never its request) and drops
  /// every route to this runner, so the host holds none to a dead one. Every
  /// terminal request this runner admitted is retired: rows a caller still
  /// holds from a finished action are valid until the next action admitted
  /// by any activation, or this destructor.
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

  /// The managed submission sum the most recently admitted action bound to
  /// its `$submission` variable; it lives until that action is retired.
  [[nodiscard]] auto lastSubmission() const noexcept
      -> std::optional<vortex::SubmissionCell> {
    return latest_;
  }

  void wake(vortex::FrontendToken token, vortex::FrontendWake why) override;

private:
  struct Activation;
  struct Stepping;

  /// The VM's Submit hook: decode the description, stage and admit the read.
  [[nodiscard]] auto admit(vortex::CapabilityCell capability,
                           zigzag::CellRef description)
      -> zigzag::vortex::VmSubmission;
  [[nodiscard]] auto
  settle(const std::shared_ptr<Activation> &activation,
         std::expected<ProgramStep, QueryExecutionError> step)
      -> ScriptRunResult;
  [[nodiscard]] auto finish(const std::shared_ptr<Activation> &activation,
                            const ProgramResult &done) -> ScriptRunResult;
  [[nodiscard]] auto number(zigzag::CellRef cell, const std::string &what,
                            std::optional<std::size_t> position) const
      -> std::expected<std::uint64_t, vortex::FrontendDiagnostic>;
  [[nodiscard]] auto operand(const IoOperand &value, std::size_t position) const
      -> std::expected<std::uint64_t, vortex::FrontendDiagnostic>;
  [[nodiscard]] auto capability(const std::string &name,
                                std::size_t position) const
      -> std::expected<vortex::CapabilityCell, vortex::FrontendDiagnostic>;
  [[nodiscard]] auto diagnose(std::string_view code, std::string message,
                              std::optional<std::size_t> position = {}) const
      -> vortex::FrontendDiagnostic;
  /// What one admitted action holds: its sum and, on the admitted arm, the
  /// request. An activation owns its own list, so several parked scripts
  /// never share or overwrite one another's.
  struct Admitted {
    vortex::SubmissionCell submission;
    std::optional<vortex::RequestCell> request;
  };
  /// Release an action's sum and retire its request when terminal. A request
  /// whose cells the arena rolled back (a failed run, a backtracked Submit) is
  /// orphaned in the service instead: never cancelled, forgotten at its end.
  void retire(const Admitted &entry) noexcept;
  void retireAll(std::vector<Admitted> &entries) noexcept;
  /// Orphan @p request in the service, first dropping the route of any parked
  /// script that still waits on it.
  void orphan(vortex::RequestCell request) noexcept;
  /// The VM's rollback notice: a backtrack is about to undo this Submit.
  void rolledBack(const zigzag::vortex::VmSubmission &abandoned) noexcept;

  VQLEngine &session_;
  zigzag::vortex::VortexVM &vm_;
  ScriptIoPorts ports_;
  QueryExecutionOptions options_;
  std::optional<vortex::SubmissionCell> latest_;
  /// The activation whose program is stepping now (owner thread, so one).
  Activation *current_{nullptr};
  /// Activations that have not ended; the destructor retires their requests.
  std::vector<std::weak_ptr<Activation>> live_;
  /// Finished activations' actions, kept readable until the next admission.
  std::vector<Admitted> finished_;
  /// Why the Submit hook refused to admit, kept for the diagnostic the VM's
  /// own typed failure message cannot carry.
  std::optional<vortex::FrontendDiagnostic> stageFailure_;
  std::deque<ScriptWake> woken_;
  std::map<vortex::ContinuationId,
           std::pair<vortex::WaiterCell, vortex::RequestCell>>
      parked_;
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
