#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_checkpoint.hpp"
#include "common/xanadu/vortex/mesh_epoch.hpp"
#include "identity_fixture.hpp"

namespace zigzag::vortex::mesh {
namespace {
using namespace std::chrono_literals;
namespace identity = xanadu::identity;

std::string shellQuote(std::string_view input) {
  std::string result{"'"};
  for (auto ch : input) result += ch == '\'' ? "'\\''" : std::string(1, ch);
  return result + "'";
}

// The more-specific route cuts only the named bootstrap in this isolated
// namespace. Cleanup restores its connected subnet route after any assertion.
class DhtPartition {
public:
  explicit DhtPartition(std::string host)
      : destination_(std::move(host) + "/32") {}
  bool block() {
    active_ =
        std::system(
            ("ip route add blackhole " + shellQuote(destination_)).c_str()) ==
        0;
    return active_;
  }
  bool heal() {
    if (!active_) return true;
    if (std::system(
            ("ip route del blackhole " + shellQuote(destination_)).c_str()) !=
        0)
      return false;
    active_ = false;
    return true;
  }
  ~DhtPartition() {
    EXPECT_TRUE(heal()) << "could not restore checkpoint DHT route";
  }

private:
  std::string destination_;
  bool active_{};
};

class MeshCheckpointNetworkTest : public testing::Test {
protected:
  xanadu::MutableKeys keys;
  identity::Fingerprint owner =
      *identity::Fingerprint::fromString(xanadu::testing::kAuthorFingerprint);
  std::filesystem::path directory;
  std::string peerHost;
  std::string testHost;
  std::uint16_t peerPort{};
  std::string checkpointHash;
  std::size_t executions{};
  std::vector<ActorMessage> replies;

  void SetUp() override {
    const auto host  = std::getenv("XUDU_PEER_HOST");
    const auto port  = std::getenv("XUDU_PEER_PORT");
    const auto local = std::getenv("XUDU_TEST_HOST");
    if (!host || !port || !local || !std::getenv("XUDU_PEER_NAMESPACE"))
      GTEST_SKIP() << "run make test/swarm for real checkpoint "
                      "DHT/restart/partition coverage";
    peerHost = host;
    testHost = local;
    ASSERT_FALSE(peerHost.empty());
    ASSERT_EQ(peerHost.find_first_not_of("0123456789."), std::string::npos);
    const auto parsed = std::stoul(port);
    ASSERT_GT(parsed, 0);
    ASSERT_LE(parsed, 65535);
    peerPort = static_cast<std::uint16_t>(parsed);
    keys.publicKey =
        xanadu::PublicKey::fromHex(xanadu::testing::kTestDevicePublicKeyHex);
    keys.secretKey =
        xanadu::SecretKey::fromHex(xanadu::testing::kTestDeviceSecretKeyHex);
    directory = std::filesystem::temp_directory_path() /
                ("xanadu-checkpoint-network-" +
                 xanadu::createMutableKeys().publicKey.hex());
    std::filesystem::create_directories(directory / "epochs");
  }
  void TearDown() override {
    if (!directory.empty()) std::filesystem::remove_all(directory);
  }
  std::shared_ptr<identity::IdentityNetworkController> controller() {
    auto result = std::make_shared<identity::IdentityNetworkController>(
        identity::IdentityNetworkController::Options{.localFingerprint = owner,
                                                     .localDeviceKeys  = keys});
    if (!result->trustDelegation(
            xanadu::testing::fixtureDelegation(keys.publicKey),
            xanadu::testing::kAuthorPublicKey))
      throw std::runtime_error("fixture device delegation refused");
    return result;
  }
  MeshTransportOptions options(std::string name, bool dht = true) {
    MeshTransportOptions result;
    result.downloads                            = directory / std::move(name);
    result.swarm.listenInterfaces               = testHost + ":0";
    result.swarm.enableDht                      = dht;
    result.swarm.enableLocalDiscovery           = false;
    result.swarm.enableTrackers                 = false;
    result.swarm.restrictDhtToDistinctNetworks  = false;
    result.swarm.allowManyConnectionsPerAddress = true;
    result.swarm.dhtPacketsPerSecond            = 100;
    result.swarm.readTimeout                    = 30s;
    result.swarm.metadataTimeout                = 30s;
    result.metadataTimeout                      = 30s;
    return result;
  }
  ActorMessage spawnRequest(bool fresh = false) {
    ActorMessage result;
    result.type       = identity::MessageType::ActorSpawn;
    result.requestId  = fresh ? "resume-counter" : "spawn-counter";
    result.source     = ActorPid{"counter-owner", "client", 1, 1};
    result.programRef = "immutable-counter-vm-program";
    result.capability =
        fresh ? "fresh-checkpoint-grant" : "initial-counter-grant";
    result.hashcashStamp = "fixture-authorized-stamp";
    return result;
  }
  ActorMessage increment(const ActorPid &pid,
                         std::string id = "increment-once") {
    ActorMessage result;
    result.type      = identity::MessageType::ActorSend;
    result.requestId = std::move(id);
    result.source    = spawnRequest().source;
    result.target    = pid;
    result.value     = "increment";
    return result;
  }
  void configure(MeshActorRuntime &runtime, bool fresh) {
    runtime.trustNode("counter-owner", owner);
    runtime.setAuthorizer(
        [this,
         fresh](const auto &identity,
                const auto &message) -> std::expected<ActorGrant, ActorError> {
          if (identity != owner ||
              message.capability !=
                  (fresh ? "fresh-checkpoint-grant" : "initial-counter-grant"))
            return std::unexpected(ActorError::NotAuthorized);
          return ActorGrant{message.programRef,
                            fresh ? checkpointHash : "initial-counter-base",
                            100000,
                            10000,
                            32768,
                            100,
                            65536,
                            std::chrono::system_clock::now() + 60s,
                            true};
        });
    runtime.setSender([this](std::string_view, const auto &message) {
      replies.push_back(message);
      return true;
    });
    runtime.setExecutor(
        [this, target = &runtime](
            const ActorExecution &execution,
            WorkerContext &context) -> std::expected<std::string, ActorError> {
          if (execution.message.value != "increment")
            return std::unexpected(ActorError::InvalidMessage);
          ++executions;
          CellRef root;
          if (execution.stateRoot)
            root = *execution.stateRoot;
          else {
            root = context.arena.makeScalarCell(std::int64_t{0});
            if (!target->setStateRoot(execution.pid, root))
              return std::unexpected(ActorError::ExecutionFailed);
          }
          const auto one  = context.arena.makeScalarCell(std::int64_t{1});
          const auto add  = context.vm.mintOpcode(OpcodeKind::Add);
          const auto halt = context.vm.mintOpcode(OpcodeKind::Halt);
          context.core.bindInput(add, root);
          context.core.bindInput(add, one);
          context.core.bindOutput(add, root);
          if (!context.arena.link(add, context.core.dims().spin, DimVector::POS,
                                  halt))
            return std::unexpected(ActorError::ExecutionFailed);
          if (!context.vm
                   .run(context.vm.spawnCursor(add),
                        execution.grant.maxInstructions)
                   .success)
            return std::unexpected(ActorError::ExecutionFailed);
          const auto value = context.arena.asInt64(root);
          if (!value) return std::unexpected(ActorError::ExecutionFailed);
          return std::to_string(*value);
        });
  }
};

TEST_F(MeshCheckpointNetworkTest,
       ExactDhtAckSurvivesPublisherRestartPartitionAndReactivation) {
  const auto oldNode =
      reserveNodeId(directory / "epochs", owner, keys.publicKey);
  ASSERT_TRUE(oldNode) << oldNode.error();
  auto original =
      std::make_unique<MeshActorRuntime>(oldNode->str(), oldNode->arenaEpoch);
  configure(*original, false);
  const auto wall   = std::chrono::system_clock::now();
  const auto oldPid = original->spawn(owner, spawnRequest(), wall);
  ASSERT_TRUE(oldPid);
  ASSERT_TRUE(original->receive(owner, increment(*oldPid), ActorTime{}, wall));
  ASSERT_EQ(original->pumpOne(wall), true);
  const auto stateRoot = original->stateRoot(*oldPid);
  ASSERT_TRUE(stateRoot);
  const auto checkpoint = original->checkpointView(*oldPid, *stateRoot);
  ASSERT_TRUE(checkpoint);
  ASSERT_EQ(checkpoint->arena.asInt64(checkpoint->root), 1);
  const auto continuation = encodeContinuation(checkpoint->continuation);
  ASSERT_TRUE(continuation) << continuation.error();
  xanadu::Store authored;
  const xanadu::SignedProvenance signer{"fixture", "signature"};
  const auto sealed = publishValue(authored, {}, checkpoint->arena,
                                   checkpoint->root, keys, *oldNode, signer,
                                   directory / "sealed", {}, {}, *continuation);
  ASSERT_TRUE(sealed) << sealed.error();
  checkpointHash = xanadu::sha256Hex(encodeValue(sealed->value));
  auto publisher =
      std::make_unique<MeshSwarmTransport>(controller(), options("publisher"));
  publisher->source().addDhtNode(peerHost, peerPort);
  ASSERT_TRUE(publisher->seed(*sealed));
  const auto listening = std::chrono::steady_clock::now() + 10s;
  auto publisherPort   = publisher->source().listenPort();
  while (!publisherPort && std::chrono::steady_clock::now() < listening) {
    publisher->poll();
    publisherPort = publisher->source().listenPort();
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(publisherPort);
  auto replicaOptions = options("replica", false);
  replicaOptions.peers.emplace_back(testHost, *publisherPort);
  auto replica = std::make_shared<MeshSwarmTransport>(
      controller(), std::move(replicaOptions));
  ASSERT_TRUE(replica->prepareValue(sealed->carrier.hash));
  const auto replicaPort = replica->source().listenPort();
  ASSERT_TRUE(replicaPort);
  auto replicated = fetchValue(sealed->carrier.hash, replica->source(),
                               std::make_shared<xanadu::UserPermascroll>());
  ASSERT_TRUE(replicated) << replicated.error();
  EXPECT_EQ(replicated->manifold->asInt64(replicated->root), 1);
  CheckpointPublisher pointer(checkpointBackend(publisher->source()));
  const auto salt =
      "actor-" + xanadu::createMutableKeys().publicKey.hex().substr(0, 32);
  auto status =
      pointer.begin(sealed->value, sealed->carrier.hash, keys, salt, 0);
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(status->state, CheckpointState::AwaitingAcknowledgement);
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  while (pointer.poll()->state != CheckpointState::Committed &&
         std::chrono::steady_clock::now() < deadline) {
    publisher->poll();
    replica->poll();
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(pointer.poll()->state, CheckpointState::Committed);
  EXPECT_TRUE(publisher->source().publicationAcknowledged(
      keys.publicKey, salt, sealed->carrier.hash, 1));
  auto wrongHash = sealed->carrier.hash;
  wrongHash.bytes[0] ^= 1;
  EXPECT_FALSE(publisher->source().publicationAcknowledged(keys.publicKey, salt,
                                                           wrongHash, 1));
  EXPECT_FALSE(publisher->source().publicationAcknowledged(
      keys.publicKey, salt, sealed->carrier.hash, 2));
  const auto name = pointer.poll()->name;
  publisher.reset();
  original.reset();
  DhtPartition partition(peerHost);
  ASSERT_TRUE(partition.block())
      << "test namespace must permit a real DHT route partition";
  {
    MeshSwarmTransport isolated(controller(),
                                options("partitioned-fresh-reader"));
    isolated.source().addDhtNode(peerHost, peerPort);
    const auto unavailable = resolveCheckpoint(
        name, isolated, std::make_shared<xanadu::UserPermascroll>(), 2s);
    ASSERT_FALSE(unavailable);
    EXPECT_NE(unavailable.error().find("pointer unavailable"),
              std::string::npos);
  }
  ASSERT_TRUE(partition.heal());
  auto readerOptions = options("restarted-fresh-reader");
  readerOptions.peers.emplace_back(testHost, *replicaPort);
  auto reader = std::make_shared<MeshSwarmTransport>(controller(),
                                                     std::move(readerOptions));
  reader->source().addDhtNode(peerHost, peerPort);
  auto restored = resolveCheckpoint(
      name, *reader, std::make_shared<xanadu::UserPermascroll>(), 30s);
  ASSERT_TRUE(restored) << restored.error();
  EXPECT_EQ(restored->sequence, 1);
  EXPECT_EQ(restored->state.value.origin, *oldNode);
  EXPECT_EQ(restored->state.manifold->asInt64(restored->state.root), 1);
  EXPECT_EQ(restored->continuation.pid, *oldPid);
  ASSERT_EQ(restored->continuation.completed.size(), 1);
  const auto newNode =
      reserveNodeId(directory / "epochs", owner, keys.publicKey);
  ASSERT_TRUE(newNode) << newNode.error();
  ASSERT_GT(newNode->arenaEpoch, oldNode->arenaEpoch);
  MeshActorRuntime resumed(newNode->str(), newNode->arenaEpoch);
  configure(resumed, true);
  executions = 0;
  replies.clear();
  const std::shared_ptr<const xanadu::ContentSource> lease(reader,
                                                           &reader->source());
  const auto newPid =
      resumed.resume(owner, spawnRequest(true), std::move(restored->state),
                     lease, std::chrono::system_clock::now());
  ASSERT_TRUE(newPid);
  EXPECT_NE(*newPid, *oldPid);
  ASSERT_TRUE(resumed.receive(owner, increment(*newPid), ActorTime{},
                              std::chrono::system_clock::now()));
  EXPECT_EQ(resumed.pending(*newPid), 0);
  EXPECT_EQ(executions, 0);
  ASSERT_EQ(replies.size(), 2U);
  EXPECT_EQ(replies.front().type, identity::MessageType::ActorDeliverValue);
  EXPECT_EQ(replies.front().value, "1");
  EXPECT_EQ(replies.back().type, identity::MessageType::ActorAck);
  ASSERT_TRUE(resumed.receive(owner,
                              increment(*newPid, "increment-after-restart"),
                              ActorTime{}, std::chrono::system_clock::now()));
  ASSERT_EQ(resumed.pumpOne(std::chrono::system_clock::now()), true);
  EXPECT_EQ(executions, 1);
  ASSERT_TRUE(resumed.stateRoot(*newPid));
  const auto advanced =
      resumed.checkpointView(*newPid, *resumed.stateRoot(*newPid));
  ASSERT_TRUE(advanced);
  EXPECT_EQ(advanced->arena.asInt64(advanced->root), 2);
}
} // namespace
} // namespace zigzag::vortex::mesh
