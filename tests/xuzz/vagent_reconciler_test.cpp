/**
 * @file vagent_reconciler_test.cpp
 * @brief Unit tests for vagent state reconciler (§4.3):
 *        - Field preservation, added defaults, dropped fields in
 * $dropped_fields
 *        - Belief preservation and discarded removed predicates
 *        - Intention plan AST equivalence (identical vs modified plan)
 *        - Upgrade event staging
 */
#include "common/xanadu/vagent/runtime/reconciler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace std::chrono_literals;
using xanadu::identity::MessageType;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;
using zigzag::vortex::mesh::ActorPid;
using zigzag::vortex::mesh::ActorTime;
using zigzag::vortex::mesh::MeshActorRuntime;

ActorGrant grantFor(std::string programRef) {
  ActorGrant g;
  g.programRef      = std::move(programRef);
  g.baseRef         = "base-1";
  g.expiresAt       = std::chrono::system_clock::now() + 1h;
  g.maxInstructions = 100000;
  g.maxCells        = 10000;
  g.maxOutputBytes  = 32768;
  g.maxSolutions    = 1;
  g.maxMailboxBytes = 65536;
  g.immutable       = false;
  return g;
}

ActorMessage spawnRequest(std::string programRef) {
  ActorMessage msg;
  msg.type          = MessageType::ActorSpawn;
  msg.requestId     = "spawn-1";
  msg.capability    = "signed-grant";
  msg.hashcashStamp = "verified-by-authorizer";
  msg.source        = ActorPid{"requester", "client", 1, 1};
  msg.programRef    = std::move(programRef);
  return msg;
}

// ---------------------------------------------------------------------------
// 1. Fields Reconciliation: Preserved, Added, Dropped in $dropped_fields
// ---------------------------------------------------------------------------

TEST(VagentReconcilerTest, PreservesFieldsAddsDefaultsAndRecordsDropped) {
  constexpr const char *kV1Source = R"vagent(
module test:v1
agent Worker {
  var count: Int := 10
  var note: Str := "active"
  var obsolete_token: Str := "secret_123"
  var old_number: Int := 99
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:v2
agent Worker {
  var count: Int := 0
  var note: Str := "fresh"
  var newly_added: Int := 42
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  WorkerContext fromCtx;
  auto compiled1 = p1->instantiate(fromCtx, "Worker");
  ASSERT_TRUE(compiled1) << compiled1.error();
  const auto fromVd = valueDims(fromCtx.core);

  // Set non-default values in fromCtx
  setField(fromCtx.arena, fromVd, compiled1->agentCell, "count",
           fromCtx.arena.makeScalarCell(std::int64_t{25}));
  setField(fromCtx.arena, fromVd, compiled1->agentCell, "note",
           fromCtx.arena.makeCell("in-flight"));
  setField(fromCtx.arena, fromVd, compiled1->agentCell, "obsolete_token",
           fromCtx.arena.makeCell("secret_123"));
  setField(fromCtx.arena, fromVd, compiled1->agentCell, "old_number",
           fromCtx.arena.makeScalarCell(std::int64_t{99}));

  WorkerContext toCtx;
  ReconcileOutcome outcome;
  ReconcileOptions options{
      .fromVersion = "1.0.0",
      .toVersion   = "2.0.0",
      .agentType   = "Worker",
  };

  auto res = reconcileState(fromCtx, toCtx, *p1, *p2, options);
  ASSERT_TRUE(res.has_value());
  outcome = *res;

  EXPECT_GE(outcome.preservedFields, 2U); // count and note (plus metadata)
  EXPECT_GE(outcome.addedFields, 1U);     // newly_added
  EXPECT_EQ(outcome.droppedFields, 2U);   // obsolete_token, old_number

  const auto toVd = valueDims(toCtx.core);
  // Find toAgent
  zigzag::CellRef toAgent = zigzag::noCell;
  for (std::uint32_t i = 0; i < toCtx.arena.cellCount(); ++i) {
    auto c = toCtx.arena.refOf(i);
    if (toCtx.arena.textOf(c) == "Worker") {
      toAgent = c;
      break;
    }
  }
  ASSERT_NE(toAgent, zigzag::noCell);

  // Preserved values
  auto countVal = fieldValue(toCtx.arena, toVd, toAgent, "count");
  ASSERT_TRUE(countVal.has_value());
  EXPECT_EQ(toCtx.arena.asInt64(*countVal).value_or(0), 25);

  auto noteVal = fieldValue(toCtx.arena, toVd, toAgent, "note");
  ASSERT_TRUE(noteVal.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*noteVal), "in-flight");

  // Dropped fields in $dropped_fields
  auto droppedRec = fieldValue(toCtx.arena, toVd, toAgent, kFieldDroppedFields);
  ASSERT_TRUE(droppedRec.has_value());

  auto dropToken = fieldValue(toCtx.arena, toVd, *droppedRec, "obsolete_token");
  ASSERT_TRUE(dropToken.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*dropToken), "secret_123");

  auto dropNum = fieldValue(toCtx.arena, toVd, *droppedRec, "old_number");
  ASSERT_TRUE(dropNum.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*dropNum), "99");
}

// ---------------------------------------------------------------------------
// 2. Belief Base: Preserved Matching Signatures and Discarded Removed
// ---------------------------------------------------------------------------

TEST(VagentReconcilerTest,
     PreservesMatchingBeliefsAndDiscardsRemovedPredicates) {
  constexpr const char *kV1Source = R"vagent(
module test:v1
predicate task(name: Str, priority: Int)
predicate temp_cache(entry: Str)

agent BeliefAgent {
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:v2
predicate task(name: Str, priority: Int)
predicate new_pred(x: Int)

agent BeliefAgent {
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  WorkerContext fromCtx;
  auto compiled1 = p1->instantiate(fromCtx, "BeliefAgent");
  ASSERT_TRUE(compiled1) << compiled1.error();
  const auto fromDims = registerDimensions(fromCtx.arena);

  // Add clauses under 'task' and 'temp_cache'
  auto fromPreds =
      zigzag::rankAfter(fromCtx.arena, compiled1->agentCell, fromDims.belief) |
      std::ranges::to<std::vector<zigzag::CellRef>>();

  auto taskPredIt = std::ranges::find_if(fromPreds, [&](zigzag::CellRef c) {
    return fromCtx.arena.textOf(c) == "task";
  });
  ASSERT_NE(taskPredIt, fromPreds.end());

  auto tempPredIt = std::ranges::find_if(fromPreds, [&](zigzag::CellRef c) {
    return fromCtx.arena.textOf(c) == "temp_cache";
  });
  ASSERT_NE(tempPredIt, fromPreds.end());

  // Add 2 task clauses
  auto cl1 = fromCtx.arena.makeCell("task1");
  appendToRank(fromCtx.arena, *taskPredIt, fromCtx.core.dims().clause, cl1);
  auto cl2 = fromCtx.arena.makeCell("task2");
  appendToRank(fromCtx.arena, *taskPredIt, fromCtx.core.dims().clause, cl2);

  // Add 1 temp_cache clause
  auto cl3 = fromCtx.arena.makeCell("cached_data");
  appendToRank(fromCtx.arena, *tempPredIt, fromCtx.core.dims().clause, cl3);

  WorkerContext toCtx;
  ReconcileOutcome outcome;
  auto res = reconcileState(fromCtx, toCtx, *p1, *p2, "BeliefAgent");
  ASSERT_TRUE(res.has_value());
  outcome = *res;

  EXPECT_EQ(outcome.preservedBeliefs, 2U);
  EXPECT_EQ(outcome.droppedBeliefs, 1U);

  const auto toDims       = registerDimensions(toCtx.arena);
  zigzag::CellRef toAgent = zigzag::noCell;
  for (std::uint32_t i = 0; i < toCtx.arena.cellCount(); ++i) {
    auto c = toCtx.arena.refOf(i);
    if (toCtx.arena.textOf(c) == "BeliefAgent") {
      toAgent = c;
      break;
    }
  }
  ASSERT_NE(toAgent, zigzag::noCell);

  auto toPreds = zigzag::rankAfter(toCtx.arena, toAgent, toDims.belief) |
                 std::ranges::to<std::vector<zigzag::CellRef>>();

  auto toTaskIt = std::ranges::find_if(toPreds, [&](zigzag::CellRef c) {
    return toCtx.arena.textOf(c) == "task";
  });
  ASSERT_NE(toTaskIt, toPreds.end());

  auto toTaskClauses =
      zigzag::rankAfter(toCtx.arena, *toTaskIt, toCtx.core.dims().clause) |
      std::ranges::to<std::vector<zigzag::CellRef>>();
  EXPECT_EQ(toTaskClauses.size(), 2U);

  auto toTempIt = std::ranges::find_if(toPreds, [&](zigzag::CellRef c) {
    return toCtx.arena.textOf(c) == "temp_cache";
  });
  if (toTempIt != toPreds.end()) {
    auto tempClauses =
        zigzag::rankAfter(toCtx.arena, *toTempIt, toCtx.core.dims().clause) |
        std::ranges::to<std::vector<zigzag::CellRef>>();
    EXPECT_EQ(tempClauses.size(), 0U);
  }
}

// ---------------------------------------------------------------------------
// 3. Intention Plan AST Equivalence: Identical vs Modified vs Removed Goal
// ---------------------------------------------------------------------------

TEST(VagentReconcilerTest, IntentionPlanEquivalenceIdenticalVsModified) {
  constexpr const char *kV1Source = R"vagent(
module test:v1
goal Clean(zone: Str) achieve
goal Obsolete() achieve

plan p_identical for Clean(zone) {
  yield
}

plan p_modified for Clean(zone) {
  yield
}

agent PlanAgent {
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:v2
goal Clean(zone: Str) achieve

plan p_identical for Clean(zone) {
  yield
}

plan p_modified for Clean(zone) {
  yield
  yield
}

agent PlanAgent {
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  WorkerContext fromCtx;
  auto compiled1 = p1->instantiate(fromCtx, "PlanAgent");
  ASSERT_TRUE(compiled1) << compiled1.error();
  const auto fromDims = registerDimensions(fromCtx.arena);
  const auto fromVd   = valueDims(fromCtx.core);

  // Intention 1: Clean("room1") with p_identical
  auto ic1 = fromCtx.arena.makeCell("Clean");
  setField(fromCtx.arena, fromVd, ic1, kIntentionGoal,
           fromCtx.arena.makeCell("Clean"));
  setField(fromCtx.arena, fromVd, ic1, kIntentionPlan,
           fromCtx.arena.makeCell("p_identical"));
  setField(fromCtx.arena, fromVd, ic1, kIntentionStep,
           fromCtx.arena.makeScalarCell(std::int64_t{1}));
  appendToRank(fromCtx.arena, compiled1->agentCell, fromDims.intention, ic1);

  // Intention 2: Clean("room2") with p_modified
  auto ic2 = fromCtx.arena.makeCell("Clean");
  setField(fromCtx.arena, fromVd, ic2, kIntentionGoal,
           fromCtx.arena.makeCell("Clean"));
  setField(fromCtx.arena, fromVd, ic2, kIntentionPlan,
           fromCtx.arena.makeCell("p_modified"));
  setField(fromCtx.arena, fromVd, ic2, kIntentionStep,
           fromCtx.arena.makeScalarCell(std::int64_t{1}));
  appendToRank(fromCtx.arena, compiled1->agentCell, fromDims.intention, ic2);

  // Intention 3: Obsolete() (goal removed in V2)
  auto ic3 = fromCtx.arena.makeCell("Obsolete");
  setField(fromCtx.arena, fromVd, ic3, kIntentionGoal,
           fromCtx.arena.makeCell("Obsolete"));
  appendToRank(fromCtx.arena, compiled1->agentCell, fromDims.intention, ic3);

  WorkerContext toCtx;
  auto res = reconcileState(fromCtx, toCtx, *p1, *p2, "PlanAgent");
  ASSERT_TRUE(res.has_value());
  auto outcome = *res;

  EXPECT_EQ(outcome.preservedIntentions, 1U); // ic1 (p_identical)
  EXPECT_EQ(outcome.abortedIntentions,
            2U); // ic2 (modified) + ic3 (removed goal)

  const auto toDims       = registerDimensions(toCtx.arena);
  const auto toVd         = valueDims(toCtx.core);
  zigzag::CellRef toAgent = zigzag::noCell;
  for (std::uint32_t i = 0; i < toCtx.arena.cellCount(); ++i) {
    auto c = toCtx.arena.refOf(i);
    if (toCtx.arena.textOf(c) == "PlanAgent") {
      toAgent = c;
      break;
    }
  }
  ASSERT_NE(toAgent, zigzag::noCell);

  auto toIntentions =
      zigzag::rankAfter(toCtx.arena, toAgent, toDims.intention) |
      std::ranges::to<std::vector<zigzag::CellRef>>();
  // ic1 and ic2 survive on rank (ic2 with replan state), ic3 is completely
  // dropped
  EXPECT_EQ(toIntentions.size(), 2U);

  // ic1 has p_identical and preserved step
  auto p1Val = fieldValue(toCtx.arena, toVd, toIntentions[0], kIntentionPlan);
  ASSERT_TRUE(p1Val.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*p1Val), "p_identical");
  auto s1Val = fieldValue(toCtx.arena, toVd, toIntentions[0], kIntentionStep);
  ASSERT_TRUE(s1Val.has_value());
  EXPECT_EQ(toCtx.arena.asInt64(*s1Val).value_or(-1), 1);

  // ic2 had its plan cleared and step set to 0 for re-deliberation
  auto p2Val = fieldValue(toCtx.arena, toVd, toIntentions[1], kIntentionPlan);
  EXPECT_FALSE(p2Val.has_value());
  auto s2Val = fieldValue(toCtx.arena, toVd, toIntentions[1], kIntentionStep);
  ASSERT_TRUE(s2Val.has_value());
  EXPECT_EQ(toCtx.arena.asInt64(*s2Val).value_or(-1), 0);
  auto f2Val =
      fieldValue(toCtx.arena, toVd, toIntentions[1], kIntentionFaulted);
  ASSERT_TRUE(f2Val.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*f2Val), "upgraded");
}

// ---------------------------------------------------------------------------
// 4. Staging Upgrade Event and Live Upgrade Transition via MeshActorRuntime
// ---------------------------------------------------------------------------

TEST(VagentReconcilerTest,
     StagesUpgradeEventAndExecutesMeshActorRuntimeUpgrade) {
  constexpr const char *kV1Source = R"vagent(
module test:upgrade_v1
agent Upgradable {
  var tag: Str := "v1_initial"
}
)vagent";

  constexpr const char *kV2Source = R"vagent(
module test:upgrade_v2
agent Upgradable {
  var tag: Str := "v2_fresh"
  var upgraded_from: Str := "none"
  on Upgrade as u {
    upgraded_from := u.from_version
  }
}
)vagent";

  auto p1 = CompiledProgram::fromSource(kV1Source);
  ASSERT_TRUE(p1) << p1.error();
  auto p2 = CompiledProgram::fromSource(kV2Source);
  ASSERT_TRUE(p2) << p2.error();

  WorkerContext fromCtx;
  auto compiled1 = p1->instantiate(fromCtx, "Upgradable");
  ASSERT_TRUE(compiled1) << compiled1.error();
  const auto fromVd = valueDims(fromCtx.core);
  setField(fromCtx.arena, fromVd, compiled1->agentCell, "tag",
           fromCtx.arena.makeCell("v1_running"));

  WorkerContext toCtx;
  ReconcileOptions options{
      .fromVersion = "1.0.0",
      .toVersion   = "2.0.0",
      .agentType   = "Upgradable",
  };
  auto outcome = reconcileState(fromCtx, toCtx, *p1, *p2, options);
  ASSERT_TRUE(outcome.has_value());

  const auto toDims       = registerDimensions(toCtx.arena);
  const auto toVd         = valueDims(toCtx.core);
  zigzag::CellRef toAgent = zigzag::noCell;
  for (std::uint32_t i = 0; i < toCtx.arena.cellCount(); ++i) {
    auto c = toCtx.arena.refOf(i);
    if (toCtx.arena.textOf(c) == "Upgradable") {
      toAgent = c;
      break;
    }
  }
  ASSERT_NE(toAgent, zigzag::noCell);

  // Check outbox has the Upgrade event
  auto outboxEntries = zigzag::rankAfter(toCtx.arena, toAgent, toDims.outbox) |
                       std::ranges::to<std::vector<zigzag::CellRef>>();
  ASSERT_GE(outboxEntries.size(), 1U);

  auto evCell = fieldValue(toCtx.arena, toVd, outboxEntries[0], kOutboxEvent);
  ASSERT_TRUE(evCell.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*evCell), kEventUpgrade);

  auto fromVerVal = fieldValue(toCtx.arena, toVd, *evCell, "from_version");
  ASSERT_TRUE(fromVerVal.has_value());
  EXPECT_EQ(toCtx.arena.textOf(*fromVerVal), "1.0.0");

  // Now test integration with MeshActorRuntime::upgrade
  MeshActorRuntime runtime("worker", 1);
  xanadu::identity::Fingerprint identity;
  runtime.trustNode("requester", identity);

  runtime.setAuthorizer(
      [&](const xanadu::identity::Fingerprint &, const ActorMessage &request)
          -> std::expected<ActorGrant, ActorError> {
        return grantFor(request.programRef);
      });

  runtime.setExecutor(
      [&](const zigzag::vortex::mesh::ActorExecution &,
          WorkerContext &) -> std::expected<std::string, ActorError> {
        return "ok";
      });

  const auto spawned = runtime.spawn(identity, spawnRequest("prog-v1"),
                                     std::chrono::system_clock::now());
  ASSERT_TRUE(spawned) << "spawn error: " << static_cast<int>(spawned.error());
  const auto pid = *spawned;

  // Set state root on actor to agent cell instantiated in actor's context
  auto *actorCtx = runtime.context(pid);
  ASSERT_NE(actorCtx, nullptr);
  auto compiledInActor = p1->instantiate(*actorCtx, "Upgradable");
  ASSERT_TRUE(compiledInActor);
  const auto actorVd = valueDims(actorCtx->core);
  setField(actorCtx->arena, actorVd, compiledInActor->agentCell, "tag",
           actorCtx->arena.makeCell("v1_running"));
  ASSERT_TRUE(
      runtime.setStateRoot(pid, compiledInActor->agentCell).has_value());

  ReconcileOutcome runtimeOutcome;
  auto reconciler = makeStateReconciler(*p1, *p2, options, &runtimeOutcome);

  auto status = runtime.upgrade(pid, grantFor("prog-v2"),
                                std::chrono::system_clock::now(), reconciler);
  ASSERT_TRUE(status.has_value()) << static_cast<int>(status.error());
  EXPECT_GE(runtimeOutcome.preservedFields, 1U);
}

} // namespace
