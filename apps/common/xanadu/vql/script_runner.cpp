#include "common/xanadu/vql/script_runner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <gleditor/logging.hpp>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vql/io_match.hpp"
#include "common/xanadu/vql/limits.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

namespace xanadu::vql {

namespace {

using vortex::FrontendDiagnostic;
using vortex::FrontendToken;
using vortex::FrontendWake;
using zigzag::CellRef;

constexpr const char *logCategory = "vql.script.io";

/// The functors the compiler gives each request description.
constexpr const char *readBytesFunctor   = "read_bytes";
constexpr const char *writeBytesFunctor  = "write_bytes";
constexpr const char *appendBytesFunctor = "append_bytes";
constexpr const char *syncFunctor        = "sync";

/// The file rights an `open` mode asks for; the host's grant may refuse any
/// mutating one.
auto rightsOf(const IoOpenMode mode) -> std::uint64_t {
  switch (mode) {
  case IoOpenMode::Read:
    return vortex::fileRightRead;
  case IoOpenMode::Write:
    return vortex::fileRightWrite;
  case IoOpenMode::WriteCreate:
    return vortex::fileRightWrite | vortex::fileRightCreate;
  case IoOpenMode::Append:
    return vortex::fileRightAppend;
  case IoOpenMode::AppendCreate:
    return vortex::fileRightAppend | vortex::fileRightCreate;
  }
  return vortex::fileRightRead;
}

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

constexpr const char *ownedLogCategory = "vortex.io.script";

/// Everything a host hook allocates while a program steps (the staged spec,
/// the request graph, a script-owned record) belongs to the host, so the
/// program's reclamation on close must never free it.
struct HostCells {
  QueryExecutor *executor;
  zigzag::ArenaManifold &arena;
  zigzag::ArenaManifold::AllocationIncarnation first;
  HostCells(QueryExecutor *executor, zigzag::ArenaManifold &arena) noexcept
      : executor(executor), arena(arena), first(arena.newestIncarnation()) {}
  ~HostCells() {
    if (executor != nullptr)
      executor->excludeHostAllocations(first, arena.newestIncarnation());
  }
  HostCells(const HostCells &)            = delete;
  HostCells &operator=(const HostCells &) = delete;
};

/// The name of a Partial's cause from the number the cell store wrote; a
/// number this build does not know is answered as it is, never invented.
auto partialCauseName(const std::string &number) -> std::string {
  unsigned value    = 0;
  const auto *end   = number.data() + number.size();
  const auto parsed = std::from_chars(number.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) return number;
  using C = vortex::PartialCause;
  switch (static_cast<C>(value)) {
  case C::EndOfStream:
    return "EndOfStream";
  case C::Cancelled:
    return "Cancelled";
  case C::TimedOut:
    return "TimedOut";
  case C::Overflow:
    return "Overflow";
  case C::BackendFailure:
    return "BackendFailure";
  }
  return number;
}

/// The tag a `match` arm names for a service outcome tag. A switch, not a
/// cast: the language's numbering is its own contract with the compiler and
/// must not silently follow a reordering of the service's enum.
auto matchTagOf(const vortex::IoOutcomeTag tag) -> IoMatchTag {
  using S = vortex::IoOutcomeTag;
  switch (tag) {
  case S::Rejected:
    return IoMatchTag::Rejected;
  case S::Complete:
    return IoMatchTag::Complete;
  case S::EndOfStream:
    return IoMatchTag::EndOfStream;
  case S::Partial:
    return IoMatchTag::Partial;
  case S::Failed:
    return IoMatchTag::Failed;
  case S::Cancelled:
    return IoMatchTag::Cancelled;
  case S::TimedOut:
    return IoMatchTag::TimedOut;
  case S::Overflow:
    return IoMatchTag::Overflow;
  case S::MessageTooLarge:
    return IoMatchTag::MessageTooLarge;
  case S::Stale:
    return IoMatchTag::Stale;
  case S::Conflict:
    return IoMatchTag::Conflict;
  case S::Unknown:
    return IoMatchTag::Unknown;
  }
  return IoMatchTag::Unknown;
}

static_assert(defaultJoinedTextBytes ==
                  vortex::FileReadHostOptions::defaultWriteBytes,
              "a joined text must fit one default write (limits.hpp)");

/// An action or a `match`: either needs the runner, since a match reads an
/// outcome only the runner's relations can answer.
auto isAction(const Statement &statement) -> bool {
  return std::holds_alternative<IoEffectStatement>(statement) ||
         std::holds_alternative<IoMatchStatement>(statement);
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
  /// Capabilities this script opened; closed when it ends.
  std::vector<vortex::CapabilityCell> opened;
  /// The owned records this script made, so each ending can give back their
  /// payload leases.
  std::vector<CellRef> ownedCells;
  /// The captures this script began (a capture cell, or the refusal outcome
  /// that ended one before any request), so each ending can abandon or free
  /// them through the capture host.
  std::vector<CellRef> captures;
  /// Each open outcome the script's `open` actions produced, with whether it
  /// minted a capability, so a later read through a failed open can say which
  /// open failed and how.
  struct OpenOutcome {
    vortex::OutcomeCell outcome;
    bool minted{};
  };
  std::vector<OpenOutcome> openOutcomes;
  /// The overall deadline of the script, when it has one, as it stands
  /// without any request's own deadline.
  std::optional<std::chrono::steady_clock::time_point> deadline;
  /// A `within` request's own deadline plus the script timeout: the script
  /// outlives the request's TimedOut outcome by its own timeout. The extension
  /// holds only while the request runs, so a later unbounded await is cut at
  /// `deadline` once the request has ended.
  struct Extension {
    vortex::RequestCell request;
    std::chrono::steady_clock::time_point until;
  };
  std::vector<Extension> extensions;
};

namespace {

/// The runner's stale marker, minted only once nothing can still refuse the
/// runner: a constructor that threw after minting would leak the cell, since
/// no destructor runs.
auto mintStaleMarker(VQLEngine &session, const zigzag::vortex::VortexVM &vm)
    -> CellRef {
  if (vm.hasSubmitHandler()) {
    throw std::logic_error(
        "a VortexVM has one Submit hook and another runner holds it");
  }
  const auto marker =
      vortex::makeStaleBindingCell(session.coordinator().arena());
  if (!marker) throw std::runtime_error("the stale marker could not be minted");
  return marker->cell;
}

/// Whether @p cell is the marker a freed handle's variable was rebound to.
/// Decided by tag, so no reader depends on having been handed the marker's
/// identity.
auto isStaleBinding(const zigzag::ArenaManifold &arena, const CellRef cell)
    -> bool {
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation) return false;
  const auto tag = vortex::ioCellTag(arena, cell, *incarnation);
  return tag && *tag == vortex::IoCellTag::StaleBinding;
}

constexpr std::string_view staleBindingAdvice =
    "a variable names an I/O handle that was freed when its script's rows "
    "were retired; bind it again with 'as'";

} // namespace

ScriptRunner::ScriptRunner(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                           ScriptIoPorts ports, QueryExecutionOptions options)
    : session_(session), vm_(vm), ports_(std::move(ports)), options_(options),
      staleMarker_(mintStaleMarker(session, vm)) {
  // The one admission hook of the VM this runner drives: a compiled action's
  // Submit lands here, once per executed instruction.
  vm_.setSubmitHandler(
      [this](const vortex::CapabilityCell capability, const CellRef description,
             const CellRef) { return admit(capability, description); });
  vm_.setSubmitRollbackHandler(
      [this](const zigzag::vortex::VmSubmission &abandoned) {
        rolledBack(abandoned);
      });
  installProviders();
}

ScriptRunner::~ScriptRunner() {
  for (const auto &name : providers_) vm_.setRelationProvider(name, nullptr);
  vm_.setSubmitHandler({});
  vm_.setSubmitRollbackHandler({});
  // A parked activation may be owned only by parked_ (its caller dropped the
  // token): hold every live one strongly before parked_ is cleared, or the
  // loops below would find nothing to close, retire or release.
  std::vector<std::shared_ptr<Activation>> alive;
  for (const auto &weak : live_) {
    if (auto activation = weak.lock()) alive.push_back(std::move(activation));
  }
  for (const auto &[continuation, held] : parked_) {
    static_cast<void>(continuation);
    static_cast<void>(ports_.host.cancelWaiter(held.waiter));
  }
  parked_.clear();
  // A waiter already completed (AlreadyComplete) keeps its route until its
  // wake; nothing may call this runner after it is gone.
  static_cast<void>(ports_.host.unrouteRunner(*this));
  // Nothing reads an outcome once the runner is gone, so every request it
  // holds leaves the service's accounting: a terminal one is retired, and a
  // running one (a detached read, a close just admitted) is forced out, which
  // orphans it. An orphan is not cancelled; it runs to its own end and its
  // terminal is dropped unpublished and its budget freed. A read that never
  // ends would hold its slot for the service's life, so each running one is
  // asked to stop first (never a close, which must reach the descriptor). The
  // files a script opened are closed first.
  for (const auto &activation : alive) closeOpened(*activation);
  stopRunning(finished_);
  for (const auto &activation : alive) stopRunning(activation->admitted);
  retireAll(finished_, true);
  for (const auto &activation : alive) retireAll(activation->admitted, true);
  reapClosing(true);
  // A capture a dropped activation left in flight is abandoned, not finished:
  // the coordinator pins it by phase, and the awaited step is orphaned.
  for (const auto &activation : alive) releaseCaptures(*activation, true);
  // Every record the runner made goes with it, including those of an
  // activation its caller dropped without ending.
  for (auto &[cell, record] : owned_) {
    static_cast<void>(cell);
    record.releasePayload(ports_.payloads);
    record.freeCells(session_.coordinator().core().arena(),
                     ports_.bridge.schema());
  }
  owned_.clear();
  retainedBytes_ = 0;
  freeFinishedRecords();
  // The engine outlives this runner, so no variable may keep naming a freed
  // handle or the marker about to be freed: the first would alias whichever
  // cell reuses its slot, the second the same. A name left is unbound.
  reconcileBindings();
  forgetStaleVariables();
  auto &arena = session_.coordinator().arena();
  if (const auto incarnation = arena.incarnationOf(staleMarker_))
    static_cast<void>(arena.freeCell(staleMarker_, *incarnation));
}

void ScriptRunner::forgetStaleVariables() noexcept {
  try {
    std::vector<std::string> names;
    for (const auto &[name, cells] : session_.variables()) {
      if (std::ranges::find(cells, staleMarker_) != cells.end())
        names.push_back(name);
    }
    for (const auto &name : names) session_.eraseVariable(name);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "stale session variables stayed bound: {}",
                      error.what());
  }
}

void ScriptRunner::retireFinished() {
  // Every name the session holds is recorded while its handle is still live;
  // the free below is what would otherwise let a slot be reused unseen.
  noteBindings();
  retireAll(finished_);
  freeFinishedRecords();
  reconcileBindings();
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
  reapClosing();
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

bool ScriptRunner::retire(const Admitted &entry, const bool force) noexcept {
  const auto &arena = session_.coordinator().arena();
  if (entry.request &&
      !arena.matches(entry.request->cell, entry.request->incarnation)) {
    // The cells were rolled back under a request that may still be running.
    orphan(*entry.request);
  } else if (entry.request && !ports_.host.isTerminal(*entry.request)) {
    // A request still running (a detached read nobody awaited) keeps its
    // entry until it ends, unless the runner itself is going away.
    if (!force) return false;
    ports_.bridge.release(entry.submission);
    orphan(*entry.request);
    // The close still has to reach the descriptor, so it is not cancelled,
    // and its cells and the file's are the host's to free when it ends.
    if (entry.closes && ports_.reapWhenEnded)
      ports_.reapWhenEnded(*entry.request, *entry.closes);
  } else {
    // The close's outcome is read before the retire takes it with the request.
    const auto closed =
        entry.closes && entry.request && closeCompleted(*entry.request);
    // A refusal has no request, so no request's reaping takes its Rejected
    // outcome along: the submission's release frees only the sum, and the
    // outcome's field, state and progress cells (five or six a run) would stay
    // for the arena's life. Whoever kept a copy holds a record of its own.
    std::optional<vortex::OutcomeCell> refusal;
    if (!entry.request) {
      const auto matched = ports_.bridge.match(entry.submission);
      if (const auto *refused =
              matched ? std::get_if<vortex::Refused>(&*matched) : nullptr) {
        refusal = refused->outcome;
      }
    }
    ports_.bridge.retire(entry.submission);
    if (refusal) {
      vortex::OwnedOutcome::freeOutcomeGraph(session_.coordinator().arena(),
                                             ports_.bridge.schema(), *refusal);
    }
    // The file is closed and nothing names its cells any more: they are not a
    // request's, so no reaper takes them, and left alone they cost an arena
    // one endpoint graph per file opened.
    if (closed) {
      static_cast<void>(vortex::reapIoEndpoint(session_.coordinator().arena(),
                                               ports_.bridge.schema(),
                                               *entry.closes));
    }
  }
  if (latest_ && *latest_ == entry.submission) latest_.reset();
  return true;
}

bool ScriptRunner::closeCompleted(const vortex::RequestCell request) const {
  const auto &arena  = session_.coordinator().arena();
  const auto outcome = arena.linked(
      request.cell, ports_.bridge.schema().dim(vortex::IoDimension::Outcome),
      zigzag::DimVector::POS);
  const auto incarnation =
      outcome ? arena.incarnationOf(*outcome) : std::nullopt;
  if (!incarnation) return false;
  const auto tag = vortex::ioOutcomeTag(
      arena,
      vortex::OutcomeCell{.cell = *outcome, .incarnation = *incarnation});
  return tag && *tag == vortex::IoOutcomeTag::Complete;
}

void ScriptRunner::stopRunning(const std::vector<Admitted> &entries) noexcept {
  const auto &arena = session_.coordinator().arena();
  for (const auto &entry : entries) {
    if (entry.request &&
        arena.matches(entry.request->cell, entry.request->incarnation) &&
        !ports_.host.isTerminal(*entry.request)) {
      static_cast<void>(ports_.host.cancelRequest(*entry.request));
    }
  }
}

void ScriptRunner::retireAll(std::vector<Admitted> &entries,
                             const bool force) noexcept {
  std::erase_if(entries,
                [&](const Admitted &entry) { return retire(entry, force); });
}

void ScriptRunner::abandonAll(std::vector<Admitted> &entries) noexcept {
  const auto &arena = session_.coordinator().arena();
  for (const auto &entry : entries) {
    if (entry.request &&
        arena.matches(entry.request->cell, entry.request->incarnation)) {
      // The view goes first so the retired request's graph is freed with it;
      // retireRequest asks a running request to stop and retires it once its
      // terminal lands.
      ports_.bridge.release(entry.submission);
      ports_.host.retireRequest(*entry.request);
      ports_.host.reapRetiredRequest(*entry.request);
    } else {
      static_cast<void>(retire(entry, true));
    }
    if (latest_ && *latest_ == entry.submission) latest_.reset();
  }
  entries.clear();
}

bool ScriptRunner::closeOne(const vortex::CapabilityCell capability) noexcept {
  try {
    // Resolved before the close: once it completes the capability's binding
    // is still readable, but this is the only place that has both.
    const auto endpoint = vortex::capabilityEndpoint(
        session_.coordinator().arena(), ports_.bridge.schema(), capability);
    const auto closing = ports_.closeCapability(capability);
    if (!closing) {
      // Not an open file any more (already closed): nothing to retry.
      GLEDITOR_LOG_WARN(logCategory, "closing an opened file failed: {}",
                        closing.error().message);
      return true;
    }
    const auto matched = ports_.bridge.match(*closing);
    if (!matched) {
      ports_.bridge.retire(*closing);
      GLEDITOR_LOG_WARN(logCategory, "a close could not be read back: {}",
                        matched.error().message);
      return false;
    }
    if (const auto *accepted = std::get_if<vortex::Admitted>(&*matched)) {
      closing_.push_back(
          {.submission = *closing,
           .request    = accepted->request,
           .closes     = endpoint ? std::optional{*endpoint} : std::nullopt});
      return true;
    }
    ports_.bridge.retire(*closing);
    GLEDITOR_LOG_WARN(logCategory,
                      "the close of an opened file was refused; it is kept "
                      "and tried again");
    return false;
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "closing an opened file threw: {}",
                      error.what());
    return true;
  }
}

void ScriptRunner::closeOpened(Activation &activation) noexcept {
  const auto opened = std::exchange(activation.opened, {});
  if (!ports_.closeCapability) return;
  for (const auto capability : opened) {
    if (!closeOne(capability))
      unclosed_.push_back({capability, 1, closesFinished_});
  }
}

void ScriptRunner::reapClosing(const bool force) noexcept {
  if (!unclosed_.empty() && ports_.closeCapability) {
    auto retry = std::exchange(unclosed_, {});
    for (auto &held : retry) {
      if (!force && held.seenFinished == closesFinished_) {
        unclosed_.push_back(held);
        continue;
      }
      if (closeOne(held.capability)) continue;
      if (force) continue;
      if (++held.attempts >= maxCloseAttempts) {
        GLEDITOR_LOG_WARN(logCategory,
                          "a close stayed refused after {} tries; the "
                          "endpoint is left open",
                          held.attempts);
        continue;
      }
      held.seenFinished = closesFinished_;
      unclosed_.push_back(held);
    }
  }
  const auto before = closing_.size();
  retireAll(closing_, force);
  closesFinished_ += before - closing_.size();
  // An abandoned capture's orphaned step ends on its own schedule and nothing
  // else visits it again, so each reaping turn gives its residue a chance to
  // go.
  if (!force) {
    if (const auto capture = captureHost()) capture->reapAbandoned();
  }
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

namespace {

using zigzag::vortex::RelationAnswer;
using zigzag::vortex::RelationQuery;
using zigzag::vortex::RelationResult;

/// One answer at scan position 0, or nothing: how a host exposes one value
/// through a relation. Anything past position 0 is exhausted.
auto answerOnce(const RelationQuery &query, const std::optional<CellRef> cell)
    -> RelationResult {
  if (query.position != 0 || !cell)
    return std::optional<RelationAnswer>{std::nullopt};
  return std::optional<RelationAnswer>{
      RelationAnswer{.nextPosition = 1, .limit = 1, .answers = {*cell}}};
}

auto liveCell(const RelationQuery &query, const std::string &who)
    -> std::expected<
        std::pair<CellRef, zigzag::ArenaManifold::AllocationIncarnation>,
        std::string> {
  if (query.arguments.size() != 1)
    return std::unexpected(who + " expects one argument");
  const auto incarnation = query.arena.incarnationOf(query.arguments[0]);
  if (!incarnation)
    return std::unexpected(who + ": the argument is not a live cell");
  return std::pair{query.arguments[0], *incarnation};
}

auto liveOutcome(const RelationQuery &query, const std::string &who)
    -> std::expected<vortex::OutcomeCell, std::string> {
  const auto cell = liveCell(query, who);
  if (!cell) return std::unexpected(cell.error());
  const vortex::OutcomeCell outcome{cell->first, cell->second};
  if (!vortex::ioOutcomeTag(query.arena, outcome))
    return std::unexpected(who + ": the argument is not an outcome");
  return outcome;
}

/// Whether the single argument is a capture's receipt: not an outcome, so the
/// outcome fields answer nothing for it rather than failing the script.
auto isCaptureReceipt(const RelationQuery &query) -> bool {
  if (query.arguments.size() != 1) return false;
  const auto incarnation = query.arena.incarnationOf(query.arguments[0]);
  if (!incarnation) return false;
  const auto tag =
      vortex::ioCellTag(query.arena, query.arguments[0], *incarnation);
  return tag && *tag == vortex::IoCellTag::CaptureReceipt;
}

/// The refusals a capture can make as a value, each a stable detail of the
/// error code it rides on.
constexpr std::uint32_t captureDetailNoPayload      = 1;
constexpr std::uint32_t captureDetailObservation    = 2;
constexpr std::uint32_t captureDetailRange          = 3;
constexpr std::uint32_t captureDetailUnreadableGate = 4;

/// An outcome field a path function reads, by the name the cell store wrote.
struct FieldRelation {
  const char *relation;
  const char *function;
  const char *field;
};
constexpr std::array<FieldRelation, 3> fieldRelations{{
    {outcomeErrorRelation, "io_error", "errorCode"},
    {outcomeDetailRelation, "io_detail", "stableDetail"},
    {outcomeUncertainRelation, "io_uncertain", "uncertain"},
}};

} // namespace

void ScriptRunner::installProviders() {
  const auto install = [&](const char *name,
                           zigzag::vortex::RelationProvider provider) {
    // A name whose handle was freed answers a typed refusal at every reader,
    // never the value of whichever cell reused the slot.
    vm_.setRelationProvider(
        name,
        [this, provider = std::move(provider)](
            const RelationQuery &query) -> RelationResult {
          // Names are recorded at script entry and after each plain line
          // (not here: rebuilding the table per call made a per-row relation
          // O(rows x variables) with allocation). The incarnation each name
          // recorded is what catches a handle freed by a pump mid-script.
          if (std::ranges::any_of(query.arguments, [&](const CellRef argument) {
                return isStaleBinding(query.arena, argument) ||
                       namesFreedHandle(argument);
              })) {
            reconcileBindings();
            return staleRefusal(std::string{staleBindingAdvice});
          }
          return provider(query);
        });
    providers_.emplace_back(name);
  };
  // Outcome fields. Each answers one cell or nothing; the VM and the compiler
  // name no I/O schema, only these relations.
  // io_tag names an outcome's tag. A script also holds the other two things
  // an action yields, so it tells them apart by name: a capability an open
  // minted answers `Capability`, a request a detached read started `Request`.
  install(outcomeTagRelation, [](const RelationQuery &query) -> RelationResult {
    const auto cell = liveCell(query, "io_tag");
    if (!cell) return std::unexpected(cell.error());
    const auto kind = vortex::ioCellTag(query.arena, cell->first, cell->second);
    if (kind && *kind == vortex::IoCellTag::Capability)
      return answerOnce(query, query.arena.makeCell(std::string{"Capability"}));
    if (kind && *kind == vortex::IoCellTag::Request)
      return answerOnce(query, query.arena.makeCell(std::string{"Request"}));
    if (kind && *kind == vortex::IoCellTag::CaptureReceipt)
      return answerOnce(query,
                        query.arena.makeCell(std::string{"CaptureReceipt"}));
    const auto outcome = liveOutcome(query, "io_tag");
    if (!outcome) return std::unexpected(outcome.error());
    const auto tag = vortex::ioOutcomeTag(query.arena, *outcome);
    return answerOnce(
        query, query.arena.makeCell(std::string{vortex::toString(*tag)}));
  });
  for (const auto &entry : fieldRelations) {
    install(entry.relation,
            [this, entry](const RelationQuery &query) -> RelationResult {
              if (isCaptureReceipt(query)) return answerOnce(query, {});
              const auto outcome = liveOutcome(query, entry.function);
              if (!outcome) return std::unexpected(outcome.error());
              const auto text =
                  ports_.bridge.outcomeField(*outcome, entry.field);
              return answerOnce(
                  query, text ? std::optional{query.arena.makeCell(*text)}
                              : std::nullopt);
            });
  }
  // Why a Partial stopped: the cell store keeps the cause as its enumerator's
  // number, which no script should have to know, so the name answers.
  install(outcomeCauseRelation,
          [this](const RelationQuery &query) -> RelationResult {
            if (isCaptureReceipt(query)) return answerOnce(query, {});
            const auto outcome = liveOutcome(query, "io_cause");
            if (!outcome) return std::unexpected(outcome.error());
            const auto number = ports_.bridge.outcomeField(*outcome, "cause");
            if (!number) return answerOnce(query, {});
            return answerOnce(query,
                              query.arena.makeCell(partialCauseName(*number)));
          });
  // The byte count is what the outcome retained, read through the same gate as
  // the payload; an outcome that retained none answers the bytes its progress
  // record counted (zero for a backend that reports no progress).
  install(outcomeBytesRelation,
          [this](const RelationQuery &query) -> RelationResult {
            if (isCaptureReceipt(query)) return answerOnce(query, {});
            const auto outcome = liveOutcome(query, "io_bytes");
            if (!outcome) return std::unexpected(outcome.error());
            if (payloadReleased(outcome->cell))
              return staleRefusal(
                  "io_bytes: the bytes belonged to the script that read "
                  "them and went back with it; read them again");
            const auto gate = gated(*outcome);
            if (!gate) return std::unexpected(gate.error().message);
            const auto counted =
                gate->payload
                    ? std::optional{std::to_string(gate->payload->length)}
                    : ports_.bridge.outcomeField(*outcome, "transferredBytes");
            return answerOnce(
                query, counted ? std::optional{query.arena.makeCell(*counted)}
                               : std::nullopt);
          });
  // The payload text goes through the shared terminal-status gate: an outcome
  // whose tag exposes no payload (or retained none) answers nothing.
  install(outcomePayloadRelation,
          [this](const RelationQuery &query) -> RelationResult {
            if (isCaptureReceipt(query)) return answerOnce(query, {});
            const auto outcome = liveOutcome(query, "io_payload");
            if (!outcome) return std::unexpected(outcome.error());
            if (payloadReleased(outcome->cell))
              return staleRefusal(
                  "io_payload: the bytes belonged to the script that read "
                  "them and went back with it; read them again");
            if (query.position != 0)
              return std::optional<RelationAnswer>{std::nullopt};
            const auto text = payloadText(*outcome);
            if (!text) return std::unexpected(text.error().message);
            if (*text) {
              if (const auto gate = gated(*outcome); gate && gate->payload)
                expose(*gate->payload);
            }
            return answerOnce(
                query, *text ? std::optional{query.arena.makeCell(**text)}
                             : std::nullopt);
          });
  install(ownOutcomeRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return ownOutcome(query, true);
          });
  install(ownCopyRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return ownOutcome(query, false);
          });
  install(matchTagRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return matchTag(query);
          });
  install(captureBeginRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return captureBegin(query, false);
          });
  install(captureRangeRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return captureBegin(query, true);
          });
  install(captureArmRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return captureArm(query);
          });
  install(captureStepRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return captureStep(query);
          });
  install(outcomeCapturedRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return capturedCells(query);
          });
  install(handleArmRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return handleArm(query, false);
          });
  install(handleCancelRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return handleArm(query, true);
          });
  install(openRelation, [this](const RelationQuery &query) -> RelationResult {
    return openPath(query);
  });
  install(openCapabilityRelation,
          [this](const RelationQuery &query) -> RelationResult {
            return openCapability(query);
          });
}

auto ScriptRunner::handleArm(const RelationQuery &query, const bool cancel)
    -> RelationResult {
  const char *who = cancel ? "cancel" : "await";
  const auto cell = liveCell(query, who);
  if (!cell) return std::unexpected(cell.error());
  const auto tag = vortex::ioCellTag(query.arena, cell->first, cell->second);
  if (!tag || (*tag != vortex::IoCellTag::Request &&
               *tag != vortex::IoCellTag::Outcome)) {
    stageFailure_ = diagnose(vortex::frontend_codes::vqlEffectArgument,
                             std::string{"the handle of '"} + who +
                                 "' is neither a request nor an outcome");
    return std::unexpected(stageFailure_->message);
  }
  const bool isRequest = *tag == vortex::IoCellTag::Request;
  if (cancel && isRequest) {
    // A request that already ended answers AlreadyTerminal, which is itself
    // the value: the statement then yields the outcome it had. Stale is the
    // same, and the await that follows reports it as a refusal outcome.
    static_cast<void>(ports_.host.cancelRequest(
        vortex::RequestCell{cell->first, cell->second}));
  }
  constexpr std::int64_t requestArm = 0;
  constexpr std::int64_t outcomeArm = 1;
  return answerOnce(
      query, query.arena.makeScalarCell(isRequest ? requestArm : outcomeArm));
}

auto ScriptRunner::openPath(const RelationQuery &query) -> RelationResult {
  const auto refuse = [&](std::string message) -> RelationResult {
    stageFailure_ =
        diagnose(vortex::frontend_codes::vqlEffectArgument, std::move(message));
    return std::unexpected(stageFailure_->message);
  };
  if (query.arguments.size() != 2)
    return refuse("open expects a path and a mode");
  if (!ports_.openFile) return refuse("this program cannot open files");
  if (current_ == nullptr)
    return refuse("an open ran outside any script activation");
  const auto path = query.arena.textOf(query.arguments[0]);
  if (path.empty()) return refuse("open names an empty path");
  const auto mode = number(query.arguments[1], "the open mode", std::nullopt);
  if (!mode) return refuse(mode.error().message);
  if (*mode > static_cast<std::uint64_t>(IoOpenMode::AppendCreate))
    return refuse("open names a mode this program does not know");
  // The request graph and submission sum are the I/O service's, as in admit().
  const HostCells hostCells{current_->executor.get(), query.arena};
  // Earlier scripts' rows are done with before this one admits anything, so a
  // budget they filled is free for this open.
  retireFinished();
  const auto submission =
      ports_.openFile(path, rightsOf(static_cast<IoOpenMode>(*mode)));
  if (!submission) return refuse(submission.error().message);
  const auto matched = ports_.bridge.match(*submission);
  if (!matched) {
    ports_.bridge.retire(*submission);
    return refuse(matched.error().message);
  }
  Admitted entry{.submission = *submission, .request = std::nullopt};
  CellRef handle{};
  if (const auto *accepted = std::get_if<vortex::Admitted>(&*matched)) {
    entry.request = accepted->request;
    handle        = accepted->request.cell;
  } else {
    handle = std::get<vortex::Refused>(*matched).outcome.cell;
  }
  current_->admitted.push_back(entry);
  // One answer per argument, as the VM requires; only the first is the handle.
  return std::optional<RelationAnswer>{RelationAnswer{
      .nextPosition = 1, .limit = 1, .answers = {handle, handle}}};
}

auto ScriptRunner::openCapability(const RelationQuery &query)
    -> RelationResult {
  const auto outcome = liveOutcome(query, "open");
  if (!outcome) return std::unexpected(outcome.error());
  if (query.position != 0 || !ports_.capabilityOf)
    return std::optional<RelationAnswer>{std::nullopt};
  const auto capability = ports_.capabilityOf(*outcome);
  if (current_ != nullptr) {
    current_->openOutcomes.push_back({*outcome, capability.has_value()});
  }
  if (!capability) return std::optional<RelationAnswer>{std::nullopt};
  if (current_ != nullptr) {
    // The relation can be evaluated again (a retry, a backtrack): one open is
    // one close.
    if (std::ranges::find(current_->opened, *capability) ==
        current_->opened.end())
      current_->opened.push_back(*capability);
  } else {
    GLEDITOR_LOG_WARN(logCategory,
                      "an open completed outside any script activation and "
                      "will not be closed by the runner");
  }
  return answerOnce(query, capability->cell);
}

auto ScriptRunner::captureBegin(const RelationQuery &query, const bool ranged)
    -> RelationResult {
  const auto refuse = [&](const std::string_view code,
                          std::string message) -> RelationResult {
    stageFailure_ = diagnose(code, std::move(message));
    return std::unexpected(stageFailure_->message);
  };
  const std::size_t arity = ranged ? 3 : 1;
  if (query.arguments.size() != arity)
    return refuse(vortex::frontend_codes::vqlEffectArgument,
                  "capture expects an outcome" +
                      std::string{ranged ? ", an offset and a length" : ""});
  if (current_ == nullptr)
    return refuse(vortex::frontend_codes::vqlScriptFailed,
                  "a capture ran outside any script activation");
  const auto capture = captureHost();
  if (capture == nullptr)
    return refuse(vortex::frontend_codes::vqlCaptureUnavailable,
                  "this program built no capture stack");
  auto &arena = query.arena;
  // Capture takes an outcome: text a script printed is scratch bytes with no
  // source observation to key overlap on (design §3).
  const auto incarnation = arena.incarnationOf(query.arguments[0]);
  if (isStaleBinding(arena, query.arguments[0]))
    return refuse(vortex::frontend_codes::vqlStaleBinding,
                  std::string{staleBindingAdvice});
  const bool isOutcome =
      incarnation &&
      vortex::ioCellTag(arena, query.arguments[0], *incarnation) ==
          vortex::IoCellTag::Outcome;
  if (!isOutcome)
    return refuse(vortex::frontend_codes::vqlCaptureSource,
                  "capture takes an outcome that exposes a payload");
  const vortex::OutcomeCell source{query.arguments[0], *incarnation};

  std::uint64_t offset = 0;
  std::optional<std::uint64_t> length;
  if (ranged) {
    const auto first =
        number(query.arguments[1], "the capture offset", std::nullopt);
    if (!first) return refuse(first.error().code, first.error().message);
    const auto second =
        number(query.arguments[2], "the capture length", std::nullopt);
    if (!second) return refuse(second.error().code, second.error().message);
    offset = *first;
    length = *second;
  }

  // The capture admits requests through the coordinator, so it is an
  // admission path: earlier scripts' rows are done with before it begins.
  const HostCells hostCells{current_->executor.get(), arena};
  retireFinished();

  const auto rejected = [&](const vortex::IoErrorCode code,
                            const std::uint32_t detail) -> RelationResult {
    const auto refused = ports_.host.refuse({code, detail});
    const auto outcome = std::get<vortex::Refused>(refused.value).outcome;
    current_->captures.push_back(outcome.cell);
    return std::optional<RelationAnswer>{
        RelationAnswer{.nextPosition = 1,
                       .limit        = 1,
                       .answers = std::vector<CellRef>(arity, outcome.cell)}};
  };

  const auto gate = gated(source);
  if (!gate)
    return rejected(vortex::IoErrorCode::Unresolved,
                    captureDetailUnreadableGate);
  if (!gate->payload)
    return rejected(vortex::IoErrorCode::Unresolved, captureDetailNoPayload);
  // The exact observation, not a re-read: a capture keeps what the outcome
  // retained. Where the read recorded an observation it must bind these very
  // bytes, or the outcome is not the observation's.
  const auto found       = owned_.find(source.cell);
  const auto observation = found != owned_.end() && found->second.cell == source
                               ? found->second.observation
                               : vortex::outcomeCellReadObservation(
                                     arena, ports_.bridge.schema(), source);
  if (observation && observation->payload &&
      !(*observation->payload == *gate->payload))
    return rejected(vortex::IoErrorCode::Unresolved, captureDetailObservation);

  vortex::PayloadRange selection = *gate->payload;
  if (length) {
    const auto whole = gate->payload->length;
    if (offset > whole || *length > whole - offset)
      return rejected(vortex::IoErrorCode::InvalidRequest, captureDetailRange);
    selection.offset += offset;
    selection.length = *length;
  }
  const auto handle = capture->begin(selection);
  current_->captures.push_back(handle);
  return std::optional<RelationAnswer>{
      RelationAnswer{.nextPosition = 1,
                     .limit        = 1,
                     .answers      = std::vector<CellRef>(arity, handle)}};
}

auto ScriptRunner::captureArm(const RelationQuery &query) -> RelationResult {
  const auto capture = captureHost();
  if (capture == nullptr || query.arguments.size() != 2) {
    stageFailure_ = diagnose(vortex::frontend_codes::vqlCaptureUnavailable,
                             "no capture is in flight to continue");
    return std::unexpected(stageFailure_->message);
  }
  const auto next = capture->next(query.arguments[0]);
  if (!next) {
    stageFailure_ = diagnose(vortex::frontend_codes::vqlScriptFailed,
                             "the capture's state is gone");
    return std::unexpected(stageFailure_->message);
  }
  constexpr std::int64_t requestArm = 0;
  constexpr std::int64_t valueArm   = 1;
  const auto &arena                 = query.arena;
  if (const auto *waiting = std::get_if<vortex::CaptureWaiting>(&*next)) {
    return std::optional<RelationAnswer>{
        RelationAnswer{.nextPosition = 1,
                       .limit        = 1,
                       .answers      = {query.arena.makeScalarCell(requestArm),
                                        waiting->request.cell}}};
  }
  static_cast<void>(arena);
  return std::optional<RelationAnswer>{RelationAnswer{
      .nextPosition = 1,
      .limit        = 1,
      .answers      = {query.arena.makeScalarCell(valueArm),
                       std::get<vortex::CaptureFinished>(*next).value}}};
}

auto ScriptRunner::captureStep(const RelationQuery &query) -> RelationResult {
  const auto capture = captureHost();
  if (capture == nullptr || query.arguments.size() != 2 ||
      current_ == nullptr) {
    stageFailure_ = diagnose(vortex::frontend_codes::vqlCaptureUnavailable,
                             "no capture is in flight to continue");
    return std::unexpected(stageFailure_->message);
  }
  const auto stepIncarnation = query.arena.incarnationOf(query.arguments[1]);
  const std::optional<vortex::OutcomeCell> outcome =
      stepIncarnation &&
              vortex::ioOutcomeTag(
                  query.arena,
                  vortex::OutcomeCell{query.arguments[1], *stepIncarnation})
          ? std::optional{vortex::OutcomeCell{query.arguments[1],
                                              *stepIncarnation}}
          : std::nullopt;
  if (!outcome) {
    stageFailure_ = diagnose(vortex::frontend_codes::vqlScriptFailed,
                             "a capture step ended with no outcome");
    return std::unexpected(stageFailure_->message);
  }
  // The receipt, the snapshot and the captured cells are the host's.
  const HostCells hostCells{current_->executor.get(), query.arena};
  capture->advance(query.arguments[0], *outcome);
  return std::optional<RelationAnswer>{
      RelationAnswer{.nextPosition = 1,
                     .limit        = 1,
                     .answers      = {query.arguments[0], query.arguments[0]}}};
}

auto ScriptRunner::capturedCells(const RelationQuery &query) -> RelationResult {
  const auto capture = captureHost();
  if (capture == nullptr || query.arguments.size() != 1 || query.position < 0)
    return std::optional<RelationAnswer>{std::nullopt};
  const auto cell = capture->capturedCell(
      query.arguments[0], static_cast<std::size_t>(query.position));
  if (!cell) return std::optional<RelationAnswer>{std::nullopt};
  return std::optional<RelationAnswer>{RelationAnswer{
      .nextPosition = query.position + 1, .limit = 1, .answers = {*cell}}};
}

void ScriptRunner::releaseCaptures(Activation &activation,
                                   const bool freeCells) noexcept {
  const auto host = captureHost();
  for (const auto handle : std::exchange(activation.captures, {})) {
    if (host == nullptr) continue;
    if (freeCells) {
      host->release(handle);
    } else {
      finishedCaptures_.push_back(handle);
    }
  }
}

auto ScriptRunner::run(QueryExpression query) -> ScriptRunResult {
  // Names made by plain lines since the last script are recorded before
  // anything is freed, or a slot reused first would be taken for theirs.
  noteBindings();
  reapClosing();
  reconcileBindings();
  // Everything that can be wrong with an action's own values is wrong before
  // anything is compiled, staged or submitted.
  const auto &statements = query.body.statements;
  // A capability an earlier `open ... as $name` of this script will mint is
  // not yet a value to check: it is checked when its read submits.
  std::set<std::string> opensLater;
  // Every name an earlier action of this script binds: such a name is a value
  // the run makes, not one to check beforehand.
  std::set<std::string> named;
  // Every name an earlier `let` of this script binds (before any `for`, whose
  // lets end with the loop). Like an action's name it is a value the run
  // makes: the compiled program narrows it to one value and the Submit hook
  // checks that value, so there is nothing to check beforehand.
  std::set<std::string> lets;
  for (std::size_t index = 0; index < statements.size(); ++index) {
    if (const auto *block = std::get_if<ExecutionBlock>(&statements[index])) {
      for (const auto &binding : block->bindings) {
        const auto *let = std::get_if<LetClause>(&binding);
        if (let == nullptr) break;
        lets.insert(let->varName);
        named.erase(let->varName);
      }
      continue;
    }
    const auto *action = std::get_if<IoEffectStatement>(&statements[index]);
    if (action == nullptr) continue;
    // A name an earlier action bound now wins over an earlier `let`.
    const auto checked = [&](const IoOperand &value)
        -> std::expected<std::uint64_t, FrontendDiagnostic> {
      if (const auto *name = std::get_if<std::string>(&value);
          name != nullptr && lets.contains(*name))
        return std::uint64_t{0};
      return operand(value, index);
    };
    const auto remember = [&] {
      if (action->resultVar) {
        named.insert(*action->resultVar);
        lets.erase(*action->resultVar);
      }
    };
    if (action->kind == IoEffectStatement::Kind::Open) {
      if (action->resultVar) opensLater.insert(*action->resultVar);
      remember();
      continue;
    }
    if (action->kind == IoEffectStatement::Kind::Capture) {
      if (ports_.ensureCaptureHost) {
        if (const auto built = ports_.ensureCaptureHost(); !built)
          return ScriptRunResult::failed(
              diagnose(vortex::frontend_codes::vqlCaptureUnavailable,
                       built.error(), index));
      }
      if (captureHost() == nullptr)
        return ScriptRunResult::failed(
            diagnose(vortex::frontend_codes::vqlCaptureUnavailable,
                     "this program built no capture stack, so nothing can be "
                     "captured",
                     index));
      if (!named.contains(action->captureVar)) {
        if (const auto stale = staleName(action->captureVar, index))
          return ScriptRunResult::failed(*stale);
        const auto &bound = session_.variables();
        const auto found  = bound.find(action->captureVar);
        if (found == bound.end() || found->second.size() != 1)
          return ScriptRunResult::failed(
              diagnose(vortex::frontend_codes::vqlEffectArgument,
                       "$" + action->captureVar +
                           " is not bound to exactly one outcome to capture",
                       index));
      }
      if (action->captureRanged) {
        if (const auto offset = checked(action->offset); !offset)
          return ScriptRunResult::failed(offset.error());
        if (const auto length = checked(action->maxBytes); !length)
          return ScriptRunResult::failed(length.error());
      }
      remember();
      continue;
    }
    if (action->kind != IoEffectStatement::Kind::Read) {
      remember();
      continue;
    }
    if (!opensLater.contains(action->capabilityVar)) {
      if (const auto cap = capability(action->capabilityVar, index); !cap)
        return ScriptRunResult::failed(cap.error());
    }
    const bool positional = action->request == IoRequestKind::ReadBytes ||
                            action->request == IoRequestKind::WriteBytes;
    if (positional) {
      if (const auto offset = checked(action->offset); !offset)
        return ScriptRunResult::failed(offset.error());
    }
    if (action->request == IoRequestKind::ReadBytes) {
      if (const auto length = checked(action->maxBytes); !length)
        return ScriptRunResult::failed(length.error());
    }
    const bool sends = action->request == IoRequestKind::WriteBytes ||
                       action->request == IoRequestKind::AppendBytes;
    if (sends && action->source.isVariable &&
        !named.contains(action->source.text) &&
        !lets.contains(action->source.text)) {
      // An unbound name would compile to a text cell holding its own name,
      // and a write would put that in the file.
      if (const auto stale = staleName(action->source.text, index))
        return ScriptRunResult::failed(*stale);
      const auto &bound = session_.variables();
      const auto found  = bound.find(action->source.text);
      if (found == bound.end() || found->second.size() != 1)
        return ScriptRunResult::failed(
            diagnose(vortex::frontend_codes::vqlEffectArgument,
                     "$" + action->source.text +
                         " is not bound to exactly one cell to write (bind "
                         "it with a 'let' or name an outcome with 'as' "
                         "before this action)",
                     index));
    }
    if (action->within) {
      if (const auto bound = checked(action->within->amount); !bound)
        return ScriptRunResult::failed(bound.error());
    }
    remember();
  }
  auto activation   = std::make_shared<Activation>();
  activation->query = std::move(query);
  activation->executor =
      std::make_unique<QueryExecutor>(session_, vm_, options_);
  if (scriptTimeout_)
    activation->deadline = now() + std::min(*scriptTimeout_, maxDeadline);
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
  if (isStaleBinding(session_.coordinator().arena(), cell))
    return std::unexpected{diagnose(vortex::frontend_codes::vqlStaleBinding,
                                    std::string{staleBindingAdvice}, position)};
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
  if (const auto stale = staleName(name)) return std::unexpected{*stale};
  const auto tag = vortex::ioCellTag(arena, cell, *incarnation);
  if (!tag || *tag != vortex::IoCellTag::Capability) {
    return refuse("$" + name + " is not a capability cell");
  }
  return vortex::CapabilityCell{cell, *incarnation};
}

auto ScriptRunner::gated(const vortex::OutcomeCell outcome) const
    -> std::expected<vortex::GatedOutcome, FrontendDiagnostic> {
  auto gate = ports_.bridge.gate(outcome);
  if (!gate) return gate;
  // A record has no payload cell: its bytes are the lease the table holds.
  const auto found = owned_.find(outcome.cell);
  if (found != owned_.end() && found->second.cell == outcome &&
      found->second.payload && vortex::tagMayCarryPayload(gate->tag)) {
    gate->payload = found->second.payload->range;
  }
  return gate;
}

auto ScriptRunner::ownOutcome(const RelationQuery &query,
                              const bool retireRequest) -> RelationResult {
  const auto keep = std::optional<RelationAnswer>{std::nullopt};
  if (query.position != 0 || query.arguments.size() != 1 || current_ == nullptr)
    return keep;
  auto &arena                   = query.arena;
  const auto outcomeIncarnation = arena.incarnationOf(query.arguments[0]);
  if (!outcomeIncarnation) return keep;
  const vortex::OutcomeCell outcome{query.arguments[0], *outcomeIncarnation};
  if (!vortex::ioOutcomeTag(arena, outcome)) return keep;
  // The entry the statement admitted: the request whose outcome this is, or,
  // for a refusal, the submission holding this Rejected outcome.
  const auto &schema = ports_.bridge.schema();
  const auto holds   = [&](const Admitted &held) {
    if (held.request) {
      return arena.linked(held.request->cell,
                            schema.dim(vortex::IoDimension::Outcome),
                            zigzag::DimVector::POS) == outcome.cell;
    }
    const auto matched = ports_.bridge.match(held.submission);
    const auto *refused =
        matched ? std::get_if<vortex::Refused>(&*matched) : nullptr;
    return refused != nullptr && refused->outcome == outcome;
  };
  // A handle an earlier script left running sits in the finished list, not in
  // this script's own admissions; awaiting it here must still name an owned
  // record, or the name would be the service's outcome, freed when the next
  // admission retires its request.
  auto *entries = &current_->admitted;
  auto entry    = std::ranges::find_if(*entries, holds);
  if (entry == entries->end()) {
    entries = &finished_;
    entry   = std::ranges::find_if(*entries, holds);
    if (entry == entries->end()) return keep;
  }

  const auto gate = ports_.bridge.gate(outcome);
  if (!gate) return keep;
  std::optional<vortex::PayloadLease> lease;
  if (gate->payload) {
    auto taken = ports_.payloads.retain(*gate->payload);
    if (!taken) {
      GLEDITOR_LOG_WARN(ownedLogCategory,
                        "a named outcome's payload could not be retained; "
                        "the service keeps the outcome");
      return keep;
    }
    // The record also keeps the read's observation by value, whose locator is
    // charged beside the chunk.
    const auto observed =
        vortex::outcomeCellReadObservation(arena, schema, outcome);
    const auto size = ports_.payloads.chunkBytes(*taken).value_or(0) +
                      (observed ? observed->locator.size() : 0);
    if (size > ports_.retainedPayloadBytes ||
        retainedBytes_ > ports_.retainedPayloadBytes - size) {
      ports_.payloads.release(taken->lease);
      GLEDITOR_LOG_WARN(ownedLogCategory,
                        "a named outcome would retain {} bytes past the "
                        "budget of {} ({} held); the service keeps the "
                        "outcome and its request slot",
                        size, ports_.retainedPayloadBytes, retainedBytes_);
      return keep;
    }
    lease = *taken;
  }
  const HostCells hostCells{current_->executor.get(), arena};
  auto record =
      vortex::OwnedOutcome::copy(arena, ports_.bridge.schema(), ports_.payloads,
                                 outcome, std::move(lease));
  if (!record) {
    GLEDITOR_LOG_WARN(ownedLogCategory, "an owned record was not made: {}",
                      vortex::toString(record.error()));
    return keep;
  }
  const auto cell = record->cell.cell;
  retainedBytes_ += record->chunkBytes + record->observationBytes();
  // A rolled-back record's slot can be reused within this activation: the
  // entry it leaves must give its lease and budget back, not be overwritten.
  if (const auto stale = owned_.find(cell); stale != owned_.end()) {
    retainedBytes_ -=
        std::min(retainedBytes_,
                 stale->second.chunkBytes + stale->second.observationBytes());
    stale->second.releasePayload(ports_.payloads);
    owned_.erase(stale);
  }
  owned_.emplace(cell, *std::move(record));
  current_->ownedCells.push_back(cell);
  // Nothing reads the service's outcome again: the record has every field it
  // had, so the request leaves the budget now rather than at the script's end.
  // A handle the script may still await keeps its request: the second await
  // must find it, and the script's end retires it with the rest.
  if (!retireRequest) return answerOnce(query, cell);
  const auto retired = *entry;
  if (retire(retired)) entries->erase(entry);
  return answerOnce(query, cell);
}

auto ScriptRunner::matchTag(const RelationQuery &query) -> RelationResult {
  const auto cell = liveCell(query, "match");
  if (!cell) return std::unexpected(cell.error());
  const auto outcome = vortex::OutcomeCell{cell->first, cell->second};
  const auto tag     = vortex::ioOutcomeTag(query.arena, outcome);
  if (!tag) {
    const auto kind = vortex::ioCellTag(query.arena, cell->first, cell->second);
    stageFailure_ =
        diagnose(vortex::frontend_codes::vqlMatch,
                 std::string{"a match is aimed at "} +
                     (kind && *kind == vortex::IoCellTag::Request
                          ? "a request handle; await it first"
                      : kind && *kind == vortex::IoCellTag::Capability
                          ? "a capability, not an outcome"
                          : "a value that is not an outcome"));
    return std::unexpected(stageFailure_->message);
  }
  return answerOnce(query, query.arena.makeScalarCell(
                               static_cast<std::int64_t>(matchTagOf(*tag))));
}

void ScriptRunner::releaseRecords(Activation &activation,
                                  const bool freeCells) noexcept {
  for (const auto cell : std::exchange(activation.ownedCells, {})) {
    const auto found = owned_.find(cell);
    if (found == owned_.end()) continue;
    auto record = std::move(found->second);
    owned_.erase(found);
    retainedBytes_ -=
        std::min(retainedBytes_, record.chunkBytes + record.observationBytes());
    record.releasePayload(ports_.payloads);
    if (freeCells) {
      record.freeCells(session_.coordinator().core().arena(),
                       ports_.bridge.schema());
    } else {
      finishedRecords_.push_back(std::move(record));
    }
  }
}

void ScriptRunner::freeFinishedRecords() noexcept {
  if (const auto host = captureHost()) {
    for (const auto handle : finishedCaptures_) host->release(handle);
  }
  finishedCaptures_.clear();
  for (auto &record : finishedRecords_)
    record.freeCells(session_.coordinator().core().arena(),
                     ports_.bridge.schema());
  finishedRecords_.clear();
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
  const HostCells hostCells{
      current_ != nullptr ? current_->executor.get() : nullptr, arena};
  const zigzag::Vlog vlog{.m     = arena,
                          .clone = dims.clone,
                          .grab  = dims.grab,
                          .step  = dims.step,
                          .vars  = dims.vars};
  const auto fail = [this](FrontendDiagnostic diagnostic) {
    stageFailure_ = diagnostic;
    throw std::runtime_error(diagnostic.message);
  };
  // An argument a program computed (a `let` of this script) is a slot that
  // holds a reference to its value cell; one the host bound, or a literal,
  // is the cell itself.
  const auto value = [&](const CellRef slot) {
    const auto held = vm_.resultCells(slot);
    return held.empty() ? slot : held.front();
  };
  auto arguments = vlog.argumentsOf(description);
  for (auto &argument : arguments) argument = value(argument);
  constexpr std::size_t boundedExtra = 2; // amount, unit of a `within`
  const auto functor                 = arena.textOf(description);
  std::optional<IoRequestKind> kind;
  std::size_t plainArguments = 0;
  if (functor == readBytesFunctor) {
    kind           = IoRequestKind::ReadBytes;
    plainArguments = 2; // offset, max
  } else if (functor == writeBytesFunctor) {
    kind           = IoRequestKind::WriteBytes;
    plainArguments = 2; // offset, source
  } else if (functor == appendBytesFunctor) {
    kind           = IoRequestKind::AppendBytes;
    plainArguments = 1; // source
  } else if (functor == syncFunctor) {
    kind           = IoRequestKind::Sync;
    plainArguments = 0;
  }
  if (!kind || (arguments.size() != plainArguments &&
                arguments.size() != plainArguments + boundedExtra))
    fail(diagnose(vortex::frontend_codes::vqlEffectArgument,
                  "the request description is not read_bytes(offset, max), "
                  "write_bytes(offset, source), append_bytes(source) or "
                  "sync()"));
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  if (*kind == IoRequestKind::ReadBytes || *kind == IoRequestKind::WriteBytes) {
    const auto parsed = number(arguments[0], "the offset", std::nullopt);
    if (!parsed) fail(parsed.error());
    offset = *parsed;
  }
  if (*kind == IoRequestKind::ReadBytes) {
    const auto parsed = number(arguments[1], "the bound", std::nullopt);
    if (!parsed) fail(parsed.error());
    length = *parsed;
  }
  std::optional<std::chrono::milliseconds> within;
  if (arguments.size() == plainArguments + boundedExtra) {
    const auto amount =
        number(arguments[plainArguments], "the 'within' bound", std::nullopt);
    if (!amount) fail(amount.error());
    const auto unit = number(arguments[plainArguments + 1], "the 'within' unit",
                             std::nullopt);
    if (!unit) fail(unit.error());
    if (*unit == 0 || *amount > maxDeadline.count() / *unit)
      fail(diagnose(vortex::frontend_codes::vqlEffectArgument,
                    "the 'within' bound is larger than a deadline can be"));
    within = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(*amount * *unit)};
  }
  // Finished scripts' rows were returned to their callers, so their requests
  // are retired before this admission: a budget they filled is free for it.
  // This script's own earlier actions stay: `as $r` names their outcomes for
  // the statements after, and the admission budget bounds how many one script
  // can hold. They join finished_ when the script ends.
  retireFinished();
  // The bytes of a write are frozen here, before anything is staged: no
  // worker can observe a half-built buffer.
  std::vector<std::byte> frozen;
  if (*kind == IoRequestKind::WriteBytes ||
      *kind == IoRequestKind::AppendBytes) {
    const auto &source = arguments[*kind == IoRequestKind::WriteBytes ? 1 : 0];
    auto bytes         = sourceBytes(vlog.deref(source));
    if (!bytes) fail(bytes.error());
    frozen = *std::move(bytes);
  }
  const auto unstaged = [&] {
    return diagnose(vortex::frontend_codes::vqlEffectArgument,
                    "this program cannot stage that request");
  };
  std::expected<CellRef, StageFailure> spec{std::unexpected{
      StageFailure{.diagnostic = unstaged(), .refusal = std::nullopt}}};
  switch (*kind) {
  case IoRequestKind::ReadBytes: {
    auto read = ports_.stageRead(capability, offset, length);
    if (read) {
      spec = *read;
    } else {
      spec = std::unexpected{
          StageFailure{.diagnostic = read.error(), .refusal = std::nullopt}};
    }
    break;
  }
  case IoRequestKind::WriteBytes:
    if (ports_.stageWrite)
      spec = ports_.stageWrite(capability, offset, std::move(frozen));
    break;
  case IoRequestKind::AppendBytes:
    if (ports_.stageAppend)
      spec = ports_.stageAppend(capability, std::move(frozen));
    break;
  case IoRequestKind::Sync:
    if (ports_.stageSync) spec = ports_.stageSync(capability);
    break;
  }
  std::expected<vortex::SubmissionCell, FrontendDiagnostic> submission =
      std::unexpected{unstaged()};
  if (spec) {
    submission = ports_.bridge.submit(capability, *spec, within);
    // The spec is the host's decoding key, read inside submit and echoed to
    // the adapter only as a number the service forgets at the terminal; it is
    // staged and consumed within this one call, so no later stage can meet a
    // reused slot. Nothing frees it otherwise: one cell per request, for ever.
    if (const auto incarnation = arena.incarnationOf(*spec)) {
      static_cast<void>(arena.freeCell(*spec, *incarnation));
    }
  } else if (spec.error().refusal) {
    // The host's own bound said no: a value the script reads, like any
    // admission refusal, never a failed script.
    submission = ports_.bridge.wrap(ports_.host.refuse(*spec.error().refusal));
  } else {
    fail(spec.error().diagnostic);
  }
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
  latest_ = *submission;
  Admitted entry{.submission = *submission, .request = std::nullopt};
  if (const auto *accepted = std::get_if<vortex::Admitted>(&*matched))
    entry.request = accepted->request;
  current_->admitted.push_back(entry);
  if (within && current_->deadline && scriptTimeout_) {
    // A request with its own deadline is not cut short by the script's: the
    // script gets until the request's deadline plus its own timeout, so the
    // request's TimedOut outcome (a value) is what the script reads.
    if (entry.request) {
      current_->extensions.push_back(
          {*entry.request,
           now() + *within + std::min(*scriptTimeout_, maxDeadline)});
    }
  }
  if (const auto *refused = std::get_if<vortex::Refused>(&*matched))
    return {*submission, 1, refused->outcome.cell};
  return {*submission, 0, std::get<vortex::Admitted>(*matched).request.cell};
}

auto ScriptRunner::failedOpen(const Activation &activation,
                              const std::optional<std::size_t> statement) const
    -> std::optional<FrontendDiagnostic> {
  const auto &statements = activation.query.body.statements;
  if (!statement || *statement >= statements.size()) return std::nullopt;
  const auto *read = std::get_if<IoEffectStatement>(&statements[*statement]);
  if (read == nullptr || read->kind != IoEffectStatement::Kind::Read)
    return std::nullopt;
  // The open that bound the name is the latest one before the read; opens
  // evaluate once each, in statement order, so its ordinal indexes the
  // outcomes the open relation recorded.
  std::optional<std::size_t> binder;
  std::size_t ordinal = 0;
  for (std::size_t at = 0; at < *statement; ++at) {
    const auto *open = std::get_if<IoEffectStatement>(&statements[at]);
    if (open == nullptr || open->kind != IoEffectStatement::Kind::Open)
      continue;
    if (open->resultVar && *open->resultVar == read->capabilityVar)
      binder = ordinal;
    ++ordinal;
  }
  if (!binder || *binder >= activation.openOutcomes.size()) return std::nullopt;
  const auto &opened = activation.openOutcomes[*binder];
  if (opened.minted) return std::nullopt;
  const auto tag =
      vortex::ioOutcomeTag(session_.coordinator().arena(), opened.outcome);
  return diagnose(vortex::frontend_codes::vqlEffectArgument,
                  "$" + read->capabilityVar +
                      " names an open that minted no capability (its outcome "
                      "was " +
                      (tag ? std::string{vortex::toString(*tag)}
                           : std::string{"unreadable"}) +
                      "), so nothing can be read through it",
                  statement);
}

auto ScriptRunner::failureOf(const std::string_view message,
                             const std::optional<std::size_t> position) const
    -> FrontendDiagnostic {
  // A refusal the compiled program can only make while it runs (a `let` that
  // is not one value, a `join` past its bound) is an Assert whose text leads
  // with its registered code; only the codes of this frontend are honoured, so
  // no user text can pose as another language's diagnostic.
  const auto colon = message.find(": ");
  if (colon != std::string_view::npos) {
    const auto code = message.substr(0, colon);
    if (code.starts_with("VQL_IO_") && ports_.diagnostics.find(code)) {
      return diagnose(code, std::string{message.substr(colon + 2)}, position);
    }
  }
  return diagnose(vortex::frontend_codes::vqlScriptFailed, std::string{message},
                  position);
}

auto ScriptRunner::settle(const std::shared_ptr<Activation> &activation,
                          std::expected<ProgramStep, QueryExecutionError> step)
    -> ScriptRunResult {
  if (!step) {
    if (!stageFailure_) {
      stageFailure_ = failedOpen(*activation, step.error().statement);
    }
    auto diagnostic =
        stageFailure_ ? std::exchange(stageFailure_, std::nullopt).value()
                      : failureOf(step.error().message, step.error().statement);
    if (!diagnostic.position) diagnostic.position = step.error().statement;
    endActivation(*activation);
    return ScriptRunResult::failed(std::move(diagnostic));
  }
  if (auto *done = std::get_if<ProgramResult>(&*step)) {
    return finish(activation, *done);
  }
  auto &parked = std::get<ParkedProgram>(*step);
  // The request the program awaits is the cell its Await reads, so a script
  // that waits on a handle it bound earlier parks on that request.
  const auto &arena = session_.coordinator().arena();
  std::optional<vortex::RequestCell> awaited;
  if (parked.request) {
    if (const auto incarnation = arena.incarnationOf(*parked.request)) {
      const auto tag = vortex::ioCellTag(arena, *parked.request, *incarnation);
      if (tag && *tag == vortex::IoCellTag::Request)
        awaited = vortex::RequestCell{*parked.request, *incarnation};
    }
  }
  if (!awaited) {
    endActivation(*activation);
    return ScriptRunResult::failed(
        diagnose(vortex::frontend_codes::tokenStale,
                 "a parked script's request is not an admitted request",
                 parked.statement));
  }
  FrontendToken token{.activation   = activation,
                      .request      = *awaited,
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

void ScriptRunner::endActivation(Activation &activation) noexcept {
  // No caller holds rows from a script that failed or was abandoned, so its
  // records go now.
  releaseRecords(activation, true);
  releaseCaptures(activation, true);
  // Terminal requests leave the budget before the closes are admitted.
  retireAll(activation.admitted);
  closeOpened(activation);
  // A request still running (a detached read nobody awaited) is retired
  // later, when a next admission finds it terminal.
  finished_.insert(finished_.end(), activation.admitted.begin(),
                   activation.admitted.end());
  activation.admitted.clear();
}

auto ScriptRunner::sourceBytes(const CellRef source) const
    -> std::expected<std::vector<std::byte>, FrontendDiagnostic> {
  const auto refuse = [&](std::string message) {
    return std::unexpected{
        diagnose(vortex::frontend_codes::vqlWriteSource, std::move(message))};
  };
  const auto &arena      = session_.coordinator().arena();
  const auto incarnation = arena.incarnationOf(source);
  if (!incarnation) return refuse("the bytes to write are not a live cell");
  const auto tag = vortex::ioCellTag(arena, source, *incarnation);
  if (tag && *tag == vortex::IoCellTag::StaleBinding)
    return std::unexpected{diagnose(vortex::frontend_codes::vqlStaleBinding,
                                    std::string{staleBindingAdvice})};
  if (!tag) {
    // A text cell: its text is the bytes, whatever they are. A constant or a
    // value a program computed (a `let` of this script) carries its text as a
    // value, not as spooled cell text, so the rendered value comes first.
    std::string text = arena.textOf(source);
    if (text.empty()) {
      if (const auto value = session_.coordinator().core().render(source)) {
        // A number's bytes are its canonical text (R6), the rendering
        // `join` and `string()` give it, so a computed `let` is written as
        // the digits a reader sees and never as nothing.
        text = std::visit(
            [](const auto &held) -> std::string {
              if constexpr (std::same_as<std::remove_cvref_t<decltype(held)>,
                                         std::string>) {
                return held;
              } else {
                return xanadu::scalarValue(held).text;
              }
            },
            *value);
      }
    }
    std::vector<std::byte> bytes(text.size());
    std::ranges::transform(text, bytes.begin(), [](const char c) {
      return static_cast<std::byte>(c);
    });
    return bytes;
  }
  if (*tag != vortex::IoCellTag::Outcome)
    return refuse("the bytes to write are a capability or request, not a text "
                  "cell or an outcome");
  const auto gate = gated(vortex::OutcomeCell{source, *incarnation});
  if (!gate) return refuse(gate.error().message);
  if (!gate->payload)
    return refuse(std::string{"the outcome ("} +
                  std::string{vortex::toString(gate->tag)} +
                  ") retained no payload to write");
  auto lease = ports_.payloads.retain(*gate->payload);
  if (!lease) return refuse("the outcome's payload was not retained");
  auto bytes = ports_.payloads.read(
      *lease, 0, static_cast<std::size_t>(lease->range.length));
  ports_.payloads.release(lease->lease);
  if (!bytes) return refuse("the outcome's payload could not be read");
  return std::vector<std::byte>{bytes->begin(), bytes->end()};
}

auto ScriptRunner::payloadText(const vortex::OutcomeCell outcome) const
    -> std::expected<std::optional<std::string>, FrontendDiagnostic> {
  const auto gate = gated(outcome);
  if (!gate) return std::unexpected{gate.error()};
  if (!gate->payload) return std::optional<std::string>{};
  // The outcome row alone would read as "no payload"; a payload the tag
  // exposed but the host cannot read is a typed failure instead.
  const auto unreadable = [&](const std::string &why) {
    return std::unexpected{
        diagnose(vortex::frontend_codes::vqlScriptFailed,
                 "the action's payload could not be read: " + why)};
  };
  auto lease = ports_.payloads.retain(*gate->payload);
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
  if (!validUtf8(text)) return unreadable("the bytes are not valid UTF-8 text");
  return std::optional<std::string>{std::move(text)};
}

void ScriptRunner::expose(const vortex::PayloadRange &range) {
  // Recorded whoever asks, a plain query that reads a variable included: the
  // text a script exposed outlives it, so the set belongs to the session.
  if (std::ranges::find(exposures_, range) == exposures_.end())
    exposures_.push_back(range);
}

auto ScriptRunner::assess() const -> Persistability {
  // The lookup, never the builder: a script that captured nothing must not
  // start a journal just to learn that nothing is covered.
  auto *const host = captureHost();
  Persistability verdict{.exposed = exposures_.size()};
  for (const auto &range : exposures_) {
    const bool covered =
        host != nullptr && host->coordinator().spansFor(range).has_value();
    if (!covered) ++verdict.uncovered;
  }
  return verdict;
}

auto ScriptRunner::staleRefusal(std::string message)
    -> zigzag::vortex::RelationResult {
  auto refusal =
      diagnose(vortex::frontend_codes::vqlStaleBinding, std::move(message));
  // Only a running script consumes stageFailure_; a plain query would leave it
  // set for whatever fails next.
  if (current_ != nullptr) stageFailure_ = refusal;
  // A plain query has no diagnostic channel, only this text, so it names the
  // code itself.
  return std::unexpected(refusal.code + ": " + refusal.message);
}

auto ScriptRunner::payloadReleased(const zigzag::CellRef cell) const -> bool {
  return std::ranges::any_of(
      finishedRecords_, [&](const vortex::OwnedOutcome &record) {
        return record.cell.cell == cell && record.payloadReleased;
      });
}

auto ScriptRunner::staleName(const std::string &name,
                             const std::optional<std::size_t> position) const
    -> std::optional<FrontendDiagnostic> {
  const auto &bound = session_.variables();
  const auto found  = bound.find(name);
  if (found == bound.end()) return std::nullopt;
  const auto &arena = session_.coordinator().arena();
  if (!std::ranges::any_of(found->second, [&](const CellRef cell) {
        return isStaleBinding(arena, cell);
      }))
    return std::nullopt;
  return diagnose(vortex::frontend_codes::vqlStaleBinding,
                  "$" + name + ": " + std::string{staleBindingAdvice},
                  position);
}

auto ScriptRunner::namesFreedHandle(const CellRef cell) const -> bool {
  const auto &arena = session_.coordinator().arena();
  return std::ranges::any_of(bound_, [&](const auto &entry) {
    return std::ranges::any_of(entry.second, [&](const BoundHandle &handle) {
      return handle.cell == cell &&
             !arena.matches(handle.cell, handle.incarnation);
    });
  });
}

auto ScriptRunner::forget() -> ForgetReport {
  ForgetReport report;
  const auto &arena = session_.coordinator().arena();
  // Coverage, not taint, is what the persist rule tracks (owner decision Q2),
  // so which variable carries uncaptured text cannot be told: any that holds
  // a plain cell might. A runtime handle holds no text of its own and is the
  // only thing kept; reading a payload through it exposes it afresh, which the
  // rule then counts again.
  const auto holdsOnlyHandles = [&](const std::vector<CellRef> &cells) {
    return std::ranges::all_of(cells, [&](const CellRef cell) {
      const auto incarnation = arena.incarnationOf(cell);
      return incarnation && vortex::ioCellTag(arena, cell, *incarnation);
    });
  };
  const auto verdict = assess();
  if (verdict.persistable()) return report;
  std::vector<std::string> doomed;
  for (const auto &[name, cells] : session_.variables()) {
    if (!holdsOnlyHandles(cells)) doomed.push_back(name);
  }
  for (const auto &name : doomed) session_.eraseVariable(name);
  report.unbound = std::move(doomed);
  // Only the uncovered ones go: a covered exposure is already durable, and
  // keeping it keeps a later `:save` of its text honest.
  auto *const host = captureHost();
  report.exposures = std::erase_if(exposures_, [&](const auto &range) {
    return host == nullptr || !host->coordinator().spansFor(range).has_value();
  });
  return report;
}

auto ScriptRunner::staleMarker() const -> zigzag::CellRef {
  return staleMarker_;
}

void ScriptRunner::noteBindings() {
  const auto &arena = session_.coordinator().arena();
  std::map<std::string, std::vector<BoundHandle>> next;
  for (const auto &[name, cells] : session_.variables()) {
    std::vector<BoundHandle> handles;
    for (const auto cell : cells) {
      const auto incarnation = arena.incarnationOf(cell);
      if (!incarnation) continue;
      if (!vortex::ioCellTag(arena, cell, *incarnation)) continue;
      // A handle already seen under this name keeps the incarnation it had
      // then; only a newly bound one takes today's.
      const auto known = bound_.find(name);
      std::optional<BoundHandle> before;
      if (known != bound_.end()) {
        const auto found =
            std::ranges::find(known->second, cell, &BoundHandle::cell);
        if (found != known->second.end()) before = *found;
      }
      handles.push_back(before.value_or(BoundHandle{cell, *incarnation}));
    }
    if (!handles.empty()) next.emplace(name, std::move(handles));
  }
  bound_ = std::move(next);
}

void ScriptRunner::reconcileBindings() noexcept {
  try {
    const auto &arena = session_.coordinator().arena();
    for (auto it = bound_.begin(); it != bound_.end();) {
      auto bound   = session_.getVariable(it->first);
      bool changed = false;
      std::erase_if(it->second, [&](const BoundHandle &handle) {
        if (arena.matches(handle.cell, handle.incarnation)) return false;
        for (auto &cell : bound) {
          if (cell != handle.cell) continue;
          cell    = staleMarker();
          changed = true;
        }
        return true;
      });
      if (changed) session_.setVariable(it->first, std::move(bound));
      it = it->second.empty() ? bound_.erase(it) : std::next(it);
    }
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "stale session handles were not marked: {}",
                      error.what());
  }
}

auto ScriptRunner::finish(const std::shared_ptr<Activation> &activation,
                          const ProgramResult &done) -> ScriptRunResult {
  // However it ends, the script's actions move to the finished list: their
  // rows stay readable until the next admission, then they are retired. The
  // files it opened are closed: their lifetime is the script's.
  closeOpened(*activation);
  finished_.insert(finished_.end(), activation->admitted.begin(),
                   activation->admitted.end());
  activation->admitted.clear();
  // The script is over, so its payload leases go back once the final row's
  // payload text (read through them) is made; the records' cells stay until
  // the next admission, as the rows a caller holds may be a record.
  struct ReleaseOnExit {
    ScriptRunner &runner;
    Activation &activation;
    ~ReleaseOnExit() { runner.releaseRecords(activation, false); }
  } releasing{*this, *activation};
  // A finished script's captures stay until the next admission: its rows may
  // be the receipt.
  releaseCaptures(*activation, false);
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
    const auto text = payloadText(vortex::OutcomeCell{outcome, *incarnation});
    if (!text) return ScriptRunResult::failed(text.error());
    if (*text) rows.push_back(session_.coordinator().arena().makeCell(**text));
  }
  // An outcome returned as a row names its payload to whoever reads the row,
  // and an action's final row is followed by the payload text itself: both
  // expose it. A write's outcome, which retained none, exposes nothing.
  {
    const auto &arena = session_.coordinator().core().arena();
    for (const auto row : done.rows) {
      const auto incarnation = arena.incarnationOf(row);
      if (!incarnation) continue;
      const vortex::OutcomeCell named{row, *incarnation};
      if (!vortex::ioOutcomeTag(arena, named)) continue;
      if (const auto gate = gated(named); gate && gate->payload)
        expose(*gate->payload);
    }
  }
  finishedAny_ = true;
  noteBindings();
  return ScriptRunResult::completed(std::move(rows));
}

auto ScriptRunner::expireIfDue(const FrontendToken &token)
    -> std::optional<FrontendDiagnostic> {
  const auto activation =
      std::dynamic_pointer_cast<Activation>(token.activation);
  if (!activation || !activation->parked || !activation->deadline) {
    return std::nullopt;
  }
  auto due          = *activation->deadline;
  const auto &arena = session_.coordinator().arena();
  std::erase_if(activation->extensions, [&](const Activation::Extension &held) {
    return !arena.matches(held.request.cell, held.request.incarnation) ||
           ports_.host.isTerminal(held.request);
  });
  for (const auto &held : activation->extensions)
    due = std::max(due, held.until);
  if (now() < due) return std::nullopt;
  GLEDITOR_LOG_DEBUG(logCategory, "a parked script passed its deadline");
  // The script gave up: the request it waits on is asked to stop, its waiter
  // and route go, and its requests are released. Nothing resumes it.
  //
  // The awaited request is asked to stop before anything that can orphan it:
  // dropping the parked program rolls its choice points back, and a rolled-
  // back request is orphaned (never cancelled), so the stop must already be
  // on its way.
  static_cast<void>(ports_.host.cancelRequest(token.request));
  static_cast<void>(ports_.host.cancelWaiter(token.waiter));
  static_cast<void>(ports_.host.unrouteWake(token.continuation));
  parked_.erase(token.continuation);
  activation->parked.reset();
  abandonAll(activation->admitted);
  releaseRecords(*activation, true);
  // A capture past its append is retired uncertain and pinned by the
  // coordinator: never finished in the background, never resubmitted.
  releaseCaptures(*activation, true);
  closeOpened(*activation);
  return diagnose(vortex::frontend_codes::vqlScriptTimeout,
                  "the script did not finish within its overall deadline; the "
                  "request it awaited was asked to stop",
                  token.position);
}

auto ScriptRunner::resume(ScriptWake wake) -> ScriptRunResult {
  noteBindings();
  reapClosing();
  reconcileBindings();
  auto activation =
      std::dynamic_pointer_cast<Activation>(wake.token.activation);
  if (!activation || !activation->parked) {
    return ScriptRunResult::failed(diagnose(
        vortex::frontend_codes::tokenStale,
        "the token names no live script activation", wake.token.position));
  }
  if (wake.why == FrontendWake::Abandoned) {
    activation->parked.reset();
    endActivation(*activation);
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

namespace {

/// A staged write, append or sync as the runner takes it. A write over the
/// host's bound is a refusal value; every other stage error is a diagnostic.
auto staged(const std::expected<CellRef, vortex::StageReadError> &result,
            const std::string_view what)
    -> std::expected<CellRef, StageFailure> {
  if (result) return *result;
  StageFailure failure{
      .diagnostic =
          FrontendDiagnostic{
              std::string{vortex::frontend_codes::vqlEffectArgument},
              "the " + std::string{what} + " was not staged: " +
                  std::string{vortex::toString(result.error())},
              {}},
      .refusal = std::nullopt};
  if (result.error() == vortex::StageReadError::OutOfBounds)
    failure.refusal = vortex::IoErrorInfo{vortex::IoErrorCode::BudgetExceeded,
                                          vortex::fileDetailWriteBound};
  return std::unexpected{std::move(failure)};
}

} // namespace

auto fileScriptPorts(vortex::FileReadHost &host) -> ScriptIoPorts {
  return ScriptIoPorts{
      host.bridge(),
      host.host(),
      host.payloads(),
      host.diagnostics(),
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
      },
      [&host](const std::string_view path, const std::uint64_t rights)
          -> std::expected<vortex::SubmissionCell, FrontendDiagnostic> {
        return host.bridge().admitOpen(host.openSpec(path, rights));
      },
      [&host](const vortex::CapabilityCell capability) {
        return host.closeCapability(capability);
      },
      [&host](const vortex::OutcomeCell open) {
        return host.capabilityOf(open);
      },
      host.retainedPayloadBytes(),
      [&host](const vortex::CapabilityCell capability,
              const std::uint64_t offset, std::vector<std::byte> bytes) {
        return staged(host.stageWrite(capability, offset, std::move(bytes)),
                      "write");
      },
      [&host](const vortex::CapabilityCell capability,
              std::vector<std::byte> bytes) {
        return staged(host.stageAppend(capability, std::move(bytes)), "append");
      },
      [&host](const vortex::CapabilityCell capability) {
        return staged(host.stageSync(capability), "sync");
      },
      [&host]() -> vortex::CaptureHost * { return host.capture(); },
      {},
      [&host](const vortex::RequestCell close,
              const vortex::EndpointCell endpoint) {
        host.reapWhenEnded(close, endpoint);
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
    const auto *parked = std::get_if<ScriptRunResult::Parked>(&result.value);
    auto waiting       = parked != nullptr ? std::optional{parked->token}
                                           : std::optional<FrontendToken>{};
    auto woken         = runner.takeWake();
    while (!woken) {
      pumpOnce();
      woken = runner.takeWake();
      if (woken) break;
      // A script past its overall deadline is abandoned with a stable code.
      if (waiting) {
        if (auto expired = runner.expireIfDue(*waiting))
          return std::unexpected{std::move(*expired)};
      }
      idle();
    }
    result = runner.resume(*std::move(woken));
  }
}

} // namespace xanadu::vql
