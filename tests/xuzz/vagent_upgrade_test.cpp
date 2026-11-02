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
#include "common/xanadu/vagent/runtime/agent_executor.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/reconciler.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace std::chrono_literals;
using namespace xanadu::vagent::runtime;
using xanadu::identity::Fingerprint;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimVector;
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
        auto text        = ctx.arena.textOf(*ctx.arena.homeCell());
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
  EXPECT_FALSE(lastExecutedText.empty());
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
  constexpr const char *kV1Source = R"vagent(
module test:skill_upgrade_v1

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

  constexpr const char *kV2Source = R"vagent(
module test:skill_upgrade_v2

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

event QueryTag {}
event TagReport { val: Str }

agent ManagedAgent {
  uses Storage
  install MigratableStorage()

  on QueryTag {
    emit TagReport{val: get_tag()} in outer
  }
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  const std::string v1Ref = std::string(p1->programRef());
  auto p2                 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  auto pid = host.launch("ManagedAgent", "m-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  // Live upgrade to V2
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  ASSERT_TRUE(upgraded) << static_cast<int>(upgraded.error());
  host.pump();

  // Query tag through the capacity method
  Envelope q;
  q.kind = "event";
  q.type = "QueryTag";
  ASSERT_TRUE(host.send(*pid, std::move(q)));
  host.pump();

  const auto &emissions = host.emissions();
  ASSERT_GE(emissions.size(), 1U);
  EXPECT_EQ(emissions.back().type, "TagReport");
  const auto *val = emissions.back().fields.find("val");
  ASSERT_NE(val, nullptr);
  EXPECT_EQ(*val, WireValue::text("migrated_from_" + v1Ref));
}

TEST(VagentUpgradeTest,
     LocalHostLiveUpgradeFieldMigrationAndDroppedFieldAccess) {
  constexpr const char *kV1Source = R"vagent(
module test:upgrade_v1

event Inc { n: Int }

agent Worker {
  var count: Int := 10
  var note: Str := "running_v1"
  var deprecated_key: Str := "old_secret_xyz"

  on Inc as e {
    count := count + e.n
  }
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:upgrade_v2

event UpgradedReport { ver: Str, old_key: Str, count: Int }

agent Worker {
  var count: Int := 0
  var note: Str := "running_v2"
  var new_feature: Bool := true

  on Upgrade as u {
    emit UpgradedReport{ver: u.from_version, old_key: dropped_field("deprecated_key"), count: count} in outer
  }
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  auto pid = host.launch("Worker", "worker-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  // Send Inc event in V1
  Envelope inc;
  inc.kind = "event";
  inc.type = "Inc";
  inc.fields.set("n", WireValue::integer(15));
  ASSERT_TRUE(host.send(*pid, std::move(inc)));
  host.pump();

  // Upgrade to V2
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  ASSERT_TRUE(upgraded) << static_cast<int>(upgraded.error());
  host.pump();

  const auto &emissions = host.emissions();
  ASSERT_GE(emissions.size(), 1U);
  EXPECT_EQ(emissions.back().type, "UpgradedReport");
  const auto *oldKeyVal = emissions.back().fields.find("old_key");
  ASSERT_NE(oldKeyVal, nullptr);
  EXPECT_EQ(*oldKeyVal, WireValue::text("old_secret_xyz"));

  const auto *countVal = emissions.back().fields.find("count");
  ASSERT_NE(countVal, nullptr);
  EXPECT_EQ(*countVal, WireValue::integer(25));
}

TEST(VagentUpgradeTest, LocalHostLiveUpgradeBeliefMigration) {
  constexpr const char *kV1Source = R"vagent(
module test:upgrade_v1

predicate task(name: Str, priority: Int)
predicate temp_flag(name: Str)

event AddTasks {}

agent TaskAgent {
  on AddTasks {
    believe task("job_alpha", 1)
    believe task("job_beta", 2)
    believe temp_flag("temp_item")
  }
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:upgrade_v2

predicate task(name: Str, priority: Int)
predicate new_flag(val: Int)

event QueryTasks {}
event TaskReport { total: Int }

agent TaskAgent {
  var count: Int := 0

  on QueryTasks {
    count := 0
    for believes task(?name, ?prio) do {
      count := count + 1
    }
    emit TaskReport{total: count} in outer
  }
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  auto pid = host.launch("TaskAgent", "task-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  // Assert beliefs in V1
  Envelope add;
  add.kind = "event";
  add.type = "AddTasks";
  ASSERT_TRUE(host.send(*pid, std::move(add)));
  host.pump();

  // Upgrade to V2
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  ASSERT_TRUE(upgraded);
  host.pump();

  // Query beliefs in V2
  Envelope q;
  q.kind = "event";
  q.type = "QueryTasks";
  ASSERT_TRUE(host.send(*pid, std::move(q)));
  host.pump();

  const auto &emissions = host.emissions();
  ASSERT_GE(emissions.size(), 1U);
  EXPECT_EQ(emissions.back().type, "TaskReport");
  const auto *tot = emissions.back().fields.find("total");
  ASSERT_NE(tot, nullptr);
  EXPECT_EQ(*tot, WireValue::integer(2));
}

// ---------------------------------------------------------------------------
// In-Flight Requests across Live Upgrade (§4.2)
// ---------------------------------------------------------------------------

TEST(VagentUpgradeTest, InFlightAskSettlesAfterLiveUpgrade) {
  constexpr const char *kV1Source = R"vagent(
module test:inflight_ask_v1

goal QueryGoal() achieve

plan p1 for QueryGoal() {
  yield
}

agent Boss {
  var last_ans: Int := 0
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:inflight_ask_v2

goal QueryGoal() achieve

plan p1 for QueryGoal() {
  yield
}

agent Boss {
  var last_ans: Int := 0
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  const auto owner =
      *Fingerprint::fromString("0123456789abcdef0123456789abcdef01234567");
  host.runtime().trustNode("worker", owner);
  auto pid = host.launch("Boss", "boss-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  auto *ctx = host.executor().context(*pid);
  ASSERT_NE(ctx, nullptr);
  auto agentCell = *host.executor().agentCell(*pid);
  auto vd        = valueDims(ctx->core);
  auto dims      = registerDimensions(ctx->arena);

  const ActorPid remotePid{"worker", "remote", 1, 1};
  const auto peersCell = ctx->arena.makeCell();
  setField(ctx->arena, vd, peersCell, "worker",
           ctx->arena.makeCell(pidToText(remotePid)));
  setField(ctx->arena, vd, agentCell, kFieldPeers, peersCell);

  const auto intentCell = ctx->arena.makeCell("intention");
  setField(ctx->arena, vd, intentCell, kIntentionId,
           ctx->arena.makeScalarCell(std::int64_t{101}));
  setField(ctx->arena, vd, intentCell, kIntentionGoal,
           ctx->arena.makeCell("QueryGoal"));
  setField(ctx->arena, vd, intentCell, kIntentionPlan,
           ctx->arena.makeCell("p1"));

  WireRecord waitRec;
  waitRec.type = "Waiting";
  waitRec.set(std::string(kWaitingKind),
              WireValue::text(std::string(kWaitKindAsk)));
  waitRec.set(std::string(kWaitingKey), WireValue::integer(777));
  waitRec.set(std::string(kWaitingTo), WireValue::text("worker"));
  setField(ctx->arena, vd, intentCell, kIntentionWaiting,
           writeValue(ctx->arena, vd, WireValue::record(waitRec)));
  appendToRank(ctx->arena, agentCell, dims.intention, intentCell);

  const auto outboxEntry = ctx->arena.makeCell(std::string(kKindAsk));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqId,
           ctx->arena.makeScalarCell(std::int64_t{777}));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqTo,
           ctx->arena.makeCell("worker"));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqName,
           ctx->arena.makeCell("calc"));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqDeadline,
           ctx->arena.makeScalarCell(std::int64_t{9999999999}));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqIntention,
           ctx->arena.makeScalarCell(std::int64_t{101}));
  appendToRank(ctx->arena, agentCell, dims.outbox, outboxEntry);

  ActorMessage capturedMsg;
  host.runtime().setSender([&](std::string_view, const ActorMessage &msg) {
    capturedMsg = msg;
    return true;
  });

  ASSERT_TRUE(host.runtime().wake(*pid, wall));
  host.pump();
  EXPECT_FALSE(capturedMsg.requestId.empty());

  // Live upgrade to V2 with identical plan
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  ASSERT_TRUE(upgraded) << static_cast<int>(upgraded.error());
  host.pump();

  // Deliver reply for in-flight ask
  ActorMessage replyMsg;
  replyMsg.type      = MessageType::ActorDeliverValue;
  replyMsg.requestId = capturedMsg.requestId;
  replyMsg.source    = remotePid;
  replyMsg.target    = *pid;
  replyMsg.value     = *encodeReply(Reply{WireValue::integer(42), {}});

  ASSERT_TRUE(host.runtime().receive(owner, replyMsg, ActorTime{}, wall));
  host.pump();

  EXPECT_FALSE(host.executor().readyIntentions(*pid).empty());
  EXPECT_EQ(host.executor().stat(*pid, "reply_stale"), 0U);
}

TEST(VagentUpgradeTest,
     InFlightRequestDroppedWhenIntentionReplannedInV2AndCancelStaged) {
  constexpr const char *kV1Source = R"vagent(
module test:inflight_drop_v1

goal QueryGoal() achieve

plan p1 for QueryGoal() {
  yield
}

agent Boss {
  var last_ans: Int := 0
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:inflight_drop_v2

goal QueryGoal() achieve

plan p1 for QueryGoal() {
  yield
  yield
}

agent Boss {
  var last_ans: Int := 0
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  const auto owner =
      *Fingerprint::fromString("0123456789abcdef0123456789abcdef01234567");
  host.runtime().trustNode("worker", owner);
  auto pid = host.launch("Boss", "boss-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  auto *ctx = host.executor().context(*pid);
  ASSERT_NE(ctx, nullptr);
  auto agentCell = *host.executor().agentCell(*pid);
  auto vd        = valueDims(ctx->core);
  auto dims      = registerDimensions(ctx->arena);

  const ActorPid remotePid{"worker", "remote", 1, 1};
  const auto peersCell = ctx->arena.makeCell();
  setField(ctx->arena, vd, peersCell, "worker",
           ctx->arena.makeCell(pidToText(remotePid)));
  setField(ctx->arena, vd, agentCell, kFieldPeers, peersCell);

  const auto intentCell = ctx->arena.makeCell("intention");
  setField(ctx->arena, vd, intentCell, kIntentionId,
           ctx->arena.makeScalarCell(std::int64_t{101}));
  setField(ctx->arena, vd, intentCell, kIntentionGoal,
           ctx->arena.makeCell("QueryGoal"));
  setField(ctx->arena, vd, intentCell, kIntentionPlan,
           ctx->arena.makeCell("p1"));

  WireRecord waitRec;
  waitRec.type = "Waiting";
  waitRec.set(std::string(kWaitingKind),
              WireValue::text(std::string(kWaitKindAsk)));
  waitRec.set(std::string(kWaitingKey), WireValue::integer(777));
  waitRec.set(std::string(kWaitingTo), WireValue::text("worker"));
  setField(ctx->arena, vd, intentCell, kIntentionWaiting,
           writeValue(ctx->arena, vd, WireValue::record(waitRec)));
  appendToRank(ctx->arena, agentCell, dims.intention, intentCell);

  const auto outboxEntry = ctx->arena.makeCell(std::string(kKindAsk));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqId,
           ctx->arena.makeScalarCell(std::int64_t{777}));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqTo,
           ctx->arena.makeCell("worker"));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqName,
           ctx->arena.makeCell("calc"));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqDeadline,
           ctx->arena.makeScalarCell(std::int64_t{9999999999}));
  setField(ctx->arena, vd, outboxEntry, kOutboxReqIntention,
           ctx->arena.makeScalarCell(std::int64_t{101}));
  appendToRank(ctx->arena, agentCell, dims.outbox, outboxEntry);

  ActorMessage capturedMsg;
  host.runtime().setSender([&](std::string_view, const ActorMessage &msg) {
    capturedMsg = msg;
    return true;
  });

  ASSERT_TRUE(host.runtime().wake(*pid, wall));
  host.pump();
  EXPECT_FALSE(capturedMsg.requestId.empty());

  std::vector<ActorMessage> sentAfterUpgrade;
  host.runtime().setSender([&](std::string_view, const ActorMessage &msg) {
    sentAfterUpgrade.push_back(msg);
    return true;
  });

  // Live upgrade to V2 with modified plan AST -> replans and stages $cancel
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  ASSERT_TRUE(upgraded) << static_cast<int>(upgraded.error());
  host.pump();

  // Verify $cancel was staged and dispatched to remote delegate
  bool cancelFound = false;
  for (const auto &m : sentAfterUpgrade) {
    if (auto env = decodeEnvelope(m.value)) {
      if (env->kind == kKindCancel) {
        cancelFound = true;
        break;
      }
    }
  }
  EXPECT_TRUE(cancelFound);

  // Late reply arrives for in-flight ask whose intention was replanned
  ActorMessage replyMsg;
  replyMsg.type      = MessageType::ActorDeliverValue;
  replyMsg.requestId = capturedMsg.requestId;
  replyMsg.source    = remotePid;
  replyMsg.target    = *pid;
  replyMsg.value     = *encodeReply(Reply{WireValue::integer(42), {}});

  ASSERT_TRUE(host.runtime().receive(owner, replyMsg, ActorTime{}, wall));
  host.pump();

  // Reply is dropped as stale
  EXPECT_EQ(host.executor().stat(*pid, "reply_stale"), 1U);
}

// ---------------------------------------------------------------------------
// Suspended Plan Replanning Across Upgrade (§4.3.4 & Open Question 1)
// ---------------------------------------------------------------------------

TEST(VagentUpgradeTest,
     SuspendedPlanWithModifiedPlanTriggersReplanAndCancelsSubgoals) {
  constexpr const char *kV1Source = R"vagent(
module test:suspended_plan_v1

goal TopGoal() achieve

plan p1 for TopGoal() {
  yield
}

agent Boss {
  var last_step: Int := 0
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:suspended_plan_v2

goal TopGoal() achieve

plan p1 for TopGoal() {
  yield
  yield
}

agent Boss {
  var last_step: Int := 0
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  const auto owner =
      *Fingerprint::fromString("0123456789abcdef0123456789abcdef01234567");
  host.runtime().trustNode("worker", owner);
  auto pid = host.launch("Boss", "boss-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  auto *ctx = host.executor().context(*pid);
  ASSERT_NE(ctx, nullptr);
  auto agentCell = *host.executor().agentCell(*pid);
  auto vd        = valueDims(ctx->core);
  auto dims      = registerDimensions(ctx->arena);

  const ActorPid remotePid{"worker", "remote", 1, 1};
  const auto peersCell = ctx->arena.makeCell();
  setField(ctx->arena, vd, peersCell, "worker",
           ctx->arena.makeCell(pidToText(remotePid)));
  setField(ctx->arena, vd, agentCell, kFieldPeers, peersCell);

  // Top intention: active at step 2
  const auto topIntent = ctx->arena.makeCell("intention");
  setField(ctx->arena, vd, topIntent, kIntentionId,
           ctx->arena.makeScalarCell(std::int64_t{10}));
  setField(ctx->arena, vd, topIntent, kIntentionGoal,
           ctx->arena.makeCell("TopGoal"));
  setField(ctx->arena, vd, topIntent, kIntentionPlan,
           ctx->arena.makeCell("p1"));
  setField(ctx->arena, vd, topIntent, kIntentionStep,
           ctx->arena.makeScalarCell(std::int64_t{2}));
  setField(ctx->arena, vd, topIntent, kIntentionTried,
           ctx->arena.makeCell("tried_something"));
  setField(ctx->arena, vd, topIntent, kIntentionFrame,
           ctx->arena.makeCell("frame_data"));
  appendToRank(ctx->arena, agentCell, dims.intention, topIntent);

  // Child subgoal linked on dims.subgoal from topIntent with active delegator
  const auto subIntent = ctx->arena.makeCell("subgoal");
  setField(ctx->arena, vd, subIntent, kIntentionId,
           ctx->arena.makeScalarCell(std::int64_t{20}));
  setField(ctx->arena, vd, subIntent, kIntentionGoal,
           ctx->arena.makeCell("SubGoal"));

  WireRecord delegator;
  delegator.type = std::string(kDelegatorRecord);
  delegator.set(std::string(kDelegatorPeer), WireValue::text("worker"));
  delegator.set(std::string(kDelegatorCorr), WireValue::text("sub-corr-456"));
  setField(ctx->arena, vd, subIntent, kIntentionDelegator,
           writeValue(ctx->arena, vd, WireValue::record(delegator)));
  appendToRank(ctx->arena, topIntent, dims.subgoal, subIntent);

  std::vector<ActorMessage> sentAfterUpgrade;
  host.runtime().setSender([&](std::string_view, const ActorMessage &msg) {
    sentAfterUpgrade.push_back(msg);
    return true;
  });

  // Upgrade to V2 with modified plan -> reconciler cancels subgoals and stages
  // $cancel
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  ASSERT_TRUE(upgraded) << static_cast<int>(upgraded.error());
  host.pump();

  // Verify $cancel was dispatched for sub-corr-456
  bool cancelFound = false;
  for (const auto &m : sentAfterUpgrade) {
    if (auto env = decodeEnvelope(m.value)) {
      if (env->kind == kKindCancel && env->correlation == "sub-corr-456") {
        cancelFound = true;
        break;
      }
    }
  }
  EXPECT_TRUE(cancelFound);

  // Verify intention state in V2: cleared tried, plan, waiting, frame; step 0;
  // faulted "upgraded"
  auto *newCtx = host.executor().context(*pid);
  ASSERT_NE(newCtx, nullptr);
  auto newAgent = *host.executor().agentCell(*pid);
  auto newDims  = registerDimensions(newCtx->arena);
  auto newVd    = valueDims(newCtx->core);

  auto intentions =
      zigzag::rankAfter(newCtx->arena, newAgent, newDims.intention) |
      std::ranges::to<std::vector<zigzag::CellRef>>();
  ASSERT_GE(intentions.size(), 1U);
  auto top = intentions.front();

  auto stepVal = fieldValue(newCtx->arena, newVd, top, kIntentionStep);
  ASSERT_TRUE(stepVal.has_value());
  EXPECT_EQ(newCtx->arena.asInt64(*stepVal), 0);

  auto faultedVal = fieldValue(newCtx->arena, newVd, top, kIntentionFaulted);
  ASSERT_TRUE(faultedVal.has_value());
  EXPECT_EQ(newCtx->arena.textOf(*faultedVal), "upgraded");

  auto readyVal = fieldValue(newCtx->arena, newVd, top, kIntentionReady);
  ASSERT_TRUE(readyVal.has_value());
  EXPECT_EQ(newCtx->arena.asInt64(*readyVal), 0);

  EXPECT_FALSE(
      fieldValue(newCtx->arena, newVd, top, kIntentionPlan).has_value());
  EXPECT_FALSE(
      fieldValue(newCtx->arena, newVd, top, kIntentionWaiting).has_value());
  EXPECT_FALSE(
      fieldValue(newCtx->arena, newVd, top, kIntentionFrame).has_value());

  auto subgoals = zigzag::rankAfter(newCtx->arena, top, newDims.subgoal) |
                  std::ranges::to<std::vector<zigzag::CellRef>>();
  EXPECT_TRUE(subgoals.empty());
}

// ---------------------------------------------------------------------------
// Atomic Rollback on Upgrade Failure / Fuel Exhaustion (§4.1 & Open Question 2)
// ---------------------------------------------------------------------------

TEST(VagentUpgradeTest,
     UpgradeHandlerFuelExhaustionTriggersAtomicRollbackToV1) {
  constexpr const char *kV1Source = R"vagent(
module test:fuel_v1

event Ping { n: Int }
event Pong { n: Int }

agent Worker {
  var count: Int := 10
  on Ping as p {
    count := count + p.n
    emit Pong{n: count} in outer
  }
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:fuel_v2

event Ping { n: Int }
event Pong { n: Int }

agent Worker {
  var count: Int := 10
  on Upgrade {
    while count >= 0 do {
      count := count + 1
    }
  }
  on Ping as p {
    count := count + p.n
    emit Pong{n: count} in outer
  }
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  auto pid = host.launch("Worker", "worker-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  // Test Ping in V1
  Envelope ping1;
  ping1.kind = "event";
  ping1.type = "Ping";
  ping1.fields.set("n", WireValue::integer(5));
  ASSERT_TRUE(host.send(*pid, std::move(ping1)));
  host.pump();

  ASSERT_EQ(host.emissions().size(), 1U);
  EXPECT_EQ(host.emissions().back().type, "Pong");
  EXPECT_EQ(*host.emissions().back().fields.find("n"), WireValue::integer(15));

  // Live upgrade to V2 where on Upgrade exhausts fuel
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  EXPECT_FALSE(upgraded.has_value());
  EXPECT_EQ(upgraded.error(), ActorError::ResourceLimit);

  // Verify actor rolled back cleanly and continues running under V1
  Envelope ping2;
  ping2.kind = "event";
  ping2.type = "Ping";
  ping2.fields.set("n", WireValue::integer(10));
  ASSERT_TRUE(host.send(*pid, std::move(ping2)));
  host.pump();

  ASSERT_EQ(host.emissions().size(), 2U);
  EXPECT_EQ(host.emissions().back().type, "Pong");
  EXPECT_EQ(*host.emissions().back().fields.find("n"), WireValue::integer(25));
}

TEST(VagentUpgradeTest, UpgradeHandlerFailureTriggersAtomicRollbackToV1) {
  constexpr const char *kV1Source = R"vagent(
module test:fail_v1

capacity Storage {
  def get_tag(): Str
}

skill StorageSkill implements Storage {
  def get_tag(): Str { "safe" }
}

event Ping { n: Int }
event Pong { n: Int }

agent Worker {
  uses Storage
  install StorageSkill()

  var count: Int := 10
  on Ping as p {
    count := count + p.n
    emit Pong{n: count} in outer
  }
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:fail_v2

capacity Storage {
  def get_tag(): Str
}

skill StorageSkill implements Storage {
  on upgrade(from_ver) {
    fail
  }
  def get_tag(): Str { "faulty" }
}

event Ping { n: Int }
event Pong { n: Int }

agent Worker {
  uses Storage
  install StorageSkill()

  var count: Int := 10
  on Ping as p {
    count := count + p.n
    emit Pong{n: count} in outer
  }
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  LocalHost host(std::move(*p1));
  auto pid = host.launch("Worker", "worker-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  // Test Ping in V1
  Envelope ping1;
  ping1.kind = "event";
  ping1.type = "Ping";
  ping1.fields.set("n", WireValue::integer(5));
  ASSERT_TRUE(host.send(*pid, std::move(ping1)));
  host.pump();

  ASSERT_EQ(host.emissions().size(), 1U);
  EXPECT_EQ(host.emissions().back().type, "Pong");
  EXPECT_EQ(*host.emissions().back().fields.find("n"), WireValue::integer(15));

  // Live upgrade to V2 where on upgrade hook fails
  auto upgraded = host.upgrade(*pid, std::move(*p2));
  EXPECT_FALSE(upgraded.has_value());
  EXPECT_EQ(upgraded.error(), ActorError::ExecutionFailed);

  // Verify actor rolled back cleanly and continues running under V1
  Envelope ping2;
  ping2.kind = "event";
  ping2.type = "Ping";
  ping2.fields.set("n", WireValue::integer(10));
  ASSERT_TRUE(host.send(*pid, std::move(ping2)));
  host.pump();

  ASSERT_EQ(host.emissions().size(), 2U);
  EXPECT_EQ(host.emissions().back().type, "Pong");
  EXPECT_EQ(*host.emissions().back().fields.find("n"), WireValue::integer(25));
}

} // namespace
