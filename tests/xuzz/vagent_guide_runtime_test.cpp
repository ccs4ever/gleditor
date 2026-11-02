/**
 * @file vagent_guide_runtime_test.cpp
 * @brief Runtime conformance tests executing worked examples from
 * docs/vagent-user-guide.md on LocalHost and ClusterNode.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/cluster/cluster_node.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vagent/tui/agent_tui.hpp"
#include "common/xanadu/vagent/tui/ncurses_actor.hpp"

namespace {

using namespace std::chrono_literals;
using namespace xanadu::vagent::cluster;
using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::tui;

// Helper to construct an Envelope
Envelope makeEvent(std::string type,
                   std::vector<std::pair<std::string, WireValue>> fields = {}) {
  Envelope e;
  e.kind = "event";
  e.type = std::move(type);
  for (auto &[name, val] : fields) {
    e.fields.set(name, std::move(val));
  }
  return e;
}

// ----------------------------------------------------------------------------
// 1. Counter: First Source File (docs/vagent-user-guide.md §"Your first source
// file")
// ----------------------------------------------------------------------------
constexpr const char *kGuideCounterSource = R"vagent(
module demo:counter

event Add { amount: Int }
event TotalReport { count: Int }

agent Counter(start: Int) {
  var count: Int := start

  on Add as a {
    let next := count + a.amount
    count := next
    emit TotalReport{count: count} in outer
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, CounterWorkedExampleExecutesOnLocalHost) {
  auto compiled =
      CompiledProgram::fromSource(kGuideCounterSource, "counter.vagent");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost host(std::move(*compiled));
  WireRecord args;
  args.set("start", WireValue::integer(0));

  auto pid = host.launch("Counter", "counter-1", std::move(args));
  ASSERT_TRUE(pid.has_value());
  host.pump();
  EXPECT_TRUE(host.emissions().empty());

  // Deliver Add{amount: 3}
  ASSERT_TRUE(
      host.send(*pid, makeEvent("Add", {{"amount", WireValue::integer(3)}}))
          .has_value());
  host.pump();

  // Deliver Add{amount: 2}
  ASSERT_TRUE(
      host.send(*pid, makeEvent("Add", {{"amount", WireValue::integer(2)}}))
          .has_value());
  host.pump();

  // Verify emissions reaching the outer launcher
  ASSERT_EQ(host.emissions().size(), 2U);
  EXPECT_EQ(host.emissions()[0].type, "TotalReport");
  const auto *c1 = host.emissions()[0].fields.find("count");
  ASSERT_NE(c1, nullptr);
  EXPECT_EQ(std::get<std::int64_t>(c1->data), 3);

  EXPECT_EQ(host.emissions()[1].type, "TotalReport");
  const auto *c2 = host.emissions()[1].fields.find("count");
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(std::get<std::int64_t>(c2->data), 5);
}

// ----------------------------------------------------------------------------
// 2. Alarms: Events, Priority & Consume (docs/vagent-user-guide.md §"Events and
// handlers")
// ----------------------------------------------------------------------------
constexpr const char *kGuideAlarmsSource = R"vagent(
module demo:alarms

event Alarm { severity: Int }
event AlarmTally { high: Int, low: Int }

agent Alarms {
  var high: Int := 0
  var low: Int := 0

  on Alarm as a [a.severity > 5] priority 2 {
    high := high + 1
    consume
    emit AlarmTally{high: high, low: low} in outer
  }

  on Alarm {
    low := low + 1
    emit AlarmTally{high: high, low: low} in outer
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, AlarmsPriorityAndConsumeWorkedExample) {
  auto compiled =
      CompiledProgram::fromSource(kGuideAlarmsSource, "alarms.vagent");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost host(std::move(*compiled));
  auto pid = host.launch("Alarms", "alarms-1");
  ASSERT_TRUE(pid.has_value());
  host.pump();

  // High severity event: severity 8 (> 5) -> triggers priority 2 handler and
  // consumes
  ASSERT_TRUE(
      host.send(*pid, makeEvent("Alarm", {{"severity", WireValue::integer(8)}}))
          .has_value());
  host.pump();

  // Low severity event: severity 2 (<= 5) -> fails guard, runs second handler
  ASSERT_TRUE(
      host.send(*pid, makeEvent("Alarm", {{"severity", WireValue::integer(2)}}))
          .has_value());
  host.pump();

  ASSERT_EQ(host.emissions().size(), 2U);

  // Turn 1: high = 1, low = 0
  EXPECT_EQ(host.emissions()[0].type, "AlarmTally");
  const auto *h1 = host.emissions()[0].fields.find("high");
  const auto *l1 = host.emissions()[0].fields.find("low");
  ASSERT_TRUE(h1 && l1);
  EXPECT_EQ(std::get<std::int64_t>(h1->data), 1);
  EXPECT_EQ(std::get<std::int64_t>(l1->data), 0);

  // Turn 2: high = 1, low = 1
  EXPECT_EQ(host.emissions()[1].type, "AlarmTally");
  const auto *h2 = host.emissions()[1].fields.find("high");
  const auto *l2 = host.emissions()[1].fields.find("low");
  ASSERT_TRUE(h2 && l2);
  EXPECT_EQ(std::get<std::int64_t>(h2->data), 1);
  EXPECT_EQ(std::get<std::int64_t>(l2->data), 1);
}

// ----------------------------------------------------------------------------
// 3. Heartbeat: Periodic Timers & Clock (docs/vagent-user-guide.md §"Schedules,
// clocks and orderly stopping")
// ----------------------------------------------------------------------------
constexpr const char *kGuideHeartbeatSource = R"vagent(
module demo:heartbeat
import std:agent

event Tick
event StopTicks
event HeartbeatPulse { count: Int }

agent Heartbeat(period: Duration) {
  var timer: Opt<Timer> := none
  var count: Int := 0

  on Initialize {
    timer := Schedules.periodic(period, Tick{})
  }

  on Tick {
    count := count + 1
    emit HeartbeatPulse{count: count} in outer
  }

  on StopTicks {
    if timer then {
      Schedules.cancel(timer)
      timer := none
    }
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, HeartbeatPeriodicTimerWithClockAdvance) {
  auto compiled =
      CompiledProgram::fromSource(kGuideHeartbeatSource, "heartbeat.vagent");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost::Options opts;
  opts.manualClock = true;
  LocalHost host(std::move(*compiled), opts);

  WireRecord args;
  args.set("period", WireValue::integer(100));

  auto pid = host.launch("Heartbeat", "hb", std::move(args));
  ASSERT_TRUE(pid.has_value());
  host.pump(); // Initialize runs and registers periodic timer

  // Advance clock by 100ms -> Tick 1
  host.advance(100ms);
  ASSERT_EQ(host.emissions().size(), 1U);
  EXPECT_EQ(host.emissions().back().type, "HeartbeatPulse");
  EXPECT_EQ(std::get<std::int64_t>(
                host.emissions().back().fields.find("count")->data),
            1);

  // Advance clock by 200ms -> Tick 2 and Tick 3
  host.advance(200ms);
  ASSERT_EQ(host.emissions().size(), 3U);
  EXPECT_EQ(std::get<std::int64_t>(
                host.emissions().back().fields.find("count")->data),
            3);

  // Cancel timer
  ASSERT_TRUE(host.send(*pid, makeEvent("StopTicks")).has_value());
  host.pump();

  // Advance clock further -> No new ticks fire
  host.advance(500ms);
  EXPECT_EQ(host.emissions().size(), 3U);
}

// ----------------------------------------------------------------------------
// 4. Parent / Child Messaging (docs/vagent-user-guide.md §"Messaging, spaces
// and identity")
// ----------------------------------------------------------------------------
constexpr const char *kGuideParentChildSource = R"vagent(
module demo:hierarchy

event Ping
event Pong
event PongTally { count: Int }

agent Child {
  on Ping {
    emit Pong{} in outer for parent
  }
}

agent Parent {
  var child: Opt<Agent> := none
  var replies: Int := 0

  on Initialize {
    child := spawn Child() in inner
    emit Ping{} in inner for child
  }

  on Pong {
    replies := replies + 1
    emit PongTally{count: replies} in outer
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, ParentChildSpawnAndInnerOuterMessaging) {
  auto compiled =
      CompiledProgram::fromSource(kGuideParentChildSource, "parent.vagent");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost host(std::move(*compiled));
  auto pid = host.launch("Parent", "parent-1");
  ASSERT_TRUE(pid.has_value());

  // Pump runs Initialize, which spawns Child in inner and sends Ping in inner.
  // Child handles Ping, emits Pong in outer for parent.
  // Parent handles Pong, increments replies, and emits PongTally in outer to
  // launcher.
  host.pump();

  ASSERT_EQ(host.emissions().size(), 1U);
  EXPECT_EQ(host.emissions()[0].type, "PongTally");
  const auto *count = host.emissions()[0].fields.find("count");
  ASSERT_NE(count, nullptr);
  EXPECT_EQ(std::get<std::int64_t>(count->data), 1);
}

// ----------------------------------------------------------------------------
// 5. Cluster Pipeline: Multi-Node TCP Peer Routing (docs/vagent-user-guide.md
// §"Concurrent cluster nodes and peer routing")
// ----------------------------------------------------------------------------
constexpr const char *kGuideClusterPipelineSource = R"vagent(
module demo:pipeline

predicate reading(batch_id: Str, sensor: Str, count: Int, raw: Int)
predicate computed(batch_id: Str, score: Int)
predicate audited(batch_id: Str, status: Str)

event StartSensor { batch_id: Str, readings: Int, raw: Int }
event SensorBatch { batch_id: Str, sensor_id: Str, readings_count: Int, total_raw: Int }
event ComputeResult { batch_id: Str, node_id: Str, total_raw: Int, scaled_score: Int }
event AuditVerdict { batch_id: Str, nodes_verified: Int, total_score: Int, status: Str }

agent SensorProducer {
  on StartSensor as s {
    emit SensorBatch{batch_id: s.batch_id, sensor_id: "node-1-sensor", readings_count: s.readings, total_raw: s.raw} in outer
  }
}

agent ComputeWorker {
  on SensorBatch as b {
    believe reading(b.batch_id, b.sensor_id, b.readings_count, b.total_raw)
    let score := b.total_raw * 2
    believe computed(b.batch_id, score)
    emit ComputeResult{batch_id: b.batch_id, node_id: "node-2-compute", total_raw: b.total_raw, scaled_score: score} in outer
  }
}

agent ClusterAuditor {
  on ComputeResult as c {
    if c.scaled_score >= 100 then {
      believe audited(c.batch_id, "VERIFIED")
      emit AuditVerdict{batch_id: c.batch_id, nodes_verified: 3, total_score: c.scaled_score, status: "VERIFIED"} in outer
    }
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, ThreeNodeClusterPipelineWithTcpPeerRouting) {
  auto c1 = CompiledProgram::fromSource(kGuideClusterPipelineSource,
                                        "pipeline.vagent");
  ASSERT_TRUE(c1.has_value()) << c1.error();
  auto c2 = CompiledProgram::fromSource(kGuideClusterPipelineSource,
                                        "pipeline.vagent");
  ASSERT_TRUE(c2.has_value());
  auto c3 = CompiledProgram::fromSource(kGuideClusterPipelineSource,
                                        "pipeline.vagent");
  ASSERT_TRUE(c3.has_value());

  // Node 3 (Auditor)
  ClusterNode::Options opts3;
  opts3.nodeId     = "node-3";
  opts3.listenHost = "127.0.0.1";
  opts3.listenPort = 0;

  ClusterNode node3(std::make_shared<CompiledProgram>(std::move(*c3)), opts3);
  ASSERT_TRUE(node3.start().has_value());
  const std::uint16_t port3 = node3.boundPort();
  ASSERT_GT(port3, 0);

  auto pidAuditor = node3.launch("ClusterAuditor", "auditor");
  ASSERT_TRUE(pidAuditor.has_value());

  // Node 2 (ComputeWorker) -> forwards ComputeResult to Node 3
  ClusterNode::Options opts2;
  opts2.nodeId     = "node-2";
  opts2.listenHost = "127.0.0.1";
  opts2.listenPort = 0;
  opts2.peers.push_back(PeerEndpoint{"node-3", "127.0.0.1", port3});
  opts2.forwardRules.push_back(
      ForwardRule{"ComputeResult", "node-3", "ClusterAuditor"});

  ClusterNode node2(std::make_shared<CompiledProgram>(std::move(*c2)), opts2);
  ASSERT_TRUE(node2.start().has_value());
  const std::uint16_t port2 = node2.boundPort();
  ASSERT_GT(port2, 0);

  auto pidWorker = node2.launch("ComputeWorker", "worker");
  ASSERT_TRUE(pidWorker.has_value());

  // Node 1 (SensorProducer) -> forwards SensorBatch to Node 2
  ClusterNode::Options opts1;
  opts1.nodeId     = "node-1";
  opts1.listenHost = "127.0.0.1";
  opts1.listenPort = 0;
  opts1.peers.push_back(PeerEndpoint{"node-2", "127.0.0.1", port2});
  opts1.forwardRules.push_back(
      ForwardRule{"SensorBatch", "node-2", "ComputeWorker"});

  ClusterNode node1(std::make_shared<CompiledProgram>(std::move(*c1)), opts1);
  ASSERT_TRUE(node1.start().has_value());

  auto pidSensor = node1.launch("SensorProducer", "sensor");
  ASSERT_TRUE(pidSensor.has_value());

  // Deliver StartSensor on Node 1: batch_id = "batch-101", raw = 75 (75 * 2 =
  // 150 >= 100)
  Envelope startEv =
      makeEvent("StartSensor", {
                                   {"batch_id", WireValue::text("batch-101")},
                                   {"readings", WireValue::integer(5)},
                                   {"raw", WireValue::integer(75)},
                               });
  ASSERT_TRUE(node1.send(*pidSensor, std::move(startEv)).has_value());

  // Tick all 3 nodes concurrently until Node 3 emits AuditVerdict
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         node3.emissions().empty()) {
    node1.tick(5ms);
    node2.tick(5ms);
    node3.tick(5ms);
  }

  // Node 1 emitted SensorBatch
  ASSERT_FALSE(node1.emissions().empty());
  EXPECT_EQ(node1.emissions().front().type, "SensorBatch");

  // Node 2 emitted ComputeResult
  ASSERT_FALSE(node2.emissions().empty());
  const auto &compRes = node2.emissions().front();
  EXPECT_EQ(compRes.type, "ComputeResult");
  EXPECT_EQ(std::get<std::int64_t>(compRes.fields.find("scaled_score")->data),
            150);

  // Node 3 emitted AuditVerdict over TCP routing
  ASSERT_FALSE(node3.emissions().empty());
  const auto &verdict = node3.emissions().front();
  EXPECT_EQ(verdict.type, "AuditVerdict");
  EXPECT_EQ(std::get<std::string>(verdict.fields.find("status")->data),
            "VERIFIED");
  EXPECT_EQ(std::get<std::int64_t>(verdict.fields.find("total_score")->data),
            150);
  EXPECT_EQ(std::get<std::int64_t>(verdict.fields.find("nodes_verified")->data),
            3);
}

// ----------------------------------------------------------------------------
// 6. Interactive Dashboard: Headless Virtual Screen (docs/vagent-user-guide.md
// §"Interactive terminal dashboard")
// ----------------------------------------------------------------------------
constexpr const char *kGuideDashboardSource = R"vagent(
module demo:dashboard

predicate at(who: Str, x: Float, y: Float)
predicate status(text: Str)

event Ping
event StepTick

agent Tracker {
  var x: Float := 10.0
  var y: Float := 20.0

  on Ping {
    believe at("scout", 12.5, 40.0)
    x := 12.5
    y := 40.0
  }

  on StepTick {
    x := x + 1.0
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, DashboardHeadlessRenderAndMonitoring) {
  auto compiled =
      CompiledProgram::fromSource(kGuideDashboardSource, "dashboard.vagent");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  LocalHost::Options hostOpts;
  hostOpts.manualClock = true;
  LocalHost host(std::move(*compiled), hostOpts);

  auto pid = host.launch("Tracker", "tracker-1");
  ASSERT_TRUE(pid.has_value());

  NCursesActor::Options actorOpts;
  actorOpts.forceHeadless = true;
  actorOpts.defaultWidth  = 80;
  actorOpts.defaultHeight = 25;
  auto actor              = std::make_shared<NCursesActor>(actorOpts);

  AgentTui tui(host.runtime(), host.executor(), actor);
  tui.step(1);
  tui.render();

  const auto &screen = tui.actor().screen();
  EXPECT_GT(screen.frameCount(), 0U);
  EXPECT_TRUE(screen.contains("VAGENT"));
  EXPECT_TRUE(screen.contains("Tracker"));
}

// ----------------------------------------------------------------------------
// 7. Workshop: BDI Dispatcher & Delegation (docs/vagent-user-guide.md §"Worked
// example: Deliberation and delegation")
// ----------------------------------------------------------------------------
constexpr const char *kGuideWorkshopSource = R"vagent(
module demo:workshop

predicate delivered(order_id: Str)
question stock(item: Str): Int

event Order { order_id: Str, item: Str, count: Int }
event Completed { order_id: Str }

goal Handle(order_id: Str, item: Str, count: Int) perform
goal Fetch(order_id: Str, item: Str, count: Int) perform

agent Warehouse(units: Int) {
  answer stock(item: Str): Int { units }
}

agent Courier {
  plan fetch for Fetch { tell parent delivered(order_id) }
}

agent Dispatcher(units: Int, deadline: Duration) {
  var warehouse: Opt<Agent> := none
  var courier: Opt<Agent> := none
  var completed: Int := 0

  on Initialize {
    warehouse := spawn Warehouse(units) in inner
    courier := spawn Courier() in inner
  }
  on Order as o { adopt Handle(o.order_id, o.item, o.count) }

  plan handle for Handle {
    let available := ask warehouse.stock(item) within deadline else 0
    if available < count then fail
    achieve Fetch(order_id, item, count) by courier within deadline
    await delivered(order_id) within deadline
    completed := completed + 1
    emit Completed{order_id: order_id}
  }
}
)vagent";

TEST(VagentGuideRuntimeTest, WorkshopWorkedExampleParsesAndChecksAsDocumented) {
  xanadu::vagent::Parser parser(kGuideWorkshopSource);
  auto program = parser.parseProgram();
  ASSERT_TRUE(program.has_value())
      << xanadu::vagent::formatDiagnostic(program.error());

  auto result = xanadu::vagent::check(*program);
  if (!result) {
    std::string errors;
    for (const auto &diagnostic : result.error()) {
      errors += xanadu::vagent::formatDiagnostic(diagnostic) + '\n';
    }
    FAIL() << errors;
  }
}

} // namespace
