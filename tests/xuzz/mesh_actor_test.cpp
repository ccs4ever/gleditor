#include <gtest/gtest.h>

#include "common/xanadu/vortex/mesh_actor.hpp"

#include <set>

namespace zigzag::vortex::mesh {
namespace {
using xanadu::identity::Fingerprint;
using xanadu::identity::MessageType;
using namespace std::chrono_literals;

const auto wall = std::chrono::system_clock::time_point(1000s);

ActorMessage spawnRequest(std::string requestId = "spawn-1") {
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = std::move(requestId);
  request.source        = ActorPid{"requester", "client", 1, 1};
  request.programRef    = "sha256:immutable-program";
  request.hashcashStamp = "verified-by-authorizer";
  request.capability    = "signed-grant";
  return request;
}

ActorGrant grantFor(const ActorMessage &request) {
  return {request.programRef,
          "sha256:immutable-base",
          100000,
          10000,
          32768,
          100,
          65536,
          wall + 100s,
          true};
}

void authorize(MeshActorRuntime &runtime, const Fingerprint &identity) {
  runtime.trustNode("requester", identity);
  runtime.setAuthorizer(
      [](const Fingerprint &,
         const ActorMessage &request) -> std::expected<ActorGrant, ActorError> {
        if (request.capability != "signed-grant") {
          return std::unexpected(ActorError::NotAuthorized);
        }
        return grantFor(request);
      });
  runtime.setExecutor(
      [](const ActorExecution &execution,
         WorkerContext &) -> std::expected<std::string, ActorError> {
        return "result:" + execution.message->value;
      });
}

ActorMessage sendTo(const ActorPid &pid, std::string request = "message-1") {
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = std::move(request);
  message.source    = ActorPid{"requester", "client", 1, 1};
  message.target    = pid;
  message.value     = "input";
  return message;
}

TEST(MeshActorTest,
     SpawnIsDefaultRefuseAndRequiresVerifiedNodeCapabilityAndBudget) {
  MeshActorRuntime runtime("worker", 1);
  Fingerprint identity;
  const auto request = spawnRequest();
  EXPECT_EQ(runtime.spawn(identity, request, wall).error(),
            ActorError::NotAuthorized);
  authorize(runtime, identity);
  auto changed       = request;
  changed.capability = "forged-grant";
  EXPECT_EQ(runtime.spawn(identity, changed, wall).error(),
            ActorError::NotAuthorized);
  EXPECT_EQ(runtime.spawn(identity, request, wall + 100s).error(),
            ActorError::NotAuthorized);
  runtime.setAuthorizer([](const Fingerprint &, const ActorMessage &request) {
    auto grant            = grantFor(request);
    grant.maxInstructions = 0;
    return std::expected<ActorGrant, ActorError>(grant);
  });
  EXPECT_EQ(runtime.spawn(identity, request, wall).error(),
            ActorError::NotAuthorized);
  EXPECT_EQ(runtime.actorCount(), 0);
}

TEST(MeshActorTest,
     EachPidOwnsPrivateContextAndRealCursorAndSpawnIsIdempotent) {
  MeshActorRuntime runtime("worker", 4);
  Fingerprint identity;
  authorize(runtime, identity);
  const auto first  = runtime.spawn(identity, spawnRequest(), wall);
  const auto second = runtime.spawn(identity, spawnRequest("spawn-2"), wall);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->cursor, second->cursor);
  EXPECT_EQ(runtime.spawn(identity, spawnRequest(), wall), first);
  EXPECT_EQ(runtime.actorCount(), 2);
  EXPECT_TRUE(runtime.localCursor(*first));
  EXPECT_TRUE(runtime.localCursor(*second));
  std::set<const WorkerContext *> contexts;
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &context) -> std::expected<std::string, ActorError> {
        contexts.insert(&context);
        EXPECT_TRUE(context.arena.contains(execution.cursor));
        const auto cursorList = context.vm.activeCursors();
        EXPECT_NE(std::ranges::find(cursorList, execution.cursor),
                  cursorList.end());
        context.arena.makeCell(execution.message->value);
        return "ok";
      });
  ASSERT_TRUE(runtime.receive(identity, sendTo(*first), ActorTime{}, wall));
  ASSERT_TRUE(runtime.receive(identity, sendTo(*second), ActorTime{}, wall));
  ASSERT_EQ(runtime.pumpOne(wall), true);
  ASSERT_EQ(runtime.pumpOne(wall), true);
  EXPECT_EQ(contexts.size(), 2);
  EXPECT_EQ(runtime.pumpOne(wall), false);
}

TEST(MeshActorTest, MailboxBackpressureDeduplicationAndCachedResultRedelivery) {
  ActorOptions options;
  options.maxMailboxMessages = 1;
  MeshActorRuntime runtime("worker", 1, options);
  Fingerprint identity;
  authorize(runtime, identity);
  const auto pid = runtime.spawn(identity, spawnRequest(), wall);
  ASSERT_TRUE(pid);
  unsigned executions = 0;
  runtime.setExecutor(
      [&](const ActorExecution &,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        ++executions;
        return "immutable-answer";
      });
  std::vector<ActorMessage> emitted;
  runtime.setSender([&](std::string_view, const ActorMessage &message) {
    emitted.push_back(message);
    return true;
  });
  const auto message = sendTo(*pid);
  ASSERT_TRUE(runtime.receive(identity, message, ActorTime{}, wall));
  ASSERT_TRUE(runtime.receive(identity, message, ActorTime{}, wall));
  EXPECT_EQ(runtime.pending(*pid), 1);
  EXPECT_EQ(runtime.receive(identity, sendTo(*pid, "other"), ActorTime{}, wall)
                .error(),
            ActorError::MailboxFull);
  auto conflict  = message;
  conflict.value = "different-content-same-id";
  EXPECT_EQ(runtime.receive(identity, conflict, ActorTime{}, wall).error(),
            ActorError::DuplicateMismatch);
  ASSERT_EQ(runtime.pumpOne(wall), true);
  ASSERT_TRUE(runtime.receive(identity, message, ActorTime{}, wall));
  EXPECT_EQ(runtime.pending(*pid), 0);
  EXPECT_EQ(executions, 1);
  const auto resultCount = std::ranges::count_if(emitted, [](const auto &sent) {
    return sent.type == MessageType::ActorDeliverValue;
  });
  EXPECT_EQ(resultCount, 2);
  auto wrongEpoch          = message;
  wrongEpoch.target->epoch = 2;
  EXPECT_EQ(runtime.receive(identity, wrongEpoch, ActorTime{}, wall).error(),
            ActorError::WrongEpoch);
}

TEST(MeshActorTest, DeliveryRetryRequiresMatchingAuthenticatedAck) {
  MeshActorRuntime runtime("worker", 1);
  Fingerprint identity;
  authorize(runtime, identity);
  runtime.trustNode("remote", identity);
  const auto pid = runtime.spawn(identity, spawnRequest(), wall);
  ASSERT_TRUE(pid);
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = "remote-request";
  message.source    = *pid;
  message.target    = ActorPid{"remote", "remote-cursor", 1, 1};
  unsigned sent     = 0;
  runtime.setSender([&](std::string_view, const ActorMessage &) {
    ++sent;
    return true;
  });
  ASSERT_TRUE(runtime.send("remote", message));
  ASSERT_TRUE(runtime.retry("remote-request"));
  EXPECT_EQ(sent, 2);
  EXPECT_FALSE(runtime.deliveryAcknowledged("remote-request"));
  auto ack              = message;
  ack.type              = MessageType::ActorAck;
  ack.source            = message.target;
  ack.target            = message.source;
  auto forged           = ack;
  forged.source->cursor = "unrelated";
  EXPECT_EQ(runtime.receive(identity, forged, ActorTime{}, wall).error(),
            ActorError::InvalidMessage);
  ASSERT_TRUE(runtime.receive(identity, ack, ActorTime{}, wall));
  EXPECT_TRUE(runtime.deliveryAcknowledged("remote-request"));
  ASSERT_TRUE(runtime.retry("remote-request"));
  EXPECT_EQ(sent, 2);
}

TEST(MeshActorTest, GoneRequiresPhiLeaseAndFreshNoSeederAndLedgerEvidence) {
  ActorOptions options;
  options.suspectPhi    = 1;
  options.leaseDuration = 10s;
  MeshActorRuntime runtime("worker", 1, options);
  const ActorPid watcher{"worker", "watcher", 1, 1};
  const ActorPid target{"remote", "target", 1, 1};
  const ActorTime start{};
  std::vector<ActorMessage> emitted;
  runtime.setSender([&](std::string_view, const ActorMessage &message) {
    emitted.push_back(message);
    return true;
  });
  ASSERT_TRUE(runtime.link(watcher, target, start));
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Alive);
  runtime.tick(start + 3s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Suspect);
  runtime.observe(target, {start + 3s, true, true, true});
  runtime.tick(start + 11s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Suspect);
  runtime.observe(target, {start + 12s, true, false, true});
  runtime.tick(start + 12s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Suspect);
  runtime.observe(target, {start + 13s, true, true, false});
  runtime.tick(start + 13s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Suspect);
  runtime.observe(target, {start + 14s, false, true, true});
  runtime.tick(start + 14s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Suspect);
  runtime.observe(target, {start + 15s, true, true, true});
  runtime.tick(start + 15s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Gone);
  ASSERT_EQ(emitted.size(), 1);
  EXPECT_EQ(emitted.front().reason, "unreachable");
  EXPECT_EQ(emitted.front().type, MessageType::ActorDown);
  runtime.tick(start + 16s);
  EXPECT_EQ(emitted.size(), 1);
  runtime.heartbeat(target, start + 17s);
  EXPECT_EQ(runtime.liveness(target), ActorLiveness::Alive);
}

TEST(MeshActorTest,
     RedispatchPreservesImmutableInputAndRefusesStatefulRestart) {
  MeshActorRuntime runtime("worker", 1);
  Fingerprint identity;
  authorize(runtime, identity);
  const auto request = spawnRequest();
  const auto pid     = runtime.spawn(identity, request, wall);
  ASSERT_TRUE(pid);
  std::optional<ActorMessage> emitted;
  runtime.setSender([&](std::string_view node, const ActorMessage &message) {
    EXPECT_EQ(node, "replacement");
    emitted = message;
    return true;
  });
  ASSERT_TRUE(runtime.redispatch(*pid, "replacement"));
  ASSERT_TRUE(emitted);
  EXPECT_EQ(*emitted, request);
  runtime.setAuthorizer([](const Fingerprint &, const ActorMessage &request) {
    auto grant      = grantFor(request);
    grant.immutable = false;
    return std::expected<ActorGrant, ActorError>(grant);
  });
  const auto stateful = runtime.spawn(identity, spawnRequest("stateful"), wall);
  ASSERT_TRUE(stateful);
  EXPECT_EQ(runtime.redispatch(*stateful, "replacement").error(),
            ActorError::NotImmutable);
}
} // namespace
} // namespace zigzag::vortex::mesh
