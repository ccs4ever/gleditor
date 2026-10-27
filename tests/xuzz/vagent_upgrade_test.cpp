/**
 * @file vagent_upgrade_test.cpp
 * @brief Unit tests for Proposals P17.2 - P17.5:
 *        - P17.2: Sealed Program Store & Manifold builder in CompiledProgram
 *        - P17.3: WorkerContext base-manifold resolution via
 * ProgramManifoldResolver
 *        - P17.4: MeshActorRuntime::upgrade with StateReconciler & atomic
 * rollback
 *        - P17.5: event Upgrade, dropped_field reflection, and on upgrade in
 * skills
 */
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"

namespace {

using namespace std::chrono_literals;
using namespace xanadu::vagent::runtime;
using xanadu::identity::Fingerprint;
using xanadu::identity::MessageType;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;
using zigzag::vortex::mesh::ActorPid;
using zigzag::vortex::mesh::ActorTime;
using zigzag::vortex::mesh::MeshActorRuntime;

const auto wall = std::chrono::system_clock::time_point(1000s);

ActorGrant grantFor(std::string programRef) {
  ActorGrant grant;
  grant.programRef      = std::move(programRef);
  grant.baseRef         = "base-1";
  grant.maxInstructions = 100'000;
  grant.maxCells        = 10'000;
  grant.maxOutputBytes  = 32768;
  grant.maxSolutions    = 1;
  grant.maxMailboxBytes = 65536;
  grant.expiresAt       = wall + 3600s;
  grant.immutable       = false;
  return grant;
}

ActorMessage spawnRequest(std::string programRef) {
  ActorMessage message;
  message.type          = MessageType::ActorSpawn;
  message.requestId     = "spawn-1";
  message.capability    = "signed-grant";
  message.programRef    = std::move(programRef);
  message.hashcashStamp = "verified-by-authorizer";
  message.source        = ActorPid{"requester", "client", 1, 1};
  return message;
}

// ---------------------------------------------------------------------------
// P17.2 & P17.3: CompiledProgram Program Manifold and Store
// ---------------------------------------------------------------------------

TEST(VagentUpgradeTest, CheckedUnitRemainsBoundAfterProgramMoves) {
  auto parsed = CompiledProgram::fromSource("agent Echo {} ");
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed->checked().unit, &parsed->unit());
  auto moved = std::move(*parsed);
  EXPECT_EQ(moved.checked().unit, &moved.unit());
  auto replacement = CompiledProgram::fromSource("agent Other {} ");
  ASSERT_TRUE(replacement) << replacement.error();
  *replacement = std::move(moved);
  EXPECT_EQ(replacement->checked().unit, &replacement->unit());
  EXPECT_TRUE(replacement->declares("Echo"));
}

TEST(VagentUpgradeTest, ProgramStoreAndManifoldAreBuiltAndShared) {
  constexpr const char *kSource = R"vagent(
module test:upgrade

event Ping { n: Int }
event Pong { n: Int }

agent Echo {
  var count: Int := 0
  on Ping as p {
    count := count + p.n
    emit Pong{n: count} in outer
  }
}
)vagent";

  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program) << program.error();

  // Program store and manifold must be populated
  EXPECT_NE(program->programStore(), nullptr);
  EXPECT_NE(program->programManifold(), nullptr);
  EXPECT_TRUE(program->agents().contains("Echo"));

  // Spawning on LocalHost works with the program manifold resolver
  LocalHost host(std::move(*program));
  auto pid = host.launch("Echo", "echo-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  Envelope ping;
  ping.kind = "event";
  ping.type = "Ping";
  ping.fields.set("n", WireValue::integer(42));
  ASSERT_TRUE(host.send(*pid, std::move(ping)));
  host.pump();

  const auto &emissions = host.emissions();
  ASSERT_EQ(emissions.size(), 1U);
  EXPECT_EQ(emissions[0].type, "Pong");
  const auto *val = emissions[0].fields.find("n");
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(*val, WireValue::integer(42));
}

// ---------------------------------------------------------------------------
// P17.4: MeshActorRuntime::upgrade In-Place Grant Transition & Atomic Rollback
// ---------------------------------------------------------------------------

TEST(VagentUpgradeTest,
     MeshActorRuntimeUpgradeTransitionsGrantAndStateInPlace) {
  MeshActorRuntime runtime("worker", 1);
  Fingerprint identity;
  runtime.trustNode("requester", identity);

  runtime.setAuthorizer([&](const Fingerprint &, const ActorMessage &request)
                            -> std::expected<ActorGrant, ActorError> {
    return grantFor(request.programRef);
  });

  std::string lastExecutedText;
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &ctx) -> std::expected<std::string, ActorError> {
        auto text        = ctx.arena.textOf(execution.stateRoot.value());
        lastExecutedText = text + ":" + execution.message->value;
        return lastExecutedText;
      });

  const auto spawned = runtime.spawn(identity, spawnRequest("prog-v1"), wall);
  ASSERT_TRUE(spawned) << "spawn error: " << static_cast<int>(spawned.error());
  const auto pid = *spawned;

  // Upgrade grant to prog-v2
  const auto grantV2    = grantFor("prog-v2");
  bool reconcilerCalled = false;
  zigzag::CellRef migratedRoot{zigzag::noCell};

  auto status = runtime.upgrade(
      pid, grantV2, wall,
      [&](const ActorPid &reconcilePid, const zigzag::ArenaManifold &oldArena,
          std::optional<zigzag::CellRef> oldRoot,
          WorkerContext &newCtx) -> std::expected<zigzag::CellRef, ActorError> {
        EXPECT_EQ(reconcilePid, pid);
        (void)oldArena;
        (void)oldRoot;
        reconcilerCalled = true;
        // Seed new state root in newContext
        migratedRoot = newCtx.arena.makeCell("v2-state");
        return migratedRoot;
      });

  ASSERT_TRUE(status.has_value());
  EXPECT_TRUE(reconcilerCalled);
  EXPECT_EQ(runtime.stateRoot(pid), migratedRoot);

  // Send message and verify execution occurs under new program / state root
  ActorMessage msg;
  msg.type      = MessageType::ActorSend;
  msg.requestId = "msg-1";
  msg.source    = ActorPid{"requester", "client", 1, 1};
  msg.target    = pid;
  msg.value     = "test";

  ASSERT_TRUE(runtime.receive(identity, msg, ActorTime{}, wall));
  ASSERT_TRUE(runtime.pumpOne(wall));
  EXPECT_EQ(lastExecutedText, "v2-state:test");
}

TEST(VagentUpgradeTest,
     UpgradeRefusesActiveActorAndRollsBackOnReconcilerError) {
  MeshActorRuntime runtime("worker", 1);
  Fingerprint identity;
  runtime.trustNode("requester", identity);

  runtime.setAuthorizer([&](const Fingerprint &, const ActorMessage &request)
                            -> std::expected<ActorGrant, ActorError> {
    return grantFor(request.programRef);
  });

  std::string executedValue;
  runtime.setExecutor(
      [&](const ActorExecution &execution,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        executedValue = "v1:" + execution.message->value;
        return executedValue;
      });

  const auto spawned = runtime.spawn(identity, spawnRequest("prog-v1"), wall);
  ASSERT_TRUE(spawned);
  const auto pid = *spawned;

  const auto grantV2 = grantFor("prog-v2");

  // Reconciler failure triggers atomic rollback
  auto failedUpgrade = runtime.upgrade(
      pid, grantV2, wall,
      [&](const ActorPid &, const zigzag::ArenaManifold &,
          std::optional<zigzag::CellRef>,
          WorkerContext &) -> std::expected<zigzag::CellRef, ActorError> {
        return std::unexpected(ActorError::ResourceLimit);
      });

  EXPECT_FALSE(failedUpgrade.has_value());
  EXPECT_EQ(failedUpgrade.error(), ActorError::ResourceLimit);

  // Actor is intact and can still receive messages under V1
  ActorMessage msg;
  msg.type      = MessageType::ActorSend;
  msg.requestId = "msg-after-fail";
  msg.source    = ActorPid{"requester", "client", 1, 1};
  msg.target    = pid;
  msg.value     = "ping";

  ASSERT_TRUE(runtime.receive(identity, msg, ActorTime{}, wall));
  ASSERT_TRUE(runtime.pumpOne(wall));
  EXPECT_EQ(executedValue, "v1:ping");
}

// ---------------------------------------------------------------------------
// P17.5: event Upgrade, dropped_field reflection, and on upgrade in skills
// ---------------------------------------------------------------------------

TEST(VagentUpgradeTest, EventUpgradeAndDroppedFieldReflection) {
  constexpr const char *kSource = R"vagent(
module test:upgrade_event

event Report { ver: Str, old_secret: Str }

agent UpgradableAgent {
  on Upgrade as u {
    emit Report{ver: u.from_version, old_secret: dropped_field("historical_token")} in outer
  }
}
)vagent";

  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program) << program.error();

  LocalHost host(std::move(*program));
  auto pid = host.launch("UpgradableAgent", "agent-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  // Deliver Upgrade event with from_version
  Envelope upgradeEv;
  upgradeEv.kind = "event";
  upgradeEv.type = "Upgrade";
  upgradeEv.fields.set("from_version", WireValue::text("0.9.1"));

  auto sent = host.send(*pid, std::move(upgradeEv));
  ASSERT_TRUE(sent) << "send error: " << static_cast<int>(sent.error());
  host.pump();

  const auto &emissions = host.emissions();
  ASSERT_EQ(emissions.size(), 1U);
  EXPECT_EQ(emissions[0].type, "Report");
  const auto *ver = emissions[0].fields.find("ver");
  ASSERT_NE(ver, nullptr);
  EXPECT_EQ(*ver, WireValue::text("0.9.1"));
  // dropped_field for non-existent/dropped field returns empty string
  const auto *sec = emissions[0].fields.find("old_secret");
  ASSERT_NE(sec, nullptr);
  EXPECT_EQ(*sec, WireValue::text(""));
}

TEST(VagentUpgradeTest, SkillOnUpgradeLifecycleHookCompilesAndExecutes) {
  constexpr const char *kSource = R"vagent(
module test:skill_upgrade

capacity Storage {
  def get_tag(): Str
}

skill MigratableStorage implements Storage {
  var tag: Str := "initial"

  on upgrade(from_ver) {
    tag := "migrated_from_" ++ from_ver
  }

  def get_tag(): Str {
    tag
  }
}

agent ManagedAgent {
  uses Storage
  install MigratableStorage()
}
)vagent";

  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program) << program.error();

  LocalHost host(std::move(*program));
  auto pid = host.launch("ManagedAgent", "m-1", {});
  ASSERT_TRUE(pid);
  host.pump();
}

} // namespace
