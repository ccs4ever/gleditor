/**
 * @file vagent_cluster_node_test.cpp
 * @brief Unit and integration tests for concurrent ClusterNode internode
 * routing.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/vagent/cluster/cluster_node.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"

namespace {

using namespace xanadu::vagent::cluster;
using namespace xanadu::vagent::runtime;

constexpr std::string_view kClusterPipelineSource = R"(
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
    let ok := c.scaled_score >= 100
    if ok then {
      believe audited(c.batch_id, "VERIFIED")
      emit AuditVerdict{batch_id: c.batch_id, nodes_verified: 3, total_score: c.scaled_score, status: "VERIFIED"} in outer
    }
  }
}
)";

TEST(VagentClusterNodeTest, ClusterNodeLifecycle) {
  auto compiled =
      CompiledProgram::fromSource(kClusterPipelineSource, "test.vagent");
  ASSERT_TRUE(compiled.has_value()) << compiled.error();

  ClusterNode::Options opts;
  opts.nodeId     = "node-test";
  opts.listenHost = "127.0.0.1";
  opts.listenPort = 0; // dynamic port

  ClusterNode node(std::make_shared<CompiledProgram>(std::move(*compiled)),
                   opts);
  EXPECT_FALSE(node.isRunning());
  EXPECT_EQ(node.boundPort(), 0);

  auto started = node.start();
  ASSERT_TRUE(started.has_value());
  EXPECT_TRUE(node.isRunning());
  EXPECT_GT(node.boundPort(), 0);

  node.stop();
  EXPECT_FALSE(node.isRunning());
}

TEST(VagentClusterNodeTest, TwoNodePeerRouting) {
  auto compiled1 =
      CompiledProgram::fromSource(kClusterPipelineSource, "test.vagent");
  ASSERT_TRUE(compiled1.has_value());
  auto compiled2 =
      CompiledProgram::fromSource(kClusterPipelineSource, "test.vagent");
  ASSERT_TRUE(compiled2.has_value());

  // Setup Node 2 (Worker)
  ClusterNode::Options opts2;
  opts2.nodeId     = "node-2";
  opts2.listenHost = "127.0.0.1";
  opts2.listenPort = 0;

  ClusterNode node2(std::make_shared<CompiledProgram>(std::move(*compiled2)),
                    opts2);
  ASSERT_TRUE(node2.start().has_value());
  const std::uint16_t port2 = node2.boundPort();
  ASSERT_GT(port2, 0);

  auto pidWorker = node2.launch("ComputeWorker", "ComputeWorker");
  ASSERT_TRUE(pidWorker.has_value());

  // Setup Node 1 (Sensor)
  ClusterNode::Options opts1;
  opts1.nodeId     = "node-1";
  opts1.listenHost = "127.0.0.1";
  opts1.listenPort = 0;
  opts1.peers.push_back(PeerEndpoint{"node-2", "127.0.0.1", port2});
  opts1.forwardRules.push_back(
      ForwardRule{"SensorBatch", "node-2", "ComputeWorker"});

  ClusterNode node1(std::make_shared<CompiledProgram>(std::move(*compiled1)),
                    opts1);
  ASSERT_TRUE(node1.start().has_value());

  auto pidSensor = node1.launch("SensorProducer", "SensorProducer");
  ASSERT_TRUE(pidSensor.has_value());

  // Deliver StartSensor on Node 1
  Envelope startEv;
  startEv.kind = "event";
  startEv.type = "StartSensor";
  startEv.fields.set("batch_id", WireValue::text("batch-42"));
  startEv.fields.set("readings", WireValue::integer(10));
  startEv.fields.set("raw", WireValue::integer(75));

  ASSERT_TRUE(node1.send(*pidSensor, std::move(startEv)).has_value());

  // Pump both nodes until Node 2 receives and processes the forwarded event
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         node2.emissions().empty()) {
    node1.tick(std::chrono::milliseconds(10));
    node2.tick(std::chrono::milliseconds(10));
  }

  // Verify Node 1 emitted SensorBatch
  ASSERT_FALSE(node1.emissions().empty());
  EXPECT_EQ(node1.emissions().front().type, "SensorBatch");

  // Verify Node 2 emitted ComputeResult over internode TCP routing
  ASSERT_FALSE(node2.emissions().empty());
  const auto &compRes = node2.emissions().front();
  EXPECT_EQ(compRes.type, "ComputeResult");

  const auto *score = compRes.fields.find("scaled_score");
  ASSERT_NE(score, nullptr);
  EXPECT_EQ(std::get<std::int64_t>(score->data), 150); // 75 * 2
}

TEST(VagentClusterNodeTest, ThreeNodeEndToEndCluster) {
  auto c1 = CompiledProgram::fromSource(kClusterPipelineSource, "test.vagent");
  ASSERT_TRUE(c1.has_value());
  auto c2 = CompiledProgram::fromSource(kClusterPipelineSource, "test.vagent");
  ASSERT_TRUE(c2.has_value());
  auto c3 = CompiledProgram::fromSource(kClusterPipelineSource, "test.vagent");
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

  auto pidAuditor = node3.launch("ClusterAuditor", "ClusterAuditor");
  ASSERT_TRUE(pidAuditor.has_value());

  // Node 2 (Worker)
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

  auto pidWorker = node2.launch("ComputeWorker", "ComputeWorker");
  ASSERT_TRUE(pidWorker.has_value());

  // Node 1 (Sensor)
  ClusterNode::Options opts1;
  opts1.nodeId     = "node-1";
  opts1.listenHost = "127.0.0.1";
  opts1.listenPort = 0;
  opts1.peers.push_back(PeerEndpoint{"node-2", "127.0.0.1", port2});
  opts1.forwardRules.push_back(
      ForwardRule{"SensorBatch", "node-2", "ComputeWorker"});

  ClusterNode node1(std::make_shared<CompiledProgram>(std::move(*c1)), opts1);
  ASSERT_TRUE(node1.start().has_value());

  auto pidSensor = node1.launch("SensorProducer", "SensorProducer");
  ASSERT_TRUE(pidSensor.has_value());

  // Deliver StartSensor on Node 1
  Envelope startEv;
  startEv.kind = "event";
  startEv.type = "StartSensor";
  startEv.fields.set("batch_id", WireValue::text("batch-999"));
  startEv.fields.set("readings", WireValue::integer(8));
  startEv.fields.set("raw", WireValue::integer(90));

  ASSERT_TRUE(node1.send(*pidSensor, std::move(startEv)).has_value());

  // Concurrently tick all 3 nodes
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         node3.emissions().empty()) {
    node1.tick(std::chrono::milliseconds(5));
    node2.tick(std::chrono::milliseconds(5));
    node3.tick(std::chrono::milliseconds(5));
  }

  // Assert Node 3 emitted AuditVerdict
  ASSERT_FALSE(node3.emissions().empty());
  const auto &verdict = node3.emissions().front();
  EXPECT_EQ(verdict.type, "AuditVerdict");

  const auto *status = verdict.fields.find("status");
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(std::get<std::string>(status->data), "VERIFIED");

  const auto *totalScore = verdict.fields.find("total_score");
  ASSERT_NE(totalScore, nullptr);
  EXPECT_EQ(std::get<std::int64_t>(totalScore->data), 180); // 90 * 2

  const auto *nodesVerified = verdict.fields.find("nodes_verified");
  ASSERT_NE(nodesVerified, nullptr);
  EXPECT_EQ(std::get<std::int64_t>(nodesVerified->data), 3);
}

} // namespace
