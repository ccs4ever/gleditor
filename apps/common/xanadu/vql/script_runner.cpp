#include "common/xanadu/vql/script_runner.hpp"

#include <algorithm>
#include <cmath>
#include <gleditor/logging.hpp>
#include <limits>
#include <string>

#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {

namespace {

using vortex::FrontendDiagnostic;
using vortex::FrontendToken;
using vortex::FrontendWake;
using zigzag::CellRef;

constexpr const char *logCategory = "vql.script.io";

/// The row range a statement list [from, to) of @p query makes: a query of
/// its own, so the executor sees only ordinary statements. Defs ride along
/// into every stretch; mounts were declared once before the first.
auto stretch(const QueryExpression &query, const std::size_t from,
             const std::size_t to) -> QueryExpression {
  QueryExpression part;
  part.defs = query.defs;
  part.body.statements.assign(
      query.body.statements.begin() + static_cast<std::ptrdiff_t>(from),
      query.body.statements.begin() + static_cast<std::ptrdiff_t>(to));
  const auto &first = part.body.statements.front();
  if (const auto *block = std::get_if<ExecutionBlock>(&first)) {
    part.expr = *block;
  } else if (const auto *path = std::get_if<PathExpression>(&first)) {
    part.expr = *path;
  }
  return part;
}

auto isAction(const Statement &statement) -> bool {
  return std::holds_alternative<IoEffectStatement>(statement);
}

} // namespace

bool hasIoEffect(const QueryExpression &query) noexcept {
  return std::ranges::any_of(query.body.statements, isAction);
}

struct ScriptRunner::Activation final : vortex::FrontendActivation {
  QueryExpression query;
  /// The statement to run next; while parked, the parked action's index.
  std::size_t next{0};
  ScriptRows rows;
  std::optional<CellRef> cursor;
};

ScriptRunner::ScriptRunner(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                           ScriptIoPorts ports, QueryExecutionOptions options)
    : session_(session), vm_(vm), ports_(std::move(ports)), options_(options) {}

ScriptRunner::~ScriptRunner() {
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

void ScriptRunner::retire(
    const std::shared_ptr<Activation> &activation) noexcept {
  if (activation->cursor) {
    vm_.setCursorOpcode(*activation->cursor, std::nullopt);
    activation->cursor.reset();
  }
}

auto ScriptRunner::run(QueryExpression query) -> ScriptRunResult {
  auto activation   = std::make_shared<Activation>();
  activation->query = std::move(query);
  const auto &q     = activation->query;
  if (!q.mounts.empty() || !q.namespaceDeclarations.empty()) {
    if (!hasIoEffect(q)) {
      // Nothing to cut: the whole script, mounts and all, is one stretch.
      QueryExecutor executor(session_, vm_, options_);
      auto rows = executor.execute(q);
      if (!rows) {
        return ScriptRunResult::failed(diagnose(
            vortex::frontend_codes::vqlScriptFailed, rows.error().message));
      }
      return ScriptRunResult::completed(*std::move(rows));
    }
    QueryExpression declarations;
    declarations.mounts                = q.mounts;
    declarations.namespaceDeclarations = q.namespaceDeclarations;
    QueryExecutor executor(session_, vm_, options_);
    if (auto declared = executor.execute(declarations); !declared) {
      return ScriptRunResult::failed(diagnose(
          vortex::frontend_codes::vqlScriptFailed, declared.error().message));
    }
  }
  return advance(activation);
}

auto ScriptRunner::advance(const std::shared_ptr<Activation> &activation)
    -> ScriptRunResult {
  const auto &statements = activation->query.body.statements;
  while (activation->next < statements.size()) {
    const auto index = activation->next;
    if (const auto *action =
            std::get_if<IoEffectStatement>(&statements[index])) {
      auto result = runAction(activation, *action);
      if (!std::holds_alternative<ScriptRunResult::Completed>(result.value)) {
        if (std::holds_alternative<ScriptRunResult::Failed>(result.value)) {
          retire(activation);
        }
        return result;
      }
      continue;
    }
    auto end = index;
    while (end < statements.size() && !isAction(statements[end])) ++end;
    QueryExecutor executor(session_, vm_, options_);
    auto rows = executor.execute(stretch(activation->query, index, end));
    if (!rows) {
      retire(activation);
      return ScriptRunResult::failed(
          diagnose(vortex::frontend_codes::vqlScriptFailed,
                   rows.error().message, index));
    }
    activation->rows = *std::move(rows);
    activation->next = end;
  }
  retire(activation);
  return ScriptRunResult::completed(std::move(activation->rows));
}

auto ScriptRunner::operand(const IoOperand &value,
                           const std::size_t position) const
    -> std::expected<std::uint64_t, FrontendDiagnostic> {
  const auto refuse = [&](std::string message) {
    return std::unexpected{diagnose(vortex::frontend_codes::vqlEffectArgument,
                                    std::move(message), position)};
  };
  std::int64_t number = 0;
  if (const auto *literal = std::get_if<std::int64_t>(&value)) {
    number = *literal;
  } else {
    const auto &name     = std::get<std::string>(value);
    const auto &bindings = session_.variables();
    const auto found     = bindings.find(name);
    if (found == bindings.end() || found->second.size() != 1) {
      return refuse("$" + name + " is not bound to exactly one numeric cell");
    }
    const auto rendered =
        session_.coordinator().core().render(found->second[0]);
    if (!rendered) return refuse("$" + name + " has no value");
    if (const auto *integer = std::get_if<std::int64_t>(&*rendered)) {
      number = *integer;
    } else if (const auto *real = std::get_if<double>(&*rendered);
               real != nullptr && std::trunc(*real) == *real &&
               std::abs(*real) <
                   static_cast<double>(
                       std::numeric_limits<std::int64_t>::max())) {
      number = static_cast<std::int64_t>(*real);
    } else {
      return refuse("$" + name + " is not an integer");
    }
  }
  if (number < 0) return refuse("an offset and a bound are not negative");
  return static_cast<std::uint64_t>(number);
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

auto ScriptRunner::runAction(const std::shared_ptr<Activation> &activation,
                             const IoEffectStatement &action)
    -> ScriptRunResult {
  const auto position = activation->next;
  // Everything that can be wrong with the action's own values is wrong before
  // anything is staged or submitted.
  const auto cap = capability(action.capabilityVar, position);
  if (!cap) return ScriptRunResult::failed(cap.error());
  const auto offset = operand(action.offset, position);
  if (!offset) return ScriptRunResult::failed(offset.error());
  const auto length = operand(action.maxBytes, position);
  if (!length) return ScriptRunResult::failed(length.error());

  const auto spec = ports_.stageRead(*cap, *offset, *length);
  if (!spec) {
    auto diagnostic     = spec.error();
    diagnostic.position = position;
    return ScriptRunResult::failed(std::move(diagnostic));
  }
  const auto submission = ports_.bridge.submit(*cap, *spec);
  if (!submission) {
    auto diagnostic     = submission.error();
    diagnostic.position = position;
    return ScriptRunResult::failed(std::move(diagnostic));
  }
  // `$submission` stays bound, as a language variable does, until the next
  // action rebinds it; the request or outcome it names is untouched by that.
  // The previous action's outcome is no longer what `$submission` names, and
  // its rows were returned by the run that finished it, so its request is
  // retired here: a long session holds one action's request, not all of them.
  if (submission_) retireSubmission(*submission_);
  submission_ = *submission;

  // The language's own match: a refusal is bound and kept as the script's
  // value, and is never awaited.
  const auto matched = ports_.bridge.match(*submission);
  if (!matched) return ScriptRunResult::failed(matched.error());
  if (const auto *refused = std::get_if<vortex::Refused>(&*matched)) {
    return finishAction(activation, refused->outcome);
  }
  if (!activation->cursor) {
    activation->cursor =
        vm_.spawnCursor(vm_.mintOpcode(zigzag::vortex::OpcodeKind::Nop));
  }
  return settle(activation,
                ports_.bridge.awaitSubmission(*submission, *activation->cursor,
                                              *this, activation, position));
}

auto ScriptRunner::settle(
    const std::shared_ptr<Activation> &activation,
    vortex::FrontendRunResult<vortex::OutcomeCell> awaited) -> ScriptRunResult {
  using Awaited = vortex::FrontendRunResult<vortex::OutcomeCell>;
  if (auto *done = std::get_if<Awaited::Completed>(&awaited.value)) {
    return finishAction(activation, done->value);
  }
  if (auto *parked = std::get_if<Awaited::Parked>(&awaited.value)) {
    parked_.insert_or_assign(
        parked->token.continuation,
        std::pair{parked->token.waiter, parked->token.request});
    return ScriptRunResult::parked(std::move(parked->token));
  }
  auto diagnostic = std::get<Awaited::Failed>(awaited.value).diagnostic;
  retire(activation);
  return ScriptRunResult::failed(std::move(diagnostic));
}

auto ScriptRunner::finishAction(const std::shared_ptr<Activation> &activation,
                                const vortex::OutcomeCell outcome)
    -> ScriptRunResult {
  const auto gated = ports_.bridge.gate(outcome);
  if (!gated) return ScriptRunResult::failed(gated.error());
  ScriptRows rows{outcome.cell};
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
  activation->rows = std::move(rows);
  ++activation->next;
  return ScriptRunResult::completed(activation->rows);
}

auto ScriptRunner::resume(ScriptWake wake) -> ScriptRunResult {
  auto activation =
      std::dynamic_pointer_cast<Activation>(wake.token.activation);
  if (!activation || !activation->cursor) {
    return ScriptRunResult::failed(diagnose(
        vortex::frontend_codes::tokenStale,
        "the token names no live script activation", wake.token.position));
  }
  if (wake.why == FrontendWake::Abandoned) {
    retire(activation);
    return ScriptRunResult::failed(diagnose(
        vortex::frontend_codes::tokenStale,
        "the parked request or cursor went stale", wake.token.position));
  }
  auto settled = settle(
      activation, ports_.bridge.resume(wake.token, *activation->cursor, *this));
  if (!std::holds_alternative<ScriptRunResult::Completed>(settled.value)) {
    return settled;
  }
  return advance(activation);
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
