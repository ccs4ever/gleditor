/**
 * @file agent_tui.cpp
 * @brief Implementation of AgentTui dashboard.
 */
#include "common/xanadu/vagent/tui/agent_tui.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::tui {

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::noCell;

std::vector<std::string> tokenize(std::string_view line) {
  std::vector<std::string> tokens;
  std::string cur;
  bool inQuote = false;
  for (char c : line) {
    if (c == '"') {
      inQuote = !inQuote;
    } else if (std::isspace(static_cast<unsigned char>(c)) && !inQuote) {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    tokens.push_back(cur);
  }
  return tokens;
}

runtime::WireValue parseWireValue(std::string_view s) {
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

AgentTui::AgentTui(zigzag::vortex::mesh::MeshActorRuntime &runtime,
                   runtime::AgentExecutor &executor,
                   std::shared_ptr<NCursesActor> actor)
    : runtime_(runtime), executor_(executor), actor_(std::move(actor)) {
  if (!actor_) {
    actor_ = std::make_shared<NCursesActor>();
  }
  refreshActorTable();
  if (!actorRows_.empty()) {
    std::size_t initialIdx = 0;
    if (actor_ && actorRows_.size() > 1 && actorRows_[0].pid == actor_->pid()) {
      initialIdx = 1;
    }
    selectedIndex_ = initialIdx;
    selectedPid_   = actorRows_[selectedIndex_].pid;
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  actionLog_.emplace_back("vagent TUI initialised. Ready for interaction.");
}

void AgentTui::setPane(Pane pane) {
  activePane_ = pane;
  if (activePane_ == Pane::Querying) {
    if (!selectedPid_ && !actorRows_.empty()) {
      selectedIndex_ = 0;
      selectedPid_   = actorRows_[0].pid;
    }
    if (selectedPid_) {
      cachedBeliefs_ = inspectBeliefs(*selectedPid_);
      cachedScores_  = epistemicScores(*selectedPid_);
    }
  }
}

void AgentTui::selectActor(const identity::ActorPid &pid) {
  selectedPid_ = pid;
  for (std::size_t i = 0; i < actorRows_.size(); ++i) {
    if (actorRows_[i].pid == pid) {
      selectedIndex_ = i;
      break;
    }
  }
  cachedBeliefs_ = inspectBeliefs(pid);
  cachedScores_  = epistemicScores(pid);
}

void AgentTui::selectNextActor() {
  if (actorRows_.empty()) return;
  selectedIndex_ = (selectedIndex_ + 1) % actorRows_.size();
  selectedPid_   = actorRows_[selectedIndex_].pid;
  cachedBeliefs_ = inspectBeliefs(*selectedPid_);
  cachedScores_  = epistemicScores(*selectedPid_);
}

void AgentTui::selectPrevActor() {
  if (actorRows_.empty()) return;
  if (selectedIndex_ == 0) {
    selectedIndex_ = actorRows_.size() - 1;
  } else {
    --selectedIndex_;
  }
  selectedPid_   = actorRows_[selectedIndex_].pid;
  cachedBeliefs_ = inspectBeliefs(*selectedPid_);
  cachedScores_  = epistemicScores(*selectedPid_);
}

std::optional<identity::ActorPid> AgentTui::selectedActor() const {
  return selectedPid_;
}

std::vector<ActorRow> AgentTui::refreshActorTable() {
  std::vector<identity::ActorPid> pids = executor_.activeActors();
  auto runtimePids                     = runtime_.actorPids();
  for (const auto &rpid : runtimePids) {
    if (!std::ranges::contains(pids, rpid)) {
      pids.push_back(rpid);
    }
  }
  if (actor_) {
    const auto &tuiPid = actor_->pid();
    if (!std::ranges::contains(pids, tuiPid)) {
      pids.push_back(tuiPid);
    }
  }

  std::vector<ActorRow> rows;
  rows.reserve(pids.size());

  for (const auto &pid : pids) {
    if (actor_ && pid == actor_->pid()) {
      ActorRow row;
      row.pid      = pid;
      row.shortPid = pid.cursor.empty() ? pid.node : pid.cursor;
      if (row.shortPid.size() > 10) {
        row.shortPid = row.shortPid.substr(0, 10);
      }

      row.name          = "tui";
      row.agentType     = "NCursesActor";
      row.programRef    = "native:ncurses";
      row.status        = "RUNNING";
      row.fuelRemaining = 1'000'000'000;
      row.cycleCount    = actor_->screen().frameCount();
      row.scratchCells =
          actor_->dimensions().width * actor_->dimensions().height;
      row.epistemicCells     = 0;
      row.mailboxDepth       = actor_->hasPendingInput() ? 1 : 0;
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

  actorRows_ = rows;
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

  return actorRows_;
}

std::vector<std::string> AgentTui::queryVlog(std::string_view query) {
  if (!selectedPid_) {
    return {"Error: No actor selected for query"};
  }
  auto *ctx = executor_.context(*selectedPid_);
  if (!ctx) ctx = runtime_.context(*selectedPid_);
  if (!ctx) {
    return {"Error: Selected actor has no execution context"};
  }

  try {
    vprolog::Compiler compiler(ctx->core);
    auto compiledQuery = compiler.compileQuery(query);
    auto solutions     = compiler.solve(compiledQuery);

    if (solutions.empty()) {
      return {"false."};
    }

    std::vector<std::string> results;
    for (const auto &sol : solutions) {
      if (sol.formatted.empty()) {
        results.push_back("true.");
      } else {
        std::string line;
        for (const auto &[var, val] : sol.formatted) {
          if (!line.empty()) line += ", ";
          line += var + " = " + val;
        }
        results.push_back(line);
      }
    }
    return results;
  } catch (const std::exception &e) {
    return {std::format("Query error: {}", e.what())};
  }
}

std::vector<BeliefRecord>
AgentTui::inspectBeliefs(const identity::ActorPid &pid) {
  std::vector<BeliefRecord> records;
  auto *ctx         = executor_.context(pid);
  auto agentCellOpt = executor_.agentCell(pid);
  if (!ctx || !agentCellOpt) return records;

  auto &arena       = const_cast<ArenaManifold &>(ctx->arena);
  auto dims         = runtime::registerDimensions(arena);
  auto values       = runtime::valueDims(ctx->core);
  CellRef agentCell = *agentCellOpt;

  for (const auto pred : zigzag::rankAfter(arena, agentCell, dims.belief)) {
    std::string predName  = arena.textOf(pred);
    bool hasEpistemicHead = arena.linked(pred, dims.epistemic).has_value();

    for (const auto clause :
         zigzag::rankAfter(arena, pred, ctx->core.dims().clause)) {
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
          br.source              = src;
          br.status              = st;
          br.acquiredUncertainty = unc;
          br.certaintyScore      = score;
          br.incarnation         = inc;

          const auto term = arena.linked(clause, ctx->core.dims().grab);
          if (term) {
            const auto first = arena.linked(*term, ctx->core.dims().grab);
            if (first) br.key = arena.textOf(*first);
          }

          br.details = std::format(
              "{}({}) from {} [unc={:.3f}, certainty={:.2f}%, {}]", predName,
              br.key.empty() ? "_" : br.key, src, unc, score * 100.0, st);
          records.push_back(std::move(br));
        }

        if (!hasEpistemicRecords) {
          BeliefRecord br;
          br.predicate           = predName;
          br.source              = src;
          br.status              = "current";
          br.acquiredUncertainty = 0.0;
          br.certaintyScore      = 1.0;
          const auto term        = arena.linked(clause, ctx->core.dims().grab);
          if (term) {
            const auto first = arena.linked(*term, ctx->core.dims().grab);
            if (first) br.key = arena.textOf(*first);
          }
          br.details =
              std::format("{}({}) from {} [certainty=100.0%, current]",
                          predName, br.key.empty() ? "_" : br.key, src);
          records.push_back(std::move(br));
        }
      }

      if (!hasSupports && !hasEpistemicHead) {
        std::string termStr;
        const auto term = arena.linked(clause, ctx->core.dims().grab);
        if (term) {
          termStr = arena.textOf(*term);
        }
        BeliefRecord br;
        br.predicate      = predName;
        br.key            = termStr;
        br.source         = "local";
        br.status         = "fact";
        br.certaintyScore = 1.0;
        br.details        = std::format("{}({}) [fact]", predName,
                                        termStr.empty() ? "" : termStr);
        records.push_back(std::move(br));
      }
    }
  }

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
    std::string err = "Spawn failed: actor error";
    actionLog_.push_back(err);
    return std::unexpected(err);
  }
  identity::ActorPid pid = *result;
  actionLog_.push_back(std::format("Spawned agent '{}' of type '{}' (pid: {})",
                                   name, agentType, pid.cursor));
  refreshActorTable();
  selectActor(pid);
  return pid;
}

std::expected<void, std::string>
AgentTui::injectPercept(const identity::ActorPid &pid,
                        std::string_view eventType,
                        runtime::WireRecord fields) {
  executor_.percept(pid, std::string(eventType), std::move(fields));
  actionLog_.push_back(
      std::format("Injected percept '{}' into pid {}", eventType, pid.cursor));
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
    std::string err = "Event delivery failed";
    actionLog_.push_back(err);
    return std::unexpected(err);
  }
  actionLog_.push_back(
      std::format("Injected event '{}' into pid {}", eventType, pid.cursor));
  return {};
}

std::size_t AgentTui::step(std::size_t turns) {
  executor_.tick();
  std::size_t ran = executor_.pumpUntilIdle(turns);
  totalTurns_ += ran;
  actionLog_.push_back(
      std::format("Stepped {} turn(s) (total: {})", ran, totalTurns_));
  refreshActorTable();
  if (selectedPid_) {
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  return ran;
}

std::size_t AgentTui::advanceClock(std::chrono::milliseconds ms) {
  executor_.tick();
  std::size_t ran = executor_.pumpUntilIdle();
  totalTurns_ += ran;
  actionLog_.push_back(
      std::format("Advanced clock by {}ms, ran {} turn(s)", ms.count(), ran));
  refreshActorTable();
  if (selectedPid_) {
    cachedBeliefs_ = inspectBeliefs(*selectedPid_);
    cachedScores_  = epistemicScores(*selectedPid_);
  }
  return ran;
}

std::expected<void, std::string>
AgentTui::upgrade(const identity::ActorPid &pid,
                  std::string_view newProgramRef) {
  auto prog = executor_.resolveProgram(newProgramRef);
  if (!prog) {
    std::string err = std::format("Program ref '{}' not found", newProgramRef);
    actionLog_.push_back(err);
    return std::unexpected(err);
  }
  auto compiledProg = std::dynamic_pointer_cast<runtime::CompiledProgram>(prog);
  if (!compiledProg) {
    std::string err = "Program cannot be cast to CompiledProgram";
    actionLog_.push_back(err);
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
    std::string err = std::format("Live upgrade failed: error code {}",
                                  static_cast<int>(res.error()));
    actionLog_.push_back(err);
    return std::unexpected(err);
  }
  actionLog_.push_back(std::format("Upgraded actor {} to programRef '{}'",
                                   pid.cursor, newProgramRef));
  refreshActorTable();
  return {};
}

std::expected<void, std::string> AgentTui::stop(const identity::ActorPid &pid) {
  if (actor_ && pid == actor_->pid()) {
    actionLog_.push_back(std::format("Stopping TUI actor {}", pid.cursor));
    requestExit();
    return {};
  }
  auto res = runtime_.stop(pid);
  if (!res) {
    std::string err = "Failed to stop actor";
    actionLog_.push_back(err);
    return std::unexpected(err);
  }
  actionLog_.push_back(std::format("Stopped actor {}", pid.cursor));
  refreshActorTable();
  return {};
}

std::optional<identity::ActorPid>
AgentTui::resolveTarget(std::string_view targetStr) const {
  if (targetStr.empty() || targetStr == "@selected") {
    return selectedPid_;
  }
  for (const auto &row : actorRows_) {
    if (row.name == targetStr || row.shortPid == targetStr ||
        row.pid.cursor == targetStr) {
      return row.pid;
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
      return std::unexpected("Usage: spawn <agentType> <name> [key=val ...]");
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
    queryResults_ = sols;
    std::string summary;
    for (const auto &s : sols) {
      if (!summary.empty()) summary += "; ";
      summary += s;
    }
    return summary;
  }
  if (cmd == "help") {
    return "Commands: spawn <type> <name>, percept <target> <evt>, send "
           "<target> <evt>, "
           "step [N], advance <ms>, upgrade <target> <ref>, stop <target>, "
           "query <vlog>, quit";
  }
  if (cmd == "quit" || cmd == "exit") {
    requestExit();
    return "Exiting...";
  }

  return std::unexpected(std::format(
      "Unknown command: '{}'. Type 'help' for available commands.", cmd));
}

void AgentTui::handleInput(const KeyEvent &event) {
  if (event.code == KeyCode::Escape) {
    if (activePane_ == Pane::Monitoring) {
      requestExit();
    } else if (activePane_ == Pane::Querying) {
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
    return;
  }

  // F1-F3 or Alt+1..3 pane switching
  if (event.code == KeyCode::F1 || (event.alt && event.ch == '1')) {
    setPane(Pane::Monitoring);
    return;
  }
  if (event.code == KeyCode::F2 || (event.alt && event.ch == '2')) {
    setPane(Pane::Querying);
    return;
  }
  if (event.code == KeyCode::F3 || (event.alt && event.ch == '3')) {
    setPane(Pane::Orchestrating);
    return;
  }

  // Global Tab / Shift-Tab cycling across panes
  if (event.code == KeyCode::Tab || event.ch == '\t') {
    if (event.shift) {
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

  if (activePane_ == Pane::Monitoring) {
    if (event.code == KeyCode::Up) {
      selectPrevActor();
    } else if (event.code == KeyCode::Down) {
      selectNextActor();
    } else if (event.code == KeyCode::Char) {
      if (event.ch == '1')
        setPane(Pane::Monitoring);
      else if (event.ch == '2')
        setPane(Pane::Querying);
      else if (event.ch == '3')
        setPane(Pane::Orchestrating);
      else if (event.ch == 's')
        step(1);
      else if (event.ch == 'q')
        requestExit();
      else if (event.ch == 'r')
        refreshActorTable();
    }
  } else if (activePane_ == Pane::Querying) {
    if (event.code == KeyCode::Enter) {
      if (!queryInput_.empty()) {
        queryResults_ = queryVlog(queryInput_);
        queryInput_.clear();
      }
    } else if (event.code == KeyCode::Backspace) {
      if (!queryInput_.empty()) queryInput_.pop_back();
    } else if (event.code == KeyCode::Char) {
      if (queryInput_.empty()) {
        if (event.ch == '1') {
          setPane(Pane::Monitoring);
          return;
        }
        if (event.ch == '2') {
          return;
        }
        if (event.ch == '3') {
          setPane(Pane::Orchestrating);
          return;
        }
        if (event.ch == 'q') {
          requestExit();
          return;
        }
        if (event.ch == 's') {
          step(1);
          return;
        }
        if (event.ch == 'r') {
          refreshActorTable();
          return;
        }
      }
      queryInput_.push_back(event.ch);
    } else if (event.code == KeyCode::Up) {
      selectPrevActor();
    } else if (event.code == KeyCode::Down) {
      selectNextActor();
    }
  } else if (activePane_ == Pane::Orchestrating) {
    if (event.code == KeyCode::Enter) {
      if (!commandInput_.empty()) {
        auto res = executeCommand(commandInput_);
        if (res) {
          actionLog_.push_back(std::format("> {}", commandInput_));
          if (!res->empty()) actionLog_.push_back(*res);
        } else {
          actionLog_.push_back(
              std::format("> {} [ERROR: {}]", commandInput_, res.error()));
        }
        commandInput_.clear();
      }
    } else if (event.code == KeyCode::Backspace) {
      if (!commandInput_.empty()) commandInput_.pop_back();
    } else if (event.code == KeyCode::Char) {
      if (commandInput_.empty()) {
        if (event.ch == '1') {
          setPane(Pane::Monitoring);
          return;
        }
        if (event.ch == '2') {
          setPane(Pane::Querying);
          return;
        }
        if (event.ch == '3') {
          return;
        }
        if (event.ch == 'q') {
          requestExit();
          return;
        }
        if (event.ch == 's') {
          step(1);
          return;
        }
        if (event.ch == 'r') {
          refreshActorTable();
          return;
        }
      }
      commandInput_.push_back(event.ch);
    } else if (event.code == KeyCode::Up) {
      selectPrevActor();
    } else if (event.code == KeyCode::Down) {
      selectNextActor();
    }
  }
}

void AgentTui::tick() {
  while (auto ev = actor_->pollInput()) {
    handleInput(*ev);
  }
  render();
}

void AgentTui::renderHeader() {
  int w = actor_->dimensions().width;
  actor_->print(0, 0, std::string(w, ' '), Style{.reverse = true});
  actor_->print(2, 0, " VAGENT TUI ", Style{.bold = true, .reverse = true});

  Style sMon = {.bold    = (activePane_ == Pane::Monitoring),
                .reverse = (activePane_ != Pane::Monitoring)};
  Style sQry = {.bold    = (activePane_ == Pane::Querying),
                .reverse = (activePane_ != Pane::Querying)};
  Style sOrc = {.bold    = (activePane_ == Pane::Orchestrating),
                .reverse = (activePane_ != Pane::Orchestrating)};

  actor_->print(16, 0, "[1: Monitoring]", sMon);
  actor_->print(34, 0, "[2: Querying]", sQry);
  actor_->print(50, 0, "[3: Orchestration]", sOrc);
  if (w > 85) {
    actor_->print(72, 0, "[Tab: Next Pane]", Style{.reverse = true});
  }
  actor_->print(w - 10, 0, "[q: Quit]", Style{.reverse = true});
}

void AgentTui::renderMonitoringPane(int top, int bottom) {
  int w           = actor_->dimensions().width;
  int tableHeight = (bottom - top) * 2 / 3;
  if (tableHeight < 6) tableHeight = 6;

  actor_->drawBox(1, top, w - 2, tableHeight, "Live Actor Table");

  // Table headers
  int y = top + 1;
  actor_->print(3, y, "PID", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(14, y, "TYPE/NAME", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(32, y, "PROGRAM REF", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(52, y, "STATUS", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(63, y, "MBOX", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(70, y, "OUTBOX", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(78, y, "FUEL", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(88, y, "MEMORY", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(98, y, "DEADLINE", Style{.fg = Color::Cyan, .bold = true});

  actor_->drawHLine(2, y + 1, w - 4);

  y += 2;
  int maxRows = tableHeight - 4;
  for (int i = 0; i < maxRows && i < static_cast<int>(actorRows_.size()); ++i) {
    const auto &row = actorRows_[i];
    bool selected   = (selectedPid_ && row.pid == *selectedPid_);
    Style style     = {.reverse = selected};

    Color statusCol = Color::Green;
    if (row.status == "STOPPED")
      statusCol = Color::Red;
    else if (row.status == "BUSY")
      statusCol = Color::Yellow;
    else if (row.status == "RUNNING")
      statusCol = Color::Cyan;

    actor_->print(3, y + i, row.shortPid, style);

    std::string typeName = row.agentType;
    if (!row.name.empty() && row.name != row.agentType) {
      if (row.agentType.size() + row.name.size() + 3 <= 17) {
        typeName += " (" + row.name + ")";
      }
    }
    if (typeName.size() > 17) typeName = typeName.substr(0, 17);
    actor_->print(14, y + i, typeName, style);

    std::string pRef = row.programRef;
    if (pRef.size() > 18) pRef = pRef.substr(0, 18);
    actor_->print(32, y + i, pRef, style);

    Style stStyle = style;
    if (!selected) stStyle.fg = statusCol;
    actor_->print(52, y + i, row.status, stStyle);

    actor_->print(63, y + i, std::to_string(row.mailboxDepth), style);
    actor_->print(70, y + i, std::to_string(row.outboxCount), style);

    std::string fuelStr;
    if (row.fuelRemaining >= 1'000'000'000) {
      fuelStr =
          std::format("{:.1f}G", static_cast<double>(row.fuelRemaining) / 1e9);
    } else if (row.fuelRemaining >= 1'000'000) {
      fuelStr =
          std::format("{:.1f}M", static_cast<double>(row.fuelRemaining) / 1e6);
    } else if (row.fuelRemaining >= 10'000) {
      fuelStr = std::format("{}k", row.fuelRemaining / 1000);
    } else {
      fuelStr = std::to_string(row.fuelRemaining);
    }
    actor_->print(78, y + i, fuelStr, style);
    actor_->print(88, y + i,
                  std::format("{}c/{}e", row.scratchCells, row.epistemicCells),
                  style);

    std::string dl =
        row.deadlineMs ? std::format("{}ms", *row.deadlineMs) : "none";
    actor_->print(98, y + i, dl, style);
  }

  // Details box for selected actor
  int detailTop = top + tableHeight;
  int detailH   = bottom - detailTop;
  if (detailH > 3) {
    actor_->drawBox(1, detailTop, w - 2, detailH,
                    "Selected Actor Metrics & Stats");
    if (selectedPid_) {
      int dy = detailTop + 1;
      actor_->print(3, dy,
                    std::format("Full PID: {}:{}", selectedPid_->node,
                                selectedPid_->cursor),
                    Style{.bold = true});
      dy++;
      if (selectedIndex_ < actorRows_.size()) {
        const auto &r = actorRows_[selectedIndex_];
        actor_->print(
            3, dy,
            std::format(
                "Cycles: {} | Dropped: {} | Malformed: {} | DeliveryFailed: {}",
                r.cycleCount, r.statDropped, r.statMalformed,
                r.statDeliveryFailed));
      }
    } else {
      actor_->print(3, detailTop + 1, "No actor selected.");
    }
  }
}

void AgentTui::renderQueryingPane(int top, int bottom) {
  int w             = actor_->dimensions().width;
  int beliefsHeight = (bottom - top) * 3 / 5;
  if (beliefsHeight < 6) beliefsHeight = 6;

  actor_->drawBox(1, top, w - 2, beliefsHeight,
                  "Agent Beliefs & Epistemic Certainty Scores (P18/P19)");

  int y = top + 1;
  actor_->print(3, y, "PREDICATE / KEY",
                Style{.fg = Color::Cyan, .bold = true});
  actor_->print(28, y, "SOURCE", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(46, y, "UNCERTAINTY", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(60, y, "CERTAINTY SCORE",
                Style{.fg = Color::Cyan, .bold = true});
  actor_->print(78, y, "STATUS", Style{.fg = Color::Cyan, .bold = true});
  actor_->print(88, y, "INCARNATION", Style{.fg = Color::Cyan, .bold = true});

  actor_->drawHLine(2, y + 1, w - 4);
  y += 2;

  int maxB = beliefsHeight - 4;
  for (int i = 0; i < maxB && i < static_cast<int>(cachedBeliefs_.size());
       ++i) {
    const auto &b = cachedBeliefs_[i];
    std::string predKey =
        b.key.empty() ? b.predicate : std::format("{}({})", b.predicate, b.key);
    if (predKey.size() > 23) predKey = predKey.substr(0, 23);

    actor_->print(3, y + i, predKey);
    actor_->print(28, y + i, b.source.substr(0, 16));

    if (b.acquiredUncertainty) {
      actor_->print(46, y + i, std::format("{:.3f}", *b.acquiredUncertainty));
    } else {
      actor_->print(46, y + i, "0.000");
    }

    // Certainty bar / score
    Color scoreCol = Color::Green;
    if (b.certaintyScore < 0.6)
      scoreCol = Color::Red;
    else if (b.certaintyScore < 0.85)
      scoreCol = Color::Yellow;

    actor_->print(60, y + i,
                  std::format("{:5.1f}% [{:.3f}]", b.certaintyScore * 100.0,
                              b.certaintyScore),
                  Style{.fg = scoreCol, .bold = true});
    actor_->print(78, y + i, b.status);
    actor_->print(88, y + i,
                  b.incarnation ? std::to_string(*b.incarnation) : "-");
  }

  if (cachedBeliefs_.empty()) {
    actor_->print(
        3, y,
        "No beliefs or epistemic observations registered for this actor.");
  }

  // Vlog Query REPL box
  int replTop = top + beliefsHeight;
  int replH   = bottom - replTop;
  if (replH > 5) {
    actor_->drawBox(1, replTop, w - 2, replH, "Vlog Deductive Query REPL");
    actor_->print(3, replTop + 1, "Query: ?- " + queryInput_ + "_",
                  Style{.bold = true});
    actor_->print(3, replTop + 2,
                  "[Enter: Run | Esc: Clear/Back | Tab: Switch Pane | 1: "
                  "Monitor | 3: Orchestrate]",
                  Style{.dim = true});
    actor_->drawHLine(2, replTop + 3, w - 4);

    int ry = replTop + 4;
    actor_->print(3, ry, "Results:", Style{.fg = Color::Yellow});
    ry++;
    int maxRes = replH - 6;
    for (int i = 0; i < maxRes && i < static_cast<int>(queryResults_.size());
         ++i) {
      actor_->print(5, ry + i, queryResults_[i], Style{.fg = Color::Green});
    }
  } else if (replH > 3) {
    actor_->drawBox(1, replTop, w - 2, replH, "Vlog Deductive Query REPL");
    actor_->print(3, replTop + 1, "Query: ?- " + queryInput_ + "_",
                  Style{.bold = true});
  }
}

void AgentTui::renderOrchestratingPane(int top, int bottom) {
  int w         = actor_->dimensions().width;
  int logHeight = (bottom - top) - 5;
  if (logHeight < 4) logHeight = 4;

  actor_->drawBox(1, top, w - 2, logHeight, "Orchestration Action Log");

  int y        = top + 1;
  int maxLog   = logHeight - 2;
  int startIdx = 0;
  if (static_cast<int>(actionLog_.size()) > maxLog) {
    startIdx = static_cast<int>(actionLog_.size()) - maxLog;
  }
  for (int i = 0;
       startIdx + i < static_cast<int>(actionLog_.size()) && i < maxLog; ++i) {
    const auto &entry = actionLog_[startIdx + i];
    Style st;
    if (entry.find("[ERROR") != std::string::npos)
      st.fg = Color::Red;
    else if (entry.starts_with(">"))
      st.fg = Color::Cyan;
    else
      st.fg = Color::White;
    actor_->print(3, y + i, entry, st);
  }

  // Command prompt box
  int promptTop = top + logHeight;
  int promptH   = bottom - promptTop;
  if (promptH >= 5) {
    actor_->drawBox(1, promptTop, w - 2, promptH, "Command Input");
    actor_->print(3, promptTop + 1, "command> " + commandInput_ + "_",
                  Style{.bold = true});
    actor_->print(3, promptTop + 2,
                  "[Enter: Execute | Esc: Clear/Back | Tab: Switch Pane | 1: "
                  "Monitor | 2: Query]",
                  Style{.dim = true});
    actor_->print(
        3, promptTop + 3,
        "spawn <type> <name> | percept <pid> <evt> | send <pid> <evt> "
        "| step [N] | advance <ms> | upgrade <pid> <ref> | stop <pid>",
        Style{.dim = true});
  } else if (promptH > 2) {
    actor_->drawBox(1, promptTop, w - 2, promptH, "Command Input");
    actor_->print(3, promptTop + 1, "command> " + commandInput_ + "_",
                  Style{.bold = true});
  }
}

void AgentTui::renderStatusBar() {
  int w = actor_->dimensions().width;
  int h = actor_->dimensions().height;
  actor_->print(0, h - 1, std::string(w, ' '), Style{.reverse = true});

  std::string status =
      std::format(" Actors: {} | Total Turns: {} | Selected: {} | [Tab: Cycle "
                  "Panes | 1/2/3: Panes | Esc: Back | q: Quit]",
                  actorRows_.size(), totalTurns_,
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
  int bottom = actor_->dimensions().height - 2;

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
}

} // namespace xanadu::vagent::tui
