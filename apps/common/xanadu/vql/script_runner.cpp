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

/// Whether @p text is well-formed UTF-8 (no overlongs, surrogates or values
/// past U+10FFFF).
auto validUtf8(const std::string_view text) -> bool {
  std::size_t at = 0;
  while (at < text.size()) {
    const auto lead     = static_cast<unsigned char>(text[at]);
    std::size_t extra   = 0;
    std::uint32_t point = 0;
    std::uint32_t floor = 0;
    if (lead < 0x80) {
      ++at;
      continue;
    }
    if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      point = lead & 0x1F;
      floor = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      point = lead & 0x0F;
      floor = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      point = lead & 0x07;
      floor = 0x10000;
    } else {
      return false;
    }
    if (at + extra >= text.size()) return false;
    for (std::size_t i = 1; i <= extra; ++i) {
      const auto next = static_cast<unsigned char>(text[at + i]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if (point < floor || point > 0x10FFFF ||
        (point >= 0xD800 && point <= 0xDFFF))
      return false;
    at += extra + 1;
  }
  return true;
}

auto isAction(const Statement &statement) -> bool {
  return std::holds_alternative<IoEffectStatement>(statement);
}

} // namespace

/// Marks the activation whose program the VM is stepping, for the hooks.
struct ScriptRunner::Stepping {
  Stepping(ScriptRunner &runner, Activation &activation)
      : runner_(runner), previous_(runner.current_) {
    runner.current_ = &activation;
  }
  ~Stepping() { runner_.current_ = previous_; }
  Stepping(const Stepping &)            = delete;
  Stepping &operator=(const Stepping &) = delete;

private:
  ScriptRunner &runner_;
  Activation *previous_;
};

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
  /// This script's admitted actions, oldest first.
  std::vector<Admitted> admitted;
};

ScriptRunner::ScriptRunner(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                           ScriptIoPorts ports, QueryExecutionOptions options)
    : session_(session), vm_(vm), ports_(std::move(ports)), options_(options) {
  if (vm_.hasSubmitHandler()) {
    throw std::logic_error(
        "a VortexVM has one Submit hook and another runner holds it");
  }
  // The one admission hook of the VM this runner drives: a compiled action's
  // Submit lands here, once per executed instruction.
  vm_.setSubmitHandler(
      [this](const vortex::CapabilityCell capability, const CellRef description,
             const CellRef) { return admit(capability, description); });
  vm_.setSubmitRollbackHandler(
      [this](const zigzag::vortex::VmSubmission &abandoned) {
        rolledBack(abandoned);
      });
  vm_.setRelationProvider(
      outcomeTagRelation,
      [](const zigzag::vortex::RelationQuery &query)
          -> zigzag::vortex::RelationResult {
        if (query.arguments.size() != 1)
          return std::unexpected("io_tag expects one outcome argument");
        if (query.position != 0)
          return std::optional<zigzag::vortex::RelationAnswer>{std::nullopt};
        const auto incarnation = query.arena.incarnationOf(query.arguments[0]);
        if (!incarnation)
          return std::unexpected("io_tag: the argument is not a live cell");
        const auto tag = vortex::ioOutcomeTag(
            query.arena, vortex::OutcomeCell{query.arguments[0], *incarnation});
        if (!tag)
          return std::unexpected("io_tag: the argument is not an outcome");
        return std::optional<zigzag::vortex::RelationAnswer>{
            zigzag::vortex::RelationAnswer{
                .nextPosition = 1,
                .limit        = 1,
                .answers      = {query.arena.makeCell(
                    std::string{vortex::toString(*tag)})}}};
      });
}

ScriptRunner::~ScriptRunner() {
  vm_.setRelationProvider(outcomeTagRelation, nullptr);
  vm_.setSubmitHandler({});
  vm_.setSubmitRollbackHandler({});
  for (const auto &[continuation, held] : parked_) {
    static_cast<void>(continuation);
    static_cast<void>(ports_.host.cancelWaiter(held.waiter));
  }
  parked_.clear();
  // A waiter already completed (AlreadyComplete) keeps its route until its
  // wake; nothing may call this runner after it is gone.
  static_cast<void>(ports_.host.unrouteRunner(*this));
  // Nothing reads an outcome once the runner is gone, so each terminal
  // request leaves the service's index and the arena with it; a running one
  // is never cancelled.
  retireAll(finished_);
  for (const auto &weak : live_) {
    if (const auto activation = weak.lock()) retireAll(activation->admitted);
  }
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

void ScriptRunner::orphan(const vortex::RequestCell request) noexcept {
  auto result = ports_.host.orphanRequest(request);
  if (result == vortex::IoService::OrphanResult::HasWaiters) {
    // A parked script is waiting on the request whose cells are going; its
    // waiter goes with them, so the route is dropped first and the orphaning
    // retried.
    for (auto it = parked_.begin(); it != parked_.end();) {
      if (it->second.request == request) {
        static_cast<void>(ports_.host.cancelWaiter(it->second.waiter));
        // Its waiter will never fire; the caller waiting on takeWake() learns
        // the script is dead from resume()'s stale-token diagnostic.
        woken_.push_back(
            ScriptWake{std::move(it->second), vortex::FrontendWake::Abandoned});
        it = parked_.erase(it);
      } else {
        ++it;
      }
    }
    result = ports_.host.orphanRequest(request);
  }
  if (result == vortex::IoService::OrphanResult::HasWaiters) {
    GLEDITOR_LOG_WARN(logCategory,
                      "an abandoned request still has a waiter and stays in "
                      "the service budget until the waiter drops");
  }
}

void ScriptRunner::retire(const Admitted &entry) noexcept {
  const auto &arena = session_.coordinator().arena();
  if (entry.request &&
      !arena.matches(entry.request->cell, entry.request->incarnation)) {
    // The cells were rolled back under a request that may still be running.
    orphan(*entry.request);
  } else {
    ports_.bridge.retire(entry.submission);
  }
  if (latest_ && *latest_ == entry.submission) latest_.reset();
}

void ScriptRunner::retireAll(std::vector<Admitted> &entries) noexcept {
  for (const auto &entry : entries) retire(entry);
  entries.clear();
}

void ScriptRunner::rolledBack(
    const zigzag::vortex::VmSubmission &abandoned) noexcept {
  // The choice point that is being undone may belong to a parked script other
  // than the one stepping (interleaved scripts share one VM), and the notice
  // can arrive with no stepping script at all (a close-time scoped abort), so
  // every live activation is searched, the stepping one first.
  const auto holds = [&](Activation &activation) -> std::vector<Admitted> * {
    auto &entries = activation.admitted;
    const auto at = std::ranges::find_if(entries, [&](const Admitted &entry) {
      return entry.submission == abandoned.submission;
    });
    return at == entries.end() ? nullptr : &entries;
  };
  std::vector<Admitted> *owner = nullptr;
  if (current_ != nullptr) owner = holds(*current_);
  for (auto it = live_.begin(); owner == nullptr && it != live_.end(); ++it) {
    if (const auto activation = it->lock()) owner = holds(*activation);
  }
  if (owner == nullptr) {
    // Not an error: a failure path retires every admitted entry of its
    // activation (retireAll orphans a rolled-back request) before or after
    // this notice, whichever finds the entry first.
    GLEDITOR_LOG_DEBUG(logCategory,
                       "rollback notice names no live admitted submission");
    return;
  }
  const auto at = std::ranges::find_if(*owner, [&](const Admitted &entry) {
    return entry.submission == abandoned.submission;
  });
  // Backtracking recalls nothing: the read keeps running, but no cell can
  // name it once the choice point's mark is released, so the service is told
  // to forget it at its end instead of publishing an outcome nobody can read.
  if (at->request) orphan(*at->request);
  if (latest_ && *latest_ == at->submission) latest_.reset();
  owner->erase(at);
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
  std::erase_if(live_, [](const auto &weak) { return weak.expired(); });
  live_.push_back(activation);
  const Stepping stepping(*this, *activation);
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
  // Everything this admission allocates (the staged spec, the request graph,
  // the submission sum) is the I/O service's, reaped by it, and is not part
  // of the script's compiled program.
  const struct HostCells {
    QueryExecutor *executor;
    zigzag::ArenaManifold &arena;
    zigzag::ArenaManifold::AllocationIncarnation first;
    ~HostCells() {
      if (executor != nullptr)
        executor->excludeHostAllocations(first, arena.newestIncarnation());
    }
  } hostCells{current_ != nullptr ? current_->executor.get() : nullptr, arena,
              arena.newestIncarnation()};
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
  if (!matched) {
    // Admitted but unreadable: nothing can name the request, so it is
    // accounted for here and not left behind.
    ports_.bridge.retire(*submission);
    fail(matched.error());
  }
  if (current_ == nullptr) {
    ports_.bridge.retire(*submission);
    fail(diagnose(vortex::frontend_codes::vqlScriptFailed,
                  "a Submit ran outside any script activation"));
  }
  // Finished scripts' rows were returned to their callers, so their requests
  // are retired here. This script's own earlier actions stay: `as $r` names
  // their outcomes for the statements after, and the admission budget bounds
  // how many one script can hold. They join finished_ when the script ends.
  retireAll(finished_);
  latest_ = *submission;
  Admitted entry{.submission = *submission, .request = std::nullopt};
  if (const auto *accepted = std::get_if<vortex::Admitted>(&*matched))
    entry.request = accepted->request;
  current_->admitted.push_back(entry);
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
    retireAll(activation->admitted);
    return ScriptRunResult::failed(std::move(diagnostic));
  }
  if (auto *done = std::get_if<ProgramResult>(&*step)) {
    return finish(activation, *done);
  }
  auto &parked = std::get<ParkedProgram>(*step);
  const auto matched =
      !activation->admitted.empty()
          ? ports_.bridge.match(activation->admitted.back().submission)
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
  parked_.insert_or_assign(token.continuation, token);
  activation->parked = std::move(parked);
  return ScriptRunResult::parked(std::move(token));
}

auto ScriptRunner::finish(const std::shared_ptr<Activation> &activation,
                          const ProgramResult &done) -> ScriptRunResult {
  // However it ends, the script's actions move to the finished list: their
  // rows stay readable until the next admission, then they are retired.
  finished_.insert(finished_.end(), activation->admitted.begin(),
                   activation->admitted.end());
  activation->admitted.clear();
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
      // The outcome row alone would read as "no payload"; a payload the tag
      // exposed but the host cannot read is a typed failure instead.
      const auto unreadable = [&](const std::string &why) {
        return ScriptRunResult::failed(
            diagnose(vortex::frontend_codes::vqlScriptFailed,
                     "the action's payload could not be read: " + why));
      };
      auto lease = ports_.payloads.retain(*gated->payload);
      if (!lease) return unreadable("it was not retained");
      const auto bytes = ports_.payloads.read(
          *lease, 0, static_cast<std::size_t>(lease->range.length));
      ports_.payloads.release(lease->lease);
      if (!bytes) return unreadable("the read was refused");
      // A text cell holds text: bytes that are not valid UTF-8 are a typed
      // refusal, never a cell that renders as something else.
      std::string text(bytes->size(), '\0');
      std::ranges::transform(*bytes, text.begin(), [](const std::byte b) {
        return static_cast<char>(b);
      });
      if (!validUtf8(text))
        return unreadable("the bytes are not valid UTF-8 text");
      rows.push_back(session_.coordinator().arena().makeCell(text));
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
    retireAll(activation->admitted);
    return ScriptRunResult::failed(diagnose(
        vortex::frontend_codes::tokenStale,
        "the parked request or cursor went stale", wake.token.position));
  }
  // The same cursor continues at the Await it parked on: the program never
  // runs the Submit before it a second time.
  auto parked = std::move(*activation->parked);
  activation->parked.reset();
  const Stepping stepping(*this, *activation);
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
