#include "common/xanadu/vql/script_runner.hpp"

#include <algorithm>
#include <cmath>
#include <gleditor/logging.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

namespace xanadu::vql {

namespace {

using vortex::FrontendDiagnostic;
using vortex::FrontendToken;
using vortex::FrontendWake;
using zigzag::CellRef;

constexpr const char *logCategory = "vql.script.io";

/// The functor the compiler gives a bounded read's request description.
constexpr const char *readBytesFunctor = "read_bytes";

auto isAction(const Statement &statement) -> bool {
  return std::holds_alternative<IoEffectStatement>(statement);
}

} // namespace

bool hasIoEffect(const QueryExpression &query) noexcept {
  return std::ranges::any_of(query.body.statements, isAction);
}

/// A script between its first step and its end. The executor is declared
/// before the parked program so the program, whose undo actions point at the
/// executor, is destroyed first.
struct ScriptRunner::Activation final : vortex::FrontendActivation {
  QueryExpression query;
  std::unique_ptr<QueryExecutor> executor;
  std::optional<ParkedProgram> parked;
};

ScriptRunner::ScriptRunner(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                           ScriptIoPorts ports, QueryExecutionOptions options)
    : session_(session), vm_(vm), ports_(std::move(ports)), options_(options) {
  // The one admission hook of the VM this runner drives: a compiled action's
  // Submit lands here, once per executed instruction.
  vm_.setSubmitHandler(
      [this](const vortex::CapabilityCell capability, const CellRef description,
             const CellRef) { return admit(capability, description); });
}

ScriptRunner::~ScriptRunner() {
  vm_.setSubmitHandler({});
  for (const auto &[continuation, held] : parked_) {
    static_cast<void>(continuation);
    static_cast<void>(ports_.host.cancelWaiter(held.first));
  }
  parked_.clear();
  // A waiter already completed (AlreadyComplete) keeps its route until its
  // wake; nothing may call this runner after it is gone.
  static_cast<void>(ports_.host.unrouteRunner(*this));
  // Nothing reads the last action's outcome once the runner is gone, so its
  // request leaves the service's index and the arena with it.
  if (submission_) retireSubmission(*submission_);
}

auto ScriptRunner::diagnose(const std::string_view code, std::string message,
                            const std::optional<std::size_t> position) const
    -> FrontendDiagnostic {
  if (auto diagnostic = ports_.diagnostics.diagnose(code, message, position)) {
    return *std::move(diagnostic);
  }
  GLEDITOR_LOG_WARN(logCategory, "diagnostic {} is unregistered", code);
  return FrontendDiagnostic{std::string{code}, std::move(message), position};
}

void ScriptRunner::wake(FrontendToken token, const FrontendWake why) {
  parked_.erase(token.continuation);
  woken_.push_back(ScriptWake{std::move(token), why});
}

auto ScriptRunner::takeWake() -> std::optional<ScriptWake> {
  if (woken_.empty()) return std::nullopt;
  auto next = std::move(woken_.front());
  woken_.pop_front();
  return next;
}

void ScriptRunner::retireSubmission(
    const vortex::SubmissionCell submission) noexcept {
  const auto matched = ports_.bridge.match(submission);
  const auto *admitted =
      matched ? std::get_if<vortex::Admitted>(&*matched) : nullptr;
  const bool awaited = admitted != nullptr &&
                       std::ranges::any_of(parked_, [&](const auto &entry) {
                         return entry.second.second == admitted->request;
                       });
  if (awaited) {
    ports_.bridge.release(submission);
  } else {
    ports_.bridge.retire(submission);
  }
}

auto ScriptRunner::run(QueryExpression query) -> ScriptRunResult {
  // Everything that can be wrong with an action's own values is wrong before
  // anything is compiled, staged or submitted.
  const auto &statements = query.body.statements;
  for (std::size_t index = 0; index < statements.size(); ++index) {
    const auto *action = std::get_if<IoEffectStatement>(&statements[index]);
    if (action == nullptr) continue;
    if (const auto cap = capability(action->capabilityVar, index); !cap)
      return ScriptRunResult::failed(cap.error());
    if (const auto offset = operand(action->offset, index); !offset)
      return ScriptRunResult::failed(offset.error());
    if (const auto length = operand(action->maxBytes, index); !length)
      return ScriptRunResult::failed(length.error());
  }
  auto activation   = std::make_shared<Activation>();
  activation->query = std::move(query);
  activation->executor =
      std::make_unique<QueryExecutor>(session_, vm_, options_);
  return settle(activation, activation->executor->begin(activation->query));
}

auto ScriptRunner::number(const CellRef cell, const std::string &what,
                          const std::optional<std::size_t> position) const
    -> std::expected<std::uint64_t, FrontendDiagnostic> {
  const auto refuse = [&](std::string message) {
    return std::unexpected{diagnose(vortex::frontend_codes::vqlEffectArgument,
                                    std::move(message), position)};
  };
  const auto rendered = session_.coordinator().core().render(cell);
  if (!rendered) return refuse(what + " has no value");
  std::int64_t value = 0;
  if (const auto *integer = std::get_if<std::int64_t>(&*rendered)) {
    value = *integer;
  } else if (const auto *real = std::get_if<double>(&*rendered);
             real != nullptr && std::trunc(*real) == *real &&
             std::abs(*real) < static_cast<double>(
                                   std::numeric_limits<std::int64_t>::max())) {
    value = static_cast<std::int64_t>(*real);
  } else {
    return refuse(what + " is not an integer");
  }
  if (value < 0) return refuse("an offset and a bound are not negative");
  return static_cast<std::uint64_t>(value);
}

auto ScriptRunner::operand(const IoOperand &value,
                           const std::size_t position) const
    -> std::expected<std::uint64_t, FrontendDiagnostic> {
  if (const auto *literal = std::get_if<std::int64_t>(&value)) {
    if (*literal < 0)
      return std::unexpected{diagnose(vortex::frontend_codes::vqlEffectArgument,
                                      "an offset and a bound are not negative",
                                      position)};
    return static_cast<std::uint64_t>(*literal);
  }
  const auto &name     = std::get<std::string>(value);
  const auto &bindings = session_.variables();
  const auto found     = bindings.find(name);
  if (found == bindings.end() || found->second.size() != 1) {
    return std::unexpected{diagnose(
        vortex::frontend_codes::vqlEffectArgument,
        "$" + name + " is not bound to exactly one numeric cell", position)};
  }
  return number(found->second[0], "$" + name, position);
}

auto ScriptRunner::capability(const std::string &name,
                              const std::size_t position) const
    -> std::expected<vortex::CapabilityCell, FrontendDiagnostic> {
  const auto refuse = [&](std::string message) {
    return std::unexpected{diagnose(vortex::frontend_codes::vqlEffectArgument,
                                    std::move(message), position)};
  };
  const auto &bindings = session_.variables();
  const auto found     = bindings.find(name);
  if (found == bindings.end() || found->second.size() != 1) {
    return refuse("$" + name + " is not bound to exactly one capability cell");
  }
  const auto &arena      = session_.coordinator().arena();
  const auto cell        = found->second[0];
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation) return refuse("$" + name + " is not a live cell");
  const auto tag = vortex::ioCellTag(arena, cell, *incarnation);
  if (!tag || *tag != vortex::IoCellTag::Capability) {
    return refuse("$" + name + " is not a capability cell");
  }
  return vortex::CapabilityCell{cell, *incarnation};
}

auto ScriptRunner::admit(const vortex::CapabilityCell capability,
                         const CellRef description)
    -> zigzag::vortex::VmSubmission {
  // The description is the term the compiled program built; decoding it is the
  // only place that knows the request is a bounded read.
  auto &arena      = session_.coordinator().core().arena();
  const auto &dims = session_.coordinator().core().dims();
  const zigzag::Vlog vlog{.m     = arena,
                          .clone = dims.clone,
                          .grab  = dims.grab,
                          .step  = dims.step,
                          .vars  = dims.vars};
  const auto fail = [this](FrontendDiagnostic diagnostic) {
    stageFailure_ = diagnostic;
    throw std::runtime_error(diagnostic.message);
  };
  const auto arguments = vlog.argumentsOf(description);
  if (arena.textOf(description) != readBytesFunctor || arguments.size() != 2)
    fail(diagnose(vortex::frontend_codes::vqlEffectArgument,
                  "the request description is not read_bytes(offset, max)"));
  const auto offset = number(arguments[0], "the offset", std::nullopt);
  if (!offset) fail(offset.error());
  const auto length = number(arguments[1], "the bound", std::nullopt);
  if (!length) fail(length.error());
  const auto spec = ports_.stageRead(capability, *offset, *length);
  if (!spec) fail(spec.error());
  const auto submission = ports_.bridge.submit(capability, *spec);
  if (!submission) fail(submission.error());
  const auto matched = ports_.bridge.match(*submission);
  if (!matched) fail(matched.error());
  // The previous action's outcome is no longer what a script can name, and its
  // rows were returned by the run that finished it, so its request is retired
  // here: a long session holds one action's request, not all of them.
  if (submission_) retireSubmission(*submission_);
  submission_ = *submission;
  if (const auto *refused = std::get_if<vortex::Refused>(&*matched))
    return {*submission, 1, refused->outcome.cell};
  return {*submission, 0, std::get<vortex::Admitted>(*matched).request.cell};
}

auto ScriptRunner::settle(const std::shared_ptr<Activation> &activation,
                          std::expected<ProgramStep, QueryExecutionError> step)
    -> ScriptRunResult {
  if (!step) {
    auto diagnostic =
        stageFailure_ ? std::exchange(stageFailure_, std::nullopt).value()
                      : diagnose(vortex::frontend_codes::vqlScriptFailed,
                                 step.error().message, step.error().statement);
    if (!diagnostic.position) diagnostic.position = step.error().statement;
    return ScriptRunResult::failed(std::move(diagnostic));
  }
  if (auto *done = std::get_if<ProgramResult>(&*step)) {
    return finish(*done);
  }
  auto &parked = std::get<ParkedProgram>(*step);
  const auto matched =
      submission_
          ? ports_.bridge.match(*submission_)
          : std::expected<vortex::IoSubmissionValue, FrontendDiagnostic>{
                std::unexpected{
                    diagnose(vortex::frontend_codes::tokenStale,
                             "a parked script holds no submission")}};
  const auto *admitted =
      matched ? std::get_if<vortex::Admitted>(&*matched) : nullptr;
  if (admitted == nullptr) {
    return ScriptRunResult::failed(
        diagnose(vortex::frontend_codes::tokenStale,
                 "a parked script's request is not an admitted submission",
                 parked.statement));
  }
  FrontendToken token{.activation   = activation,
                      .request      = admitted->request,
                      .waiter       = parked.pending.waiter,
                      .continuation = parked.pending.continuation,
                      .position     = parked.statement};
  if (const auto routed = ports_.host.routeWake(token, *this); !routed) {
    return ScriptRunResult::failed(
        diagnose(vortex::frontend_codes::tokenStale,
                 "the parked await could not be routed to the script runner",
                 parked.statement));
  }
  parked_.insert_or_assign(token.continuation,
                           std::pair{token.waiter, token.request});
  activation->parked = std::move(parked);
  return ScriptRunResult::parked(std::move(token));
}

auto ScriptRunner::finish(const ProgramResult &done) -> ScriptRunResult {
  ScriptRows rows = done.rows;
  if (done.finalIsIoEffect && !rows.empty()) {
    // The compiled action yields its outcome cell; the retained bytes, when
    // the terminal tag exposes any, follow as one text cell. Reading them is
    // the host's, since a payload lives outside the arena.
    const auto outcome = rows.front();
    const auto incarnation =
        session_.coordinator().core().arena().incarnationOf(outcome);
    if (!incarnation) {
      return ScriptRunResult::failed(
          diagnose(vortex::frontend_codes::vqlScriptFailed,
                   "the action's outcome is not a live cell"));
    }
    const vortex::OutcomeCell handle{outcome, *incarnation};
    const auto gated = ports_.bridge.gate(handle);
    if (!gated) return ScriptRunResult::failed(gated.error());
    if (gated->payload) {
      auto lease = ports_.payloads.retain(*gated->payload);
      if (lease) {
        const auto bytes = ports_.payloads.read(
            *lease, 0, static_cast<std::size_t>(lease->range.length));
        ports_.payloads.release(lease->lease);
        if (bytes) {
          std::string text(bytes->size(), '\0');
          std::ranges::transform(*bytes, text.begin(), [](const std::byte b) {
            return static_cast<char>(b);
          });
          rows.push_back(session_.coordinator().arena().makeCell(text));
        }
      }
    }
  }
  return ScriptRunResult::completed(std::move(rows));
}

auto ScriptRunner::resume(ScriptWake wake) -> ScriptRunResult {
  auto activation =
      std::dynamic_pointer_cast<Activation>(wake.token.activation);
  if (!activation || !activation->parked) {
    return ScriptRunResult::failed(diagnose(
        vortex::frontend_codes::tokenStale,
        "the token names no live script activation", wake.token.position));
  }
  if (wake.why == FrontendWake::Abandoned) {
    activation->parked.reset();
    return ScriptRunResult::failed(diagnose(
        vortex::frontend_codes::tokenStale,
        "the parked request or cursor went stale", wake.token.position));
  }
  // The same cursor continues at the Await it parked on: the program never
  // runs the Submit before it a second time.
  auto parked = std::move(*activation->parked);
  activation->parked.reset();
  return settle(activation, activation->executor->resume(parked));
}

auto fileScriptPorts(vortex::FileReadHost &host) -> ScriptIoPorts {
  return ScriptIoPorts{
      host.bridge(), host.host(), host.payloads(), host.diagnostics(),
      [&host](const vortex::CapabilityCell capability,
              const std::uint64_t offset, const std::uint64_t length)
          -> std::expected<CellRef, FrontendDiagnostic> {
        const auto staged = host.stageRead(capability, offset, length);
        if (staged) return *staged;
        return std::unexpected{FrontendDiagnostic{
            std::string{vortex::frontend_codes::vqlEffectArgument},
            "the read was not staged: " +
                std::string{vortex::toString(staged.error())},
            {}}};
      }};
}

auto driveToCompletion(ScriptRunner &runner, ScriptRunResult first,
                       gleditor::cpp26::function_ref<void()> pumpOnce,
                       gleditor::cpp26::function_ref<void()> idle)
    -> std::expected<ScriptRows, FrontendDiagnostic> {
  auto result = std::move(first);
  while (true) {
    if (auto *done = std::get_if<ScriptRunResult::Completed>(&result.value)) {
      return std::move(done->value);
    }
    if (auto *failed = std::get_if<ScriptRunResult::Failed>(&result.value)) {
      return std::unexpected{std::move(failed->diagnostic)};
    }
    auto woken = runner.takeWake();
    while (!woken) {
      pumpOnce();
      woken = runner.takeWake();
      if (!woken) idle();
    }
    result = runner.resume(*std::move(woken));
  }
}

} // namespace xanadu::vql
