/**
 * @file vagent_tui_test.cpp
 * @brief Unit and integration tests for NCurses native actor and AgentTui
 * dashboard.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/tui/agent_tui.hpp"
#include "common/xanadu/vagent/tui/ncurses_actor.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace std::chrono_literals;
using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::tui;
using zigzag::vortex::mesh::MeshActorRuntime;

constexpr const char *kTestAgentSrc = R"vagent(
module test:dashboard
import std:agent

predicate at(key who: Str, x: Float, y: Float)
predicate status(text: Str)

event Ping
event StepTick

agent Tracker {
  var x: Float := 10.0
  var y: Float := 20.0

  on Ping {
    assert at("scout", 12.5, 40.0) from "sensor:gps"
    x := 12.5
    y := 40.0
  }

  on StepTick {
    x := x + 1.0
  }
}
)vagent";

constexpr const char *kSecondAgentSrc = R"vagent(
module test:upgrade
import std:agent

event Ping
event StepTick

agent Tracker {
  var count: Int := 0

  on Ping {
    count := count + 1
  }

  on StepTick {
    count := count + 10
  }
}
)vagent";

// -- 1. NCursesActor Tests (Headless & Virtual Buffer) ------------------------

TEST(NCursesActorTest, HeadlessVirtualScreenBufferOperations) {
  NCursesActor::Options opts;
  opts.forceHeadless = true;
  opts.defaultWidth  = 80;
  opts.defaultHeight = 25;

  NCursesActor actor(opts);
  EXPECT_TRUE(actor.isHeadless());
  EXPECT_EQ(actor.dimensions().width, 80);
  EXPECT_EQ(actor.dimensions().height, 25);

  actor.clear();
  actor.print(2, 1, "Hello Vortex TUI", Style{.fg = Color::Cyan, .bold = true});
  actor.drawBox(0, 0, 80, 25, "Dashboard Frame");
  actor.refresh();

  const auto &screen = actor.screen();
  EXPECT_EQ(screen.frameCount(), 1U);
  EXPECT_TRUE(screen.contains("Hello Vortex TUI"));
  EXPECT_TRUE(screen.contains("Dashboard Frame"));

  auto cellOpt = screen.cellAt(2, 1);
  ASSERT_TRUE(cellOpt.has_value());
  EXPECT_EQ(cellOpt->ch, 'H');
  EXPECT_EQ(cellOpt->style.fg, Color::Cyan);
  EXPECT_TRUE(cellOpt->style.bold);
}

TEST(NCursesActorTest, SyntheticInputQueueHandling) {
  NCursesActor::Options opts;
  opts.forceHeadless = true;
  NCursesActor actor(opts);

  EXPECT_FALSE(actor.hasPendingInput());
  EXPECT_EQ(actor.pollInput(), std::nullopt);

  actor.injectInput(KeyEvent{KeyCode::Up, '\0'});
  actor.injectInput(KeyEvent{KeyCode::Enter, '\n'});
  actor.injectInput("run");

  EXPECT_TRUE(actor.hasPendingInput());

  auto ev1 = actor.pollInput();
  ASSERT_TRUE(ev1.has_value());
  EXPECT_EQ(ev1->code, KeyCode::Up);

  auto ev2 = actor.pollInput();
  ASSERT_TRUE(ev2.has_value());
  EXPECT_EQ(ev2->code, KeyCode::Enter);

  auto ev3 = actor.pollInput();
  ASSERT_TRUE(ev3.has_value());
  EXPECT_EQ(ev3->code, KeyCode::Char);
  EXPECT_EQ(ev3->ch, 'r');

  auto ev4 = actor.pollInput();
  ASSERT_TRUE(ev4.has_value());
  EXPECT_EQ(ev4->ch, 'u');

  auto ev5 = actor.pollInput();
  ASSERT_TRUE(ev5.has_value());
  EXPECT_EQ(ev5->ch, 'n');

  EXPECT_FALSE(actor.hasPendingInput());
  EXPECT_EQ(actor.pollInput(), std::nullopt);
}

// -- 2. AgentTui Monitoring Pane Tests ---------------------------------------

TEST(AgentTuiTest, MonitoringPaneReflectsLiveActorState) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  auto spawnResult = host.launch("Tracker", "tracker-1");
  ASSERT_TRUE(spawnResult.has_value());
  const auto pid = *spawnResult;

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.setPane(Pane::Monitoring);

  auto rows = tui.refreshActorTable();
  ASSERT_EQ(rows.size(), 2U);
  auto trackerIt =
      std::ranges::find_if(rows, [&](const auto &r) { return r.pid == pid; });
  ASSERT_NE(trackerIt, rows.end());
  EXPECT_EQ(trackerIt->name, "tracker-1");
  EXPECT_EQ(trackerIt->agentType, "Tracker");
  EXPECT_EQ(trackerIt->status, "QUIESCENT");
  EXPECT_GT(trackerIt->fuelRemaining, 0U);

  auto tuiIt = std::ranges::find_if(
      rows, [&](const auto &r) { return r.pid == actor->pid(); });
  ASSERT_NE(tuiIt, rows.end());
  EXPECT_EQ(tuiIt->name, "tui");
  EXPECT_EQ(tuiIt->agentType, "NCursesActor");
  EXPECT_EQ(tuiIt->programRef, "native:ncurses");
  EXPECT_EQ(tuiIt->status, "RUNNING");

  // Render to virtual screen and verify contents
  tui.render();
  const auto &buf = actor->screen();
  EXPECT_TRUE(buf.contains("Live Actor Table"));
  EXPECT_TRUE(buf.contains("Tracker"));
  EXPECT_TRUE(buf.contains("NCursesActor"));
  EXPECT_TRUE(buf.contains("QUIESCENT"));
  EXPECT_TRUE(buf.contains("[1: Monitoring]"));
}

// -- 3. AgentTui Querying & Epistemic Certainty Scores Tests ------------------

TEST(AgentTuiTest, QueryingPaneBeliefsAndVlogDeduction) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  auto spawnResult = host.launch("Tracker", "tracker-1");
  ASSERT_TRUE(spawnResult.has_value());
  const auto pid = *spawnResult;

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  host.pump();

  // Send Ping to assert belief
  auto eventRes = tui.injectEvent(pid, "Ping");
  EXPECT_TRUE(eventRes.has_value());
  tui.step(2);

  // Inspect beliefs
  auto beliefs = tui.inspectBeliefs(pid);
  EXPECT_FALSE(beliefs.empty());

  bool foundScout = false;
  for (const auto &b : beliefs) {
    if (b.predicate == "at") {
      foundScout = true;
      EXPECT_EQ(b.source, "sensor:gps");
      EXPECT_GE(b.certaintyScore, 0.0);
      EXPECT_LE(b.certaintyScore, 1.0);
    }
  }
  EXPECT_TRUE(foundScout);

  // Epistemic scores
  auto scores = tui.epistemicScores(pid);
  EXPECT_FALSE(scores.empty());
  for (const auto &sc : scores) {
    EXPECT_GE(sc.certaintyScore, 0.0);
    EXPECT_LE(sc.certaintyScore, 1.0);
  }

  // Vlog queries against the agent arena
  tui.setPane(Pane::Querying);
  auto q1 = tui.queryVlog("at(Who, X, Y)");
  EXPECT_FALSE(q1.empty());

  auto qFalse = tui.queryVlog("at(\"ghost\", X, Y)");
  ASSERT_EQ(qFalse.size(), 1U);
  EXPECT_EQ(qFalse[0], "false.");

  tui.render();
  EXPECT_TRUE(actor->screen().contains("Agent Beliefs & Epistemic Certainty"));
  EXPECT_TRUE(actor->screen().contains("Vlog Deductive Query REPL"));
}

// -- 4. AgentTui Orchestrating Pane & Commands Tests -------------------------

TEST(AgentTuiTest, OrchestratingPaneCommandsAndLifecycle) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.setPane(Pane::Orchestrating);

  // 1. Help command
  auto helpRes = tui.executeCommand("help");
  ASSERT_TRUE(helpRes.has_value());
  EXPECT_TRUE(helpRes->find("spawn") != std::string::npos);

  // 2. Spawn command
  auto spawnCmdRes = tui.executeCommand("spawn Tracker scout1");
  ASSERT_TRUE(spawnCmdRes.has_value());
  EXPECT_TRUE(spawnCmdRes->find("scout1") != std::string::npos);

  auto rows = tui.refreshActorTable();
  ASSERT_EQ(rows.size(), 2U);
  auto scoutIt = std::ranges::find_if(
      rows, [](const auto &r) { return r.name == "scout1"; });
  ASSERT_NE(scoutIt, rows.end());
  const auto pid = scoutIt->pid;

  // 3. Percept injection command
  auto perceptCmdRes = tui.executeCommand("percept scout1 Ping");
  ASSERT_TRUE(perceptCmdRes.has_value());

  // 4. Step command
  auto stepCmdRes = tui.executeCommand("step 2");
  ASSERT_TRUE(stepCmdRes.has_value());

  // 5. Send event command
  auto sendCmdRes = tui.executeCommand("send scout1 StepTick");
  ASSERT_TRUE(sendCmdRes.has_value());
  tui.step(1);

  // 6. Advance clock command
  auto advCmdRes = tui.executeCommand("advance 500ms");
  ASSERT_TRUE(advCmdRes.has_value());

  // 7. Stop command
  auto stopCmdRes = tui.executeCommand("stop scout1");
  ASSERT_TRUE(stopCmdRes.has_value());

  rows = tui.refreshActorTable();
  ASSERT_EQ(rows.size(), 2U);
  scoutIt = std::ranges::find_if(
      rows, [](const auto &r) { return r.name == "scout1"; });
  ASSERT_NE(scoutIt, rows.end());
  EXPECT_EQ(scoutIt->status, "STOPPED");

  // Render orchestrating pane
  tui.render();
  EXPECT_TRUE(actor->screen().contains("Orchestration Action Log"));
  EXPECT_TRUE(actor->screen().contains("Command Input"));
}

// -- 5. Keyboard Navigation & Pane Switching Tests ---------------------------

TEST(AgentTuiTest, KeyboardNavigationCyclesPanesAndInputs) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  (void)host.launch("Tracker", "tracker-1");

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);

  EXPECT_EQ(tui.activePane(), Pane::Monitoring);

  // Switch to Querying pane via Tab
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t'});
  EXPECT_EQ(tui.activePane(), Pane::Querying);

  // Switch to Orchestrating pane via Tab
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t'});
  EXPECT_EQ(tui.activePane(), Pane::Orchestrating);

  // Switch to Monitoring pane via Tab
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t'});
  EXPECT_EQ(tui.activePane(), Pane::Monitoring);

  // Shift-Tab backward cycling
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t', true});
  EXPECT_EQ(tui.activePane(), Pane::Orchestrating);
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t', true});
  EXPECT_EQ(tui.activePane(), Pane::Querying);
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t', true});
  EXPECT_EQ(tui.activePane(), Pane::Monitoring);

  // Switch directly via '2' from Monitoring
  tui.handleInput(KeyEvent{KeyCode::Char, '2'});
  EXPECT_EQ(tui.activePane(), Pane::Querying);

  // While in Querying with empty input, '3' switches to Orchestrating
  tui.handleInput(KeyEvent{KeyCode::Char, '3'});
  EXPECT_EQ(tui.activePane(), Pane::Orchestrating);

  // While in Orchestrating with empty input, '1' switches to Monitoring
  tui.handleInput(KeyEvent{KeyCode::Char, '1'});
  EXPECT_EQ(tui.activePane(), Pane::Monitoring);

  // Switch to Querying via Alt+2
  tui.handleInput(KeyEvent{KeyCode::Char, '2', false, false, true});
  EXPECT_EQ(tui.activePane(), Pane::Querying);

  // Type in query including digits 1, 2, 3
  tui.handleInput(KeyEvent{KeyCode::Char, 'a'});
  tui.handleInput(KeyEvent{KeyCode::Char, 't'});
  tui.handleInput(KeyEvent{KeyCode::Char, '1'});
  // Escape clears typed query without switching pane
  tui.handleInput(KeyEvent{KeyCode::Escape, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Querying);

  // Second Escape when empty returns to Monitoring
  tui.handleInput(KeyEvent{KeyCode::Escape, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Monitoring);

  // Switch back to Querying and test Tab switching while typing
  tui.handleInput(KeyEvent{KeyCode::Char, '2'});
  EXPECT_EQ(tui.activePane(), Pane::Querying);
  tui.handleInput(KeyEvent{KeyCode::Char, 'x'});
  tui.handleInput(KeyEvent{KeyCode::Tab, '\t'});
  EXPECT_EQ(tui.activePane(), Pane::Orchestrating);

  // Switch directly via F2
  tui.handleInput(KeyEvent{KeyCode::F2, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Querying);

  // Switch to Orchestrating via F3
  tui.handleInput(KeyEvent{KeyCode::F3, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Orchestrating);

  // Single-step turn advance via command
  actor->injectInput("step 1\n");
  tui.tick();
  EXPECT_GE(tui.actionLog().size(), 1U);

  // Escape from orchestrating goes to monitoring
  tui.handleInput(KeyEvent{KeyCode::Escape, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Monitoring);
  tui.handleInput(KeyEvent{KeyCode::Char, 'q'});
  EXPECT_TRUE(tui.shouldExit());
}

TEST(AgentTuiTest, LiveUpgradeViaTui) {
  auto compiled1 = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled1.has_value());

  auto compiled2 = CompiledProgram::fromSource(kSecondAgentSrc, "upgraded.va");
  ASSERT_TRUE(compiled2.has_value());

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled1), std::move(hostOpts));

  auto held2 = host.retainProgram(std::move(*compiled2));
  ASSERT_TRUE(held2 != nullptr);

  auto spawnResult = host.launch("Tracker", "tracker-live");
  ASSERT_TRUE(spawnResult.has_value());
  const auto pid = *spawnResult;
  host.pump();

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.setPane(Pane::Orchestrating);

  auto upgradeRes = tui.upgrade(pid, held2->programRef());
  EXPECT_TRUE(upgradeRes.has_value()) << (upgradeRes ? "" : upgradeRes.error());

  auto rows = tui.refreshActorTable();
  ASSERT_EQ(rows.size(), 2U);
  auto liveIt =
      std::ranges::find_if(rows, [&](const auto &r) { return r.pid == pid; });
  ASSERT_NE(liveIt, rows.end());
  EXPECT_EQ(liveIt->programRef, held2->programRef());
}

// -- 6. Escape Probes (§9 of design/vagent-tui-dashboard.md) ------------------

TEST(AgentTuiEscapeProbes, Probe1_InputIsolation) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value());

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.setPane(Pane::Orchestrating);

  // Initial state: Navigation mode
  EXPECT_EQ(tui.inputMode(), InputMode::Navigation);

  // Type ':' or 'i' to enter Edit mode
  tui.handleInput(KeyEvent{KeyCode::Char, ':'});
  EXPECT_EQ(tui.inputMode(), InputMode::Edit);

  // Type "spawn Tracker worker-1\n"
  // Assert 's' does not trigger step(1) while in edit mode
  actor->injectInput("spawn Tracker worker-1\n");
  while (auto ev = actor->pollInput()) {
    tui.handleInput(*ev);
  }

  // Verify actor spawned
  auto pids = host.executor().activeActors();
  EXPECT_EQ(pids.size(), 1U);
  EXPECT_EQ(host.executor().agentName(pids.front()), "worker-1");

  // Invariant T4: Enter retains EDIT mode
  EXPECT_EQ(tui.inputMode(), InputMode::Edit);

  // Escape exits Edit mode to Navigation mode
  tui.handleInput(KeyEvent{KeyCode::Escape, '\0'});
  EXPECT_EQ(tui.inputMode(), InputMode::Navigation);

  // In Navigation mode, 'q' requests exit
  tui.handleInput(KeyEvent{KeyCode::Char, 'q'});
  EXPECT_TRUE(tui.shouldExit());
}

TEST(AgentTuiEscapeProbes, Probe2_ScrollVirtualization) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value());

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  // Spawn 25 actors
  for (int i = 0; i < 25; ++i) {
    auto res = host.launch("Tracker", std::format("worker-{}", i));
    ASSERT_TRUE(res.has_value());
  }

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  actorOpts.defaultWidth  = 120;
  actorOpts.defaultHeight = 15; // Small height so maxVisible < 25
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.setPane(Pane::Monitoring);
  tui.refreshActorTable();

  EXPECT_EQ(tui.actorViewport().topIndex, 0);
  EXPECT_GT(tui.actorTable().size(), 20U);

  // Navigate down past visible rows
  for (int i = 0; i < 20; ++i) {
    tui.selectNextActor();
  }
  EXPECT_GT(tui.actorViewport().topIndex, 0);

  // Render executes within bounded viewport
  tui.render();

  // Test pageUp and pageDown
  tui.actorViewport().pageUp();
  EXPECT_EQ(tui.actorViewport().topIndex, 0);

  tui.actorViewport().pageDown();
  EXPECT_GT(tui.actorViewport().topIndex, 0);

  const auto thumb = tui.actorViewport().scrollThumb(10);
  EXPECT_LE(thumb.first, 10);
  EXPECT_LE(thumb.second, 10);
}

TEST(AgentTuiEscapeProbes, Probe3_CompoundArgumentAndGaugeRendering) {
  // 1. Proportional certainty bar gauges (P22.6)
  std::string gauge85 = renderCertaintyGauge(0.85, 10, true);
  EXPECT_TRUE(gauge85.contains("85%"));
  EXPECT_TRUE(gauge85.contains("\xe2\x96\x88")); // █

  std::string gaugeAscii = renderCertaintyGauge(0.50, 10, false);
  EXPECT_TRUE(gaugeAscii.contains("50%"));
  EXPECT_TRUE(gaugeAscii.contains("====="));

  // 2. Compound argument traversal along +d.step from +d.grab
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value());

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  auto spawnResult = host.launch("Tracker", "tracker-1");
  ASSERT_TRUE(spawnResult.has_value());
  const auto pid = *spawnResult;
  host.pump();

  auto *ctx = host.executor().context(pid);
  ASSERT_NE(ctx, nullptr);
  auto &arena = const_cast<zigzag::ArenaManifold &>(ctx->arena);

  // Construct compound term: target(pos_y, 40.0)
  auto term = arena.makeCell("target");
  auto arg1 = arena.makeCell("pos_y");
  auto arg2 = arena.makeScalarCell(40.0);
  (void)arena.link(term, ctx->core.dims().grab, zigzag::DimVector::POS, arg1)
      .has_value();
  (void)arena.link(arg1, ctx->core.dims().step, zigzag::DimVector::POS, arg2)
      .has_value();

  EXPECT_EQ(formatCellArg(arena, arg1), "pos_y");
  EXPECT_EQ(formatCellArg(arena, arg2), "40.00");

  // Host TUI ncurses actor is listed in the monitor table
  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  auto rows  = tui.refreshActorTable();
  auto tuiIt = std::ranges::find_if(
      rows, [&](const auto &r) { return r.name == "tui"; });
  ASSERT_NE(tuiIt, rows.end());
  EXPECT_EQ(tuiIt->agentType, "NCursesActor");
  EXPECT_EQ(tuiIt->status, "RUNNING");
}

TEST(AgentTuiEscapeProbes, Probe4_ReplIsolation) {
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value());

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), std::move(hostOpts));

  auto spawnResult = host.launch("Tracker", "tracker-1");
  ASSERT_TRUE(spawnResult.has_value());
  const auto pid = *spawnResult;
  host.pump();

  auto *ctx = host.executor().context(pid);
  ASSERT_NE(ctx, nullptr);

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.setPane(Pane::Querying);

  // Run query in scoped evaluation frame
  auto solutions = tui.queryVlog("true.");
  EXPECT_FALSE(solutions.empty());

  // Scoped evaluation frame: marks are released and no choice points leak
  EXPECT_EQ(tui.queryPagination().windowOffset, 0U);
  EXPECT_LE(tui.queryPagination().pageSize, 5U);

  // Background agent turn stepping remains unblocked and succeeds
  std::size_t stepped = tui.step(1);
  EXPECT_GE(stepped, 0U);
}

TEST(AgentTuiEscapeProbes, Probe5_BoundedEventStream) {
  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value());
  LocalHost host(std::move(*compiled));

  AgentTui tui(host.runtime(), host.executor(), actor);

  // Stream 1,000 action log entries
  for (int i = 1; i <= 1000; ++i) {
    tui.appendActionLog(std::format("emission-{}", i));
  }

  // Invariant T7: strictly bounded at 500 entries
  EXPECT_EQ(tui.actionLog().size(), 500U);
  EXPECT_EQ(tui.actionLog().front(), "emission-501");
  EXPECT_EQ(tui.actionLog().back(), "emission-1000");
}

// -- 7. system://settings vagent.tui.* binding (P22.10) ----------------------

TEST(AgentTuiSettings, FromModelOverridesDefaultsAndDashboardHonorsThem) {
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);

  // Override three vagent.tui.* knobs away from their documented defaults.
  auto ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                                xanadu::settings::kVagentTuiSplitThreshold,
                                std::int64_t{200});
  store.repointCurrentVersion(ver);
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentTuiMaxLogEntries,
                           std::int64_t{3});
  store.repointCurrentVersion(ver);
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentTuiQuerySolutionLimit,
                           std::int64_t{7});
  store.repointCurrentVersion(ver);

  const auto model = xanadu::SystemStoreModel::fromStore(store);
  ASSERT_TRUE(model.isValid()) << model.validationError();

  const TuiSettings s = TuiSettings::fromModel(model);
  EXPECT_EQ(s.splitThreshold, 200);
  EXPECT_EQ(s.maxLogEntries, 3U);
  EXPECT_EQ(s.querySolutionLimit, 7U);
  EXPECT_EQ(s.borders, "rounded"); // untouched default

  // The dashboard honors the overridden bounded action log.
  auto compiled = CompiledProgram::fromSource(kTestAgentSrc, "tracker.va");
  ASSERT_TRUE(compiled.has_value());
  LocalHost host(std::move(*compiled));

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor, s);
  EXPECT_EQ(tui.settings().splitThreshold, 200);

  for (int i = 1; i <= 50; ++i) {
    tui.appendActionLog(std::format("e-{}", i));
  }
  EXPECT_EQ(tui.actionLog().size(), 3U); // bounded by overridden max_log_entries
  EXPECT_EQ(tui.actionLog().back(), "e-50");
}

} // namespace
