#include <gtest/gtest.h>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/vortex/mesh_lens_channel.hpp"
#include "common/xanadu/vortex/spool_mesh.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

using namespace zigzag;
using namespace zigzag::vortex::mesh;

TEST(MeshLensChannelTest, SimulatedTwoPeerMaterializationHop) {
  auto wire = std::make_shared<LoopbackChannel>();

  NodeId clientNode{
      .authorKey = "alice", .deviceKey = "client-dev", .arenaEpoch = 1};
  NodeId serverNode{
      .authorKey = "bob", .deviceKey = "server-dev", .arenaEpoch = 1};

  MeshLensChannel client(clientNode, wire);
  MeshLensChannel server(serverNode, wire);

  ArenaManifold clientArena;

  // 1. Client creates unmaterialized proxy cell
  const CellRef pCell =
      client.createProxy(clientArena, serverNode, "proc/42/status");
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Unmaterialized);

  // 2. Client requests materialization
  client.requestMaterialization(pCell, 1000);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Resolving);

  // 3. Server serves peer requests
  std::size_t providerCalls = 0;
  auto provider             = [&providerCalls](
                      std::string_view cellName) -> std::optional<std::string> {
    ++providerCalls;
    if (cellName == "proc/42/status") {
      return "PID 42: RUNNING, mem=64MB";
    }
    return std::nullopt;
  };

  const auto answered = server.servePeerRequests(provider);
  EXPECT_EQ(answered, 1U);
  EXPECT_EQ(providerCalls, 1U);

  // 4. Client polls incoming
  const auto resolved = client.pollIncoming(clientArena);
  EXPECT_EQ(resolved, 1U);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Active);

  const auto *proxy = client.getProxy(pCell);
  ASSERT_NE(proxy, nullptr);
  EXPECT_EQ(proxy->content, "PID 42: RUNNING, mem=64MB");
}

TEST(MeshLensChannelTest, FrontierExpansionPrefetchesAlongDimension) {
  auto wire = std::make_shared<LoopbackChannel>();

  NodeId clientNode{
      .authorKey = "alice", .deviceKey = "client-dev", .arenaEpoch = 1};
  NodeId serverNode{
      .authorKey = "bob", .deviceKey = "server-dev", .arenaEpoch = 1};

  MeshLensChannel client(clientNode, wire);
  ArenaManifold clientArena;

  const auto dNext = clientArena.ensureDimension("d.next");

  const CellRef p0 = client.createProxy(clientArena, serverNode, "item/0");
  const CellRef p1 = client.createProxy(clientArena, serverNode, "item/1");
  const CellRef p2 = client.createProxy(clientArena, serverNode, "item/2");

  // Link along d.next: p0 -> p1 -> p2
  expectWritten(clientArena.link(p0, dNext, DimVector::POS, p1));
  expectWritten(clientArena.link(p1, dNext, DimVector::POS, p2));

  // Request p0 and trigger frontier expansion with depth 2
  client.requestMaterialization(p0, 1000);
  client.frontierExpand(clientArena, p0, dNext, DimVector::POS, 2, 1000);

  // Both p1 and p2 should have been prefetched and moved to Resolving
  EXPECT_EQ(client.stateOf(p0), ProxyCellState::Resolving);
  EXPECT_EQ(client.stateOf(p1), ProxyCellState::Resolving);
  EXPECT_EQ(client.stateOf(p2), ProxyCellState::Resolving);
}

TEST(MeshLensChannelTest, TimeoutTransitionsToDisconnectedWithoutBlocking) {
  auto wire = std::make_shared<LoopbackChannel>();

  NodeId clientNode{
      .authorKey = "alice", .deviceKey = "client-dev", .arenaEpoch = 1};
  NodeId serverNode{
      .authorKey = "unreachable", .deviceKey = "server-dev", .arenaEpoch = 1};

  MeshLensChannel client(clientNode, wire);
  ArenaManifold clientArena;

  const CellRef pCell =
      client.createProxy(clientArena, serverNode, "sensors/temp");
  client.requestMaterialization(pCell, 1000);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Resolving);

  // Advance time to 1200 (200ms < 500ms timeout)
  EXPECT_EQ(client.checkTimeouts(1200, 500), 0U);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Resolving);

  // Advance time to 1600 (600ms >= 500ms timeout)
  EXPECT_EQ(client.checkTimeouts(1600, 500), 1U);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Disconnected);

  const auto *proxy = client.getProxy(pCell);
  ASSERT_NE(proxy, nullptr);
  EXPECT_EQ(proxy->content, "[Disconnected: sensors/temp]");
}

TEST(MeshLensChannelTest, DelimiterBytesInNamesAndValuesRoundTripExactly) {
  auto wire = std::make_shared<LoopbackChannel>();
  NodeId clientNode{
      .authorKey = "alice", .deviceKey = "client-dev", .arenaEpoch = 1};
  NodeId serverNode{
      .authorKey = "bo|b", .deviceKey = "server|dev", .arenaEpoch = 1};
  MeshLensChannel client(clientNode, wire);
  MeshLensChannel server(serverNode, wire);
  ArenaManifold clientArena;

  const std::string name  = "x|y";
  const std::string value = "a|b||c|";
  const CellRef pCell     = client.createProxy(clientArena, serverNode, name);
  client.requestMaterialization(pCell, 1000);

  std::string asked;
  const auto answered = server.servePeerRequests(
      [&](std::string_view cell) -> std::optional<std::string> {
        asked = std::string(cell);
        return value;
      });
  EXPECT_EQ(answered, 1U);
  EXPECT_EQ(asked, name) << "the cell name must not be split at '|'";

  EXPECT_EQ(client.pollIncoming(clientArena), 1U);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Active);
  EXPECT_EQ(client.getProxy(pCell)->content, value);
}

TEST(MeshLensChannelTest, MalformedLensFramesAreRefusedNotReadShort) {
  auto wire = std::make_shared<LoopbackChannel>();
  NodeId clientNode{
      .authorKey = "alice", .deviceKey = "client-dev", .arenaEpoch = 1};
  NodeId serverNode{
      .authorKey = "bob", .deviceKey = "server-dev", .arenaEpoch = 1};
  MeshLensChannel client(clientNode, wire);
  MeshLensChannel server(serverNode, wire);
  ArenaManifold clientArena;

  const CellRef pCell = client.createProxy(clientArena, serverNode, "cell");
  client.requestMaterialization(pCell, 1000);
  // Tagged as a response but one field short, and one with trailing bytes.
  wire->send(MeshMessage::value(serverNode, "l9:LENS_RESP3:bob4:celle"));
  wire->send(MeshMessage::value(
      serverNode, "l9:LENS_RESP" + std::to_string(serverNode.str().size()) +
                      ":" + serverNode.str() + "4:cell3:valeEXTRA"));

  EXPECT_EQ(client.pollIncoming(clientArena), 0U);
  EXPECT_EQ(client.refusedFrames(), 2U);
  ASSERT_TRUE(client.lastRefusal().has_value());
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Resolving)
      << "a short frame must not activate the proxy with short content";
}

TEST(MeshLensChannelTest, ALateResponseDoesNotReviveATimedOutProxy) {
  auto wire = std::make_shared<LoopbackChannel>();
  NodeId clientNode{
      .authorKey = "alice", .deviceKey = "client-dev", .arenaEpoch = 1};
  NodeId serverNode{
      .authorKey = "bob", .deviceKey = "server-dev", .arenaEpoch = 1};
  MeshLensChannel client(clientNode, wire);
  MeshLensChannel server(serverNode, wire);
  ArenaManifold clientArena;

  const CellRef pCell = client.createProxy(clientArena, serverNode, "slow");
  client.requestMaterialization(pCell, 1000);
  EXPECT_EQ(client.checkTimeouts(2000, 500), 1U);
  EXPECT_EQ(server.servePeerRequests(
                [](std::string_view) -> std::optional<std::string> {
                  return "late";
                }),
            1U);
  EXPECT_EQ(client.pollIncoming(clientArena), 0U);
  EXPECT_EQ(client.stateOf(pCell), ProxyCellState::Disconnected);
}
