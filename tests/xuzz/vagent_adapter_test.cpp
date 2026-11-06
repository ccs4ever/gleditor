// The adapter seam (actor-executor's greenfield half): VagentAdapter
// drives VagentExecutor turns the way MeshActorRuntime's callbacks
// hand them down — mesh pids translated through the member's durable
// `pid` record, ActorDown mailbox messages becoming childDown, each
// committed turn ending in the §3.7 outbox drain.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "../../apps/common/xanadu/vagent/adapter.hpp"
#include "../../apps/common/xanadu/vagent/decls.hpp"
#include "../../apps/common/xanadu/vagent/frontend.hpp"
#include "../../apps/common/xanadu/vagent/lineage.hpp"
#include "../../apps/common/xanadu/vagent/marshal.hpp"
#include "../../apps/common/xanadu/vagent/percept_codec.hpp"
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

TEST(VagentAdapterTest, MovedFromParentRenamesParentPidAndRebinds) {
  AdapterHarness h;
  const auto image = h.lower("module demo\nevent Boot\nagent A {\n"
                             "  on Boot { emit Boot{} }\n"
                             "}");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;

  // The child's durable parent shape: a $parent member under the old
  // incarnation's pid plus the $parentPid text genesis rebinds through.
  const ActorPid oldParent{"n1", "parentid", 1, 1};
  const auto parent = arena.makeCell();
  h.adapter.noteMemberPid(h.ctx, parent, oldParent);
  const auto pidRec = varsRecord(arena, core, parent, "pid", false);
  ASSERT_TRUE(pidRec);
  const auto pidText =
      arena.linked(*pidRec, core.dims().values, DimVector::POS);
  ASSERT_TRUE(pidText);
  const auto ppRec =
      varsRecord(arena, core, image->agentCell, "$parentPid", true);
  ASSERT_TRUE(ppRec);
  zigzag::expectWritten(arena.link(*ppRec, core.dims().values,
                                 DimVector::POS,
                                 arena.makeCell(arena.textOf(*pidText))));
  const auto psRec =
      varsRecord(arena, core, image->agentCell, "$parent", true);
  ASSERT_TRUE(psRec);
  zigzag::expectWritten(arena.link(*psRec, core.dims().values,
                                 DimVector::POS,
                                 refCell(arena, core, parent)));

  // Once any member is noted, an unresolvable exec pid binds nobody —
  // the agent's own genesis pid goes on record first.
  h.adapter.noteMemberPid(h.ctx, image->agentCell,
                          ActorPid{"worker", "agent", 1, 1});

  // The wire Moved: the parent's new incarnation — same cursor, new
  // node and epoch.
  auto msg      = h.send("Moved{name: \"P\"}");
  msg.source    = ActorPid{"n2", "parentid", 2, 1};
  ASSERT_TRUE(h.adapter(h.execution(msg), h.ctx));

  const auto rebound = varsRecord(arena, core, image->agentCell, "$parent",
                                  false);
  ASSERT_TRUE(rebound);
  const auto held =
      arena.linked(*rebound, core.dims().values, DimVector::POS);
  ASSERT_TRUE(held);
  const auto heldRef = arena.asInt64(*held);
  ASSERT_TRUE(heldRef);
  const auto heldMember = static_cast<CellRef>(*heldRef);
  EXPECT_NE(heldMember, parent) << "$parent still names the old member";
  const auto heldName =
      arena.linked(heldMember, core.dims().name, DimVector::POS);
  ASSERT_TRUE(heldName);
  EXPECT_EQ(arena.textOf(*heldName), "P");

  // $parentPid renamed to the new incarnation — the text the rebound
  // member's own pid record carries — and the pid resolves to it.
  const auto pp = varsRecord(arena, core, image->agentCell, "$parentPid",
                             false);
  ASSERT_TRUE(pp);
  const auto ppv =
      arena.linked(*pp, core.dims().values, DimVector::POS);
  ASSERT_TRUE(ppv);
  const auto newPidRec = varsRecord(arena, core, heldMember, "pid", false);
  ASSERT_TRUE(newPidRec);
  const auto newPidText =
      arena.linked(*newPidRec, core.dims().values, DimVector::POS);
  ASSERT_TRUE(newPidText);
  EXPECT_EQ(arena.textOf(*ppv), arena.textOf(*newPidText));
  const auto resolved =
      h.adapter.memberForPid(h.ctx, ActorPid{"n2", "parentid", 2, 1});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, heldMember);
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

TEST(VagentAdapterTest, RemoteMemberSendRidesTheWireOnce) {
  // §3.7's remote arm: an emit whose delivery resolves to a member
  // whose `pid` record names another incarnation goes out as an
  // ActorSend — source/target rendered from the pid records, the
  // fielded payload rendered `Name{f: v}`, the request id carrying
  // the target key. `dispatched` lands only after the send, so a
  // re-drain never re-emits.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Tick\n"
              "event Ping { x: Int, note: Str }\n"
              "agent A {\n"
              "  var peer: Opt<Agent> := none\n"
              "  on Boot { peer := spawn A() }\n"
              "  on Tick { emit Ping{x: 1, note: \"hi\"} for peer }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid childPid{"node9", "childid", 2, 7};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  // Boot mints the member (the spawn entry itself is inert without a
  // spawnSender); the pid notes make it a remote incarnation, which
  // is what turns the next drain's delivery into a wire send.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "peer");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, childPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_EQ(wire.size(), 1u);
  EXPECT_EQ(wire[0].type, MessageType::ActorSend);
  ASSERT_TRUE(wire[0].source);
  ASSERT_TRUE(wire[0].target);
  EXPECT_EQ(*wire[0].source, selfPid);
  EXPECT_EQ(*wire[0].target, childPid);
  EXPECT_EQ(wire[0].value, "Ping{x: 1, note: \"hi\"}");
  // The id carries the entry's idempotence id and the target's key —
  // a broadcast to two remote members can't collide on the wire's
  // dedupe.
  EXPECT_THAT(wire[0].requestId,
              ::testing::EndsWith(":5:node97:childid:2:7"));
  // dispatched is durable: a second committed turn reads the same
  // outbox and sends nothing.
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  EXPECT_EQ(wire.size(), 1u);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, MissingTransportFaultsTheRemoteSender) {
  // A remote delivery with no sendSender installed is the
  // "transport" arm of delivery failure — the entry stays
  // undispatched so a sender arriving later still picks it up. The
  // same-named local member still receives its leg: `for peer`
  // resolved to the name "A", and name routing fans out to every
  // member carrying it — the DeliveryFailed reports only the leg
  // that never left.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Tick\nevent Ping\n"
              "agent A {\n"
              "  var peer: Opt<Agent> := none\n"
              "  on Boot { peer := spawn A() }\n"
              "  on Tick { emit Ping{} for peer }\n"
              "}");
  ASSERT_TRUE(image);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "peer");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, ActorPid{"n", "c", 2, 7});
  h.adapter.noteMemberPid(h.ctx, image->agentCell,
                          ActorPid{"worker", "agent", 1, 1});
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("DeliveryFailed"));
  EXPECT_THAT(journal, ::testing::Contains("Ping"));
}

TEST(VagentAdapterTest, VerbStopOnARemoteChildSendsThePerformative) {
  // `stop child` where child has a wire identity: §4.5's orderly
  // sequence belongs to the child's own agent, so the verb stages a
  // member-addressed send rather than mutating reference state — the
  // parent's ChildStopping arrives when the child's own agent acts,
  // not when the verb lands.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\nevent Tick\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "  on Tick { stop child }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid childPid{"node4", "childid", 3, 9};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, childPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_EQ(wire.size(), 1u);
  ASSERT_TRUE(wire[0].target);
  EXPECT_EQ(*wire[0].target, childPid);
  EXPECT_EQ(wire[0].value, "stop");
  // The local sequence never ran — no Destroy on the member, no
  // ChildStopping raise on the parent journal.
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Not(::testing::Contains("ChildStopping")));
}

TEST(VagentAdapterTest, WireStopFromParentRunsDestroyAndReportsBack) {
  // §7.1's "accepted from the parent only": genesis bound $parent
  // from the spawn request's source, so a wire `stop` from that pid
  // runs the §4.5 sequence — Destroy dispatches this turn and the
  // ChildStopping report rides the drain back to the parent's pid.
  AdapterHarness h;
  const auto image = h.lower("module demo\nagent A { }");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node1", "selfid", 1, 4};
  const ActorPid parentPid{"node0", "parentid", 1, 2};
  ActorMessage request;
  request.type   = MessageType::ActorSpawn;
  request.source = parentPid;
  ActorGenesis genesis;
  genesis.pid     = selfPid;
  genesis.request = &request;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });

  ActorMessage stop;
  stop.type   = MessageType::ActorSend;
  stop.source = parentPid;
  stop.target = selfPid;
  stop.value  = "stop";
  auto turn   = h.execution(stop);
  turn.pid    = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Destroy"));
  ASSERT_EQ(wire.size(), 1u);
  ASSERT_TRUE(wire[0].target);
  EXPECT_EQ(*wire[0].target, parentPid);
  EXPECT_EQ(wire[0].value, "ChildStopping{cause: \"stop\"}");
}

TEST(VagentAdapterTest, WireStopFromANonParentIsDropped) {
  // Same shape, wrong sender: a stop whose source is not the member
  // $parent's pid is consumed with no Destroy and no report — the
  // performative has no refuse kind.
  AdapterHarness h;
  const auto image = h.lower("module demo\nagent A { }");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node1", "selfid", 1, 4};
  const ActorPid parentPid{"node0", "parentid", 1, 2};
  const ActorPid strangerPid{"node7", "stranger", 5, 5};
  ActorMessage request;
  request.type   = MessageType::ActorSpawn;
  request.source = parentPid;
  ActorGenesis genesis;
  genesis.pid     = selfPid;
  genesis.request = &request;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });

  ActorMessage stop;
  stop.type   = MessageType::ActorSend;
  stop.source = strangerPid;
  stop.target = selfPid;
  stop.value  = "stop";
  auto turn   = h.execution(stop);
  turn.pid    = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Not(::testing::Contains("Destroy")));
  EXPECT_TRUE(wire.empty());
}

TEST(VagentAdapterTest, WireChildStoppingCollapsesTheTrailingDown) {
  // §4.5's "the parent reports one ChildStopping even when the
  // runtime's ActorDown follows it": the wire report marks the
  // shared dedupe key, so the orderly-reason ActorDown that trails
  // it raises nothing — and no ChildDown either.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid childPid{"node3", "childid", 2, 7};
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, childPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  ActorMessage stopping;
  stopping.type      = MessageType::ActorSend;
  stopping.requestId = "cs-1";
  stopping.source    = childPid;
  stopping.target    = selfPid;
  stopping.value     = "ChildStopping{cause: \"stop\"}";
  auto turnStop      = h.execution(stopping);
  turnStop.pid       = selfPid;
  ASSERT_TRUE(h.adapter(turnStop, h.ctx));

  ActorMessage down;
  down.type      = MessageType::ActorDown;
  down.requestId = "mon-1";
  down.source    = childPid;
  down.target    = selfPid;
  down.reason    = "stopped";
  auto turnDown  = h.execution(down);
  turnDown.pid   = selfPid;
  ASSERT_TRUE(h.adapter(turnDown, h.ctx));

  const auto journal = h.journalOf(image->agentCell);
  EXPECT_EQ(std::ranges::count(journal, "ChildStopping"), 1);
  EXPECT_THAT(journal, ::testing::Not(::testing::Contains("ChildDown")));
}

TEST(VagentAdapterTest, ActorDownUnreachableIsLossNotStopping) {
  // The monitor's verdict is the loss arm: suspicion + lease +
  // discovery evidence → ChildDown, never ChildStopping — "missing
  // evidence is never interpreted as absence" keeps local children
  // off this path entirely.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid childPid{"node3", "childid", 2, 7};
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, childPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  ActorMessage down;
  down.type      = MessageType::ActorDown;
  down.requestId = "mon-2";
  down.source    = childPid;
  down.target    = selfPid;
  down.reason    = "unreachable";
  auto turnDown  = h.execution(down);
  turnDown.pid   = selfPid;
  ASSERT_TRUE(h.adapter(turnDown, h.ctx));

  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("ChildDown"));
  EXPECT_THAT(journal,
              ::testing::Not(::testing::Contains("ChildStopping")));
}

TEST(VagentAdapterTest, OrderlyActorDownReasonsTakeTheStoppingArm) {
  // §4.5's closed cause set arrives on ActorDown.reason too — a
  // "terminate" verdict reports ChildStopping{cause:"terminate"},
  // and its pre-mark collapses a trailing loss-reason report.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid childPid{"node3", "childid", 2, 7};
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, childPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  ActorMessage down;
  down.type      = MessageType::ActorDown;
  down.requestId = "mon-3";
  down.source    = childPid;
  down.target    = selfPid;
  down.reason    = "terminate";
  auto turnDown  = h.execution(down);
  turnDown.pid   = selfPid;
  ASSERT_TRUE(h.adapter(turnDown, h.ctx));

  ActorMessage trailing;
  trailing.type      = MessageType::ActorDown;
  trailing.requestId = "mon-4";
  trailing.source    = childPid;
  trailing.target    = selfPid;
  trailing.reason    = "unreachable";
  auto turnLoss      = h.execution(trailing);
  turnLoss.pid       = selfPid;
  ASSERT_TRUE(h.adapter(turnLoss, h.ctx));

  const auto journal = h.journalOf(image->agentCell);
  EXPECT_EQ(std::ranges::count(journal, "ChildStopping"), 1);
  EXPECT_THAT(journal, ::testing::Not(::testing::Contains("ChildDown")));
}

TEST(VagentAdapterTest, MigrationGenesisWiresMovedToParentAndChild) {
  // §5.4's outbound half: a genesis carrying a checkpoint means the
  // agent reincarnated — it announces Moved{name} to the parent the
  // spawn request names and to each member whose $parent is the
  // agent, member-addressed sends the next drain puts on the wire.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Boot\n"
              "agent A {\n"
              "  var child: Opt<Agent> := none\n"
              "  on Boot { child := spawn A() }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node8", "reincarnation", 5, 88};
  const ActorPid parentPid{"node0", "parentid", 1, 2};
  const ActorPid childPid{"node3", "childid", 2, 7};
  // The child's member exists with its remote pid and its $parent
  // bound to the agent — the shape a spawned child keeps across the
  // same context. The parent's member and the $parentPid text are
  // what the checkpoint carried: first spawn minted them, promote
  // kept them, and the resume genesis rebinds $parent through them.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Boot")), h.ctx));
  const auto child = h.memberCell(image->agentCell, "child");
  ASSERT_TRUE(child);
  h.adapter.noteMemberPid(h.ctx, *child, childPid);
  auto &arena = h.ctx.arena;
  auto &core  = h.ctx.core;
  if (const auto rec =
          varsRecord(arena, core, *child, "$parent", true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, DimVector::POS,
                   arena.makeScalarCell(
                       static_cast<std::int64_t>(image->agentCell))));
  const auto parent = arena.makeCell();
  {
    const auto dirs   = core.findOrMintDimension("d.dirs");
    const auto agents = core.findOrMintDimension("d.agents");
    for (const CellRef dir :
         zigzag::rankAfter(arena, core.home(), dirs)) {
      zigzag::expectWritten(
          arena.link(zigzag::rankTail(arena, dir, agents), agents,
                     DimVector::POS, parent));
      break;
    }
  }
  h.adapter.noteMemberPid(h.ctx, parent, parentPid);
  if (const auto rec =
          varsRecord(arena, core, image->agentCell, "$parentPid", true))
    zigzag::expectWritten(
        arena.link(*rec, core.dims().values, DimVector::POS,
                   arena.makeCell("5:node08:parentid:1:2")));

  xanadu::Store transported;
  ActorMessage request;
  request.type   = MessageType::ActorSpawn;
  request.source = ActorPid{"node6", "migrator", 4, 40};
  ActorGenesis genesis;
  genesis.pid         = selfPid;
  genesis.request     = &request;
  genesis.transported = &transported;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));

  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  auto turn  = h.execution();
  turn.pid   = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  // One Moved per remote peer — parent and child — rendered with the
  // member's durable name so receivers can rebind their $peers.
  ASSERT_EQ(wire.size(), 2u);
  std::vector<ActorPid> targets;
  for (const auto &m : wire) {
    EXPECT_EQ(m.value, "Moved{name: \"A\"}");
    ASSERT_TRUE(m.source);
    EXPECT_EQ(*m.source, selfPid);
    ASSERT_TRUE(m.target);
    targets.push_back(*m.target);
  }
  EXPECT_THAT(targets,
              ::testing::UnorderedElementsAre(parentPid, childPid));
}

// A remote member minted straight into the directory — the shape
// `var := spawn A()` + noteMemberPid produces, without the
// one-agent-decl limit. Bound into `varName` when given so
// `by <var>` handles resolve through +d.name.
CellRef mintRemoteMember(AdapterHarness &h, CellRef agent,
                         std::string_view name, const ActorPid &pid,
                         std::string_view varName = {}) {
  auto &arena       = h.ctx.arena;
  const auto dirs   = h.ctx.core.findOrMintDimension("d.dirs");
  const auto agents = h.ctx.core.findOrMintDimension("d.agents");
  const auto member = arena.makeCell();
  for (const CellRef dir :
       zigzag::rankAfter(arena, h.ctx.core.home(), dirs)) {
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, dir, agents), agents,
                   DimVector::POS, member));
    break;
  }
  zigzag::expectWritten(
      arena.link(member, h.ctx.core.dims().name, DimVector::POS,
                 arena.makeCell(name)));
  h.adapter.noteMemberPid(h.ctx, member, pid);
  if (!varName.empty())
    if (const auto rec =
            varsRecord(arena, h.ctx.core, agent, varName, true))
      zigzag::expectWritten(
          arena.link(*rec, h.ctx.core.dims().values, DimVector::POS,
                     refCell(arena, h.ctx.core, member)));
  return member;
}

TEST(VagentAdapterTest, WireAchieveFromParentAdoptsAndReportsOutcome) {
  // §953/§955 receiver side: an achieve the wire carries as
  // `Deliver{__kind,__correlation}` adopts when accepts admits the
  // sender (the member $parent is kin), and the adopted child's
  // retire stages `outcome{status:"achieved"}` member-addressed back
  // to the delegator's pid — the semantic correlation echoes, never
  // the transport requestId.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent ChildDone\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  plan d for Deliver { emit ChildDone{} }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node1", "selfid", 1, 4};
  const ActorPid parentPid{"node0", "parentid", 1, 2};
  ActorMessage request;
  request.type   = MessageType::ActorSpawn;
  request.source = parentPid;
  ActorGenesis genesis;
  genesis.pid     = selfPid;
  genesis.request = &request;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });

  ActorMessage achieve;
  achieve.type      = MessageType::ActorSend;
  achieve.source    = parentPid;
  achieve.target    = selfPid;
  achieve.requestId = "w-achieve-1";
  achieve.value = "Deliver{__kind: \"achieve\", __correlation: \"r1\"}";
  auto turn         = h.execution(achieve);
  turn.pid          = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  // The plan ran in the same settle: ChildDone broadcast took its
  // remote leg to the parent, and the child's retire staged the
  // outcome reply on the same drain.
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");
  EXPECT_TRUE(zigzag::rankAfter(h.ctx.arena, image->agentCell, iDim)
                  .empty());
  ASSERT_EQ(wire.size(), 2u);
  EXPECT_EQ(wire[0].value, "ChildDone");
  ASSERT_TRUE(wire[1].target);
  EXPECT_EQ(*wire[1].target, parentPid);
  const auto outcome = parsePercept(wire[1].value);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->name, "outcome");
  std::string status, corr;
  for (const auto &[f, v] : outcome->fields) {
    if (f == "status")
      if (const auto *s = std::get_if<std::string>(&v)) status = *s;
    if (f == "__correlation")
      if (const auto *s = std::get_if<std::string>(&v)) corr = *s;
  }
  EXPECT_EQ(status, "achieved");
  EXPECT_EQ(corr, "r1");
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, WireAchieveFromNonKinIsRefusedOnTheWire) {
  // §7.1's default accepts: a remote member with no lineage either
  // way is refused — the reply rides the member-addressed arm to the
  // stranger's pid and nothing adopts.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  plan d for Deliver { }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node1", "selfid", 1, 4};
  const ActorPid strangerPid{"node7", "stranger", 5, 5};
  auto &arena = h.ctx.arena;
  const auto dirs   = h.ctx.core.findOrMintDimension("d.dirs");
  const auto agents = h.ctx.core.findOrMintDimension("d.agents");
  const auto stranger = arena.makeCell();
  for (const CellRef dir :
       zigzag::rankAfter(arena, h.ctx.core.home(), dirs)) {
    zigzag::expectWritten(
        arena.link(zigzag::rankTail(arena, dir, agents), agents,
                   DimVector::POS, stranger));
    break;
  }
  zigzag::expectWritten(
      arena.link(stranger, h.ctx.core.dims().name, DimVector::POS,
                 arena.makeCell("Stranger")));
  h.adapter.noteMemberPid(h.ctx, stranger, strangerPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });

  ActorMessage achieve;
  achieve.type      = MessageType::ActorSend;
  achieve.source    = strangerPid;
  achieve.target    = selfPid;
  achieve.requestId = "w-achieve-2";
  achieve.value = "Deliver{__kind: \"achieve\", __correlation: \"r2\"}";
  auto turn         = h.execution(achieve);
  turn.pid          = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");
  EXPECT_TRUE(zigzag::rankAfter(arena, image->agentCell, iDim).empty());
  ASSERT_EQ(wire.size(), 1u);
  ASSERT_TRUE(wire[0].target);
  EXPECT_EQ(*wire[0].target, strangerPid);
  const auto refuse = parsePercept(wire[0].value);
  ASSERT_TRUE(refuse);
  // The refused goal's name is the percept's own — the same term the
  // achieve carried, so the wire reads `Deliver{__kind:"refuse",…}`.
  EXPECT_EQ(refuse->name, "Deliver");
  std::string kind, corr;
  for (const auto &[f, v] : refuse->fields) {
    if (f == "__kind")
      if (const auto *s = std::get_if<std::string>(&v)) kind = *s;
    if (f == "__correlation")
      if (const auto *s = std::get_if<std::string>(&v)) corr = *s;
  }
  EXPECT_EQ(kind, "refuse");
  EXPECT_EQ(corr, "r2");
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, WireOutcomeReadiesTheParkedAchieve) {
  // Sender side: `achieve … by peer` stages the wire request with
  // its self-correlation, the delegate's `outcome{achieved}` echoes
  // the token through `__correlation`, and the parked intention
  // readies — the spine resumes into emit Done.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "goal Outer() perform\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  var peer: Opt<Agent> := none\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { achieve Deliver() by peer "
              "within 9s; emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid peerPid{"node9", "peerid", 2, 7};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  mintRemoteMember(h, image->agentCell, "Peer", peerPid, "peer");
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  // Turn for Tick: the achieve leaves as `Deliver{__kind,__correlation}`
  // — Peer-named members resolve remote-only, so one leg wires.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_EQ(wire.size(), 1u);
  const auto sent = parsePercept(wire[0].value);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent->name, "Deliver");
  std::string corr;
  for (const auto &[f, v] : sent->fields)
    if (f == "__correlation")
      if (const auto *s = std::get_if<std::string>(&v)) corr = *s;
  ASSERT_FALSE(corr.empty());

  // The delegate's outcome arrives on the wire: the parked
  // intention's `correlation` record matches and it readies.
  ActorMessage reply;
  reply.type      = MessageType::ActorSend;
  reply.source    = peerPid;
  reply.target    = selfPid;
  reply.requestId = "w-outcome-1";
  reply.value     = "outcome{status: \"achieved\", __kind: \"outcome\", "
                    "__correlation: \"" +
                    corr + "\"}";
  auto turn       = h.execution(reply);
  turn.pid        = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  // The resumed spine emits Done — the broadcast's remote leg wires
  // to the peer, the local leg journals.
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("Done"));
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, WireOutcomeFailureFaultsTheParkedAchieve) {
  // §955's failure arm: `outcome{status:"failed",cause:"nope"}`
  // writes the cause onto the parked intention's refused field;
  // with no else recovery the delegated rung faults it — PlanFailed
  // journals with the wire's cause intact.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\nevent Done\n"
              "goal Outer() perform\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  var peer: Opt<Agent> := none\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { achieve Deliver() by peer "
              "within 9s;\n"
              "                   emit Done{} }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid peerPid{"node9", "peerid", 2, 7};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  mintRemoteMember(h, image->agentCell, "Peer", peerPid, "peer");
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_EQ(wire.size(), 1u);
  const auto sent = parsePercept(wire[0].value);
  ASSERT_TRUE(sent);
  std::string corr;
  for (const auto &[f, v] : sent->fields)
    if (f == "__correlation")
      if (const auto *s = std::get_if<std::string>(&v)) corr = *s;
  ASSERT_FALSE(corr.empty());

  ActorMessage reply;
  reply.type      = MessageType::ActorSend;
  reply.source    = peerPid;
  reply.target    = selfPid;
  reply.requestId = "w-outcome-2";
  reply.value     = "outcome{status: \"failed\", cause: \"nope\", "
                    "__kind: \"outcome\", __correlation: \"" +
                    corr + "\"}";
  auto turn       = h.execution(reply);
  turn.pid        = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  // The delegated rung sees the refused mark the same settle —
  // PlanFailed raises with the wire's cause and the intention
  // carries faulted, never Done.
  const auto journal = h.journalOf(image->agentCell);
  EXPECT_THAT(journal, ::testing::Contains("PlanFailed"));
  EXPECT_THAT(journal, ::testing::Not(::testing::Contains("Done")));
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, WireCancelDropsTheAdoptedChild) {
  // §954 receiver side: the adopted child records the achieve's
  // correlation as delegatorCorrelation, and a wire `cancel` naming
  // the same token drops it — GoalDropped raises, the rank clears.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Grant\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  plan d for Deliver { await Grant as g within 9s }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node1", "selfid", 1, 4};
  const ActorPid parentPid{"node0", "parentid", 1, 2};
  ActorMessage request;
  request.type   = MessageType::ActorSpawn;
  request.source = parentPid;
  ActorGenesis genesis;
  genesis.pid     = selfPid;
  genesis.request = &request;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });

  ActorMessage achieve;
  achieve.type      = MessageType::ActorSend;
  achieve.source    = parentPid;
  achieve.target    = selfPid;
  achieve.requestId = "w-achieve-3";
  achieve.value = "Deliver{__kind: \"achieve\", __correlation: \"r3\"}";
  auto turn         = h.execution(achieve);
  turn.pid          = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  // Adopted and parked on the await — one live intention carrying
  // the delegation correlation.
  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");
  ASSERT_EQ((zigzag::rankAfter(h.ctx.arena, image->agentCell, iDim) |
             std::ranges::to<std::vector>())
                .size(),
            1u);

  ActorMessage cancel;
  cancel.type      = MessageType::ActorSend;
  cancel.source    = parentPid;
  cancel.target    = selfPid;
  cancel.requestId = "w-cancel-1";
  cancel.value     = "cancel{__kind: \"cancel\", __correlation: \"r3\"}";
  auto turn2       = h.execution(cancel);
  turn2.pid        = selfPid;
  ASSERT_TRUE(h.adapter(turn2, h.ctx));

  EXPECT_TRUE(zigzag::rankAfter(h.ctx.arena, image->agentCell, iDim)
                  .empty());
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("GoalDropped"));
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, WireCancelBeforeAdoptionIsRemembered) {
  // §954's in-flight case: a cancel naming a correlation nobody
  // adopted yet mints $cancelled:<id>, and the late achieve with the
  // same token is consumed silently — no adoption, no reply.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  plan d for Deliver { }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"node1", "selfid", 1, 4};
  const ActorPid parentPid{"node0", "parentid", 1, 2};
  ActorMessage request;
  request.type   = MessageType::ActorSpawn;
  request.source = parentPid;
  ActorGenesis genesis;
  genesis.pid     = selfPid;
  genesis.request = &request;
  ASSERT_TRUE(h.adapter.genesis(genesis, h.ctx));
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });

  ActorMessage cancel;
  cancel.type      = MessageType::ActorSend;
  cancel.source    = parentPid;
  cancel.target    = selfPid;
  cancel.requestId = "w-cancel-2";
  cancel.value     = "cancel{__kind: \"cancel\", __correlation: \"r9\"}";
  auto turn        = h.execution(cancel);
  turn.pid         = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));
  ASSERT_TRUE(
      varsRecord(h.ctx.arena, h.ctx.core, image->agentCell,
                 "$cancelled:r9", false)
          .has_value());

  ActorMessage achieve;
  achieve.type      = MessageType::ActorSend;
  achieve.source    = parentPid;
  achieve.target    = selfPid;
  achieve.requestId = "w-achieve-4";
  achieve.value = "Deliver{__kind: \"achieve\", __correlation: \"r9\"}";
  auto turn2        = h.execution(achieve);
  turn2.pid         = selfPid;
  ASSERT_TRUE(h.adapter(turn2, h.ctx));

  const auto iDim = h.ctx.core.findOrMintDimension("d.intention");
  EXPECT_TRUE(zigzag::rankAfter(h.ctx.arena, image->agentCell, iDim)
                  .empty());
  EXPECT_TRUE(wire.empty());

  // The marker holds only the retry window — send_retries ×
  // send_retry_interval turns. Run it out: the same achieve id
  // afterwards is fresh, adopts, and the child's retire reports the
  // outcome the delegator long gave up waiting for.
  for (int i = 0; i < 6; ++i) {
    auto t = h.execution();
    t.pid  = selfPid;
    ASSERT_TRUE(h.adapter(t, h.ctx));
  }
  ActorMessage late;
  late.type      = MessageType::ActorSend;
  late.source    = parentPid;
  late.target    = selfPid;
  late.requestId = "w-achieve-5";
  late.value = "Deliver{__kind: \"achieve\", __correlation: \"r9\"}";
  auto turn3     = h.execution(late);
  turn3.pid      = selfPid;
  ASSERT_TRUE(h.adapter(turn3, h.ctx));
  ASSERT_EQ(wire.size(), 1u);
  const auto outcome = parsePercept(wire[0].value);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->name, "outcome");
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, UnmarshalableRemotePayloadFaultsHonestly) {
  // A payload field the flat Name{f: v} codec can't represent —
  // here a relocation-marked live position — must not render as a
  // corrupt integer field. The remote arm refuses through
  // deliveryFailed instead of emitting it.
  AdapterHarness h;
  const auto image = h.lower("module demo\nagent A { }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid peerPid{"node9", "peerid", 2, 7};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  mintRemoteMember(h, image->agentCell, "Peer", peerPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  // An emit entry whose payload carries a ref-marked field — the
  // record shape stageOutbox writes, with the one child the codec
  // cannot honor.
  const auto outboxDim = h.ctx.core.findOrMintDimension("d.outbox");
  const auto entry     = arena.makeCell();
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, image->agentCell, outboxDim),
                 outboxDim, DimVector::POS, entry));
  const auto field = [&](std::string_view name, CellRef value) {
    if (const auto rec =
            varsRecord(arena, h.ctx.core, entry, name, true))
      zigzag::expectWritten(
          arena.link(*rec, h.ctx.core.dims().values, DimVector::POS,
                     value));
  };
  field("kind", arena.makeCell("emit"));
  field("target", arena.makeCell("Peer"));
  field("id", arena.makeCell("e-marshal"));
  const auto payload = arena.makeCell("Ping");
  const auto refArg  = refCell(arena, h.ctx.core, image->agentCell);
  zigzag::expectWritten(arena.link(
      refArg, h.ctx.core.dims().name, DimVector::POS,
      arena.makeCell("who")));
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, payload,
                                  h.ctx.core.dims().step),
                 h.ctx.core.dims().step, DimVector::POS, refArg));
  field("payload", refCell(arena, h.ctx.core, payload));

  auto turn = h.execution();
  turn.pid  = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  EXPECT_TRUE(wire.empty());
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("DeliveryFailed"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, NestedPayloadFaultsHonestly) {
  // A compound field marshals through marshalTermExpr with the
  // `+d.marshal` mark — +d.step is one rank, so a nested term's args
  // splice in as apparent siblings and only the mark keeps the flat
  // codec honest. The remote arm must refuse the send rather than
  // render `Outer{loc: "Inner", x: 1}`.
  AdapterHarness h;
  const auto image = h.lower("module demo\nagent A { }");
  ASSERT_TRUE(image);
  auto &arena = h.ctx.arena;
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid peerPid{"node9", "peerid", 2, 7};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  mintRemoteMember(h, image->agentCell, "Peer", peerPid);
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  // Outer{loc: Inner{x: 1}} — the nested head's own arg splices into
  // Outer's step rank at marshal time.
  auto one   = std::make_unique<Expr>();
  one->kind  = ExprKind::Integer;
  one->text  = "1";
  auto inner = std::make_unique<Expr>();
  inner->kind = ExprKind::Record;
  inner->text = "Inner";
  inner->names = {"x"};
  inner->children.push_back(std::move(one));
  Expr outer;
  outer.kind = ExprKind::Record;
  outer.text = "Outer";
  outer.names = {"loc"};
  outer.children.push_back(std::move(inner));
  const auto payload =
      xanadu::vagent::marshalTermExpr(arena, h.ctx.core, outer);
  ASSERT_TRUE(payload);

  const auto outboxDim = h.ctx.core.findOrMintDimension("d.outbox");
  const auto entry     = arena.makeCell();
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, image->agentCell, outboxDim),
                 outboxDim, DimVector::POS, entry));
  const auto field = [&](std::string_view name, CellRef value) {
    if (const auto rec =
            varsRecord(arena, h.ctx.core, entry, name, true))
      zigzag::expectWritten(
          arena.link(*rec, h.ctx.core.dims().values, DimVector::POS,
                     value));
  };
  field("kind", arena.makeCell("emit"));
  field("target", arena.makeCell("Peer"));
  field("id", arena.makeCell("e-nested"));
  field("payload", refCell(arena, h.ctx.core, *payload));

  auto turn = h.execution();
  turn.pid  = selfPid;
  ASSERT_TRUE(h.adapter(turn, h.ctx));

  EXPECT_TRUE(wire.empty());
  EXPECT_THAT(h.journalOf(image->agentCell),
              ::testing::Contains("DeliveryFailed"));
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}

TEST(VagentAdapterTest, DelegatedDeadlineStagesTheCancelWire) {
  // §954 sender side: a `by peer` wait that reaches its within bound
  // owes the delegate a best-effort cancel — the drain renders it
  // member-addressed with the achieve's correlation echoed.
  AdapterHarness h;
  const auto image =
      h.lower("module demo\nevent Tick\n"
              "goal Outer() perform\n"
              "goal Deliver() perform\n"
              "agent A {\n"
              "  var peer: Opt<Agent> := none\n"
              "  on Tick { adopt Outer() }\n"
              "  plan o for Outer { achieve Deliver() by peer "
              "within 2s }\n"
              "}");
  ASSERT_TRUE(image);
  const ActorPid selfPid{"worker", "agent", 1, 1};
  const ActorPid peerPid{"node9", "peerid", 2, 7};
  std::vector<ActorMessage> wire;
  h.adapter.setSendSender(
      [&wire](WorkerContext &, const ActorMessage &message)
          -> std::expected<void, ActorError> {
        wire.push_back(message);
        return {};
      });
  mintRemoteMember(h, image->agentCell, "Peer", peerPid, "peer");
  h.adapter.noteMemberPid(h.ctx, image->agentCell, selfPid);

  // Turn 1 stages the achieve; turns 2–3 run the bound out — the
  // expiry stages cancel before the fault ladder owns the wait.
  ASSERT_TRUE(h.adapter(h.execution(h.send("Tick")), h.ctx));
  ASSERT_EQ(wire.size(), 1u);
  const auto sent = parsePercept(wire[0].value);
  ASSERT_TRUE(sent);
  std::string corr;
  for (const auto &[f, v] : sent->fields)
    if (f == "__correlation")
      if (const auto *s = std::get_if<std::string>(&v)) corr = *s;
  ASSERT_FALSE(corr.empty());
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));
  ASSERT_TRUE(h.adapter(h.execution(), h.ctx));

  bool sawCancel = false;
  for (const auto &m : wire) {
    if (m.value.find("cancel") == std::string::npos) continue;
    sawCancel = true;
    ASSERT_TRUE(m.target);
    EXPECT_EQ(*m.target, peerPid);
    EXPECT_THAT(m.value, ::testing::HasSubstr("__correlation: \"" + corr +
                                                "\""));
  }
  EXPECT_TRUE(sawCancel);
  EXPECT_EQ(h.ctx.arena.outstandingMarks(), 0u);
}

} // namespace
