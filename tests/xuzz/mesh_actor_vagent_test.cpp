#include <gtest/gtest.h>

#include "common/xanadu/vortex/mesh_actor.hpp"

#include <format>

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

ActorMessage sendTo(const ActorPid &pid, std::string request,
                    std::string value = "input") {
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = std::move(request);
  message.source    = ActorPid{"requester", "client", 1, 1};
  message.target    = pid;
  message.value     = std::move(value);
  return message;
}

const ActorPid peerPid{"peer", "remote", 1, 1};

ActorMessage outbound(const ActorPid &from, std::string request) {
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = std::move(request);
  message.source    = from;
  message.target    = peerPid;
  message.value     = "ask";
  return message;
}

ActorMessage reply(const ActorPid &to, std::string request) {
  ActorMessage message;
  message.type      = MessageType::ActorDeliverValue;
  message.requestId = std::move(request);
  message.source    = peerPid;
  message.target    = to;
  message.value     = "answer";
  return message;
}

class MeshActorVagent : public testing::Test {
protected:
  Fingerprint identity     = *Fingerprint::fromString(std::string(40, 'A'));
  Fingerprint peerIdentity = *Fingerprint::fromString(std::string(40, 'B'));
  std::vector<ActorMessage> emitted;
  std::vector<std::optional<ActorMessage>> executions;
  std::vector<ActorPid> executedBy;

  void configure(MeshActorRuntime &runtime) {
    runtime.trustNode("requester", identity);
    runtime.trustNode("peer", peerIdentity);
    runtime.setAuthorizer([](const Fingerprint &, const ActorMessage &request)
                              -> std::expected<ActorGrant, ActorError> {
      if (request.capability != "signed-grant") {
        return std::unexpected(ActorError::NotAuthorized);
      }
      return grantFor(request);
    });
    runtime.setSender([this](std::string_view, const ActorMessage &message) {
      emitted.push_back(message);
      return true;
    });
    runtime.setExecutor(
        [this](const ActorExecution &execution,
               WorkerContext &) -> std::expected<std::string, ActorError> {
          executions.push_back(execution.message);
          executedBy.push_back(execution.pid);
          return "ok";
        });
  }

  ActorPid spawnOne(MeshActorRuntime &runtime, std::string id = "spawn-1") {
    const auto pid = runtime.spawn(identity, spawnRequest(std::move(id)), wall);
    EXPECT_TRUE(pid.has_value());
    return pid.value_or(ActorPid{});
  }
};

TEST_F(MeshActorVagent, CompletedDeliveriesAreReapedSoTheCeilingIsNotLifetime) {
  ActorOptions options;
  options.maxOutstandingDeliveries = 4;
  MeshActorRuntime runtime("worker", 1, options);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  for (int i = 0; i < 20; ++i) {
    const auto id = std::format("ask-{}", i);
    ASSERT_TRUE(runtime.send("peer", outbound(pid, id))) << i;
    ASSERT_TRUE(
        runtime.receive(peerIdentity, reply(pid, id), ActorTime{}, wall))
        << i;
    ASSERT_EQ(runtime.pumpOne(wall), true);
  }
  EXPECT_EQ(runtime.deliveryCount(), 0U);
}

TEST_F(MeshActorVagent, IncompleteDeliveriesStillCountAgainstTheCeiling) {
  ActorOptions options;
  options.maxOutstandingDeliveries = 2;
  MeshActorRuntime runtime("worker", 1, options);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.send("peer", outbound(pid, "a")));
  ASSERT_TRUE(runtime.send("peer", outbound(pid, "b")));
  EXPECT_EQ(runtime.send("peer", outbound(pid, "c")).error(),
            ActorError::ResourceLimit);
}

TEST_F(MeshActorVagent, AbandonErasesTheDeliveryAndIgnoresALateReply) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.send("peer", outbound(pid, "ask")));
  ASSERT_TRUE(runtime.abandon("ask"));
  EXPECT_EQ(runtime.deliveryCount(), 0U);
  EXPECT_EQ(runtime.abandon("ask").error(), ActorError::InvalidMessage);
  EXPECT_EQ(runtime.receive(peerIdentity, reply(pid, "ask"), ActorTime{}, wall)
                .error(),
            ActorError::NotAuthorized);
  EXPECT_EQ(runtime.pending(pid), 0U);
}

TEST_F(MeshActorVagent, RedeliverEmitsTheStoredMessageEvenAfterAnAck) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.send("peer", outbound(pid, "ask")));
  ActorMessage ack;
  ack.type      = MessageType::ActorAck;
  ack.requestId = "ask";
  ack.source    = peerPid;
  ack.target    = pid;
  ASSERT_TRUE(runtime.receive(peerIdentity, ack, ActorTime{}, wall));
  ASSERT_TRUE(runtime.deliveryAcknowledged("ask"));
  emitted.clear();
  ASSERT_TRUE(runtime.retry("ask"));
  EXPECT_TRUE(emitted.empty());
  ASSERT_TRUE(runtime.redeliver("ask"));
  ASSERT_EQ(emitted.size(), 1U);
  EXPECT_EQ(emitted.front().requestId, "ask");
  EXPECT_EQ(runtime.redeliver("nope").error(), ActorError::InvalidMessage);
}

TEST_F(MeshActorVagent, SpawnAckCompletesAndReapsItsSpawnDelivery) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  ASSERT_TRUE(runtime.send("peer", spawnRequest("remote-spawn")));
  ASSERT_EQ(runtime.deliveryCount(), 1U);
  ActorMessage ack;
  ack.type      = MessageType::ActorSpawnAck;
  ack.requestId = "remote-spawn";
  ack.target    = peerPid;
  ASSERT_TRUE(runtime.receive(peerIdentity, ack, ActorTime{}, wall));
  EXPECT_EQ(runtime.deliveryCount(), 0U);
}

ActorOptions floorOptions() {
  ActorOptions options;
  options.maxRememberedMessages = 8;
  options.floorOf =
      [](const ActorMessage &message) -> std::optional<FloorMark> {
    if (!message.value.starts_with("floor:")) return std::nullopt;
    return FloorMark{"s:", message.requestId};
  };
  return options;
}

TEST_F(MeshActorVagent, MoreMessagesThanTheRememberedCeilingSucceedWithFloors) {
  MeshActorRuntime runtime("worker", 1, floorOptions());
  configure(runtime);
  const auto pid = spawnOne(runtime);
  for (int i = 1; i <= 40; ++i) {
    const auto id    = std::format("s:{:04}", i);
    const auto value = i % 4 == 0 ? "floor:" + id : std::string("input");
    ASSERT_TRUE(
        runtime.receive(identity, sendTo(pid, id, value), ActorTime{}, wall))
        << i;
    ASSERT_EQ(runtime.pumpOne(wall), true);
  }
  EXPECT_EQ(executions.size(), 40U);
}

TEST_F(MeshActorVagent, WithoutFloorsTheRememberedCeilingIsLifetime) {
  ActorOptions options;
  options.maxRememberedMessages = 8;
  MeshActorRuntime runtime("worker", 1, options);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  for (int i = 1; i <= 8; ++i) {
    ASSERT_TRUE(runtime.receive(
        identity, sendTo(pid, std::format("s:{:04}", i)), ActorTime{}, wall));
    ASSERT_EQ(runtime.pumpOne(wall), true);
  }
  EXPECT_EQ(runtime.receive(identity, sendTo(pid, "s:0009"), ActorTime{}, wall)
                .error(),
            ActorError::MailboxFull);
}

TEST_F(MeshActorVagent, AnIdBelowTheFloorIsRefusedAsStaleNotRerun) {
  MeshActorRuntime runtime("worker", 1, floorOptions());
  configure(runtime);
  const auto pid = spawnOne(runtime);
  for (const auto *id : {"s:0001", "s:0002"}) {
    ASSERT_TRUE(runtime.receive(identity, sendTo(pid, id), ActorTime{}, wall));
    ASSERT_EQ(runtime.pumpOne(wall), true);
  }
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "s:0003", "floor:"),
                              ActorTime{}, wall));
  ASSERT_EQ(runtime.pumpOne(wall), true);
  const auto before = executions.size();
  EXPECT_EQ(runtime.receive(identity, sendTo(pid, "s:0001"), ActorTime{}, wall)
                .error(),
            ActorError::StaleRequest);
  EXPECT_EQ(runtime.receive(identity, sendTo(pid, "s:0002"), ActorTime{}, wall)
                .error(),
            ActorError::StaleRequest);
  EXPECT_EQ(runtime.pumpOne(wall), false);
  EXPECT_EQ(executions.size(), before);
  // The floor id itself is at the floor, not below it.
  EXPECT_TRUE(runtime.receive(identity, sendTo(pid, "s:0003", "floor:"),
                              ActorTime{}, wall));
}

TEST_F(MeshActorVagent, ForgetSenderErasesOnlyIdsWithThePrefix) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  for (const auto *id : {"a:1", "a:2", "b:1"}) {
    ASSERT_TRUE(runtime.receive(identity, sendTo(pid, id), ActorTime{}, wall));
    ASSERT_EQ(runtime.pumpOne(wall), true);
  }
  ASSERT_EQ(runtime.forgetSender(pid, "a:"), 2U);
  EXPECT_EQ(runtime.forgetSender(ActorPid{"worker", "x", 1, 99}, "a:").error(),
            ActorError::UnknownProcess);
  // Forgotten ids run again; remembered ones are deduplicated.
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "a:1"), ActorTime{}, wall));
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "b:1"), ActorTime{}, wall));
  EXPECT_EQ(runtime.pending(pid), 1U);
}

TEST_F(MeshActorVagent, WakeRunsTheExecutorWithoutAMessageOrAReply) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  emitted.clear();
  ASSERT_TRUE(runtime.wake(pid, wall));
  ASSERT_EQ(executions.size(), 1U);
  EXPECT_FALSE(executions.front().has_value());
  EXPECT_EQ(executedBy.front(), pid);
  EXPECT_TRUE(emitted.empty());
  // No dedup entry: a later message with any id is simply fresh.
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "m"), ActorTime{}, wall));
  EXPECT_EQ(runtime.pending(pid), 1U);
}

TEST_F(MeshActorVagent, WakeAppliesTheLeaseAndStopsTheActorOnFailure) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  EXPECT_EQ(runtime.wake(pid, wall + 100s).error(), ActorError::NotAuthorized);
  EXPECT_EQ(runtime.wake(pid, wall).error(), ActorError::UnknownProcess);
  EXPECT_TRUE(executions.empty());

  MeshActorRuntime failing("worker", 2);
  configure(failing);
  failing.setExecutor(
      [](const ActorExecution &,
         WorkerContext &) -> std::expected<std::string, ActorError> {
        return std::unexpected(ActorError::ExecutionFailed);
      });
  const auto second = spawnOne(failing);
  EXPECT_EQ(failing.wake(second, wall).error(), ActorError::ExecutionFailed);
  EXPECT_EQ(failing.wake(second, wall).error(), ActorError::UnknownProcess);
}

TEST_F(MeshActorVagent, RenewReplacesTheGrantAndKeepsPidAndMailbox) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  runtime.setAuthorizer(
      [](const Fingerprint &,
         const ActorMessage &request) -> std::expected<ActorGrant, ActorError> {
        auto grant = grantFor(request);
        if (request.capability == "other-program")
          grant.programRef = "sha256:x";
        if (request.capability == "renewed") grant.expiresAt = wall + 500s;
        return grant;
      });
  std::vector<std::chrono::system_clock::time_point> leases;
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        leases.push_back(execution.grant.expiresAt);
        return "ok";
      });
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "m"), ActorTime{}, wall));
  auto renewal       = spawnRequest("renew");
  renewal.capability = "other-program";
  EXPECT_EQ(runtime.renew(pid, identity, renewal, wall).error(),
            ActorError::NotAuthorized);
  renewal.capability = "renewed";
  EXPECT_EQ(runtime.renew(pid, peerIdentity, renewal, wall).error(),
            ActorError::NotAuthorized);
  ASSERT_TRUE(runtime.renew(pid, identity, renewal, wall));
  EXPECT_EQ(runtime.actorCount(), 1U);
  EXPECT_EQ(runtime.pending(pid), 1U);
  // Past the old lease, inside the new one.
  ASSERT_EQ(runtime.pumpOne(wall + 200s), true);
  ASSERT_EQ(leases.size(), 1U);
  EXPECT_EQ(leases.front(), wall + 500s);
}

TEST_F(MeshActorVagent, RenewRevivesAnActorWhoseLeaseRanOut) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "m"), ActorTime{}, wall));
  const auto later = wall + 200s;
  EXPECT_EQ(runtime.pumpOne(later).error(), ActorError::NotAuthorized);
  auto renewal = spawnRequest("renew");
  runtime.setAuthorizer([&](const Fingerprint &, const ActorMessage &request)
                            -> std::expected<ActorGrant, ActorError> {
    auto grant      = grantFor(request);
    grant.expiresAt = later + 100s;
    return grant;
  });
  ASSERT_TRUE(runtime.renew(pid, identity, renewal, later));
  EXPECT_EQ(runtime.pumpOne(later), true);
}

TEST_F(MeshActorVagent, StopFreesTheSlotSoMoreThanMaxActorsSpawnOverTime) {
  ActorOptions options;
  options.maxActors = 2;
  MeshActorRuntime runtime("worker", 1, options);
  configure(runtime);
  for (int i = 0; i < 10; ++i) {
    const auto pid = spawnOne(runtime, std::format("spawn-{}", i));
    ASSERT_TRUE(runtime.stop(pid)) << i;
    EXPECT_EQ(runtime.actorCount(), 0U);
  }
  EXPECT_EQ(runtime.stop(ActorPid{"worker", "x", 1, 99}).error(),
            ActorError::UnknownProcess);
  // The spawn entry went too, so the same request id is a fresh spawn.
  const auto again = spawnOne(runtime, "spawn-0");
  EXPECT_EQ(again.spawnSequence, 11U);
}

TEST_F(MeshActorVagent, StopTellsMonitorsAndErasesTheMonitorEntries) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto watched = spawnOne(runtime, "spawn-1");
  const auto watcher = spawnOne(runtime, "spawn-2");
  ASSERT_TRUE(runtime.monitor(watcher, watched, ActorTime{}));
  ASSERT_TRUE(runtime.monitor(watched, watcher, ActorTime{}));
  emitted.clear();
  ASSERT_TRUE(runtime.stop(watched));
  ASSERT_EQ(emitted.size(), 1U);
  EXPECT_EQ(emitted.front().type, MessageType::ActorDown);
  EXPECT_EQ(emitted.front().source, watched);
  EXPECT_EQ(emitted.front().target, watcher);
  EXPECT_FALSE(runtime.liveness(watched));
  // watched was a watcher of watcher; that edge went with it.
  EXPECT_FALSE(runtime.liveness(watcher));
  EXPECT_EQ(runtime.actorCount(), 1U);
}

TEST_F(MeshActorVagent, StopWaitsForOutstandingDeliveriesToDrain) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.send("peer", outbound(pid, "ask")));
  ASSERT_TRUE(runtime.stop(pid));
  EXPECT_EQ(runtime.actorCount(), 1U);
  EXPECT_EQ(
      runtime.receive(identity, sendTo(pid, "late"), ActorTime{}, wall).error(),
      ActorError::UnknownProcess);
  ASSERT_TRUE(
      runtime.receive(peerIdentity, reply(pid, "ask"), ActorTime{}, wall));
  EXPECT_EQ(runtime.actorCount(), 0U);
}

TEST_F(MeshActorVagent, AbandoningTheLastDeliveryReapsAStoppedActor) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  ASSERT_TRUE(runtime.send("peer", outbound(pid, "ask")));
  ASSERT_TRUE(runtime.stop(pid));
  ASSERT_EQ(runtime.actorCount(), 1U);
  ASSERT_TRUE(runtime.abandon("ask"));
  EXPECT_EQ(runtime.actorCount(), 0U);
}

TEST_F(MeshActorVagent, PumpOneServesBusyActorsRoundRobin) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto first  = spawnOne(runtime, "spawn-1");
  const auto second = spawnOne(runtime, "spawn-2");
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(runtime.receive(identity, sendTo(first, std::format("f{}", i)),
                                ActorTime{}, wall));
    ASSERT_TRUE(runtime.receive(identity, sendTo(second, std::format("g{}", i)),
                                ActorTime{}, wall));
  }
  for (int i = 0; i < 6; ++i) ASSERT_EQ(runtime.pumpOne(wall), true);
  ASSERT_EQ(executedBy.size(), 6U);
  for (std::size_t i = 2; i < executedBy.size(); ++i) {
    EXPECT_NE(executedBy[i], executedBy[i - 1]) << i;
  }
  EXPECT_EQ(runtime.pumpOne(wall), false);
}

TEST_F(MeshActorVagent, AnExecutorMayStopItsOwnActorReentrantly) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto other = spawnOne(runtime, "spawn-2");
  const auto pid   = spawnOne(runtime, "spawn-1");
  bool spawned     = false;
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        EXPECT_TRUE(
            runtime.forgetSender(execution.pid, execution.message->requestId));
        EXPECT_TRUE(runtime.send("peer", outbound(execution.pid, "child")));
        EXPECT_TRUE(runtime.stop(other));
        spawned =
            runtime.spawn(identity, spawnRequest("spawn-3"), wall).has_value();
        EXPECT_TRUE(runtime.stop(execution.pid));
        return "bye";
      });
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "m"), ActorTime{}, wall));
  emitted.clear();
  ASSERT_EQ(runtime.pumpOne(wall), true);
  EXPECT_TRUE(spawned);
  // The reply is still sent for the message that was running, and the actor
  // stays until the delivery it opened has drained.
  EXPECT_FALSE(emitted.empty());
  EXPECT_EQ(runtime.actorCount(), 2U);
  ASSERT_TRUE(runtime.abandon("child"));
  EXPECT_EQ(runtime.actorCount(), 1U);
}

TEST_F(MeshActorVagent, AnExecutorStoppingItselfWithNoDeliveriesIsReapedAfter) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        EXPECT_TRUE(runtime.stop(execution.pid));
        EXPECT_EQ(runtime.actorCount(), 1U);
        return "bye";
      });
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "m"), ActorTime{}, wall));
  ASSERT_EQ(runtime.pumpOne(wall), true);
  EXPECT_EQ(runtime.actorCount(), 0U);
  EXPECT_EQ(runtime.pumpOne(wall), false);
}

TEST_F(MeshActorVagent, AFloorArrivingDuringExecutionSparesTheRunningEntry) {
  auto options = ActorOptions{};
  options.floorOf =
      [](const ActorMessage &message) -> std::optional<FloorMark> {
    if (!message.value.starts_with("floor:")) return std::nullopt;
    return FloorMark{"s:", message.requestId};
  };
  MeshActorRuntime runtime("worker", 1, options);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  runtime.setExecutor([&](const ActorExecution &execution, WorkerContext &)
                          -> std::expected<std::string, ActorError> {
    // Owner-thread re-entry: a floor covering the very id being executed.
    EXPECT_TRUE(runtime.receive(
        identity, sendTo(execution.pid, "s:2", "floor:"), ActorTime{}, wall));
    return "ok";
  });
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "s:1"), ActorTime{}, wall));
  ASSERT_EQ(runtime.pumpOne(wall), true);
  ASSERT_EQ(runtime.pumpOne(wall), true);
}

TEST_F(MeshActorVagent, TheExecutorSeesTheSpawnRequestOnMessageAndWakeTurns) {
  MeshActorRuntime runtime("worker", 1);
  configure(runtime);
  const auto pid = spawnOne(runtime);
  std::vector<ActorMessage> seen;
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        seen.push_back(execution.spawnRequest);
        return "ok";
      });
  ASSERT_TRUE(runtime.receive(identity, sendTo(pid, "m"), ActorTime{}, wall));
  ASSERT_EQ(runtime.pumpOne(wall), true);
  ASSERT_TRUE(runtime.wake(pid, wall));
  ASSERT_EQ(seen.size(), 2U);
  for (const auto &request : seen) {
    EXPECT_EQ(request.type, MessageType::ActorSpawn);
    EXPECT_EQ(request.requestId, "spawn-1");
    EXPECT_EQ(request.capability, "signed-grant");
    EXPECT_EQ(request.programRef, "sha256:immutable-program");
  }
}

} // namespace
} // namespace zigzag::vortex::mesh
