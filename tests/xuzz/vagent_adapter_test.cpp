// The adapter seam (actor-executor's greenfield half): VagentAdapter
// drives VagentExecutor turns the way MeshActorRuntime's callbacks
// hand them down — mesh pids translated through the member's durable
// `pid` record, ActorDown mailbox messages becoming childDown, each
// committed turn ending in the §3.7 outbox drain.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../apps/common/xanadu/vagent/adapter.hpp"
#include "../../apps/common/xanadu/vagent/decls.hpp"
#include "../../apps/common/xanadu/vagent/frontend.hpp"
#include "../../apps/common/xanadu/vagent/lineage.hpp"
#include "../../apps/common/xanadu/vagent/records.hpp"
#include "../../apps/common/xanadu/store.hpp"

namespace {

using namespace xanadu::vagent;
using xanadu::identity::ActorPid;
using xanadu::identity::MessageType;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGenesis;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

struct AdapterHarness {
  WorkerContext ctx;
  VagentAdapter adapter;

  std::expected<LoweredImage, std::vector<Diagnostic>>
  lower(std::string_view source) {
    auto artifacts = compileArtifacts(source);
    if (!artifacts) {
      for (const auto &d : artifacts.error())
        fprintf(stderr, "frontend: %s\n", d.detail.c_str());
      return std::unexpected(artifacts.error());
    }
    auto image = lowerProgram(artifacts->program, ctx.core, ctx.vm);
    if (!image)
      for (const auto &d : image.error())
        fprintf(stderr, "lower: %s\n", d.detail.c_str());
    return image;
  }

  ActorExecution execution(std::optional<ActorMessage> message =
                               std::nullopt) {
    ActorExecution exec;
    exec.pid   = ActorPid{"worker", "agent", 1, 1};
    exec.grant = ActorGrant{.programRef      = "sha256:p",
                            .baseRef         = "sha256:b",
                            .maxInstructions = 100'000,
                            .maxCells        = 10'000};
    exec.message = std::move(message);
    exec.cursor =
        ctx.vm.spawnCursor(ctx.vm.mintOpcode(OpcodeKind::Halt), "actor");
    return exec;
  }

  ActorMessage send(std::string value) {
    ActorMessage msg;
    msg.type      = MessageType::ActorSend;
    msg.requestId = "req-1";
    msg.value     = std::move(value);
    return msg;
  }

  std::vector<std::string> journalOf(CellRef agent) {
    const auto seen = ctx.core.findOrMintDimension("d.seen");
    std::vector<std::string> names;
    for (const CellRef cell :
         zigzag::rankAfter(ctx.arena, agent, seen))
      names.emplace_back(ctx.arena.textOf(cell));
    return names;
  }

  std::optional<CellRef> memberCell(CellRef agent,
                                  std::string_view varName) {
    const auto rec = varsRecord(ctx.arena, ctx.core, agent, varName, false);
    if (!rec) return std::nullopt;
    const auto ref = ctx.arena.linked(*rec, ctx.core.dims().values,
                                    DimVector::POS);
    if (!ref) return std::nullopt;
    const auto member = ctx.arena.asInt64(*ref);
    if (!member) return std::nullopt;
    return static_cast<CellRef>(*member);
  }
};

TEST(VagentAdapterTest, NotedPidResolvesBackToItsMember) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nagent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);

  const ActorPid meshPid{"node7", "contentid", 3, 41};
  // A mesh spawnSequence is the runtime's global counter — it is not
  // a member index, so before the note the pid resolves to nothing.
  EXPECT_FALSE(h.adapter.memberForPid(h.ctx, meshPid).has_value());
  h.adapter.noteMemberPid(h.ctx, *member, meshPid);
  const auto found = h.adapter.memberForPid(h.ctx, meshPid);
  ASSERT_TRUE(found);
  EXPECT_EQ(*found, *member);
  EXPECT_EQ(h.adapter.memberIndex(h.ctx, *member), 1u);
  // The declared agent is member 0; the spawned member shares its
  // declared +d.name — the pid record is what tells the two apart.
  EXPECT_EQ(h.adapter.memberNamed(h.ctx, "A"), image->agentCell);
  // A rewritten incarnation resolves through the new key only.
  const ActorPid moved{"node8", "contentid", 4, 42};
  h.adapter.noteMemberPid(h.ctx, *member, moved);
  EXPECT_FALSE(h.adapter.memberForPid(h.ctx, meshPid).has_value());
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, moved));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, moved), *member);
}

TEST(VagentAdapterTest, ActorDownBecomesChildDownAndFailsTheWait) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Lost\nevent Down\n"
              "question stock(item: Str): Str\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); adopt Work() }\n"
              "  on ChildDown { emit Down{} }\n"
              "  plan w for Work { let r := ask child.stock(\"oil\") within "
              "9s else cause; emit Lost{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  const ActorPid childPid{"node3", "childid", 2, 7};
  h.adapter.noteMemberPid(h.ctx, *member, childPid);
  // Once any member is noted, harness pids stop resolving — the turn
  // arrives with the mesh pid genesis recorded for this actor.
  const ActorPid parentPid{"worker", "agentid", 1, 5};
  h.adapter.noteMemberPid(h.ctx, image->agentCell, parentPid);

  // The mesh delivers the monitored member's ActorDown as a mailbox
  // message — the adapter consumes it, raises ChildDown, and the same
  // turn's settle faults the ask parked on the child's name.
  ActorMessage down;
  down.type      = MessageType::ActorDown;
  down.requestId = "mon-1";
  down.source    = childPid;
  down.value     = "ActorDown";
  auto turnDown = h.execution(down);
  turnDown.pid  = parentPid;
  ASSERT_TRUE(h.adapter(turnDown, h.ctx));
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("ChildDown"));
  // The raw message never dispatches as a generic event — ChildDown
  // is the only loss percept the journal holds.
  EXPECT_THAT(journal,
              ::testing::Not(::testing::Contains("ActorDown")));
  // The faulted wait's else ran with cause "down"; the continuation
  // needs its own turn to emit Lost, and the handler's Down rides the
  // same drain — journal order is Initialize, ChildDown, Down, Lost.
  auto turnWake = h.execution();
  turnWake.pid  = parentPid;
  ASSERT_TRUE(h.adapter(turnWake, h.ctx));
  const auto after = h.journalOf(image->agentCell);
  EXPECT_THAT(after, ::testing::Contains("Down"));
  EXPECT_THAT(after, ::testing::Contains("Lost"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, CommittedTurnDrainsTheOutbox) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nagent A {\n"
              "  on Initialize { emit Tick{} }\n"
              "}");
  ASSERT_TRUE(image);
  // One adapter turn: the Initialize handler stages Tick, the
  // post-commit drain routes it broadcast — back onto the sender's
  // own journal like every other member's.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Tick"));
  // The staged record is delivered, not re-staged: the drain is
  // read-only and deliver's +d.delivered dedupe makes a second turn
  // stage nothing new — the journal stays Initialize + Tick.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_EQ(h.journalOf(image->agentCell).size(), 2u);
}

TEST(VagentAdapterTest, UnroutableTargetFaultsTheSender) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Ping\nagent A {\n"
              "  var target: Opt<Agent> := none\n"
              "  on Initialize { emit Ping{} for target }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("DeliveryFailed"));
  EXPECT_THAT(journal, ::testing::Not(::testing::Contains("Ping")));
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, GenesisRewritesPidAndFailsEffectWaits) {
  AdapterHarness h;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Str within 9s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else cause }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  const auto staged =
      VagentExecutor::drainOutbox(arena, core, image->agentCell);
  ASSERT_EQ(staged.size(), 1u);
  EXPECT_EQ(staged[0].kind, "effect");

  // A genesis carrying a checkpoint means the actor reincarnated:
  // §5.4's effect waits cannot follow the device, so the parked
  // intention takes the else ladder with cause "migrated" — and the
  // member's durable pid record follows the new incarnation.
  xanadu::Store transported;
  ActorGenesis genesis;
  genesis.pid         = ActorPid{"node9", "reincarnation", 5, 88};
  genesis.transported = &transported;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, genesis.pid));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, genesis.pid),
            image->agentCell);
  auto turnWake = h.execution();
  turnWake.pid  = genesis.pid;
  ASSERT_TRUE(h.adapter(turnWake, h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv =
      arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "migrated");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, FirstSpawnGenesisOnlyNotesThePid) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nagent A { }");
  ASSERT_TRUE(image);
  // transport=null means first spawn — the incarnation's pid lands
  // durably without the migrated-agent mark.
  ActorGenesis genesis;
  genesis.pid = ActorPid{"node1", "firstspawn", 1, 3};
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, genesis.pid));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, genesis.pid),
            image->agentCell);
}

TEST(VagentAdapterTest, SpawnRefusedFailsTheMemberAndItsDeliveries) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Wave\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() in inner; emit Wave{} in "
              "inner for child }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  // The mesh answered the spawn request with no pid — §8.6 raises
  // SpawnFailed on the parent and fails deliveries the refused name
  // still stages.
  h.adapter.spawnRefused(h.ctx, image->agentCell, *member);
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("SpawnFailed"));
  EXPECT_THAT(journal, ::testing::Contains("DeliveryFailed"));
}

TEST(VagentAdapterTest, MemberReboundUpdatesStaleReferences) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  // The adapter's replacement reference for the migrated member —
  // same +d.name, fresh cell — rebinds the var still holding the
  // stale one, then Moved dispatches. (Opt vars that merely copied
  // the binding keep their opt node; only direct member refs rebind.)
  const auto replacement = arena.makeCell();
  zigzag::expectWritten(
      arena.link(replacement, core.dims().name, DimVector::POS,
                 arena.makeCell("A")));
  h.adapter.memberRebound(h.ctx, image->agentCell, replacement);
  const auto rebound = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(rebound);
  EXPECT_EQ(*rebound, replacement);
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Moved"));
}

TEST(VagentAdapterTest, NativeBodyRepliesDoneAndBindsTheResult) {
  AdapterHarness h;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Str within 9s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else cause }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  int invoked = 0;
  h.adapter.bindNative("Navigation", "read",
                       [&](WorkerContext &c, CellRef, CellRef) {
                         ++invoked;
                         return c.arena.makeCell("berries");
                       });
  // The Tick turn stages + parks the effect; the post-commit drain
  // invokes the binding and replies EffectDone on the correlation —
  // the next turn's wake match binds r.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  EXPECT_EQ(invoked, 1);
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv =
      arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "berries");
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("EffectDone"));
  // `dispatched` makes the invoke once-durable: later drains see the
  // same staged entry and leave the body alone.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_EQ(invoked, 1);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, NativeFaultCarriesTheCauseIntoElse) {
  AdapterHarness h;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Str within 9s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else cause }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  h.adapter.bindNative(
      "Navigation", "read",
      [](WorkerContext &, CellRef, CellRef)
          -> std::expected<CellRef, std::string> {
        return std::unexpected("denied");
      });
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv =
      arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "denied");
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("EffectFailed"));
}

TEST(VagentAdapterTest, UnboundNativeFaultsUnbound) {
  AdapterHarness h;
  const auto image = h.lower(
      "module demo\nevent Tick\ngoal Work() perform\n"
      "capacity Navigation { effect def read(): Str within 9s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { adopt Work() }\n"
      "  plan w for Work { let r := Navigation.read() else cause }\n"
      "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  // No binding registered — the staged effect still answers, with
  // the honest provider-absent cause.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv =
      arena.linked(*r, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "unbound");
}

TEST(VagentAdapterTest, HandlerFormEffectDispatchesItsReply) {
  AdapterHarness h;
  const auto image = h.lower(
      "module demo\nevent Tick\nevent Got\n"
      "capacity Navigation { effect def ping(): Str within 9s }\n"
      "native skill Device implements Navigation\n"
      "agent A {\n"
      "  uses Navigation\n"
      "  install Device\n"
      "  on Tick { Navigation.ping() }\n"
      "  on EffectDone { emit Got{} }\n"
      "}");
  ASSERT_TRUE(image);
  h.adapter.bindNative("Navigation", "ping",
                       [](WorkerContext &c, CellRef, CellRef) {
                         return c.arena.makeCell("pong");
                       });
  // The handler form parks nothing — the reply arrives as an ordinary
  // percept and its handler dispatches on the next turn.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("EffectDone"));
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Got"));
}

TEST(VagentAdapterTest, SpawnOutcomeNotesGrantAndFailsRefusal) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  var spare: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); spare := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto granted = h.memberCell(image->agentCell, "child");
  const auto refused = h.memberCell(image->agentCell, "spare");
  ASSERT_TRUE(granted);
  ASSERT_TRUE(refused);
  // The integration layer's half: the durable request correlation,
  // written before the wire send.
  h.adapter.noteSpawnRequest(h.ctx, *granted, "req-grant");
  h.adapter.noteSpawnRequest(h.ctx, *refused, "req-refuse");
  ActorMessage request;
  request.requestId = "req-grant";
  const ActorPid meshPid{"host", "spawned-cursor", 2, 41};
  h.adapter.spawnOutcome(h.ctx, request, meshPid);
  // The grant lands the member's real mesh identity and consumes
  // the pending correlation.
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, meshPid));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, meshPid), *granted);
  h.adapter.spawnOutcome(h.ctx, request, meshPid);
  // A replayed ack finds no spawnRequest and reports nothing — the
  // member still resolves.
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, meshPid), *granted);
  // The refused member's staged name fails §8.6-style on the agent.
  request.requestId = "req-refuse";
  h.adapter.spawnOutcome(
      h.ctx, request,
      std::expected<ActorPid, ActorError>{
          std::unexpected(ActorError::Refused)});
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("SpawnFailed"));
  // An id no member ever claimed reports nothing at all.
  request.requestId = "req-nobody";
  h.adapter.spawnOutcome(
      h.ctx, request,
      std::expected<ActorPid, ActorError>{
          std::unexpected(ActorError::Refused)});
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, SpawnStagesTheWireRequestThroughTheSender) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  int calls = 0;
  std::string requestId;
  CellRef sentMember = zigzag::noCell;
  h.adapter.setSpawnSender(
      [&](WorkerContext &, CellRef member, std::string_view id)
          -> std::expected<void, ActorError> {
        ++calls;
        requestId   = std::string{id};
        sentMember  = member;
        return {};
      });
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(sentMember, *member);
  // The durable correlation the ack resolves: the member's
  // spawnRequest record names the staged entry's own id.
  const auto req =
      varsRecord(arena, core, *member, "spawnRequest", false);
  ASSERT_TRUE(req);
  const auto rv = arena.linked(*req, core.dims().values, DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), requestId);
  // The remote's grant resolves the pending request end to end.
  ActorMessage request;
  request.requestId = requestId;
  const ActorPid meshPid{"host", "spawned-cursor", 1, 9};
  h.adapter.spawnOutcome(h.ctx, request, meshPid);
  ASSERT_TRUE(h.adapter.memberForPid(h.ctx, meshPid));
  EXPECT_EQ(*h.adapter.memberForPid(h.ctx, meshPid), *member);
  // Once-durable: a later drain does not re-send.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_EQ(calls, 1);
}

TEST(VagentAdapterTest, MemberStoppingRaisesOnceAndDestroysTheMember) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto member = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(member);
  // The mesh-side half of §4.5: the adapter hands the cause the
  // runtime observed and the executor writes the full envelope —
  // member flagged destroying, its Destroy queued, the parent's
  // ChildStopping raised once.
  h.adapter.memberStopping(h.ctx, image->agentCell, *member, "lease");
  h.adapter.memberStopping(h.ctx, image->agentCell, *member, "lease");
  const auto flag = varsRecord(arena, core, *member, "destroying", false);
  ASSERT_TRUE(flag);
  const auto fv = arena.linked(*flag, core.dims().values,
                               DimVector::POS);
  ASSERT_TRUE(fv);
  EXPECT_EQ(arena.asInt64(*fv).value_or(0), 1);
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("ChildStopping"));
  EXPECT_THAT(h.journalOf(*member), ::testing::Contains("Destroy"));
  // The dedupe is a journal fact: one ChildStopping across repeats.
  std::size_t count = 0;
  for (const auto &name : h.journalOf(image->agentCell))
    if (name == "ChildStopping") ++count;
  EXPECT_EQ(count, 1u);
}

TEST(VagentAdapterTest, NextDeadlineIsTheEarliestParkedAwait) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Lost\n"
              "question stock(item: Str): Str\n"
              "goal Work() perform\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A(); adopt Work() }\n"
              "  plan w for Work { let r := ask child.stock(\"oil\") within "
              "9s else cause; emit Lost{} }\n"
              "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  // The ask parked inside Boot's turn — its deadline is the
  // park-turn ordinal plus the within bound, exactly the heap entry
  // the host reads to know turns are owed.
  const auto deadline = h.adapter.nextDeadline(h.ctx, image->agentCell);
  ASSERT_TRUE(deadline);
  const auto turnRec =
      varsRecord(arena, core, image->agentCell, "$turn", false);
  ASSERT_TRUE(turnRec);
  const auto now = arena.asInt64(
      *arena.linked(*turnRec, core.dims().values, DimVector::POS));
  ASSERT_TRUE(now);
  EXPECT_EQ(*deadline, *now + 9);
  // Wakes are how an idle actor's clock moves: pump the message-less
  // turns P4's runtime would inject until the deadline's rung fires.
  for (std::int64_t t = *now; t < *deadline; ++t)
    ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  // The else bound "deadline" and the intention unparked — the heap
  // owes this member nothing until it parks again.
  EXPECT_FALSE(h.adapter.nextDeadline(h.ctx, image->agentCell)
                   .has_value());
  const auto r = varsRecord(arena, core, image->agentCell, "r", false);
  ASSERT_TRUE(r);
  const auto rv = arena.linked(*r, core.dims().values,
                               DimVector::POS);
  ASSERT_TRUE(rv);
  EXPECT_EQ(arena.textOf(*rv), "unanswered");
}

TEST(VagentAdapterTest, SpawnSendFailureFailsTheMember) {
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  h.adapter.setSpawnSender(
      [](WorkerContext &, CellRef, std::string_view)
          -> std::expected<void, ActorError> {
        return std::unexpected(ActorError::TransportUnavailable);
      });
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("SpawnFailed"));
}

TEST(VagentAdapterTest, GenesisOverASeededLineageRunsTheCeremony) {
  // The real spawn-lineage shape end to end: the lineage carries its
  // genesis and filed dim vocabulary first, the program lowers into a
  // compile context over that fold — dims resolve to the filed cells,
  // never promoted twins — seedProgramImage promotes just the new
  // program band, and the spawn pin's WorkerContext opens on the
  // rebuilt fold.
  xanadu::Store lineage;
  zigzag::ArenaManifold stage;
  const auto filed = fileDimensionVocabulary(lineage, stage, {});
  ASSERT_TRUE(filed) << (filed ? "" : filed.error());
  WorkerContext compile{&filed->fold, &lineage};
  const auto artifacts = compileArtifacts(
      "module demo\nagent A { def f(): Int { 1 + 2 } }");
  ASSERT_TRUE(artifacts);
  const auto image =
      lowerProgram(artifacts->program, compile.core, compile.vm);
  ASSERT_TRUE(image);
  ASSERT_NE(image->imageRecord, zigzag::noCell);
  const auto seeded = seedProgramImage(lineage, compile.arena,
                                       image->programRoot);
  ASSERT_TRUE(seeded) << (seeded ? "" : seeded.error());
  const auto fold = std::make_unique<zigzag::Manifold>(
      lineage.rebuildManifold(seeded->version));
  WorkerContext ctx{fold.get(), &lineage};

  VagentAdapter adapter;
  ActorGenesis genesis;
  genesis.pid = ActorPid{"worker", "agent", 1, 9};
  ASSERT_TRUE(adapter.genesis(genesis, ctx));
  ASSERT_TRUE(adapter.memberForPid(ctx, genesis.pid).has_value());

  // The discriminating arm: a foreign transported comparand must
  // refuse — (a)'s band verification is the only piece that can
  // decide it, so a refusal is proof the ceremony ran rather than
  // the image records going unread.
  xanadu::Store foreign;
  ActorGenesis reincarnation;
  reincarnation.pid         = ActorPid{"worker", "agent", 1, 10};
  reincarnation.transported = &foreign;
  EXPECT_FALSE(adapter.genesis(reincarnation, ctx));
}

TEST(VagentAdapterTest, SeededLineageExecutesTheEmitChain) {
  // The genesis fixture carried one turn further: a clause's guard and
  // spine run off operand slots whose Int64s are positions, and the
  // emit host-step's self-delivery is the observable proof the publish
  // restated them — a stale slot leaves the guard produced=0 and
  // Ready never reaches the journal.
  xanadu::Store lineage;
  zigzag::ArenaManifold stage;
  const auto filed = fileDimensionVocabulary(lineage, stage, {});
  ASSERT_TRUE(filed) << (filed ? "" : filed.error());
  WorkerContext compile{&filed->fold, &lineage};
  const auto artifacts = compileArtifacts(
      "module demo\nevent Ready\nagent A {\n"
      "  on Initialize { emit Ready{} }\n"
      "  on Ready { }\n"
      "}");
  ASSERT_TRUE(artifacts);
  const auto image =
      lowerProgram(artifacts->program, compile.core, compile.vm);
  ASSERT_TRUE(image);
  const auto seeded = seedProgramImage(lineage, compile.arena,
                                       image->programRoot);
  ASSERT_TRUE(seeded) << (seeded ? "" : seeded.error());
  const auto fold = std::make_unique<zigzag::Manifold>(
      lineage.rebuildManifold(seeded->version));
  WorkerContext ctx{fold.get(), &lineage};

  VagentAdapter adapter;
  ActorGenesis genesis;
  genesis.pid = ActorPid{"worker", "agent", 1, 9};
  ASSERT_TRUE(adapter.genesis(genesis, ctx));
  const auto member = adapter.memberForPid(ctx, genesis.pid);
  ASSERT_TRUE(member);

  const auto journal = [&] {
    const auto seen = ctx.core.findOrMintDimension("d.seen");
    std::vector<std::string> names;
    for (const CellRef cell : zigzag::rankAfter(ctx.arena, *member, seen))
      names.emplace_back(ctx.arena.textOf(cell));
    return names;
  };
  ActorExecution exec;
  exec.pid   = genesis.pid;
  exec.grant = ActorGrant{.programRef      = "sha256:p",
                          .baseRef         = "sha256:b",
                          .maxInstructions = 100'000,
                          .maxCells        = 10'000};
  exec.cursor =
      ctx.vm.spawnCursor(ctx.vm.mintOpcode(OpcodeKind::Halt), "actor");
  ASSERT_TRUE(adapter(exec, ctx));
  ASSERT_TRUE(adapter.pendingWork(ctx, *member));
  ASSERT_TRUE(adapter(exec, ctx));
  EXPECT_THAT(journal(), ::testing::ElementsAre("Initialize", "Ready"));
}

TEST(VagentAdapterTest, SendPerceptBindsNamedFieldsAndDeduplicates) {
  // The host's fielded send — what a wire ActorSend becomes when the
  // bare `value` string can't carry the payload. Staged through
  // deliver, the term's +d.step fields bind in the handler's guard:
  // total reaches 7 only if `a.amount` resolved off the payload. A
  // repeated id rides +d.delivered and never reaches the journal.
  xanadu::Store lineage;
  zigzag::ArenaManifold stage;
  const auto filed = fileDimensionVocabulary(lineage, stage, {});
  ASSERT_TRUE(filed) << (filed ? "" : filed.error());
  WorkerContext compile{&filed->fold, &lineage};
  const auto artifacts = compileArtifacts(
      "module demo\nevent Add { amount: Int }\nagent A {\n"
      "  var total: Int := 0\n"
      "  on Add as a {\n"
      "    let next := total + a.amount\n"
      "    total := next\n"
      "  }\n"
      "}");
  ASSERT_TRUE(artifacts);
  const auto image =
      lowerProgram(artifacts->program, compile.core, compile.vm);
  ASSERT_TRUE(image);
  const auto seeded = seedProgramImage(lineage, compile.arena,
                                       image->programRoot);
  ASSERT_TRUE(seeded) << (seeded ? "" : seeded.error());
  const auto fold = std::make_unique<zigzag::Manifold>(
      lineage.rebuildManifold(seeded->version));
  WorkerContext ctx{fold.get(), &lineage};

  VagentAdapter adapter;
  ActorGenesis genesis;
  genesis.pid = ActorPid{"worker", "agent", 1, 9};
  ASSERT_TRUE(adapter.genesis(genesis, ctx));
  const auto member = adapter.memberForPid(ctx, genesis.pid);
  ASSERT_TRUE(member);

  const auto total = [&] {
    const auto rec =
        varsRecord(ctx.arena, ctx.core, *member, "total", false);
    if (!rec) return std::optional<std::int64_t>{};
    const auto value = ctx.arena.linked(*rec, ctx.core.dims().values);
    return value ? ctx.arena.asInt64(*value)
                 : std::optional<std::int64_t>{};
  };
  const auto journal = [&] {
    const auto seen = ctx.core.findOrMintDimension("d.seen");
    std::vector<std::string> names;
    for (const CellRef cell : zigzag::rankAfter(ctx.arena, *member, seen))
      names.emplace_back(ctx.arena.textOf(cell));
    return names;
  };
  ActorExecution exec;
  exec.pid   = genesis.pid;
  exec.grant = ActorGrant{.programRef      = "sha256:p",
                          .baseRef         = "sha256:b",
                          .maxInstructions = 100'000,
                          .maxCells        = 10'000};
  exec.cursor =
      ctx.vm.spawnCursor(ctx.vm.mintOpcode(OpcodeKind::Halt), "actor");

  const std::vector<std::pair<std::string, zigzag::vortex::CellValue>>
      seven{{"amount", std::int64_t{7}}};
  adapter.sendPercept(ctx, *member, "Add", seven, "s1");
  adapter.sendPercept(ctx, *member, "Add", seven, "s1");
  ASSERT_TRUE(adapter(exec, ctx));
  EXPECT_THAT(journal(), ::testing::ElementsAre("Add", "Initialize"));
  EXPECT_THAT(total(), ::testing::Optional(7));

  const std::vector<std::pair<std::string, zigzag::vortex::CellValue>>
      three{{"amount", std::int64_t{3}}};
  adapter.sendPercept(ctx, *member, "Add", three, "s2");
  ASSERT_TRUE(adapter(exec, ctx));
  EXPECT_THAT(journal(),
              ::testing::ElementsAre("Add", "Initialize", "Add"));
  EXPECT_THAT(total(), ::testing::Optional(10));
}

} // namespace
