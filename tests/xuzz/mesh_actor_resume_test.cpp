#include <gtest/gtest.h>

#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vortex/mesh_checkpoint.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"

namespace zigzag::vortex::mesh {
namespace {
using xanadu::identity::Fingerprint;
using xanadu::identity::MessageType;
using namespace std::chrono_literals;
const auto wall = std::chrono::system_clock::time_point(1000s);

ActorMessage request(std::string id = "initial-spawn") {
  ActorMessage message;
  message.type          = MessageType::ActorSpawn;
  message.requestId     = std::move(id);
  message.source        = ActorPid{"owner", "client", 1, 1};
  message.programRef    = "immutable-counter-program";
  message.capability    = "initial-capability";
  message.hashcashStamp = "validated-stamp";
  return message;
}

ActorMessage increment(const ActorPid &pid, std::string id = "increment-once") {
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = std::move(id);
  message.source    = request().source;
  message.target    = pid;
  message.value     = "increment";
  return message;
}

class MeshActorResumeTest : public testing::Test {
protected:
  xanadu::MutableKeys keys = xanadu::createMutableKeys();
  NodeId priorNode{"prior-author", keys.publicKey.hex(), 7};
  Fingerprint owner = *Fingerprint::fromString(std::string(40, 'C'));
  std::shared_ptr<xanadu::DirectoryContentSource> source =
      std::make_shared<xanadu::DirectoryContentSource>();
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("mesh-actor-resume-" + keys.publicKey.hex());
  MeshActorRuntime previous{priorNode.str(), 7};
  std::string checkpointHash;
  WorkerContext *previousContext{};
  std::size_t resumedExecutions{};
  std::vector<ActorMessage> replies;

  ~MeshActorResumeTest() override { std::filesystem::remove_all(directory); }

  ActorGrant grant(const ActorMessage &message, std::string base) {
    return {message.programRef,
            std::move(base),
            100000,
            10000,
            32768,
            100,
            65536,
            wall + 100s,
            true};
  }

  void configure(MeshActorRuntime &runtime, bool resuming = false) {
    runtime.trustNode("owner", owner);
    runtime.setAuthorizer(
        [&, resuming](const Fingerprint &identity, const ActorMessage &message)
            -> std::expected<ActorGrant, ActorError> {
          if (identity != owner ||
              message.capability !=
                  (resuming ? "fresh-capability" : "initial-capability")) {
            return std::unexpected(ActorError::NotAuthorized);
          }
          return grant(message, resuming ? checkpointHash : "initial-base");
        });
    runtime.setSender([&](std::string_view, const ActorMessage &message) {
      replies.push_back(message);
      return true;
    });
    auto *runtimePointer = &runtime;
    runtime.setExecutor(
        [&, resuming, runtimePointer](
            const ActorExecution &execution,
            WorkerContext &context) -> std::expected<std::string, ActorError> {
          if (resuming)
            ++resumedExecutions;
          else
            previousContext = &context;
          CellRef root;
          if (execution.stateRoot)
            root = *execution.stateRoot;
          else {
            root = context.arena.makeScalarCell(std::int64_t{0});
            if (!runtimePointer->setStateRoot(execution.pid, root)) {
              return std::unexpected(ActorError::ExecutionFailed);
            }
          }
          EXPECT_EQ(runtimePointer->checkpointView(execution.pid, root).error(),
                    ActorError::NotQuiescent);
          const auto value = context.arena.asInt64(root);
          if (!value) return std::unexpected(ActorError::ExecutionFailed);
          const auto next = *value + 1;
          if (!context.arena.setValueBits(root, xanadu::ValueKind::Int64,
                                          static_cast<std::uint64_t>(next))) {
            return std::unexpected(ActorError::ExecutionFailed);
          }
          return std::to_string(next);
        });
  }

  std::expected<ValueSeed, std::string> seal() {
    configure(previous);
    const auto pid = previous.spawn(owner, request(), wall);
    if (!pid) return std::unexpected("fixture spawn failed");
    if (!previous.receive(owner, increment(*pid), ActorTime{}, wall) ||
        !previous.pumpOne(wall))
      return std::unexpected("fixture execution failed");
    const auto root = previous.stateRoot(*pid);
    if (!root) return std::unexpected("fixture state root missing");
    const auto snapshot = previous.checkpointView(*pid, *root);
    if (!snapshot) return std::unexpected("fixture not quiescent");
    const auto continuation = encodeContinuation(snapshot->continuation);
    if (!continuation) return std::unexpected(continuation.error());
    xanadu::Store store;
    const xanadu::SignedProvenance signer{"fixture", "signature"};
    auto seed =
        publishValue(store, {}, snapshot->arena, snapshot->root, keys,
                     priorNode, signer, directory, {}, {}, *continuation);
    if (!seed) return seed;
    for (const auto &payload : seed->payloads)
      source->add(payload.torrent.file, payload.path.string());
    source->add(seed->history.torrentFile,
                (directory / seed->history.hash.hex()).string());
    source->add(seed->carrier.file, seed->carrierPath.string());
    checkpointHash = xanadu::sha256Hex(encodeValue(seed->value));
    return seed;
  }

  std::expected<ReceivedValue, std::string> load(const ValueSeed &seed) {
    return fetchValue(seed.carrier.hash, *source,
                      std::make_shared<xanadu::UserPermascroll>());
  }

  ActorMessage freshRequest() {
    auto message       = request("resume-spawn");
    message.capability = "fresh-capability";
    return message;
  }
};

TEST_F(MeshActorResumeTest, ReactivationPreservesStateDedupAndChangesPid) {
  const auto seed = seal();
  ASSERT_TRUE(seed) << seed.error();
  auto checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint) << checkpoint.error();
  const auto continuation = decodeContinuation(checkpoint->value.continuation);
  ASSERT_TRUE(continuation);
  MeshActorRuntime resumed("new-device-node", 11);
  configure(resumed, true);
  const auto pid = resumed.resume(owner, freshRequest(), std::move(*checkpoint),
                                  source, wall);
  ASSERT_TRUE(pid);
  EXPECT_EQ(pid->node, "new-device-node");
  EXPECT_EQ(pid->epoch, 11);
  EXPECT_NE(*pid, continuation->pid);
  ASSERT_TRUE(resumed.stateRoot(*pid));
  const auto state = resumed.checkpointView(*pid, *resumed.stateRoot(*pid));
  ASSERT_TRUE(state);
  EXPECT_EQ(state->arena.asInt64(state->root), 1);
  EXPECT_EQ(state->continuation.baseRef, checkpointHash);
  ASSERT_EQ(state->continuation.completed.size(), 1U);
  ASSERT_TRUE(state->continuation.completed.front().response);
  EXPECT_EQ(state->continuation.completed.front().response->source, pid);
  replies.clear();
  ASSERT_TRUE(resumed.receive(owner, increment(*pid), ActorTime{}, wall));
  EXPECT_EQ(resumed.pending(*pid), 0);
  EXPECT_EQ(resumedExecutions, 0);
  ASSERT_GE(replies.size(), 1U);
  EXPECT_EQ(replies.front().type, MessageType::ActorDeliverValue);
  EXPECT_EQ(replies.front().source, pid);
  EXPECT_EQ(replies.front().value, "1");
  ASSERT_TRUE(resumed.receive(owner, increment(*pid, "new-increment"),
                              ActorTime{}, wall));
  ASSERT_EQ(resumed.pumpOne(wall), true);
  EXPECT_EQ(resumedExecutions, 1);
  const auto advanced = resumed.checkpointView(*pid, *resumed.stateRoot(*pid));
  ASSERT_TRUE(advanced);
  EXPECT_EQ(advanced->arena.asInt64(advanced->root), 2);
  // A resumed write shadows the snapshot and cannot mutate the old actor.
  const auto oldRoot = previous.stateRoot(continuation->pid);
  ASSERT_TRUE(oldRoot);
  EXPECT_EQ(previousContext->arena.asInt64(*oldRoot), 1);
}

TEST_F(MeshActorResumeTest, FreshCapabilityMustNameExactCheckpointAndOwner) {
  const auto seed = seal();
  ASSERT_TRUE(seed) << seed.error();
  MeshActorRuntime resumed("new-device-node", 11);
  auto checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(
      resumed
          .resume(owner, freshRequest(), std::move(*checkpoint), source, wall)
          .error(),
      ActorError::NotAuthorized);
  configure(resumed, true);
  resumed.setAuthorizer([&](const Fingerprint &, const ActorMessage &message) {
    return std::expected<ActorGrant, ActorError>(grant(message, "stale-base"));
  });
  checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(
      resumed
          .resume(owner, freshRequest(), std::move(*checkpoint), source, wall)
          .error(),
      ActorError::NotAuthorized);
  configure(resumed, true);
  checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(
      resumed.resume(owner, request(), std::move(*checkpoint), source, wall)
          .error(),
      ActorError::NotAuthorized);
  checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  auto wrongProgram       = freshRequest();
  wrongProgram.programRef = "different-program";
  EXPECT_EQ(
      resumed.resume(owner, wrongProgram, std::move(*checkpoint), source, wall)
          .error(),
      ActorError::InvalidMessage);
  checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(resumed
                .resume(owner, freshRequest(), std::move(*checkpoint), source,
                        wall + 100s)
                .error(),
            ActorError::NotAuthorized);
  EXPECT_EQ(resumed.actorCount(), 0);
}

TEST_F(MeshActorResumeTest,
       SignedStateIsRebuiltAndPinnedUntilActorDestruction) {
  const auto seed = seal();
  ASSERT_TRUE(seed) << seed.error();
  auto checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  // An altered receiver-local root is ignored; only the signed GlobalOpRef
  // determines which graph is rehydrated.
  checkpoint->root                                                   = 123456;
  const std::weak_ptr<xanadu::DirectoryContentSource> sourceLifetime = source;
  {
    MeshActorRuntime resumed("new-device-node", 11);
    configure(resumed, true);
    const auto pid = resumed.resume(owner, freshRequest(),
                                    std::move(*checkpoint), source, wall);
    ASSERT_TRUE(pid);
    source.reset();
    EXPECT_FALSE(sourceLifetime.expired());
    const auto snapshot =
        resumed.checkpointView(*pid, *resumed.stateRoot(*pid));
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->arena.textOf(snapshot->root), "1");
    EXPECT_EQ(snapshot->arena.asInt64(snapshot->root), 1);
  }
  EXPECT_TRUE(sourceLifetime.expired());
}

TEST_F(MeshActorResumeTest,
       CheckpointRefusesPendingMessagesChoicePointsAndMarks) {
  configure(previous);
  const auto pid = previous.spawn(owner, request(), wall);
  ASSERT_TRUE(pid);
  ASSERT_TRUE(previous.receive(owner, increment(*pid), ActorTime{}, wall));
  const auto cursor = previous.localCursor(*pid);
  ASSERT_TRUE(cursor);
  EXPECT_EQ(previous.checkpointView(*pid, *cursor).error(),
            ActorError::NotQuiescent);
  ASSERT_EQ(previous.pumpOne(wall), true);
  const auto root = previous.stateRoot(*pid);
  ASSERT_TRUE(root);
  previousContext->vm.pushChoicePoint(*cursor, *cursor);
  EXPECT_EQ(previous.checkpointView(*pid, *root).error(),
            ActorError::NotQuiescent);
  previousContext->vm.clearChoicePoints();
  const auto mark = previousContext->arena.mark();
  EXPECT_EQ(previous.checkpointView(*pid, *root).error(),
            ActorError::NotQuiescent);
  previousContext->arena.release(mark);
  EXPECT_TRUE(previous.checkpointView(*pid, *root));
}

TEST_F(MeshActorResumeTest, ForgedCheckpointAndReusedNodeEpochAreRefused) {
  const auto seed = seal();
  ASSERT_TRUE(seed) << seed.error();
  MeshActorRuntime resumed("new-device-node", 11);
  configure(resumed, true);
  auto checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  checkpoint->value.continuation += "tampered";
  EXPECT_EQ(
      resumed
          .resume(owner, freshRequest(), std::move(*checkpoint), source, wall)
          .error(),
      ActorError::InvalidMessage);
  MeshActorRuntime reused(priorNode.str(), 7);
  configure(reused, true);
  checkpoint = load(*seed);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(
      reused.resume(owner, freshRequest(), std::move(*checkpoint), source, wall)
          .error(),
      ActorError::WrongEpoch);
}
} // namespace
} // namespace zigzag::vortex::mesh
