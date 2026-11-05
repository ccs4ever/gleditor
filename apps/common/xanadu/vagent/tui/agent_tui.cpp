/**
 * @file agent_tui.cpp
 * @brief Terminal User Interface dashboard implementation for vagent agents.
 */
#include "common/xanadu/vagent/tui/agent_tui.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>

#include "common/ui/tui/viewport.hpp"
#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::tui {

namespace {

std::vector<std::string> tokenize(std::string_view line) {
  std::vector<std::string> tokens;
  std::string cur;
  bool inQuote = false;
  for (char c : line) {
    if (c == '"') {
      inQuote = !inQuote;
    } else if (c == ' ' && !inQuote) {
      if (!cur.empty()) {
        tokens.push_back(std::move(cur));
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) tokens.push_back(std::move(cur));
  return tokens;
}

runtime::WireValue parseWireValue(std::string_view s) {
  if (s.empty()) return runtime::WireValue::text("");
  if (s.front() == '"' && s.back() == '"' && s.size() >= 2) {
    return runtime::WireValue::text(std::string(s.substr(1, s.size() - 2)));
  }
  if (s == "true") return runtime::WireValue::boolean(true);
  if (s == "false") return runtime::WireValue::boolean(false);

  // Try int
  std::int64_t ival = 0;
  auto [p1, ec1]    = std::from_chars(s.data(), s.data() + s.size(), ival);
  if (ec1 == std::errc{} && p1 == s.data() + s.size()) {
    return runtime::WireValue::integer(ival);
  }

  // Try double
  try {
    std::size_t idx = 0;
    std::string str(s);
    double dval = std::stod(str, &idx);
    if (idx == s.size()) {
      return runtime::WireValue::real(dval);
    }
  } catch (...) {
  }

  return runtime::WireValue::text(std::string(s));
}

runtime::WireRecord parseFields(const std::vector<std::string> &args,
                                std::size_t startIdx) {
  runtime::WireRecord record;
  for (std::size_t i = startIdx; i < args.size(); ++i) {
    const auto &arg = args[i];
    auto eqPos      = arg.find('=');
    if (eqPos != std::string::npos && eqPos > 0) {
      std::string key = arg.substr(0, eqPos);
      std::string val = arg.substr(eqPos + 1);
      record.set(key, parseWireValue(val));
    }
  }
  return record;
}

} // namespace

std::string formatCellArg(const zigzag::ArenaManifold &arena,
                          zigzag::CellRef cell) {
  if (auto d = arena.asDouble(cell)) return std::format("{:.2f}", *d);
  if (auto i = arena.asInt64(cell)) return std::to_string(*i);
  auto txt = arena.textOf(cell);
  if (!txt.empty()) return txt;
  return std::format("#{}", cell);
}

std::string renderCertaintyGauge(double score, int width, bool unicode) {
  score            = std::clamp(score, 0.0, 1.0);
  const int filled = static_cast<int>(std::round(score * width));
  std::string bar;
  bar.reserve(static_cast<std::size_t>(width * 3 + 10));
  bar += '[';
  for (int i = 0; i < width; ++i) {
    if (i < filled) {
      bar += (unicode ? "\xe2\x96\x88" : "="); // █ or =
    } else {
      bar += (unicode ? "\xe2\x96\x91" : "."); // ░ or .
    }
  }
  bar += std::format("] {:3.0f}%", score * 100.0);
  return bar;
}

TuiSettings TuiSettings::fromModel(const SystemStoreModel &model) {
  TuiSettings s;
  s.borders = model.getString(xanadu::settings::kVagentTuiBorders, s.borders);
  s.historySize = static_cast<std::size_t>(
      model.getInt64(xanadu::settings::kVagentTuiHistorySize,
                     static_cast<std::int64_t>(s.historySize)));
  s.splitThreshold = static_cast<int>(model.getInt64(
      xanadu::settings::kVagentTuiSplitThreshold, s.splitThreshold));
  s.maxLogEntries  = static_cast<std::size_t>(
      model.getInt64(xanadu::settings::kVagentTuiMaxLogEntries,
                      static_cast<std::int64_t>(s.maxLogEntries)));
  s.querySolutionLimit = static_cast<std::size_t>(
      model.getInt64(xanadu::settings::kVagentTuiQuerySolutionLimit,
                     static_cast<std::int64_t>(s.querySolutionLimit)));
  s.inspectDepth = static_cast<std::size_t>(
      model.getInt64(xanadu::settings::kVagentTuiInspectDepth,
                     static_cast<std::int64_t>(s.inspectDepth)));
  return s;
}

AgentTui::AgentTui(zigzag::vortex::mesh::MeshActorRuntime &runtime,
                   runtime::AgentExecutor &executor,
                   std::shared_ptr<NCursesActor> actor, TuiSettings settings)
    : runtime_(runtime), executor_(executor), actor_(std::move(actor)),
      settings_(std::move(settings)) {
  if (!actor_) {
    actor_ = std::make_shared<NCursesActor>();
  }
  refreshActorTable();
  if (!actorRows_.empty()) {
    std::size_t initialIdx = 0;
    for (std::size_t i = 0; i < actorRows_.size(); ++i) {
      if (actorRows_[i].status == "RUNNING" ||
          actorRows_[i].status == "QUIESCENT") {
        initialIdx = i;
        break;
      }
    }
    selectedIndex_ = initialIdx;
    selectedPid_   = actorRows_[selectedIndex_].pid;
  }
  if (selectedPid_) {
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  actorViewport_.visibleRows     = 15;
  beliefViewport_.visibleRows    = 10;
  actionLogViewport_.visibleRows = 20;
}

void AgentTui::setPane(Pane pane) {
  activePane_ = pane;
  inputMode_  = InputMode::Navigation;
  if (activePane_ == Pane::Querying && selectedPid_) {
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  markDirty();
}

void AgentTui::selectActor(const identity::ActorPid &pid) {
  for (std::size_t i = 0; i < actorRows_.size(); ++i) {
    if (actorRows_[i].pid == pid) {
      selectedIndex_ = i;
      selectedPid_   = pid;
      cachedBeliefs_ = inspectBeliefs(pid);
      cachedScores_  = epistemicScores(pid);
      actorViewport_.scrollTo(static_cast<int>(i));
      markDirty();
      return;
    }
  }
}

void AgentTui::selectNextActor() {
  if (actorRows_.empty()) return;
  selectedIndex_ = (selectedIndex_ + 1) % actorRows_.size();
  selectedPid_   = actorRows_[selectedIndex_].pid;
  cachedBeliefs_ = inspectBeliefs(*selectedPid_);
  cachedScores_  = epistemicScores(*selectedPid_);
  actorViewport_.scrollTo(static_cast<int>(selectedIndex_));
  markDirty();
}

void AgentTui::selectPrevActor() {
  if (actorRows_.empty()) return;
  selectedIndex_ =
      (selectedIndex_ == 0) ? actorRows_.size() - 1 : selectedIndex_ - 1;
  selectedPid_   = actorRows_[selectedIndex_].pid;
  cachedBeliefs_ = inspectBeliefs(*selectedPid_);
  cachedScores_  = epistemicScores(*selectedPid_);
  actorViewport_.scrollTo(static_cast<int>(selectedIndex_));
  markDirty();
}

std::optional<identity::ActorPid> AgentTui::selectedActor() const {
  return selectedPid_;
}

std::vector<ActorRow> AgentTui::refreshActorTable() {
  std::vector<ActorRow> rows;
  auto pids = executor_.activeActors();

  for (const auto &pid : pids) {
    if (runtime_.isStopped(pid)) {
      ActorRow row;
      row.pid                = pid;
      row.shortPid           = pid.cursor.empty() ? pid.node : pid.cursor;
      row.name               = executor_.agentName(pid).value_or("");
      row.agentType          = executor_.agentType(pid).value_or("unknown");
      row.programRef         = executor_.programRef(pid).value_or("");
      row.status             = "STOPPED";
      row.fuelRemaining      = 0;
      row.cycleCount         = 0;
      row.scratchCells       = 0;
      row.epistemicCells     = 0;
      row.mailboxDepth       = 0;
      row.outboxCount        = 0;
      row.statDropped        = 0;
      row.statMalformed      = 0;
      row.statDeliveryFailed = 0;
      row.deadlineMs         = std::nullopt;

      rows.push_back(std::move(row));
      continue;
    }

    ActorRow row;
    row.pid      = pid;
    row.shortPid = pid.cursor.empty() ? pid.node : pid.cursor;
    if (row.shortPid.size() > 10) {
      row.shortPid = row.shortPid.substr(0, 10);
    }

    row.name       = executor_.agentName(pid).value_or("");
    row.agentType  = executor_.agentType(pid).value_or("unknown");
    row.programRef = executor_.programRef(pid).value_or("");

    if (row.programRef.empty()) {
      auto grant = runtime_.grant(pid);
      if (grant) row.programRef = grant->programRef;
    }

    if (runtime_.isStopped(pid)) {
      row.status = "STOPPED";
    } else if (runtime_.isRunning(pid)) {
      row.status = "RUNNING";
    } else if (runtime_.pending(pid) > 0) {
      row.status = "BUSY";
    } else {
      row.status = "QUIESCENT";
    }

    auto grant = runtime_.grant(pid);
    if (!grant) grant = executor_.grant(pid);
    row.fuelRemaining = grant ? grant->maxInstructions : 0;

    auto *ctx = (row.status == "STOPPED")
                    ? nullptr
                    : (executor_.context(pid) ? executor_.context(pid)
                                              : runtime_.context(pid));

    if (ctx && row.status != "STOPPED") {
      row.cycleCount   = totalTurns_;
      row.scratchCells = ctx->arena.cellCount();
    }
    row.epistemicCells = executor_.epistemicCells(pid).value_or(0);
    row.mailboxDepth   = runtime_.pending(pid);
    row.outboxCount = executor_.effectsInFlight(pid) + executor_.outstanding();
    row.statDropped = executor_.stat(pid, runtime::kStatDropped);
    row.statMalformed      = executor_.stat(pid, runtime::kStatMalformed);
    row.statDeliveryFailed = executor_.stat(pid, runtime::kStatDeliveryFailed);
    row.deadlineMs         = executor_.nextDeadline(pid);

    rows.push_back(std::move(row));
  }

  // Include the host TUI actor in the monitor list if not already present
  if (actor_) {
    const auto &hostPid = actor_->pid();
    bool foundHost      = false;
    for (const auto &r : rows) {
      if (r.pid == hostPid) {
        foundHost = true;
        break;
      }
    }
    if (!foundHost) {
      ActorRow hostRow;
      hostRow.pid      = hostPid;
      hostRow.shortPid = hostPid.cursor.empty() ? hostPid.node : hostPid.cursor;
      hostRow.name     = "tui";
      hostRow.agentType     = "NCursesActor";
      hostRow.programRef    = "native:ncurses";
      hostRow.status        = "RUNNING";
      hostRow.cycleCount    = totalTurns_;
      hostRow.fuelRemaining = 1'000'000;
      rows.push_back(std::move(hostRow));
    }
  }

  actorRows_               = rows;
  actorViewport_.totalRows = static_cast<int>(actorRows_.size());
  actorViewport_.clamp();

  if (selectedPid_) {
    bool found = false;
    for (std::size_t i = 0; i < actorRows_.size(); ++i) {
      if (actorRows_[i].pid == *selectedPid_) {
        selectedIndex_ = i;
        found          = true;
        break;
      }
    }
    if (!found && !actorRows_.empty()) {
      selectedIndex_ = 0;
      selectedPid_   = actorRows_.front().pid;
    }
  } else if (!actorRows_.empty()) {
    selectedIndex_ = 0;
    selectedPid_   = actorRows_.front().pid;
  }

  markDirty();
  return actorRows_;
}

std::vector<std::string> AgentTui::queryVlog(std::string_view query) {
  queryPagination_.activeQuery = std::string(query);
  queryPagination_.solutionBuffer.clear();
  queryPagination_.windowOffset = 0;

  if (!selectedPid_) {
    return {"Error: No actor selected for query"};
  }
  auto *ctx = executor_.context(*selectedPid_);
  if (!ctx) ctx = runtime_.context(*selectedPid_);
  if (!ctx) {
    return {"Error: Selected actor has no execution context"};
  }

  // Scoped evaluation frame: preserve arena marks and choice depth across turns
  // (P22.7)
  const auto mark = ctx->arena.mark();
  try {
    vprolog::Compiler compiler(ctx->core);
    auto compiledQuery = compiler.compileQuery(query);
    auto solutions =
        compiler.solve(compiledQuery, settings_.querySolutionLimit);

    if (solutions.empty()) {
      queryPagination_.solutionBuffer.push_back("false.");
    } else {
      for (const auto &sol : solutions) {
        if (sol.formatted.empty()) {
          queryPagination_.solutionBuffer.push_back("true.");
        } else {
          std::string line;
          for (const auto &[var, val] : sol.formatted) {
            if (!line.empty()) line += ", ";
            line += var + " = " + val;
          }
          queryPagination_.solutionBuffer.push_back(line);
        }
      }
    }
  } catch (const std::exception &e) {
    queryPagination_.solutionBuffer.push_back(
        std::format("Query error: {}", e.what()));
  }
  ctx->arena.release(mark);

  std::vector<std::string> page;
  const auto &buf = queryPagination_.solutionBuffer;
  for (std::size_t i = 0;
       i < std::min(buf.size(), QueryPaginationState::pageSize); ++i) {
    page.push_back(buf[i]);
  }
  queryResults_ = page;
  markDirty();
  return page;
}

void AgentTui::nextQueryPage() {
  const auto &buf = queryPagination_.solutionBuffer;
  if (buf.empty()) return;
  queryPagination_.windowOffset += QueryPaginationState::pageSize;
  if (queryPagination_.windowOffset >= buf.size()) {
    queryPagination_.windowOffset = 0;
  }
  std::vector<std::string> page;
  const std::size_t end =
      std::min(buf.size(),
               queryPagination_.windowOffset + QueryPaginationState::pageSize);
  for (std::size_t i = queryPagination_.windowOffset; i < end; ++i) {
    page.push_back(buf[i]);
  }
  queryResults_ = page;
  markDirty();
}

std::vector<BeliefRecord>
AgentTui::inspectBeliefs(const identity::ActorPid &pid) {
  std::vector<BeliefRecord> records;
  auto *ctx         = executor_.context(pid);
  auto agentCellOpt = executor_.agentCell(pid);
  if (!ctx || !agentCellOpt) return records;

  auto &arena               = const_cast<zigzag::ArenaManifold &>(ctx->arena);
  auto dims                 = runtime::registerDimensions(arena);
  auto values               = runtime::valueDims(ctx->core);
  zigzag::CellRef agentCell = *agentCellOpt;

  for (const auto pred : zigzag::rankAfter(arena, agentCell, dims.belief)) {
    std::string predName  = arena.textOf(pred);
    bool hasEpistemicHead = arena.linked(pred, dims.epistemic).has_value();

    for (const auto clause :
         zigzag::rankAfter(arena, pred, ctx->core.dims().clause)) {
      // Step compound arguments along +d.step after +d.grab (P22.6)
      std::string argsStr;
      if (const auto term = arena.linked(clause, ctx->core.dims().grab)) {
        for (const auto arg :
             zigzag::rank(arena, *term, ctx->core.dims().step)) {
          if (!argsStr.empty()) argsStr += ", ";
          argsStr += formatCellArg(arena, arg);
        }
      }

      bool hasSupports = false;
      for (const auto support : zigzag::rankAfter(arena, clause, dims.source)) {
        hasSupports              = true;
        std::string src          = arena.textOf(support);
        bool hasEpistemicRecords = false;
        for (const auto record :
             zigzag::rankAfter(arena, support, dims.epistemic)) {
          hasEpistemicRecords = true;
          double unc          = 0.0;
          std::string st      = "current";
          std::int64_t inc    = 0;

          if (auto c = runtime::fieldValue(arena, values, record,
                                           runtime::kObsAcquiredUncertainty)) {
            if (auto v = arena.asDouble(*c)) unc = *v;
          }
          if (auto c = runtime::fieldValue(arena, values, record,
                                           runtime::kObsStatus)) {
            st = arena.textOf(*c);
          }
          if (auto c = runtime::fieldValue(arena, values, record,
                                           runtime::kObsIncarnation)) {
            if (auto v = arena.asInt64(*c)) inc = *v;
          }

          double score = 1.0 / (1.0 + std::max(0.0, unc));
          BeliefRecord br;
          br.predicate           = predName;
          br.key                 = argsStr;
          br.source              = src;
          br.status              = st;
          br.acquiredUncertainty = unc;
          br.certaintyScore      = score;
          br.incarnation         = inc;

          br.details = std::format(
              "{}({}) from {} [unc={:.3f}, certainty={:.2f}%, {}]", predName,
              br.key.empty() ? "_" : br.key, src, unc, score * 100.0, st);
          records.push_back(std::move(br));
        }

        if (!hasEpistemicRecords) {
          BeliefRecord br;
          br.predicate           = predName;
          br.key                 = argsStr;
          br.source              = src;
          br.status              = "current";
          br.acquiredUncertainty = 0.0;
          br.certaintyScore      = 1.0;
          br.details =
              std::format("{}({}) from {} [certainty=100.0%, current]",
                          predName, br.key.empty() ? "_" : br.key, src);
          records.push_back(std::move(br));
        }
      }

      if (!hasSupports && !hasEpistemicHead) {
        BeliefRecord br;
        br.predicate      = predName;
        br.key            = argsStr;
        br.source         = "local";
        br.status         = "fact";
        br.certaintyScore = 1.0;
        br.details        = std::format("{}({}) [fact]", predName,
                                 argsStr.empty() ? "" : argsStr);
        records.push_back(std::move(br));
      }
    }
  }

  beliefViewport_.totalRows = static_cast<int>(records.size());
  beliefViewport_.clamp();
  return records;
}

std::vector<EpistemicScore>
AgentTui::epistemicScores(const identity::ActorPid &pid) {
  std::vector<EpistemicScore> scores;
  auto beliefs = inspectBeliefs(pid);
  for (const auto &b : beliefs) {
    if (b.status != "fact") {
      scores.push_back(EpistemicScore{
          .predicate      = b.predicate,
          .principal      = b.source,
          .key            = b.key,
          .uncertainty    = b.acquiredUncertainty.value_or(0.0),
          .certaintyScore = b.certaintyScore,
          .status         = b.status,
          .incarnation    = b.incarnation.value_or(0),
      });
    }
  }
  return scores;
}

std::expected<identity::ActorPid, std::string>
AgentTui::spawn(std::string_view agentType, std::string_view name,
                runtime::WireRecord args) {
  auto result = executor_.launch(agentType, name, std::move(args));
  if (!result) {
    std::string err = std::format("Spawn failed: error code {}",
                                  static_cast<int>(result.error()));
    appendActionLog(err);
    return std::unexpected(err);
  }
  identity::ActorPid pid = *result;
  appendActionLog(std::format("Spawned agent '{}' of type '{}' (pid: {})", name,
                              agentType, pid.cursor));
  refreshActorTable();
  selectActor(pid);
  markDirty();
  return pid;
}

std::expected<void, std::string>
AgentTui::injectPercept(const identity::ActorPid &pid,
                        std::string_view eventType,
                        runtime::WireRecord fields) {
  executor_.percept(pid, std::string(eventType), std::move(fields));
  appendActionLog(
      std::format("Injected percept '{}' into pid {}", eventType, pid.cursor));
  markDirty();
  return {};
}

std::expected<void, std::string>
AgentTui::injectEvent(const identity::ActorPid &pid, std::string_view eventType,
                      runtime::WireRecord fields) {
  runtime::Envelope envelope;
  envelope.type   = std::string(eventType);
  envelope.fields = std::move(fields);
  auto res        = executor_.deliver(pid, std::move(envelope));
  if (!res) {
    std::string err = std::format("Event delivery failed: error code {}",
                                  static_cast<int>(res.error()));
    appendActionLog(err);
    return std::unexpected(err);
  }
  appendActionLog(
      std::format("Injected event '{}' into pid {}", eventType, pid.cursor));
  markDirty();
  return {};
}

std::size_t AgentTui::step(std::size_t turns) {
  executor_.tick();
  std::size_t ran = executor_.pumpUntilIdle(turns);
  totalTurns_ += ran;
  appendActionLog(
      std::format("Stepped {} turn(s) (total: {})", ran, totalTurns_));
  refreshActorTable();
  if (selectedPid_) {
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  markDirty();
  return ran;
}

std::size_t AgentTui::advanceClock(std::chrono::milliseconds ms) {
  auto ran = executor_.advanceSimulatedTime(ms);
  totalTurns_ += ran;
  appendActionLog(
      std::format("Advanced clock by {}ms, ran {} turn(s)", ms.count(), ran));
  refreshActorTable();
  if (selectedPid_) {
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  markDirty();
  return ran;
}

std::expected<void, std::string>
AgentTui::upgrade(const identity::ActorPid &pid,
                  std::string_view newProgramRef) {
  auto prog = executor_.resolveProgram(newProgramRef);
  if (!prog) {
    std::string err = std::format("Program ref '{}' not found", newProgramRef);
    appendActionLog(err);
    return std::unexpected(err);
  }
  auto compiledProg = std::dynamic_pointer_cast<runtime::CompiledProgram>(prog);
  if (!compiledProg) {
    std::string err = "Program cannot be cast to CompiledProgram";
    appendActionLog(err);
    return std::unexpected(err);
  }

  zigzag::vortex::mesh::ActorGrant grant{std::string(newProgramRef),
                                         std::string(newProgramRef),
                                         1'000'000,
                                         1U << 22,
                                         32768,
                                         1U << 12,
                                         1U << 20,
                                         std::chrono::system_clock::now() +
                                             std::chrono::hours(24),
                                         true};

  auto res = executor_.upgrade(pid, compiledProg, grant);
  if (!res) {
    std::string err =
        std::format("Live upgrade failed: {}", toString(res.error()));
    appendActionLog(err);
    return std::unexpected(err);
  }
  appendActionLog(std::format("Upgraded actor {} to programRef '{}'",
                              pid.cursor, newProgramRef));
  refreshActorTable();
  markDirty();
  return {};
}

std::expected<void, std::string> AgentTui::stop(const identity::ActorPid &pid) {
  if (actor_ && pid == actor_->pid()) {
    appendActionLog(std::format("Stopping TUI actor {}", pid.cursor));
    requestExit();
    return {};
  }
  auto res = runtime_.stop(pid);
  if (!res) {
    std::string err = "Failed to stop actor";
    appendActionLog(err);
    return std::unexpected(err);
  }
  appendActionLog(std::format("Stopped actor {}", pid.cursor));
  refreshActorTable();
  markDirty();
  return {};
}

void AgentTui::appendActionLog(std::string entry) {
  if (actionLog_.size() >= settings_.maxLogEntries) {
    actionLog_.erase(actionLog_.begin(),
                     actionLog_.begin() +
                         (actionLog_.size() - settings_.maxLogEntries + 1));
  }
  actionLog_.push_back(std::move(entry));
  actionLogViewport_.totalRows = static_cast<int>(actionLog_.size());
  actionLogViewport_.scrollTo(static_cast<int>(actionLog_.size()) - 1);
  markDirty();
}

std::optional<identity::ActorPid>
AgentTui::resolveTarget(std::string_view targetStr) const {
  if (targetStr == "self" || targetStr == "selected") {
    return selectedPid_;
  }
  for (const auto &r : actorRows_) {
    if (r.shortPid == targetStr || r.name == targetStr ||
        r.pid.cursor == targetStr) {
      return r.pid;
    }
  }
  return std::nullopt;
}

std::expected<std::string, std::string>
AgentTui::executeCommand(std::string_view cmdLine) {
  auto tokens = tokenize(cmdLine);
  if (tokens.empty()) return "";

  const auto &cmd = tokens[0];
  if (cmd == "spawn") {
    if (tokens.size() < 3) {
      return std::unexpected("Usage: spawn <type> <name> [arg=val ...]");
    }
    auto args = parseFields(tokens, 3);
    auto res  = spawn(tokens[1], tokens[2], std::move(args));
    if (!res) return std::unexpected(res.error());
    return std::format("Spawned {} (pid: {})", tokens[2], res->cursor);
  }
  if (cmd == "percept") {
    if (tokens.size() < 3) {
      return std::unexpected("Usage: percept <target> <type> [key=val ...]");
    }
    auto target = resolveTarget(tokens[1]);
    if (!target) return std::unexpected("Target actor not found");
    auto fields = parseFields(tokens, 3);
    auto res    = injectPercept(*target, tokens[2], std::move(fields));
    if (!res) return std::unexpected(res.error());
    return std::format("Percept {} sent", tokens[2]);
  }
  if (cmd == "send") {
    if (tokens.size() < 3) {
      return std::unexpected("Usage: send <target> <type> [key=val ...]");
    }
    auto target = resolveTarget(tokens[1]);
    if (!target) return std::unexpected("Target actor not found");
    auto fields = parseFields(tokens, 3);
    auto res    = injectEvent(*target, tokens[2], std::move(fields));
    if (!res) return std::unexpected(res.error());
    return std::format("Event {} sent", tokens[2]);
  }
  if (cmd == "step") {
    std::size_t turns = 1;
    if (tokens.size() > 1) {
      (void)std::from_chars(tokens[1].data(),
                            tokens[1].data() + tokens[1].size(), turns);
    }
    auto ran = step(turns);
    return std::format("Stepped {} turns", ran);
  }
  if (cmd == "advance") {
    long long ms = 1000;
    if (tokens.size() > 1) {
      std::string s = tokens[1];
      if (s.ends_with("ms"))
        s.resize(s.size() - 2);
      else if (s.ends_with("s")) {
        s.resize(s.size() - 1);
        ms = std::stoll(s) * 1000;
      } else {
        ms = std::stoll(s);
      }
    }
    advanceClock(std::chrono::milliseconds(ms));
    return std::format("Advanced {}ms", ms);
  }
  if (cmd == "upgrade") {
    if (tokens.size() < 3) {
      return std::unexpected("Usage: upgrade <target> <programRef>");
    }
    auto target = resolveTarget(tokens[1]);
    if (!target) return std::unexpected("Target actor not found");
    auto res = upgrade(*target, tokens[2]);
    if (!res) return std::unexpected(res.error());
    return std::format("Upgraded to {}", tokens[2]);
  }
  if (cmd == "stop") {
    if (tokens.size() < 2) {
      return std::unexpected("Usage: stop <target>");
    }
    auto target = resolveTarget(tokens[1]);
    if (!target) return std::unexpected("Target actor not found");
    auto res = stop(*target);
    if (!res) return std::unexpected(res.error());
    return "Actor stopped";
  }
  if (cmd == "query") {
    if (tokens.size() < 2) {
      return std::unexpected("Usage: query <vlog_goal>");
    }
    std::string q = std::string(cmdLine.substr(6));
    auto sols     = queryVlog(q);
    std::string summary;
    for (const auto &s : sols) {
      if (!summary.empty()) summary += "; ";
      summary += s;
    }
    return summary;
  }
  if (cmd == "clear") {
    actionLog_.clear();
    actionLogViewport_.totalRows = 0;
    actionLogViewport_.clamp();
    markDirty();
    return "Action log cleared";
  }
  if (cmd == "help") {
    return "Commands: spawn <type> <name>, percept <target> <evt>, send "
           "<target> <evt>, "
           "step [N], advance <ms>, upgrade <target> <ref>, stop <target>, "
           "query <vlog>, clear, quit";
  }
  if (cmd == "quit" || cmd == "exit") {
    requestExit();
    return "Exiting...";
  }

  return std::unexpected(std::format(
      "Unknown command: '{}'. Type 'help' for available commands.", cmd));
}

void AgentTui::handleInput(const KeyEvent &event) {
  const char c = event.charValue();

  // Global Pane Switching via F1-F3 or Alt+1..3
  if (event.code == KeyCode::F1 ||
      (event.alt && (c == '1' || event.codepoint == U'1'))) {
    setInputMode(InputMode::Navigation);
    setPane(Pane::Monitoring);
    return;
  }
  if (event.code == KeyCode::F2 ||
      (event.alt && (c == '2' || event.codepoint == U'2'))) {
    setInputMode(InputMode::Navigation);
    setPane(Pane::Querying);
    return;
  }
  if (event.code == KeyCode::F3 ||
      (event.alt && (c == '3' || event.codepoint == U'3'))) {
    setInputMode(InputMode::Navigation);
    setPane(Pane::Orchestrating);
    return;
  }

  // Global Tab / Shift-Tab cycling across panes
  if (event.code == KeyCode::Tab || c == '\t' ||
      event.code == KeyCode::Backtab) {
    setInputMode(InputMode::Navigation);
    if (event.shift || event.code == KeyCode::Backtab) {
      if (activePane_ == Pane::Monitoring)
        setPane(Pane::Orchestrating);
      else if (activePane_ == Pane::Querying)
        setPane(Pane::Monitoring);
      else
        setPane(Pane::Querying);
    } else {
      if (activePane_ == Pane::Monitoring)
        setPane(Pane::Querying);
      else if (activePane_ == Pane::Querying)
        setPane(Pane::Orchestrating);
      else
        setPane(Pane::Monitoring);
    }
    return;
  }

  // NAVIGATION MODE DISPATCH (Invariant T4)
  if (inputMode_ == InputMode::Navigation) {
    if (event.code == KeyCode::Escape) {
      if (activePane_ == Pane::Monitoring) {
        requestExit();
      } else {
        setPane(Pane::Monitoring);
      }
      return;
    }

    if (event.code == KeyCode::Char) {
      if (c == '1') {
        setPane(Pane::Monitoring);
        return;
      }
      if (c == '2') {
        setPane(Pane::Querying);
        return;
      }
      if (c == '3') {
        setPane(Pane::Orchestrating);
        return;
      }
      if (c == 'q') {
        requestExit();
        return;
      }
      if (c == 's') {
        step(1);
        return;
      }
      if (c == 'r') {
        refreshActorTable();
        return;
      }
      if (c == 'i' || c == ':') {
        if (activePane_ == Pane::Querying ||
            activePane_ == Pane::Orchestrating) {
          setInputMode(InputMode::Edit);
        }
        return;
      }
      if (c == 'j') {
        if (activePane_ == Pane::Monitoring)
          selectNextActor();
        else if (activePane_ == Pane::Querying)
          beliefViewport_.scrollDown(1);
        else if (activePane_ == Pane::Orchestrating)
          actionLogViewport_.scrollDown(1);
        markDirty();
        return;
      }
      if (c == 'k') {
        if (activePane_ == Pane::Monitoring)
          selectPrevActor();
        else if (activePane_ == Pane::Querying)
          beliefViewport_.scrollUp(1);
        else if (activePane_ == Pane::Orchestrating)
          actionLogViewport_.scrollUp(1);
        markDirty();
        return;
      }
      if (c == ';') {
        if (activePane_ == Pane::Querying) {
          nextQueryPage();
        }
        return;
      }
      if (activePane_ == Pane::Querying) {
        setInputMode(InputMode::Edit);
        if (c != '\0') queryInput_.push_back(c);
        markDirty();
        return;
      }
      if (activePane_ == Pane::Orchestrating) {
        setInputMode(InputMode::Edit);
        if (c != '\0') commandInput_.push_back(c);
        markDirty();
        return;
      }
    }

    if (event.code == KeyCode::Enter) {
      if (activePane_ == Pane::Querying || activePane_ == Pane::Orchestrating) {
        setInputMode(InputMode::Edit);
      }
      return;
    }

    if (event.code == KeyCode::Down) {
      if (activePane_ == Pane::Monitoring)
        selectNextActor();
      else if (activePane_ == Pane::Querying)
        beliefViewport_.scrollDown(1);
      else if (activePane_ == Pane::Orchestrating)
        actionLogViewport_.scrollDown(1);
      markDirty();
      return;
    }
    if (event.code == KeyCode::Up) {
      if (activePane_ == Pane::Monitoring)
        selectPrevActor();
      else if (activePane_ == Pane::Querying)
        beliefViewport_.scrollUp(1);
      else if (activePane_ == Pane::Orchestrating)
        actionLogViewport_.scrollUp(1);
      markDirty();
      return;
    }
    if (event.code == KeyCode::PageDown) {
      if (activePane_ == Pane::Querying)
        beliefViewport_.pageDown();
      else if (activePane_ == Pane::Orchestrating)
        actionLogViewport_.pageDown();
      markDirty();
      return;
    }
    if (event.code == KeyCode::PageUp) {
      if (activePane_ == Pane::Querying)
        beliefViewport_.pageUp();
      else if (activePane_ == Pane::Orchestrating)
        actionLogViewport_.pageUp();
      markDirty();
      return;
    }
    return;
  }

  // EDIT MODE DISPATCH (Invariant T4)
  if (inputMode_ == InputMode::Edit) {
    if (event.code == KeyCode::Escape) {
      if (activePane_ == Pane::Querying) {
        if (!queryInput_.empty()) {
          queryInput_.clear();
        } else {
          setPane(Pane::Monitoring);
        }
      } else if (activePane_ == Pane::Orchestrating) {
        if (!commandInput_.empty()) {
          commandInput_.clear();
        } else {
          setPane(Pane::Monitoring);
        }
      }
      setInputMode(InputMode::Navigation);
      markDirty();
      return;
    }

    if (activePane_ == Pane::Querying) {
      if (event.code == KeyCode::Enter) {
        if (!queryInput_.empty()) {
          queryHistory_.push_back(queryInput_);
          queryHistoryIndex_ = queryHistory_.size();
          queryDraft_.clear();
          (void)queryVlog(queryInput_);
          queryInput_.clear();
        }
        // Invariant T4: Retain EDIT mode upon Enter
        markDirty();
        return;
      }
      if (event.code == KeyCode::Backspace) {
        if (!queryInput_.empty()) {
          queryInput_.pop_back();
          markDirty();
        }
        return;
      }
      if (event.code == KeyCode::Up) {
        if (!queryHistory_.empty()) {
          if (queryHistoryIndex_ == queryHistory_.size()) {
            queryDraft_ = queryInput_;
          }
          if (queryHistoryIndex_ > 0) {
            queryHistoryIndex_--;
            queryInput_ = queryHistory_[queryHistoryIndex_];
            markDirty();
          }
        }
        return;
      }
      if (event.code == KeyCode::Down) {
        if (!queryHistory_.empty() &&
            queryHistoryIndex_ < queryHistory_.size()) {
          queryHistoryIndex_++;
          if (queryHistoryIndex_ == queryHistory_.size()) {
            queryInput_ = queryDraft_;
          } else {
            queryInput_ = queryHistory_[queryHistoryIndex_];
          }
          markDirty();
        }
        return;
      }
      if (event.code == KeyCode::Char) {
        if (c != '\0') {
          queryInput_.push_back(c);
          markDirty();
        }
        return;
      }
    } else if (activePane_ == Pane::Orchestrating) {
      if (event.code == KeyCode::Enter) {
        if (!commandInput_.empty()) {
          commandHistory_.push_back(commandInput_);
          commandHistoryIndex_ = commandHistory_.size();
          commandDraft_.clear();
          const std::string cmd = std::move(commandInput_);
          commandInput_.clear();
          appendActionLog(std::format("> {}", cmd));
          auto res = executeCommand(cmd);
          if (res) {
            if (!res->empty()) appendActionLog(*res);
          } else {
            appendActionLog(std::format("[ERROR: {}]", res.error()));
          }
        }
        // Invariant T4: Retain EDIT mode upon Enter
        markDirty();
        return;
      }
      if (event.code == KeyCode::Backspace) {
        if (!commandInput_.empty()) {
          commandInput_.pop_back();
          markDirty();
        }
        return;
      }
      if (event.code == KeyCode::Up) {
        if (!commandHistory_.empty()) {
          if (commandHistoryIndex_ == commandHistory_.size()) {
            commandDraft_ = commandInput_;
          }
          if (commandHistoryIndex_ > 0) {
            commandHistoryIndex_--;
            commandInput_ = commandHistory_[commandHistoryIndex_];
            markDirty();
          }
        }
        return;
      }
      if (event.code == KeyCode::Down) {
        if (!commandHistory_.empty() &&
            commandHistoryIndex_ < commandHistory_.size()) {
          commandHistoryIndex_++;
          if (commandHistoryIndex_ == commandHistory_.size()) {
            commandInput_ = commandDraft_;
          } else {
            commandInput_ = commandHistory_[commandHistoryIndex_];
          }
          markDirty();
        }
        return;
      }
      if (event.code == KeyCode::Char) {
        if (c != '\0') {
          commandInput_.push_back(c);
          markDirty();
        }
        return;
      }
    }
  }
}

void AgentTui::tick() {
  refreshActorTable();
  while (auto ev = actor_->pollInput()) {
    handleInput(*ev);
  }
  render();
}

void AgentTui::renderHeader() {
  int w = actor_->dimensions().width;
  actor_->print(0, 0, std::string(w, ' '), Style{.reverse = true});
  actor_->print(1, 0, "VAGENT DASHBOARD", Style{.bold = true, .reverse = true});

  Style sMon = (activePane_ == Pane::Monitoring)
                   ? Style{.fg = Color::Cyan, .bold = true, .reverse = true}
                   : Style{.reverse = true};
  Style sQry = (activePane_ == Pane::Querying)
                   ? Style{.fg = Color::Cyan, .bold = true, .reverse = true}
                   : Style{.reverse = true};
  Style sOrc = (activePane_ == Pane::Orchestrating)
                   ? Style{.fg = Color::Cyan, .bold = true, .reverse = true}
                   : Style{.reverse = true};

  actor_->print(18, 0, "[1: Monitoring]", sMon);
  actor_->print(35, 0, "[2: Querying]", sQry);
  actor_->print(50, 0, "[3: Orchestrating]", sOrc);

  // Mode badge (Invariant T4)
  const std::string modeStr =
      (inputMode_ == InputMode::Navigation) ? "[NAV]" : "[EDIT]";
  Style modeStyle =
      (inputMode_ == InputMode::Navigation)
          ? Style{.fg = Color::Green, .bold = true, .reverse = true}
          : Style{.fg = Color::Yellow, .bold = true, .reverse = true};
  actor_->print(70, 0, modeStr, modeStyle);

  if (w > 85) {
    actor_->print(78, 0, std::format("Turns: {}", totalTurns_),
                  Style{.reverse = true});
  }
  actor_->print(w - 10, 0, "[q: Quit]", Style{.reverse = true});
}

void AgentTui::renderMonitoringPane(int top, int bottom) {
  int w = actor_->dimensions().width;
  int h = bottom - top;
  if (h <= 0) return;

  // Responsive Master-Detail Layout (P22.6) for width >= 110 cols
  if (w >= settings_.splitThreshold) {
    const int masterW = 38;
    const int detailW = w - masterW - 1;

    // Left Panel: Master Actor Table
    actor_->drawBox(0, top, masterW, h, BorderStyle::UnicodeRounded, {},
                    "Live Actor Table");
    actor_->print(2, top + 1, "PID", Style{.fg = Color::Cyan, .bold = true});
    actor_->print(14, top + 1, "TYPE", Style{.fg = Color::Cyan, .bold = true});
    actor_->print(28, top + 1, "STATUS",
                  Style{.fg = Color::Cyan, .bold = true});
    actor_->drawHLine(1, top + 2, masterW - 2);

    const int maxVisible       = h - 4;
    actorViewport_.visibleRows = maxVisible;
    actorViewport_.clamp();

    for (int i = 0; i < maxVisible && (actorViewport_.topIndex + i) <
                                          static_cast<int>(actorRows_.size());
         ++i) {
      const int rowIdx = actorViewport_.topIndex + i;
      const auto &row  = actorRows_[rowIdx];
      const bool isSel = (selectedPid_ && row.pid == *selectedPid_);
      const int y      = top + 3 + i;

      Style rowStyle =
          isSel ? Style{.fg = Color::Cyan, .bold = true, .reverse = true}
                : Style{};
      actor_->print(1, y, isSel ? ">" : " ", rowStyle);
      actor_->print(2, y, row.shortPid, rowStyle);

      std::string t = row.agentType;
      if (t.size() > 12) t = t.substr(0, 12);
      actor_->print(14, y, t, rowStyle);

      Color statusCol = Color::Green;
      if (row.status == "STOPPED")
        statusCol = Color::Red;
      else if (row.status == "BUSY")
        statusCol = Color::Yellow;
      else if (row.status == "RUNNING")
        statusCol = Color::Cyan;

      Style stStyle = rowStyle;
      if (!isSel) stStyle.fg = statusCol;
      actor_->print(28, y, row.status, stStyle);
    }

    // Right Panel: Detailed Metrics Card
    actor_->drawBox(masterW, top, detailW, h, BorderStyle::UnicodeRounded, {},
                    "Actor Details");
    if (selectedPid_) {
      int dy = top + 1;
      actor_->print(masterW + 2, dy++,
                    std::format("Node: {} | Cursor: {}", selectedPid_->node,
                                selectedPid_->cursor),
                    Style{.bold = true});
      if (selectedIndex_ < actorRows_.size()) {
        const auto &r = actorRows_[selectedIndex_];
        actor_->print(masterW + 2, dy++,
                      std::format("Name: {}  Type: {}  Status: {}",
                                  r.name.empty() ? "(unnamed)" : r.name,
                                  r.agentType, r.status));
        actor_->print(masterW + 2, dy++,
                      std::format("Program Ref: {}", r.programRef));
        actor_->print(masterW + 2, dy++,
                      std::format("Cycles: {} | Fuel Remaining: {}",
                                  r.cycleCount, r.fuelRemaining));
        actor_->print(masterW + 2, dy++,
                      std::format("Scratch Cells: {} | Epistemic Cells: {}",
                                  r.scratchCells, r.epistemicCells));
        actor_->print(masterW + 2, dy++,
                      std::format("Mailbox Depth: {} | Outbox Count: {}",
                                  r.mailboxDepth, r.outboxCount));
        actor_->print(
            masterW + 2, dy++,
            std::format("Stats: dropped={} malformed={} deliveryFailed={}",
                        r.statDropped, r.statMalformed, r.statDeliveryFailed));
        actor_->print(masterW + 2, dy++,
                      std::format("Next Deadline: {}",
                                  r.deadlineMs
                                      ? std::format("{}ms", *r.deadlineMs)
                                      : "none"));

        dy++;
        actor_->drawHLine(masterW + 1, dy++, detailW - 2);
        actor_->print(masterW + 2, dy++,
                      "Active Beliefs & Epistemic Cert:", Style{.bold = true});
        const int maxBeliefPreview = h - (dy - top) - 1;
        for (int b = 0; b < maxBeliefPreview &&
                        b < static_cast<int>(cachedBeliefs_.size());
             ++b) {
          const auto &rec         = cachedBeliefs_[b];
          const std::string gauge = renderCertaintyGauge(rec.certaintyScore, 8);
          std::string bStr = std::format("{} {}({}) [{}]", gauge, rec.predicate,
                                         rec.key, rec.source);
          if (static_cast<int>(bStr.size()) > detailW - 4) {
            bStr = bStr.substr(0, static_cast<std::size_t>(detailW - 4));
          }
          actor_->print(masterW + 2, dy++, bStr);
        }
      }
    } else {
      actor_->print(masterW + 2, top + 1, "No actor selected.");
    }
    return;
  }

  // Single full-width layout for width < 110
  actor_->drawBox(0, top, w, h, BorderStyle::UnicodeRounded, {},
                  "Live Actor Table");
  int y = top + 1;
  actor_->print(2, y, "PID", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(14, y, "TYPE/NAME", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(32, y, "PROGRAM REF", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(50, y, "STATUS", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(60, y, "MBOX", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(67, y, "OUTBOX", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(75, y, "FUEL", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(85, y, "CELLS", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(95, y, "DEADLINE", Style{.fg = Color::Cyan, .bold = true});

  actor_->drawHLine(1, y + 1, w - 2);
  y += 2;

  const int maxRows          = h - 4;
  actorViewport_.visibleRows = maxRows;
  actorViewport_.clamp();

  for (int i = 0; i < maxRows && (actorViewport_.topIndex + i) <
                                     static_cast<int>(actorRows_.size());
       ++i) {
    const int rowIdx    = actorViewport_.topIndex + i;
    const auto &row     = actorRows_[rowIdx];
    const bool selected = (selectedPid_ && row.pid == *selectedPid_);
    Style style         = selected ? Style{.reverse = true} : Style{};

    Color statusCol = Color::Green;
    if (row.status == "STOPPED")
      statusCol = Color::Red;
    else if (row.status == "BUSY")
      statusCol = Color::Yellow;
    else if (row.status == "RUNNING")
      statusCol = Color::Cyan;

    actor_->print(2, y + i, row.shortPid, style);

    std::string typeName = row.agentType;
    if (!row.name.empty() && row.name != row.agentType) {
      if (row.agentType.size() + row.name.size() + 3 <= 16) {
        typeName += " (" + row.name + ")";
      }
    }
    if (typeName.size() > 16) typeName = typeName.substr(0, 16);
    actor_->print(14, y + i, typeName, style);

    std::string pRef = row.programRef;
    if (pRef.size() > 16) pRef = pRef.substr(0, 16);
    actor_->print(32, y + i, pRef, style);

    Style stStyle = style;
    if (!selected) stStyle.fg = statusCol;
    actor_->print(50, y + i, row.status, stStyle);

    actor_->print(60, y + i, std::to_string(row.mailboxDepth), style);
    actor_->print(67, y + i, std::to_string(row.outboxCount), style);

    std::string fuelStr = std::to_string(row.fuelRemaining);
    if (row.fuelRemaining >= 1'000'000) {
      fuelStr =
          std::format("{:.1f}M", static_cast<double>(row.fuelRemaining) / 1e6);
    }
    actor_->print(75, y + i, fuelStr, style);
    actor_->print(85, y + i,
                  std::format("{}c/{}e", row.scratchCells, row.epistemicCells),
                  style);

    std::string dl =
        row.deadlineMs ? std::format("{}ms", *row.deadlineMs) : "none";
    actor_->print(95, y + i, dl, style);
  }
}

void AgentTui::renderQueryingPane(int top, int bottom) {
  int w = actor_->dimensions().width;
  int h = bottom - top;
  if (h <= 0) return;

  int beliefsHeight = std::max(6, (h * 3) / 5);
  actor_->drawBox(0, top, w, beliefsHeight, BorderStyle::UnicodeRounded, {},
                  "Agent Beliefs & Epistemic Certainty Scores (P18/P19)");

  int y = top + 1;
  actor_->print(2, y, "PREDICATE / KEY",
                Style{.fg = Color::Cyan, .bold = true});
  actor_->print(30, y, "SOURCE", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(48, y, "CERTAINTY GAUGE",
                Style{.fg = Color::Cyan, .bold = true});
  actor_->print(70, y, "STATUS", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(82, y, "INCARNATION", Style{.fg = Color::Cyan, .bold = true});

  actor_->drawHLine(1, y + 1, w - 2);
  y += 2;

  const int maxB              = beliefsHeight - 4;
  beliefViewport_.visibleRows = maxB;
  beliefViewport_.clamp();

  for (int i = 0; i < maxB && (beliefViewport_.topIndex + i) <
                                  static_cast<int>(cachedBeliefs_.size());
       ++i) {
    const int bIdx = beliefViewport_.topIndex + i;
    const auto &b  = cachedBeliefs_[bIdx];
    std::string predKey =
        b.key.empty() ? b.predicate : std::format("{}({})", b.predicate, b.key);
    if (predKey.size() > 26) predKey = predKey.substr(0, 26);

    actor_->print(2, y + i, predKey);
    actor_->print(30, y + i, b.source.substr(0, 16));

    // Certainty bar gauge (P22.6)
    const std::string gauge = renderCertaintyGauge(b.certaintyScore, 10);
    Color scoreCol          = Color::Green;
    if (b.certaintyScore < 0.6)
      scoreCol = Color::Red;
    else if (b.certaintyScore < 0.85)
      scoreCol = Color::Yellow;
    actor_->print(48, y + i, gauge, Style{.fg = scoreCol, .bold = true});

    actor_->print(70, y + i, b.status);
    actor_->print(82, y + i,
                  b.incarnation ? std::to_string(*b.incarnation) : "-");
  }

  if (cachedBeliefs_.empty()) {
    actor_->print(
        2, y,
        "No beliefs or epistemic observations registered for this actor.");
  }

  // Vlog Query REPL Box
  int replTop = top + beliefsHeight;
  int replH   = bottom - replTop;
  if (replH > 3) {
    actor_->drawBox(0, replTop, w, replH, BorderStyle::UnicodeRounded, {},
                    "Vlog Deductive Query REPL [; for next page]");

    const std::string prompt =
        "?- " + queryInput_ + (inputMode_ == InputMode::Edit ? "_" : "");
    actor_->print(2, replTop + 1, prompt, Style{.bold = true});

    actor_->drawHLine(1, replTop + 2, w - 2);

    int ry                      = replTop + 3;
    const std::size_t totalSols = queryPagination_.solutionBuffer.size();
    actor_->print(
        2, ry++,
        std::format("Solutions (page {} of {}):",
                    totalSols == 0 ? 0
                                   : (queryPagination_.windowOffset /
                                          QueryPaginationState::pageSize +
                                      1),
                    totalSols == 0
                        ? 0
                        : (totalSols + QueryPaginationState::pageSize - 1) /
                              QueryPaginationState::pageSize),
        Style{.fg = Color::Yellow});

    const int maxRes = replH - 5;
    for (int i = 0; i < maxRes && i < static_cast<int>(queryResults_.size());
         ++i) {
      actor_->print(4, ry + i, queryResults_[i], Style{.fg = Color::Green});
    }
  }
}

void AgentTui::renderOrchestratingPane(int top, int bottom) {
  int w = actor_->dimensions().width;
  int h = bottom - top;
  if (h <= 0) return;

  const int promptH   = 4;
  const int logHeight = std::max(4, h - promptH);

  actor_->drawBox(0, top, w, logHeight, BorderStyle::UnicodeRounded, {},
                  "Orchestration Action Log");

  int y                          = top + 1;
  const int maxLog               = logHeight - 2;
  actionLogViewport_.visibleRows = maxLog;
  actionLogViewport_.totalRows   = static_cast<int>(actionLog_.size());
  actionLogViewport_.clamp();

  for (int i = 0; i < maxLog && (actionLogViewport_.topIndex + i) <
                                    static_cast<int>(actionLog_.size());
       ++i) {
    const int entryIdx = actionLogViewport_.topIndex + i;
    const auto &entry  = actionLog_[entryIdx];
    Style st;
    if (entry.find("[ERROR") != std::string::npos)
      st.fg = Color::Red;
    else if (entry.starts_with(">"))
      st.fg = Color::Cyan;
    else
      st.fg = Color::White;

    actor_->print(2, y + i, entry, st);
  }

  // Command prompt box
  int promptTop = top + logHeight;
  actor_->drawBox(0, promptTop, w, promptH, BorderStyle::UnicodeRounded, {},
                  "Command Input");
  const std::string prompt =
      "command> " + commandInput_ + (inputMode_ == InputMode::Edit ? "_" : "");
  actor_->print(2, promptTop + 1, prompt, Style{.bold = true});
  actor_->print(2, promptTop + 2,
                "spawn <type> <name> | percept <pid> <evt> | send <pid> <evt> "
                "| step [N] | advance <ms> | upgrade <pid> <ref> | stop <pid>",
                Style{.dim = true});
}

void AgentTui::renderStatusBar() {
  int w = actor_->dimensions().width;
  int h = actor_->dimensions().height;
  actor_->print(0, h - 1, std::string(w, ' '), Style{.reverse = true});

  std::string status = std::format(
      " Mode: {} │ Actors: {} │ Selected: {} │ [Tab]: Switch Pane │ [i]: Edit "
      "│ [s]: Step │ [r]: Refresh │ [q]: Quit",
      inputMode_ == InputMode::Navigation ? "NAV" : "EDIT", actorRows_.size(),
      selectedPid_ ? selectedPid_->cursor : "none");

  if (static_cast<int>(status.size()) > w - 2) {
    status = status.substr(0, static_cast<std::size_t>(w - 2));
  }
  actor_->print(1, h - 1, status, Style{.bold = true, .reverse = true});
}

void AgentTui::render() {
  actor_->clear();
  renderHeader();

  int top    = 1;
  int bottom = actor_->dimensions().height - 1;

  switch (activePane_) {
  case Pane::Monitoring:
    renderMonitoringPane(top, bottom);
    break;
  case Pane::Querying:
    renderQueryingPane(top, bottom);
    break;
  case Pane::Orchestrating:
    renderOrchestratingPane(top, bottom);
    break;
  }

  renderStatusBar();
  actor_->refresh();
  clearDirty();
}

} // namespace xanadu::vagent::tui
