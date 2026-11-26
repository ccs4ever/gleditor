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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vortex/file_read_host.hpp"
#include "common/xanadu/vortex/frontend_bridge.hpp"
#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vortex/frontend_token.hpp"
#include "common/xanadu/vortex/io_host.hpp"
#include "common/xanadu/vortex/owned_outcome.hpp"
#include "common/xanadu/vortex/payload_resolver.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/ast.hpp"
#include "common/xanadu/vql/compiler.hpp"
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

/// Why a write, append or sync could not be staged: always a diagnostic, and
/// additionally a typed refusal when the host's own bound stopped it, which
/// the script sees as a `Rejected` outcome value instead of a failure.
struct StageFailure {
  vortex::FrontendDiagnostic diagnostic;
  std::optional<vortex::IoErrorInfo> refusal;
};
/// Stage a positional write of frozen bytes; the bytes are the caller's copy,
/// complete before this returns.
using StageWriteFn = std::function<std::expected<zigzag::CellRef, StageFailure>(
    vortex::CapabilityCell, std::uint64_t offset, std::vector<std::byte>)>;
using StageAppendFn =
    std::function<std::expected<zigzag::CellRef, StageFailure>(
        vortex::CapabilityCell, std::vector<std::byte>)>;
using StageSyncFn = std::function<std::expected<zigzag::CellRef, StageFailure>(
    vortex::CapabilityCell)>;

/// Admit an open of a path as a managed submission (`effect { open(...) }`),
/// asking for @p rights (the file rights bits). Optional: a host that cannot
/// open files leaves it empty and the action is refused as
/// `VQL_IO_EFFECT_ARGUMENT`. The host, not the script, decides whether a
/// mutating right is granted.
using OpenFileFn = std::function<
    std::expected<vortex::SubmissionCell, vortex::FrontendDiagnostic>(
        std::string_view path, std::uint64_t rights)>;
/// Admit the close of the endpoint behind a capability the script opened.
using CloseCapabilityFn = std::function<
    std::expected<vortex::SubmissionCell, vortex::FrontendDiagnostic>(
        vortex::CapabilityCell)>;
/// The capability a completed open outcome minted.
using CapabilityOfFn =
    std::function<std::optional<vortex::CapabilityCell>(vortex::OutcomeCell)>;

/// What a script action needs from its embedding program; the runner owns
/// none of it and every reference must outlive the runner.
struct ScriptIoPorts {
  vortex::FrontendBridge &bridge;
  vortex::IoHost &host;
  vortex::PayloadResolver &payloads;
  const vortex::FrontendDiagnosticRegistry &diagnostics;
  StageReadFn stageRead;
  OpenFileFn openFile;
  CloseCapabilityFn closeCapability;
  CapabilityOfFn capabilityOf;
  /// What named outcomes may retain between them, in physical chunk bytes
  /// (`FileReadHostOptions::retainedPayloadBytes`). A copy that would pass it
  /// is not made: that outcome stays the service's, as before the owned
  /// record existed.
  std::size_t retainedPayloadBytes{
      vortex::FileReadHostOptions::defaultBudgetBytes};
  /// The mutating requests. A host that stages none leaves them empty and a
  /// script that submits one stops with `VQL_IO_EFFECT_ARGUMENT`.
  StageWriteFn stageWrite;
  StageAppendFn stageAppend;
  StageSyncFn stageSync;
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

  /// The overall deadline a script gets from `run` on: when it passes while
  /// the script is parked, `expireIfDue` abandons it. Absent: no deadline.
  /// Applies to scripts started after the call.
  auto setScriptTimeout(std::optional<std::chrono::milliseconds> timeout)
      -> ScriptRunner * {
    scriptTimeout_ = timeout;
    return this;
  }
  /// The longest deadline (a `within`, or a script timeout) that can be added
  /// to a time point on the steady clock without overflowing: a quarter of
  /// the tick range leaves headroom for the sum. A representability limit, not
  /// a policy ceiling; a longer script timeout is clamped to it. The script
  /// deadline is read on the I/O service's clock (`IoHost::now`), the one
  /// request deadlines use, so a test injects one clock on the service.
  static constexpr std::chrono::milliseconds maxDeadline =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::duration::max() / 4);
  /// If @p token's script is parked past its overall deadline, abandon it and
  /// say so: the awaited request is asked to stop, the waiter and route are
  /// dropped, the script's requests are released, and the diagnostic is
  /// `VQL_SCRIPT_TIMEOUT`. A script still inside its deadline (or without
  /// one) is left alone, so a driver calls this on every idle turn.
  [[nodiscard]] auto expireIfDue(const vortex::FrontendToken &token)
      -> std::optional<vortex::FrontendDiagnostic>;

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

  /// Script-owned outcome records alive now (named outcomes whose request was
  /// retired at once), and the physical payload bytes their leases pin.
  [[nodiscard]] auto ownedRecordCount() const noexcept -> std::size_t {
    return owned_.size() + finishedRecords_.size();
  }
  [[nodiscard]] auto retainedBytes() const noexcept -> std::size_t {
    return retainedBytes_;
  }

private:
  struct Activation;
  struct Stepping;

  /// The VM's Submit hook: decode the description, stage and admit the read.
  [[nodiscard]] auto admit(vortex::CapabilityCell capability,
                           zigzag::CellRef description)
      -> zigzag::vortex::VmSubmission;
  void installProviders();
  /// The relation providers a script's host-facing opcodes call.
  [[nodiscard]] auto handleArm(const zigzag::vortex::RelationQuery &query,
                               bool cancel) -> zigzag::vortex::RelationResult;
  [[nodiscard]] auto openPath(const zigzag::vortex::RelationQuery &query)
      -> zigzag::vortex::RelationResult;
  [[nodiscard]] auto openCapability(const zigzag::vortex::RelationQuery &query)
      -> zigzag::vortex::RelationResult;
  /// The bytes a write or append sends, frozen now: a text cell's text, or the
  /// payload of an outcome (binary-safe). Anything else, or an outcome that
  /// retained no payload, is `VQL_IO_WRITE_SOURCE`.
  [[nodiscard]] auto sourceBytes(zigzag::CellRef source) const
      -> std::expected<std::vector<std::byte>, vortex::FrontendDiagnostic>;
  /// The payload of a terminal outcome as text, only where the shared status
  /// gate exposes one; nullopt for an outcome that retained none.
  [[nodiscard]] auto payloadText(vortex::OutcomeCell outcome) const
      -> std::expected<std::optional<std::string>, vortex::FrontendDiagnostic>;
  /// An outcome's status gate, reading a script-owned record's payload from
  /// the runner's table (a record has no payload cell) and anything else
  /// through the bridge.
  [[nodiscard]] auto gated(vortex::OutcomeCell outcome) const
      -> std::expected<vortex::GatedOutcome, vortex::FrontendDiagnostic>;
  /// `io_own_outcome`: copy the finished outcome a named read yielded into a
  /// script-owned record and retire its request (§12.1).
  [[nodiscard]] auto ownOutcome(const zigzag::vortex::RelationQuery &query)
      -> zigzag::vortex::RelationResult;
  /// Give back every payload lease @p activation's records hold. Their cells
  /// are freed now (@p freeCells) or, when rows may still name them, at the
  /// next admission.
  void releaseRecords(Activation &activation, bool freeCells) noexcept;
  void freeFinishedRecords() noexcept;
  [[nodiscard]] auto now() const -> std::chrono::steady_clock::time_point {
    return ports_.host.now();
  }
  [[nodiscard]] auto
  settle(const std::shared_ptr<Activation> &activation,
         std::expected<ProgramStep, QueryExecutionError> step)
      -> ScriptRunResult;
  [[nodiscard]] auto finish(const std::shared_ptr<Activation> &activation,
                            const ProgramResult &done) -> ScriptRunResult;
  /// The diagnostic for a read through a name an open bound without minting a
  /// capability: names the open and its outcome tag. Nullopt for any other
  /// failure of statement @p statement.
  [[nodiscard]] auto failedOpen(const Activation &activation,
                                std::optional<std::size_t> statement) const
      -> std::optional<vortex::FrontendDiagnostic>;
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
  /// Retiring an action's request leaves a still-running one in place unless
  /// @p force: the next call finds it terminal. Forced, the sum is released
  /// and the service forgets the request at its end. True when done with it.
  [[nodiscard]] bool retire(const Admitted &entry, bool force = false) noexcept;
  void retireAll(std::vector<Admitted> &entries, bool force = false) noexcept;
  /// Ask every running request of @p entries to stop, for a runner going away.
  void stopRunning(const std::vector<Admitted> &entries) noexcept;
  /// Release every request of @p entries now: a running one is asked to stop
  /// and retired once its terminal lands. For a script the host gave up on.
  void abandonAll(std::vector<Admitted> &entries) noexcept;
  /// Admit the close of every capability @p activation opened; the closes are
  /// reaped as they finish (`reapClosing`). A close the service refuses is
  /// kept in `unclosed_` and retried.
  void closeOpened(Activation &activation) noexcept;
  /// One close admission; false when it was refused and must be retried.
  [[nodiscard]] bool closeOne(vortex::CapabilityCell capability) noexcept;
  /// An activation that failed or was abandoned: close what it opened and
  /// retire what it admitted.
  void endActivation(Activation &activation) noexcept;
  void reapClosing(bool force = false) noexcept;
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
  std::optional<std::chrono::milliseconds> scriptTimeout_;
  /// The closes of capabilities scripts opened, until each is terminal.
  std::vector<Admitted> closing_;
  /// Capabilities whose close was refused (by value) and is tried again at the
  /// next `reapClosing`: an opened file's lifetime is the script's, so a
  /// refusal never ends it.
  struct Unclosed {
    vortex::CapabilityCell capability;
    std::size_t attempts{};
    /// `closesFinished_` when it was last tried: a refusal that is a conflict
    /// with a close in flight can only clear when one finishes, so it is not
    /// tried again before that.
    std::size_t seenFinished{};
  };
  std::vector<Unclosed> unclosed_;
  std::size_t closesFinished_{};
  /// How many times a refused close is tried before the capability is left to
  /// the service: a refusal that is not a conflict (a stopped scheduler) never
  /// clears, and each try mints and retires a refusal cell.
  static constexpr std::size_t maxCloseAttempts = 4;
  /// A script-owned record and what it costs: keyed by the record's cell and
  /// validated against its incarnation, never rebuilt from cells.
  std::unordered_map<zigzag::CellRef, vortex::OwnedOutcome> owned_;
  /// Records of scripts that finished: payload leases already returned, cells
  /// kept until the next admission because the rows a caller holds may name
  /// them.
  std::vector<vortex::OwnedOutcome> finishedRecords_;
  std::size_t retainedBytes_{};
  std::vector<std::string> providers_;
  /// The whole token, so a waiter dropped with its request's cells can still
  /// tell its caller (an Abandoned wake) that the script is dead.
  std::map<vortex::ContinuationId, vortex::FrontendToken> parked_;
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
