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

  auto cellOpt = screen.at(2, 1);
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
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].pid, pid);
  EXPECT_EQ(rows[0].name, "tracker-1");
  EXPECT_EQ(rows[0].agentType, "Tracker");
  EXPECT_EQ(rows[0].status, "QUIESCENT");
  EXPECT_GT(rows[0].fuelRemaining, 0U);

  // Render to virtual screen and verify contents
  tui.render();
  const auto &buf = actor->screen();
  EXPECT_TRUE(buf.contains("Live Actor Table"));
  EXPECT_TRUE(buf.contains("Tracker"));
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
  ASSERT_EQ(rows.size(), 1U);
  const auto pid = rows[0].pid;

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
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].status, "STOPPED");

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

  // Switch directly via F2
  tui.handleInput(KeyEvent{KeyCode::F2, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Querying);

  // Type in query
  tui.handleInput(KeyEvent{KeyCode::Char, 'a'});
  tui.handleInput(KeyEvent{KeyCode::Char, 't'});
  tui.handleInput(KeyEvent{KeyCode::Enter, '\n'});

  // Switch to Orchestrating via F3
  tui.handleInput(KeyEvent{KeyCode::F3, '\0'});
  EXPECT_EQ(tui.activePane(), Pane::Orchestrating);

  // Single-step turn advance via command
  actor->injectInput("step 1\n");
  tui.tick();
  EXPECT_GE(tui.actionLog().size(), 1U);

  // Quit request
  tui.handleInput(KeyEvent{KeyCode::Escape, '\0'});
  // Escape from orchestrating goes to monitoring
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
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].programRef, held2->programRef());
}

} // namespace
